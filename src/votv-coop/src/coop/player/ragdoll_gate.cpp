// coop/player/ragdoll_gate.cpp -- see coop/player/ragdoll_gate.h.

#include "coop/player/ragdoll_gate.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

namespace coop::ragdoll_gate {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

uint32_t g_held = 0;          // bitmask of Holder
// The pawn we last corrected, held ACROSS ticks -- so it is a CachedObjRef, not a
// bare pointer. `Hold`/`Release` re-apply through it at a moment of their caller's
// choosing, which can be after a world died: a bare `IsLive` there dereferences a
// possibly-freed object (the 2026-08-23 dying-world storm shape) and, under a
// co-resident VEH crash reporter, the absorbed AV surfaces to the player as a
// crash. `Get()` is array-slot reads plus the world stamp, and returns null when
// the pawn belongs to a world that no longer exists.
ue_wrap::CachedObjRef g_pawn;
uint32_t g_corrections = 0;   // times the live value disagreed with us, this pawn

const char* HolderName(Holder who) {
    switch (who) {
        case Holder::WispFalseGrab: return "wisp_false_grab";
    }
    return "?";
}

// Push the desired value onto `pawn` by READING IT BACK and correcting a
// mismatch -- never by remembering that we once wrote it.
//
// The first version cached "the value I last wrote to this pointer" and skipped
// the write when it matched. The acceptance test killed it in one run: the gate
// applied at 10:17:52 and read back TRUE two minutes later, because the write
// had been remembered against a POINTER, and a pointer says nothing about
// whether the byte behind it still holds our value. A world load builds a fresh
// mainPlayer_C with the class default (and can land it on the recycled address),
// so a remembered write is not evidence of a held invariant. A gate you cannot
// re-interrogate is not a gate.
//
// Cost is one masked byte read per tick, against a `FindBoolProperty` resolved
// once for the process -- cheaper than the pointer compare it replaces was
// wrong.
void Apply(void* pawn) {
    if (!pawn) return;
    const bool want = (g_held == 0);
    if (pawn != g_pawn.Raw()) { g_pawn.Set(pawn); g_corrections = 0; }

    bool live = true;
    if (!E::ReadMainPlayerCanRagdoll(pawn, live)) return;  // logs its own failure
    if (live == want) return;

    if (!E::SetMainPlayerCanRagdoll(pawn, want)) return;
    ++g_corrections;
    // Loud for the first few (the interesting ones: the initial take, and a
    // re-take after a world load), then quiet. If something in the game writes
    // this flag every tick we still want to know, so a rare heartbeat survives.
    if (g_corrections <= 3 || (g_corrections % 1000) == 0) {
        UE_LOGI("ragdoll_gate: canRagdoll %d -> %d on pawn %p (holders=0x%X, correction #%u)",
                live ? 1 : 0, want ? 1 : 0, pawn, g_held, g_corrections);
    }
}

}  // namespace

void Hold(Holder who) {
    const uint32_t bit = static_cast<uint32_t>(who);
    if (g_held & bit) return;
    g_held |= bit;
    UE_LOGI("ragdoll_gate: %s HOLDS the ragdoll gate (holders=0x%X)", HolderName(who), g_held);
    Apply(g_pawn.Get());
}

void Release(Holder who) {
    const uint32_t bit = static_cast<uint32_t>(who);
    if (!(g_held & bit)) return;
    g_held &= ~bit;
    UE_LOGI("ragdoll_gate: %s releases the ragdoll gate (holders=0x%X)", HolderName(who), g_held);
    Apply(g_pawn.Get());
}

void Tick(void* localPawn) {
    // A FRESH pointer from the caller's own resolve this task -- bare IsLive is
    // legal on it (nothing has cached it across a task boundary yet).
    if (!localPawn || !R::IsLive(localPawn)) return;
    Apply(localPawn);
}

}  // namespace coop::ragdoll_gate
