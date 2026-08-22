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

#include <cstdint>

namespace ue_wrap {

class CachedObjRef {
public:
    CachedObjRef() = default;

    // Capture `p` (fresh in THIS game-thread task) + its slot index + the slot's
    // current serial. Set(nullptr) == Reset().
    void Set(void* p) {
        if (!p) { Reset(); return; }
        ptr_ = p;
        idx_ = reflection::InternalIndexOf(p);
        serial_ = reflection::SlotSerial(idx_);
    }

    void Reset() {
        ptr_ = nullptr;
        idx_ = -1;
        serial_ = 0;
    }

    // Slot-validated liveness: slot still points at ptr_, no kill flags, serial
    // rule holds. Never touches the object's own memory.
    bool Alive() const {
        if (!reflection::IsLiveByIndex(ptr_, idx_)) return false;
        if (serial_ != 0 && reflection::SlotSerial(idx_) != serial_) return false;
        return true;
    }

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
};

}  // namespace ue_wrap
