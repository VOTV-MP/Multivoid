#include "ue_wrap/core/gc_pin.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ue_wrap {
namespace {

namespace R = ue_wrap::reflection;

// The outstanding-pin registry. Its ONLY job is to make "what is still pinned" a
// question that can be asked -- the failure this class exists to prevent was invisible
// precisely because nothing could enumerate the pins. Keyed by object pointer; the
// value is the UWorld the object belonged to when it was pinned (null = not
// world-scoped: an asset, a CDO, an attenuation settings object).
//
// A mutex rather than a game-thread assert: it keeps the REGISTRY consistent if a
// diagnostic ever reads it off the game thread. That is all it buys -- reading a pinned
// object's class still requires the game thread, so ReportWorldScopedPins is game-thread
// like everything else here. (Audit 2026-09-01 caught the earlier comment claiming more.)
//
// BOTH ARE DELIBERATELY LEAKED. A GcPin can live inside another static (trash_proxy's
// proxy map, engine_audio's permanent pins), so a Release() can run during static
// destruction at DLL unload -- after a normally-declared registry would already have been
// destroyed. Leaking them makes that ordering unobservable. Nothing is lost: the process
// is exiting, and the memory goes back with it.
std::mutex& Mu() {
    static auto* mu = new std::mutex();
    return *mu;
}
std::unordered_map<void*, void*>& Pins() {
    static auto* pins = new std::unordered_map<void*, void*>();
    return *pins;
}

// Once tripped, Release() stops touching the ENGINE. Un-rooting reads the object's own
// InternalIndex and then walks GUObjectArray, and neither is a safe read once the process
// is tearing down -- while the un-root itself has no meaning any more, because there is no
// collector left to run. The handle is still cleared and the registry still updated, so
// the bookkeeping stays honest for anything that logs on the way out.
std::atomic<bool> g_releasesStopped{false};

// The stamp for "world-scoped, but we could not name which world" -- world_identity was
// Degraded at pin time. Not a real pointer and never dereferenced: it exists so the report
// can treat an unknown as SUSPECT rather than silently as "process-lifetime, ignore me".
void* const kWorldUnknown = reinterpret_cast<void*>(static_cast<uintptr_t>(1));

}  // namespace

bool GcPin::Pin(void* obj) {
    Release();
    if (!obj) return false;
    // Refuse an object that is ALREADY in the root set. RootSet is a flag, not a refcount,
    // so a second pin over someone else's -- ours or the ENGINE's -- looks identical while
    // it is held, and the first release to run clears the bit for both. Refusing is the only
    // honest answer: whoever rooted it first still owns it.
    if (R::InternalFlagsOf(obj) & 0x40000000) {
        UE_LOGW("gc_pin: refusing to pin %p -- it is ALREADY in the root set; the existing "
                "owner keeps it, and a second pin would be cleared by whichever released first",
                obj);
        return false;
    }
    if (!R::AddToRoot(obj)) return false;
    obj_ = obj;
    // Stamp the world at pin time, for the same reason CachedObjRef does: it is the only
    // moment the answer is cheap and certain. WorldOf is a pure read.
    // Warm the world memo BEFORE asking. `WorldOf` is a pure read that answers nullptr
    // whenever world_identity's offsets are unresolved, and nullptr is the SAME value it
    // uses for "not world-scoped" -- so a pin taken while the memo was cold would be filed
    // as a harmless process-lifetime pin and ReportWorldScopedPins would stay silent with
    // hundreds of world anchors outstanding. That is the one instrument able to catch a
    // regression of the bug this class exists for, so it must not be able to pass blind.
    (void)world_identity::CurrentWorld();
    void* world = world_identity::WorldOf(obj);
    if (!world && world_identity::Degraded()) world = kWorldUnknown;
    std::lock_guard<std::mutex> lk(Mu());
    Pins()[obj] = world;
    return true;
}

void GcPin::Release() {
    if (!obj_) return;
    void* const obj = obj_;
    obj_ = nullptr;
    // UNCONDITIONAL on the object's STATE. No Alive() test, no world test: this is exactly
    // the branch whose absence leaked 871 actors and, with them, a whole UWorld. The only
    // thing that suppresses it is the process itself going away (see g_releasesStopped).
    if (g_releasesStopped.load(std::memory_order_relaxed)) {
        // Process teardown. Return BEFORE the lock, not just before the engine read: at exit
        // the mutex's owner may be a thread Windows has already terminated, and such a mutex
        // never unlocks -- the same census `coop/session/shutdown.h` makes about its own
        // slow-path lock. The registry is meaningless now; the handle is already cleared.
        return;
    }
    R::RemoveFromRoot(obj);
    std::lock_guard<std::mutex> lk(Mu());
    Pins().erase(obj);
}

void GcPin::StopReleases() { g_releasesStopped.store(true, std::memory_order_relaxed); }

size_t GcPin::Outstanding() {
    std::lock_guard<std::mutex> lk(Mu());
    return Pins().size();
}

size_t GcPin::ReportWorldScopedPins(const char* tag) {
    std::vector<void*> stale;
    {
        std::lock_guard<std::mutex> lk(Mu());
        for (const auto& kv : Pins())
            if (kv.second) stale.push_back(kv.first);
    }
    if (stale.empty()) return 0;
    // Name the classes -- "12 pins outstanding" is a number, "12 StaticMeshActor" is a
    // pointer at the module that owns them.
    // `other` counts what does not fit the 64-class table, so a summary can never imply a
    // total smaller than the count on the same line.
    std::vector<std::pair<std::wstring, int>> byClass;
    int other = 0;
    for (void* o : stale) {
        const std::wstring cn = R::ClassNameOf(o);
        bool found = false;
        for (auto& e : byClass)
            if (e.first == cn) { ++e.second; found = true; break; }
        if (found) continue;
        if (byClass.size() < 64) byClass.emplace_back(cn, 1); else ++other;
    }
    std::sort(byClass.begin(), byClass.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::wstring top;
    for (size_t i = 0; i < byClass.size() && i < 8; ++i)
        top += byClass[i].first + L"x" + std::to_wstring(byClass[i].second) + L" ";
    if (other) top += L"(+" + std::to_wstring(other) + L" in other classes) ";
    UE_LOGW("gc_pin[%s]: %zu WORLD-SCOPED pin(s) still outstanding of %zu total -- each one "
            "anchors its world's Outer chain and will keep that UWorld from ever being "
            "collected: %ls",
            tag, stale.size(), Outstanding(), top.c_str());
    return stale.size();
}

}  // namespace ue_wrap
