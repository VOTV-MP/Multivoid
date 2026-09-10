// coop/creatures/wisp_tear_mirror.h -- Killer Wisp coop, receiver side. The host's detect and route
// live in coop/wisp_attack_sync; this module owns the two receiver paths.
//
// OnWispGrab reaches the VICTIM alone: the host's killerwisp grabbed this client's puppet and
// neutralised its own false grab, so this client now dies for real. The kill is per-peer
// authoritative -- ragdollMode(true, false, true) on our own possessed player after the host's
// delay, not a montage notify, since our wisp mirror is a kinematic puppet that fires none.
// Native ragdoll death, then the menu, then rejoin.
//
// OnWispTear reaches every peer, and the host calls it locally because it never receives its own
// broadcast: play the fatality tear on the LOCAL wisp mirror by forcing its parked mesh to tick
// and playing the montage, which the mirror never runs itself. PlayTearOnWisp is that core.
//
// Body placement across the grab window belongs to coop::wisp_grab_hold. The limb gibs and the
// blood particles are still a follow-on.

#pragma once

#include <cstdint>

namespace coop::net {
struct WispGrabPayload;
struct WispTearPayload;
}  // namespace coop::net

namespace coop::wisp_tear_mirror {

// VICTIM receiver (host to the victim's slot). Verifies the packet addresses our own Player
// Element id, engages the local grab hold for the window, and arms the ragdoll death at the
// host's delay, clamped to a sane range. Game thread.
void OnWispGrab(const coop::net::WispGrabPayload& p, uint8_t senderPeerSlot);

// ALL-peers receiver (host to all). Resolves the local wisp mirror by wispElementId, plays the
// tear on it, and holds the victim's puppet at its socket on every peer but the victim's own.
// Game thread.
void OnWispTear(const coop::net::WispTearPayload& p, uint8_t senderPeerSlot);

// Shared tear core: force the wisp mesh to tick, then play the fatality montage on it. Used by
// OnWispTear on the resolved mirror AND by the host's wisp_attack_sync directly on its own wisp.
// `victimSlot` is logged, not acted on -- the hold is wisp_grab_hold's. Game thread.
void PlayTearOnWisp(void* wispActor, uint32_t victimSlot);

// Per-tick: release the grab ride and discharge the armed victim ragdoll death once its deadline
// elapses. No-op until armed. Game thread (ragdollMode is a UFunction call).
void Tick();

// Clear armed state on disconnect.
void OnDisconnect();

}  // namespace coop::wisp_tear_mirror
