// coop/player/ragdoll_gate.cpp -- see coop/player/ragdoll_gate.h.

#include "coop/player/ragdoll_gate.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

namespace coop::ragdoll_gate {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

uint32_t g_held = 0;          // bitmask of Holder
void*    g_pawn = nullptr;    // the pawn we last corrected
uint32_t g_corrections = 0;   // times the live value disagreed with us, this pawn

const char* HolderName(Holder who) {
    switch (who) {
        case Holder::KoRespawn:     return "ko_respawn";
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
    if (!pawn || !R::IsLive(pawn)) return;
    const bool want = (g_held == 0);
    if (pawn != g_pawn) { g_pawn = pawn; g_corrections = 0; }

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
    Apply(g_pawn);
}

void Release(Holder who) {
    const uint32_t bit = static_cast<uint32_t>(who);
    if (!(g_held & bit)) return;
    g_held &= ~bit;
    UE_LOGI("ragdoll_gate: %s releases the ragdoll gate (holders=0x%X)", HolderName(who), g_held);
    Apply(g_pawn);
}

bool Holds(Holder who) { return (g_held & static_cast<uint32_t>(who)) != 0; }

void Tick(void* localPawn) {
    if (!localPawn) return;
    Apply(localPawn);
}

ScopedOpen::ScopedOpen(void* localPawn) : pawn_(localPawn) {
    if (!pawn_ || !R::IsLive(pawn_)) { pawn_ = nullptr; return; }
    if (g_held == 0) return;  // already open -- nothing to borrow, nothing to restore
    if (!E::SetMainPlayerCanRagdoll(pawn_, true)) { pawn_ = nullptr; return; }
    reclose_ = true;
}

ScopedOpen::~ScopedOpen() {
    if (!reclose_ || !pawn_) return;
    // Restore unconditionally, even if the pawn died inside the scope: leaving
    // the gate open is the failure mode that lets a real death through.
    E::SetMainPlayerCanRagdoll(pawn_, false);
}

void ReleaseAll() {
    const bool had = (g_held != 0);
    g_held = 0;
    if (had && g_pawn && R::IsLive(g_pawn)) {
        E::SetMainPlayerCanRagdoll(g_pawn, true);
        UE_LOGI("ragdoll_gate: all holds released -- canRagdoll restored on %p", g_pawn);
    }
    g_pawn = nullptr;
    g_corrections = 0;
}

}  // namespace coop::ragdoll_gate
