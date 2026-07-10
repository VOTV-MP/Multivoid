// coop/creatures/owner_entity_sync.h -- the OWNER-ENTITY mirror lane
// (v108, 2026-07-10 eve; user rule: "Each peer must have its own but visible
// for other peers").
//
// TIER (extends [[feedback-owner-effect-rule]] to full CREATURES): some
// entities are designed to stalk THE player -- suppressing them on clients
// would delete the encounter, host-rolling them would anchor them to the
// wrong player. So each peer KEEPS its native roll (the spawner ticker is
// NOT parked), OWNS the entity it rolled (native AI targets its player), and
// BROADCASTS it so every other peer renders a display mirror.
//
// First member: eyer_C (ticker_eyers -- the night "Eyes" stalker: watches
// you, angers when stared at, shows teeth, dashes to kill; bytecode RE
// 2026-07-10). The ticker's roll itself is world-anchored, but the ENTITY's
// whole behavior loop (isLooking/angrify/dash) reads the LOCAL player --
// per-peer ownership is the only shape that preserves the encounter for
// every peer.
//
// SHAPE (self-contained; deliberately NOT the host-auth element registry --
// peer-owned identity is a different axis, and npc_mirror hard-asserts
// host-range eids):
//   - identity  = (transport senderPeerSlot, owner-local seq).
//   - OWNER side: a BeginDeferred POST observer catches the local native
//     spawn of a member class (ScopedMirrorSpawn excludes mirror spawns) ->
//     broadcast OwnerEntitySpawn; a 4 Hz driver streams OwnerEntityPose on
//     movement, re-announces Spawn every ~10 s (keepalive = the late-joiner
//     delivery; receivers treat a known key as a pose refresh), and
//     death-watches the actor (IsLiveByIndex) -> OwnerEntityDestroy.
//   - RECEIVER side: materialize the class at the wire transform inside a
//     ScopedMirrorSpawn, then PARK it: DisableCharacterTicks (actor + CMC --
//     no AI, no anger loop, no dash) + SetActorEnableCollision(false) (the
//     killsphere must never overlap-kill a viewing peer). Pose msgs drive
//     SetActorLocation/Rotation.
//   - relay: all three kinds are client-relayable (client -> host -> other
//     clients; the origin never receives its own send).
//
// Wiring: subsystems Install/Tick/OnDisconnect + DisconnectSlot (a leaver's
// mirrors are destroyed); dispatch cases in event_dispatch_entity.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct OwnerEntitySpawnPayload;
struct OwnerEntityPosePayload;
struct OwnerEntityDestroyPayload;
}  // namespace coop::net

namespace coop::owner_entity_sync {

// Cache the session + register the BeginDeferred POST observer + resolve the
// member classes (lazy retry; classes load with the world). Net-pump ensure.
void Install(coop::net::Session* session);

// 4 Hz driver (TickGameplay): owner pose stream + keepalive re-announce +
// owner death-watch; receiver-side dead-mirror prune. Cheap early-outs.
void Tick();

// Receivers (event_feed drain, game thread). senderPeerSlot keys the mirror.
void OnSpawnMsg(const coop::net::OwnerEntitySpawnPayload& p, int senderPeerSlot);
void OnPoseMsg(const coop::net::OwnerEntityPosePayload& p, int senderPeerSlot);
void OnDestroyMsg(const coop::net::OwnerEntityDestroyPayload& p, int senderPeerSlot);

// A peer left: destroy every mirror keyed to its slot (DisconnectSlot fanout).
void OnPeerLeftSlot(int slot);

// Session end: destroy ALL mirrors (they are OUR spawned actors -- they must
// not linger into SP) + clear owned tracking. Full-teardown fanout.
void OnDisconnect();

}  // namespace coop::owner_entity_sync
