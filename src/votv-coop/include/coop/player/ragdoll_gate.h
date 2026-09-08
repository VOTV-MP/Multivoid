// coop/player/ragdoll_gate.h -- the SINGLE owner of the local player's `mainPlayer_C::canRagdoll`.
//
// WHY THIS EXISTS. `canRagdoll` is the first thing `ragdollMode` tests, and so the game's own gate
// on EVERY ragdoll cause -- death included, since `dead := true` is reachable only through
// `fallen(true)`, which is called only from `ragdollMode`.
//
// It has exactly one holder: `wisp_attack_sync`, for the Killer Wisp false-grab window, whose drop
// notify fires `ragdollMode(true,false,true)` from bytecode we cannot intercept -- an HP pin cannot
// stop a ragdoll-DEATH, this can. It is a refcount rather than a bool because the hazard is
// SHARING: a flag with two writers and no arbitration lets one lane's window-close silently re-open
// the other's invariant, so the next lane adds a Holder bit and cannot free anyone else's hold.
//
// GAME THREAD ONLY -- it calls engine UFunctions and writes BP properties. It addresses the LOCAL
// possessed pawn only; a puppet's ragdoll is driven by `remote_player`.

#pragma once

#include <cstdint>

namespace coop::ragdoll_gate {

// Who is asking. One bit each; add a bit rather than sharing one, so a release can never free a
// hold another lane still needs.
enum class Holder : uint32_t {
    WispFalseGrab = 1u << 0,  // the Killer Wisp false-grab window
};

// Take / drop a block on the local player's native ragdoll. Both are idempotent per holder. While
// at least one holder holds, `canRagdoll` is false.
void Hold(Holder who);
void Release(Holder who);

// Re-assert the desired value on the pawn the caller just resolved. Call it right after a Hold,
// which can only re-apply through a pawn the gate has already seen, and again whenever the pawn may
// have changed: a world load hands back a fresh mainPlayer_C whose `canRagdoll` is at the class
// default. It writes only when the live value disagrees, so calling it every tick is cheap.
void Tick(void* localPawn);

}  // namespace coop::ragdoll_gate
