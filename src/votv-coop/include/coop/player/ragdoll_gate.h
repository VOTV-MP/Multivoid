// coop/player/ragdoll_gate.h -- the SINGLE owner of the local player's
// `mainPlayer_C::canRagdoll`.
//
// WHY THIS EXISTS. `canRagdoll` is the first instruction of `ragdollMode`
// (`IFNOT(canRagdoll) POP`, mainPlayer.uasset bytecode @5) and therefore the
// game's own unconditional gate on EVERY ragdoll cause on a pawn -- including
// death, because `dead := true` is reachable only through `fallen(true)`, which
// is reachable only from `ragdollMode`. That makes it the single choke point two
// different coop lanes want to hold:
//
//   * `wisp_attack_sync` holds it shut for the Killer Wisp false-grab window
//     (the drop notify fires `ragdollMode(true,false,true)` from bytecode we
//     cannot intercept -- an HP pin cannot stop a ragdoll-DEATH, this can).
//   * `ko_respawn` holds it shut for the WHOLE session, which is what makes a
//     lethal hit survivable at all.
//
// Before this module both wrote the raw bool directly, so the wisp's
// window-close `SetMainPlayerCanRagdoll(local, true)` re-OPENED the KO lane's
// death gate -- one lane silently undoing another's safety invariant. A shared
// primitive with two writers and no arbitration is the bug, not the symptom, so
// the gate is now a refcount: `canRagdoll` is false while ANY holder holds it,
// and true only when every holder has released.
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
    KoRespawn     = 1u << 0,  // the standing death gate (whole session)
    WispFalseGrab = 1u << 1,  // the Killer Wisp false-grab window
};

// Take / drop a block on the local player's native ragdoll. Both are idempotent
// per holder. While at least one holder holds, `canRagdoll` is false.
void Hold(Holder who);
void Release(Holder who);

// True while `who` currently holds.
bool Holds(Holder who);

// Re-assert the desired value on the CURRENT local pawn. Cheap and safe to call
// every tick: it writes only when the pawn changed or the desired value changed.
// A pawn change is why this is needed at all -- a world load hands us a fresh
// mainPlayer_C whose `canRagdoll` is back at its class default.
void Tick(void* localPawn);

// Open the gate for exactly one of OUR OWN `ragdollMode` calls, then restore.
// The KO's own flop has to pass the same gate it installs, so it borrows it
// rather than dropping the hold (dropping it would leave a window in which the
// game could author a real death). RAII, game thread, non-reentrant.
class ScopedOpen {
public:
    explicit ScopedOpen(void* localPawn);
    ~ScopedOpen();
    ScopedOpen(const ScopedOpen&) = delete;
    ScopedOpen& operator=(const ScopedOpen&) = delete;

private:
    void* pawn_ = nullptr;
    bool  reclose_ = false;
};

// Session teardown: drop every hold and restore `canRagdoll` on the pawn. Never
// strand the local player un-ragdollable past the session -- that would block
// every future ragdoll cause in single-player too.
void ReleaseAll();

}  // namespace coop::ragdoll_gate
