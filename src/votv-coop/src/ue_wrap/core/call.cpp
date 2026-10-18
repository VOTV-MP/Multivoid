#include "ue_wrap/core/call.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ue_wrap {

namespace {

// Process-wide cache of UFunction to ParamFrame::Metadata, built lazily the first time a given
// function is framed. It exists because the alternative is per-call: walking the FProperty chain,
// heap-allocating a wstring per parameter and filling a fresh vector, on every frame construction,
// tens of thousands of allocations a second under the per-snapshot drive path's load. Cached, each
// later frame costs one map lookup, two slot reads and a buffer assign.
//
// Thread safety: the lookup takes the mutex briefly, and the resolve work runs with it held
// the first time a function is seen. That is not belt and braces -- the engine dispatches
// ProcessEvent from task-graph workers too, for parallel animation, which is why the detour
// forwards off-thread rather than draining our queue there (ue_wrap/core/pe_detour.cpp).

// An entry holds while the function's object-array slot still holds it at the serial the entry
// captured: a world can load a Blueprint class anew (127 classes did in one same-process rejoin),
// and a new function can then sit at a dead one's address with another frame. A replaced entry is
// retired, never freed, since a frame built from it may still be in use.
struct MetaEntry {
    std::unique_ptr<ParamFrame::Metadata> meta;
    int32_t idx = -1;     // the function's object-array slot as the entry was built
    int32_t serial = 0;   // and that slot's serial then
};
std::mutex g_metaMutex;
std::unordered_map<void*, MetaEntry> g_meta;
std::vector<std::unique_ptr<ParamFrame::Metadata>> g_retired;

// Whether the entry still describes the function object at `fn`: two array reads.
bool Current(const MetaEntry& e, void* fn) {
    return e.idx >= 0 && reflection::ObjectAt(e.idx) == fn && reflection::SlotSerial(e.idx) == e.serial;
}

const ParamFrame::Metadata* GetOrBuildMetadata(void* fn) {
    {
        std::lock_guard<std::mutex> lk(g_metaMutex);
        auto it = g_meta.find(fn);
        if (it != g_meta.end() && Current(it->second, fn)) return it->second.meta.get();
    }
    // First sighting: build outside the cache lock, then commit under it.
    // The reflection calls don't recurse into ParamFrame, so it's safe to
    // hold the lock across the resolve too -- but doing the work outside
    // shortens the contended critical section in the (unlikely) case two
    // threads race a first-time resolve for different fns.
    auto owned = std::make_unique<ParamFrame::Metadata>();
    ParamFrame::Metadata& built = *owned;
    built.frameSize = reflection::FunctionFrameSize(fn);
    // Trust boundary: frameSize is UStruct::PropertiesSize read straight out of
    // engine memory. A real UFunction parameter frame is at most a few KB (UE4's
    // own ProcessEvent stack-allocates it). A garbage-large value here -- e.g. fn
    // points at a recycled GUObjectArray slot whose UFunction* now aliases a freed
    // or foreign UObject, so PropertiesSize is read from the wrong field -- would
    // otherwise drive buf_.assign(frameSize) below into a multi-hundred-MB / GB
    // allocation, repeated every ParamFrame construction (the buf_ alloc is
    // per-call, NOT cached). That is the 4-peer-smoke client RSS balloon: one
    // ApplyToEngine/Drive ParamFrame per net_pump tick allocating ~GB, the
    // following CallFunction AVing on the bogus frame (the one absorbed exception
    // per tick), RSS climbing to the harness cap. Reject an out-of-range frame the
    // same way a negative one is rejected (frameSize < 0 below) -- the ParamFrame
    // stays invalid, Call() no-ops, and the bogus dispatch never happens.
    // 64 KiB is orders of magnitude above any legitimate UFunction frame yet far
    // below any value that balloons RSS.
    constexpr int32_t kMaxFrameSize = 64 * 1024;
    if (built.frameSize > kMaxFrameSize) {
        UE_LOGE("ParamFrame: function %p reports frame size %d > %d cap -- "
                "rejecting (corrupt/stale UFunction*?)",
                fn, built.frameSize, kMaxFrameSize);
        built.frameSize = -1;  // mark malformed; ParamFrame ctor refuses below
    } else if (built.frameSize > 0) {
        for (const auto& p : reflection::FunctionParams(fn)) {
            built.offsets.emplace_back(p.name, p.offset);
        }
    }
    const int32_t idx = reflection::InternalIndexOf(fn);
    const int32_t serial = reflection::AllocateSlotSerial(idx);
    std::lock_guard<std::mutex> lk(g_metaMutex);
    auto it = g_meta.find(fn);
    if (it == g_meta.end()) {
        auto [ins, ok] = g_meta.emplace(fn, MetaEntry{std::move(owned), idx, serial});
        return ins->second.meta.get();
    }
    // Another thread may have built the same fn in the window above; a stale entry is replaced.
    if (Current(it->second, fn)) return it->second.meta.get();
    g_retired.push_back(std::move(it->second.meta));
    it->second = MetaEntry{std::move(owned), idx, serial};
    return it->second.meta.get();
}

// The frame counters behind GetFrameStats. Relaxed adds: each is read once a second by the perf
// probe, and no reader needs one counter to be consistent with another at an instant.
std::atomic<unsigned long long> g_frames{0}, g_allocs{0}, g_bytes{0};
std::atomic<unsigned long long> g_bucket[5]{};
std::atomic<int32_t> g_maxSize{0};

// Band an allocating frame by size -- disjoint, first match wins -- so the number can answer what
// an inline buffer would cover and not only how often one would be used.
void NoteFrame(int32_t frameSize) {
    g_frames.fetch_add(1, std::memory_order_relaxed);
    if (frameSize <= 0) return;
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(static_cast<unsigned long long>(frameSize), std::memory_order_relaxed);
    const int32_t edges[5] = {16, 32, 64, 128, 256};
    for (int i = 0; i < 5; ++i) {
        if (frameSize <= edges[i]) { g_bucket[i].fetch_add(1, std::memory_order_relaxed); break; }
    }
    int32_t seen = g_maxSize.load(std::memory_order_relaxed);
    while (frameSize > seen &&
           !g_maxSize.compare_exchange_weak(seen, frameSize, std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
    }
}

// A read of a parameter the function does not have is said once per function name, not once per call: a caller
// that asks every request would otherwise fill the log with one drift. The set grows only on this error path.
bool FirstMissingRead(void* fn) {
    static std::mutex s_mutex;
    static std::unordered_set<std::wstring> s_said;
    const std::wstring key = reflection::ToString(reflection::NameOf(fn));
    std::lock_guard<std::mutex> lk(s_mutex);
    return s_said.insert(key).second;
}

}  // namespace

FrameStats GetFrameStats() {
    FrameStats s{};
    s.frames = g_frames.load(std::memory_order_relaxed);
    s.allocs = g_allocs.load(std::memory_order_relaxed);
    s.bytes  = g_bytes.load(std::memory_order_relaxed);
    for (int i = 0; i < 5; ++i) s.bucket[i] = g_bucket[i].load(std::memory_order_relaxed);
    s.maxSize = g_maxSize.load(std::memory_order_relaxed);
    return s;
}

ParamFrame::ParamFrame(void* function) : fn_(function) {
    if (!fn_) return;
    meta_ = GetOrBuildMetadata(fn_);
    if (meta_->frameSize < 0) {  // genuinely malformed UFunction; refuse
        UE_LOGE("ParamFrame: function %p has negative frame size %d",
                fn_, meta_->frameSize);
        fn_ = nullptr;
        meta_ = nullptr;
        return;
    }
    // frameSize == 0 is VALID: a no-param/no-return UFunction (e.g. K2_DestroyActor).
    // ProcessEvent is then invoked with a null params buffer (buf_ stays empty).
    // Previously this was treated as an error and the call became a silent no-op --
    // so DestroyActor never destroyed anything (freecam cams / nameplates / puppets
    // leaked). Keep fn_ so the call goes through.
    NoteFrame(meta_->frameSize);
    if (meta_->frameSize > 0) {
        buf_.assign(static_cast<size_t>(meta_->frameSize), 0);
    }
}

int32_t ParamFrame::OffsetOf(const wchar_t* name) const {
    if (!meta_) return -1;
    for (const auto& o : meta_->offsets) {
        // Case-INSENSITIVE, because parameter names are FNames: the engine compares by comparison
        // index and the rendered casing is whichever spelling was registered FIRST in this process,
        // the same load-order roulette reflection::NameEquals handles. A frame whose parameter is
        // rendered under another casing reads as an unknown parameter under a case-sensitive
        // compare.
        if (::_wcsicmp(o.first.c_str(), name) == 0) return o.second;
    }
    return -1;
}

bool ParamFrame::SetRaw(const wchar_t* name, const void* src, int32_t size) {
    if (fn_ == nullptr || buf_.empty()) return false;  // empty => zero-param frame; nothing to set
    const int32_t off = OffsetOf(name);
    if (off < 0) {
        UE_LOGE("ParamFrame::Set: unknown param '%ls'", name);
        return false;
    }
    if (off + size > static_cast<int32_t>(buf_.size())) {
        UE_LOGE("ParamFrame::Set: param '%ls' off=%d size=%d overflows frame %zu",
                name, off, size, buf_.size());
        return false;
    }
    std::memcpy(buf_.data() + off, src, static_cast<size_t>(size));
    return true;
}

bool ParamFrame::GetRaw(const wchar_t* name, void* dst, int32_t size) const {
    if (fn_ == nullptr || buf_.empty()) return false;
    const int32_t off = OffsetOf(name);
    if (off < 0) {
        if (FirstMissingRead(fn_)) UE_LOGE("ParamFrame::Get: unknown param '%ls' (said once per function)", name);
        return false;
    }
    if (off + size > static_cast<int32_t>(buf_.size())) {
        UE_LOGE("ParamFrame::Get: param '%ls' off=%d size=%d overflows frame %zu",
                name, off, size, buf_.size());
        return false;
    }
    std::memcpy(dst, buf_.data() + off, static_cast<size_t>(size));
    return true;
}

bool Call(void* object, ParamFrame& frame) {
    if (!frame.valid()) return false;
    return reflection::CallFunction(object, frame.function(), frame.data());
}

}  // namespace ue_wrap
