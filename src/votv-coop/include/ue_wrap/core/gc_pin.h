#pragma once

#include <cstddef>

namespace ue_wrap {

// ---------------------------------------------------------------------------------
// GcPin -- an OWNED GC pin. The only supported way to keep a runtime-constructed
// UObject alive from C++.
//
// WHY IT EXISTS (measured 2026-09-01, rooting the rejoin crash):
//
// A C++ pointer is invisible to UE's reachability scan, so a runtime spawn we mean to
// keep must be added to the root set. Until this class, that was a bare pair of flag
// writes -- `reflection::AddToRoot` at the spawn, `reflection::RemoveFromRoot` written
// out again by hand at each teardown -- and the release was guarded by a liveness test:
//
//     void* liveActor = entry.actor.Get();   // null unless Alive()
//     if (liveActor) { E::DestroyActor(liveActor); R::RemoveFromRoot(liveActor); }
//
// The destroy needs liveness. THE UN-ROOT DOES NOT, and pairing them under one guard
// inverted the intent exactly when it mattered: at a world teardown every mirror is
// PendingKill (and its stamped world is no longer current), so `Get()` returns null,
// the whole branch is skipped, and the actors stay ROOTED. `[V]` 871 spawned trash
// proxies, 871 root-set actors still reaching the departed world through their Outer
// chain, and the world therefore never collected -- it stayed PendingKill with
// BeginDestroy never called, so the next `open <map>` in the same process adopted the
// corpse and died dereferencing its null WorldSettings
// (research/findings/join-identity/votv-rejoin-loadmap-null-worldsettings-RE-2026-08-31.md).
//
// A rooted PendingKill object is the worst of both states: dead to every caller, immortal
// to the collector, and still an Outer-chain anchor for the entire world it was spawned in.
//
// THE INVARIANT THIS CLASS ENFORCES: the pin is released by the destructor, so the pin
// lives exactly as long as the C++ object that owns it, and no teardown path can forget
// it or condition it on anything. Release is UNCONDITIONAL -- no liveness test, no world
// test -- because un-rooting a dead object is not merely safe, it is the entire point.
//
// Hold one BY VALUE inside whatever structure owns the engine object (MTA shape:
// CClientEntity owns its engine entity and drops it in ~CClientEntity). Erasing that
// structure then releases the pin with no extra line of teardown.
// ---------------------------------------------------------------------------------
class GcPin {
public:
    GcPin() = default;
    explicit GcPin(void* obj) { Pin(obj); }
    ~GcPin() { Release(); }

    // Move-only: two owners of one pin would double-release (harmless in isolation --
    // the second clear is a no-op -- but it would mean two structures each believing
    // they hold the object alive, which is how a use-after-free is authored).
    GcPin(GcPin&& other) noexcept : obj_(other.obj_) { other.obj_ = nullptr; }
    GcPin& operator=(GcPin&& other) noexcept {
        if (this != &other) {
            Release();
            obj_ = other.obj_;
            other.obj_ = nullptr;
        }
        return *this;
    }
    GcPin(const GcPin&) = delete;
    GcPin& operator=(const GcPin&) = delete;

    // Root `obj` and take ownership of the pin, releasing any pin held before.
    // Returns false (and holds nothing) if obj is null or has no GUObjectArray slot.
    // Game thread only -- the registry is not a synchronisation point for the engine.
    bool Pin(void* obj);

    // Un-root unconditionally and forget the object. Safe on a PendingKill object, on a
    // recycled slot (the slot's identity is re-checked before the flag is cleared), and
    // on an empty pin. Idempotent.
    void Release();

    void* Raw() const { return obj_; }
    bool  Held() const { return obj_ != nullptr; }

    // Stop touching the engine on release, permanently. Called from the shutdown path: a
    // GcPin can live inside another static, so a Release() can run during static destruction
    // at DLL unload, where reading the object's InternalIndex and walking GUObjectArray are
    // no longer safe reads -- and where un-rooting has no meaning, since no collector will
    // run again. Handles are still cleared and the registry still updated.
    static void StopReleases();

    // Total pins outstanding, across every owner.
    static size_t Outstanding();

    // How many outstanding pins are stamped to a WORLD (as opposed to process-lifetime
    // pins on assets/CDOs, which stamp null). Called after a session teardown, this is
    // the assertion the old hand-written releases had no way to make: a non-zero answer
    // means somebody's mirror is still anchoring a world that is on its way out. Logs a
    // WARN naming the count and the classes when it is non-zero; returns the count.
    static size_t ReportWorldScopedPins(const char* tag);

private:
    void* obj_ = nullptr;  // the pinned object, or null when this handle holds nothing
};

}  // namespace ue_wrap
