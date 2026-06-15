// coop/wisp_tear_mirror.h -- Killer Wisp coop, RECEIVER side (Inc2, parts B + C).
//
// See research/findings/votv-killerwisp-coop-design-2026-06-13.md. The HOST detect +
// route lives in coop/wisp_attack_sync; this module owns the two receiver paths:
//
//   * OnWispGrab (the VICTIM only): the host's killerwisp grabbed THIS client's puppet
//     and neutralized its own false-grab; this client now dies for real. Per-peer-
//     authoritative kill: schedule ragdollMode(true,false,true) on our OWN possessed
//     player after the host's fixed delay (NOT a montage notify -- our wisp mirror is a
//     kinematic puppet that fires no notifies). Native ragdoll death -> menu -> rejoin.
//
//   * OnWispTear (ALL peers + the host's own local call): play the fatality TEAR on the
//     LOCAL wisp mirror -- force its parked mesh to tick + Montage_Play 'fatality' (the
//     mirror never runs the BP montage itself). PlayTearOnWisp is the shared core (the
//     host calls it directly on its own wisp actor since it doesn't receive its own
//     WispTear broadcast).
//
// DEFERRED to a follow-on (documented, no stubs): the 4 limb gibs + blood particles, the
// victim socket-attach hold ('playerGrab' + a RemotePlayer wisp-held display mode), and
// the victim CMC park. The first version is the cross-peer KILL + the wisp tear animation.

#pragma once

#include <cstdint>

namespace coop::net {
struct WispGrabPayload;
struct WispTearPayload;
}  // namespace coop::net

namespace coop::wisp_tear_mirror {

// VICTIM receiver (host->victim slot). Self-verifies the addressed Player Element id, then
// arms a fixed-delay ragdoll death on the local player. Game thread.
void OnWispGrab(const coop::net::WispGrabPayload& p, uint8_t senderPeerSlot);

// ALL-peers receiver (host->all). Resolves the local wisp NPC mirror by wispElementId and
// plays the tear on it. Game thread.
void OnWispTear(const coop::net::WispTearPayload& p, uint8_t senderPeerSlot);

// Shared tear core: force the wisp mesh to tick + play the fatality montage on it. Used by
// OnWispTear (resolved mirror) AND by the host's wisp_attack_sync directly (its own wisp).
// `victimSlot` is reserved for the deferred socket-attach hold. Game thread.
void PlayTearOnWisp(void* wispActor, uint32_t victimSlot);

// Per-tick: discharge the armed victim ragdoll death once its deadline elapses. No-op until
// armed. Game thread (ragdollMode is a UFunction call).
void Tick();

// Clear armed state on disconnect.
void OnDisconnect();

}  // namespace coop::wisp_tear_mirror
