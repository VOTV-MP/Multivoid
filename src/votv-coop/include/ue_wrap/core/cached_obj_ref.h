// ue_wrap/core/cached_obj_ref.h -- THE type for a UObject pointer cached ACROSS
// game-thread tasks/frames.
//
// Design of record: research/findings/tooling/votv-islive-zeroav-cachedobjref-DESIGN-2026-08-22.md
// (10-round /qf). The ratified discipline (OPUS_48_DISCIPLINE.md:59): a pointer
// cached across ticks must never be probed with bare IsLive -- IsLive dereferences
// the possibly-GC-freed object, and while its SEH absorbs the fault by contract,
// a co-resident VEH crash reporter (CrashContext in the real modded stack) sees
// the first-chance AV FIRST and pops a user-visible "crash" report. The 2026-08-22
// census found the discipline violated at 78 sites; this type carries the
// invariant so a cached use cannot silently regress. Acceptance criterion: ZERO
// first-chance AVs from our probes in normal operation.
//
// Contract:
//  - Set(p): `p` must be fresh-or-just-ByIndex-validated in the SAME game-thread
//    task (Set dereferences p once to read its InternalIndex).
//  - Alive(): GUObjectArray-slot reads ONLY (never the object's memory) -> zero
//    AV by construction; valid off-thread (the chunk table is never freed; a torn
//    read yields a benign false).
//  - WORLD CURRENCY (2026-08-23, the Linux 9-fps triage): liveness is NOT enough for
//    a WORLD-SCOPED object. A dying world's actors are not kill-flagged until the
//    eventual GC purge -- measured at 44+ SECONDS after a solo quit-to-menu -- so a
//    slot-validated cache keeps handing out an actor of a world that no longer
//    exists, and the engine faults when you pass it back in
//    ([[lesson-dying-world-actors-not-killflagged-at-menu]], and the storm it caused:
//    ~2,508 absorbed AVs/second for 44 s). Set() therefore STAMPS the object's UWorld
//    and Alive() compares that stamp against the current one. The stamp is taken
//    while the object is healthy and never re-read afterwards, so a teardown that
//    scribbles or nulls the dead object's Outer chain cannot defeat it.
//    Objects with NO world -- a UClass, a UFunction, a CDO, a cooked asset, the
//    GameInstance -- stamp nullptr and are unaffected: they legitimately outlive
//    worlds, and that is the whole discrimination.
//  - Serial rule (FWeakObjectPtr semantics): if the captured serial != 0, the
//    current slot serial MUST equal it (mismatch INCLUDING current==0 -> dead).
//    UE assigns serials LAZILY (0 until a weak ref exists), so captured==0
//    contributes nothing -- the ABA impostor residual is filed in the design's
//    section 6, NOT closed by this type, and no caller may assume it is.
//  - Bare reflection::IsLive KEEPS its fresh-same-task contract; its SEH guard is
//    the dead-man's brake and its caller-attribution WARN the tripwire for any
//    residual violator.

#pragma once

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"

#include <cstdint>

namespace ue_wrap {

class CachedObjRef {
public:
    CachedObjRef() = default;

    // Capture `p` (fresh in THIS game-thread task) + its slot index + the slot's
    // current serial + the UWorld it belongs to. Set(nullptr) == Reset().
    void Set(void* p) {
        if (!p) { Reset(); return; }
        ptr_ = p;
        idx_ = reflection::InternalIndexOf(p);
        serial_ = reflection::SlotSerial(idx_);
        // Stamped HERE, while `p` is healthy by the Set contract, and never read from
        // the object again -- see the world-currency note above.
        world_ = world_identity::WorldOf(p);
    }

    void Reset() {
        ptr_ = nullptr;
        idx_ = -1;
        serial_ = 0;
        world_ = nullptr;
    }

    // Slot-validated liveness AND world currency: the slot still points at ptr_, no
    // kill flags, the serial rule holds, and -- for a world-scoped object -- its
    // world is still the one the game is running. Never touches the object's memory.
    bool Alive() const {
        if (!reflection::IsLiveByIndex(ptr_, idx_)) return false;
        if (serial_ != 0 && reflection::SlotSerial(idx_) != serial_) return false;
        // Two nulls are two different "skip me"s and both are deliberate:
        //   world_ == nullptr  -> this object is not world-scoped (a class, a CDO, an
        //                         asset) and outliving a world is CORRECT for it.
        //   current == nullptr -> we cannot tell right now (boot, mid-travel, or the
        //                         degraded state after a recook renamed a field). Fail
        //                         OPEN: the pre-2026-08-23 behaviour is a performance
        //                         defect, whereas failing closed would read every
        //                         cached actor as dead and take the mod down.
        void* const current = world_identity::CurrentWorld();
        if (world_ && current && world_ != current) return false;
        return true;
    }

    // The UWorld stamped at Set(), or nullptr for a non-world-scoped object. A
    // comparison token -- never dereference it. Exposed for the world-currency drill.
    void* StampedWorld() const { return world_; }

    // The validated pointer: ptr_ if Alive(), else nullptr. The common call-site
    // shape `if (void* p = ref.Get()) use(p);`.
    void* Get() const { return Alive() ? ptr_ : nullptr; }

    // The raw cached pointer WITHOUT validation -- for identity compares and
    // "is something cached at all" checks only; never dereference the result.
    void* Raw() const { return ptr_; }

    int32_t Idx() const { return idx_; }

private:
    void* ptr_ = nullptr;
    int32_t idx_ = -1;
    int32_t serial_ = 0;
    void* world_ = nullptr;  // the UWorld stamped at Set(); nullptr = not world-scoped
};

}  // namespace ue_wrap
