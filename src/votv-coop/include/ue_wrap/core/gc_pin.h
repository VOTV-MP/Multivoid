#pragma once

#include <cstddef>

namespace ue_wrap {

// GcPin -- an OWNED GC pin: the only supported way to keep a runtime-constructed UObject alive
// from C++, a raw pointer being invisible to UE's reachability scan.
//
// Release is UNCONDITIONAL -- no liveness test, no world test -- because un-rooting a dead
// object is the entire point. Guarding it on liveness inverts the intent exactly where it
// matters: at a world teardown every mirror is already PendingKill, so the guard skips and the
// objects stay ROOTED. A rooted PendingKill object is the worst of both states, dead to every
// caller and immortal to the collector, and through its Outer chain it still anchors the world
// it was spawned in -- which then never collects. That world stays PendingKill with
// BeginDestroy never called, and the next map open in the same process adopts the corpse and
// dies on its null WorldSettings.
//
// So the destructor releases it: the pin lives exactly as long as the C++ object holding it,
// and no teardown path can forget or condition it. Hold one BY VALUE in whatever structure
// owns the engine object (the MTA shape, CClientEntity), and erasing that structure is enough.
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

    // How many outstanding pins are stamped to a WORLD, as opposed to the process-lifetime pins on
    // assets and CDOs, which stamp null. Called after a session teardown, a non-zero answer means
    // some mirror is still anchoring a world on its way out -- the assertion a hand-written
    // release pair has no way to make. Returns the count, and logs a WARN naming it and the
    // classes only when it is non-zero.
    static size_t ReportWorldScopedPins(const char* tag);

private:
    void* obj_ = nullptr;  // the pinned object, or null when this handle holds nothing
};

}  // namespace ue_wrap
