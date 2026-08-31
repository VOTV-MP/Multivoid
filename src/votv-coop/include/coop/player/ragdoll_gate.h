// coop/player/ragdoll_gate.h -- the SINGLE owner of the local player's
// `mainPlayer_C::canRagdoll`.
//
// WHY THIS EXISTS. `canRagdoll` is the first instruction of `ragdollMode`
// (`IFNOT(canRagdoll) POP`, mainPlayer.uasset bytecode @5) and therefore the
// game's own unconditional gate on EVERY ragdoll cause on a pawn -- including
// death, because `dead := true` is reachable only through `fallen(true)`, which
// is reachable only from `ragdollMode`.
//
// TODAY IT HAS EXACTLY ONE HOLDER: `wisp_attack_sync`, for the Killer Wisp
// false-grab window (the drop notify fires `ragdollMode(true,false,true)` from
// bytecode we cannot intercept -- an HP pin cannot stop a ragdoll-DEATH, this
// can). The module is still a refcount rather than a bool because the failure
// it was born from is about SHARING, not about counting: the retired
// `ko_respawn` lane held the same flag, and this lane's window-close wrote it
// back to true unconditionally, silently re-opening the other lane's safety
// invariant. A shared primitive with two writers and no arbitration is the bug,
// not the symptom -- so the next lane that needs the flag adds a Holder bit and
// cannot free anyone else's hold. (`ko_respawn` is retired 2026-08-31 by
// `docs/DEATH_ARC.md`: the user's decision is that the native death RUNS and the
// mod intervenes at the level travel, so no death lane holds this flag again.)
//
// Everything here is GAME THREAD ONLY (it calls engine UFunctions / writes BP
// properties). The gate addresses the LOCAL possessed pawn only -- a puppet's
// ragdoll is driven by `remote_player`, never by this.

#pragma once

#include <cstdint>

namespace coop::ragdoll_gate {

// Who is asking. One bit each; add a bit rather than sharing one, so a release
// can never free a hold another lane still needs.
enum class Holder : uint32_t {
    WispFalseGrab = 1u << 0,  // the Killer Wisp false-grab window
};

// Take / drop a block on the local player's native ragdoll. Both are idempotent
// per holder. While at least one holder holds, `canRagdoll` is false.
void Hold(Holder who);
void Release(Holder who);

// Re-assert the desired value on the CURRENT local pawn. Cheap and safe to call
// every tick: it writes only when the pawn changed or the desired value changed.
// A pawn change is why this is needed at all -- a world load hands us a fresh
// mainPlayer_C whose `canRagdoll` is back at its class default.
void Tick(void* localPawn);

}  // namespace coop::ragdoll_gate
