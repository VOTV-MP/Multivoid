#include "ue_wrap/call.h"

#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <cstring>
#include <mutex>
#include <unordered_map>

namespace ue_wrap {

namespace {

// Process-wide cache of UFunction* -> ParamFrame::Metadata. Built lazily on
// first construction for a given fn; entries live for the process lifetime
// (UE4 UFunctions are class-static, never GC'd during a running game).
//
// Audit fix 2026-05-29 D4-1: prior ParamFrame ctor walked the FProperty
// chain + heap-allocated a wstring per param + emplaced into a fresh
// std::vector<std::pair<std::wstring, int32_t>> on every call. With the
// per-snapshot Drive() path dispatching N UFunctions per tick at 60 Hz,
// that's tens of thousands of allocs/sec under load. The cache reduces
// each subsequent ParamFrame for the same fn to a single std::unordered_
// map::find + buf_.assign (the per-call zeroed frame remains).
//
// Thread safety: lookup acquires the mutex briefly; resolution work
// (FunctionFrameSize + FunctionParams) happens with the mutex held the
// first time a function is seen. UE4 ProcessEvent dispatch is game-
// thread-only in practice (the parallel anim task-graph worker reaches
// ProcessEvent only via Pump-posted lambdas which dispatch back to the
// game thread per ue_wrap/game_thread.h:118-120), but the mutex makes
// the cache safe under any caller pattern.
std::mutex g_metaMutex;
std::unordered_map<void*, ParamFrame::Metadata> g_meta;

const ParamFrame::Metadata* GetOrBuildMetadata(void* fn) {
    {
        std::lock_guard<std::mutex> lk(g_metaMutex);
        auto it = g_meta.find(fn);
        if (it != g_meta.end()) return &it->second;
    }
    // First sighting: build outside the cache lock, then commit under it.
    // The reflection calls don't recurse into ParamFrame, so it's safe to
    // hold the lock across the resolve too -- but doing the work outside
    // shortens the contended critical section in the (unlikely) case two
    // threads race a first-time resolve for different fns.
    ParamFrame::Metadata built;
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
    std::lock_guard<std::mutex> lk(g_metaMutex);
    // Race-against-self: another thread may have inserted the same fn in
    // the window above; emplace returns the existing entry in that case
    // (built drops on function return).
    auto [it, ok] = g_meta.emplace(fn, std::move(built));
    return &it->second;
}

}  // namespace

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
    if (meta_->frameSize > 0) {
        buf_.assign(static_cast<size_t>(meta_->frameSize), 0);
    }
}

int32_t ParamFrame::OffsetOf(const wchar_t* name) const {
    if (!meta_) return -1;
    for (const auto& o : meta_->offsets) {
        // Case-INSENSITIVE: param names are FNames (engine compares by
        // ComparisonIndex; the rendered casing is whatever got registered
        // FIRST in this process) -- the same load-order roulette as
        // reflection::NameEquals. Found via the email mirror's addEmail
        // frame: "unknown param 'item'" while the BP param rendered under a
        // different first-registered casing (smoke 2026-06-12).
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
        UE_LOGE("ParamFrame::Get: unknown param '%ls'", name);
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
