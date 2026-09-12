// ue_wrap/core/cached_obj_ref.h -- the type for a UObject pointer cached across game-thread
// tasks or frames. A pointer cached across ticks must never be probed with the bare liveness
// check: that dereferences the possibly freed object, and while its exception guard absorbs
// the fault, a co-resident crash reporter sees the first-chance access violation first and
// pops a user-visible crash report. This type carries the invariant: Set reads the object
// once (its internal index, the slot serial and its world) and Alive reads only the object
// array's slot afterwards, so it never touches the object's memory and is valid off-thread
// (the chunk table is never freed; a torn read yields a benign false). Liveness alone is not
// enough for a world-scoped object: a dying world's actors are not kill-flagged until the
// eventual purge, tens of seconds after a quit to menu, so a slot-validated cache kept
// handing out actors of a world that no longer existed and the engine faulted on them. Set
// therefore stamps the object's world and Alive compares that stamp against the current
// one; objects with no world (a class, a function, a CDO, an asset, the game instance) stamp
// null and are unaffected, since they legitimately outlive worlds. Known gap: widgets are
// outered to the game instance, not a level, so the world term is inert for them.

#pragma once

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"

#include <cstdint>

namespace ue_wrap {

class CachedObjRef {
public:
    CachedObjRef() = default;

    // Capture `p` (fresh in this game-thread task), its slot index, the slot's serial and the
    // world it belongs to. Set(nullptr) is Reset(). The serial is allocated if the slot has none,
    // as the engine's weak pointer does, so a successor in the same slot at the same address
    // (the engine resets the serial when the object finishes destroying) reads as dead.
    void Set(void* p) {
        if (!p) { Reset(); return; }
        ptr_ = p;
        idx_ = reflection::InternalIndexOf(p);
        serial_ = reflection::AllocateSlotSerial(idx_);
        // Stamped here, while `p` is healthy by the Set contract, and never read from the object
        // again, so a teardown that scribbles the dead object's outer chain cannot defeat it.
        world_ = world_identity::WorldOf(p);
    }

    void Reset() {
        ptr_ = nullptr;
        idx_ = -1;
        serial_ = 0;
        world_ = nullptr;
    }

    // Slot-validated liveness and world currency: the slot still points at the pointer, no kill
    // flags, the serial captured at Set is still the slot's, and, for a world-scoped object, its
    // world is still the one the game is running. Never touches the object's memory.
    bool Alive() const {
        if (!reflection::IsLiveByIndex(ptr_, idx_)) return false;
        if (reflection::SlotSerial(idx_) != serial_) return false;
        // Two nulls are two different skips, both deliberate: a null stamp means the object is not
        // world-scoped and outliving a world is correct for it; a null current world means we
        // cannot tell right now (boot, mid-travel, or the degraded state after a recook renamed a
        // field) and we fail open, since failing closed would read every cached actor as dead. The
        // stamp test guards the call, and that nesting is a measured performance requirement:
        // Alive runs per object inside full object-array walks (the prop base class lookup under
        // the prop-descendant test is this call, unconditional for every object), and the current
        // world lookup is a cross-module call plus a thread check plus a thread-local read plus a
        // clock read; evaluated before the stamp test it multiplied by the object count per walk,
        // and the heaviest walk runs during a join. Every holder in those hot loops is a class, CDO
        // or asset, so the guard removes the entire cost for exactly that set. The current world
        // memoises on a 100 ms timer and is refreshed by whoever calls it; the input owner's
        // game-thread tick calls it unconditionally as the floor, and that call must stay.
        if (world_) {
            void* const current = world_identity::CurrentWorld();
            if (current && world_ != current) return false;
        }
        return true;
    }

    // The world stamped at Set, or null for a non-world-scoped object. A comparison token, never
    // dereferenced; exposed for the world-currency drill.
    void* StampedWorld() const { return world_; }

    // The validated pointer: the cached one if Alive, else null. The common call-site shape is
    // `if (void* p = ref.Get()) use(p);`.
    void* Get() const { return Alive() ? ptr_ : nullptr; }

    // The raw cached pointer without validation, for identity compares and is-anything-cached
    // checks only; never dereference the result.
    void* Raw() const { return ptr_; }

    int32_t Idx() const { return idx_; }

private:
    void* ptr_ = nullptr;
    int32_t idx_ = -1;
    int32_t serial_ = 0;
    void* world_ = nullptr;  // the UWorld stamped at Set(); nullptr = not world-scoped
};

}  // namespace ue_wrap
