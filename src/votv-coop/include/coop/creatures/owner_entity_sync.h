// coop/creatures/owner_entity_sync.h -- the OWNER-ENTITY mirror lane: each peer has its own, and
// every other peer can see it.
//
// Some entities are designed to stalk THE player. Suppressing them on clients would delete the
// encounter; host-rolling them would anchor them to the wrong player. So each peer KEEPS its native
// roll -- the spawner ticker is not parked -- OWNS the entity it rolled, whose native AI targets
// its own player, and BROADCASTS it so every other peer renders a display mirror.
//
// The first member is eyer_C, from ticker_eyers: the night stalker that watches you, angers when
// stared at and dashes to kill. Its roll is world-anchored, but its behaviour loop reads the LOCAL
// player, so per-peer ownership is the only shape that preserves the encounter for everyone.
//
// Identity is (transport senderPeerSlot, owner-local seq). The lane is deliberately NOT the
// host-authoritative element registry: peer-owned identity is a different axis, and npc_mirror
// hard-asserts host-range eids. Wired through subsystems; dispatch in event_dispatch_entity.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct OwnerEntitySpawnPayload;
struct OwnerEntityPosePayload;
struct OwnerEntityDestroyPayload;
}  // namespace coop::net

namespace coop::owner_entity_sync {

// Cache the session, resolve the member classes (lazy retry -- they load with the world), and
// register the BeginDeferred POST observer that catches a local native spawn of a member class and
// broadcasts OwnerEntitySpawn. ScopedMirrorSpawn excludes our own mirror spawns from it. Net-pump
// ensure.
void Install(coop::net::Session* session);

// 4 Hz driver (TickGameplay), the OWNER side of the lane: stream OwnerEntityPose while the entity
// moves, re-announce Spawn about every 10 s, and death-watch the actor through IsLiveByIndex to
// send OwnerEntityDestroy. The keepalive re-announce IS the late-joiner delivery, since a receiver
// treats a known key as a pose refresh. Also prunes dead mirrors on the receiver side. Cheap
// early-outs.
void Tick();

// Receivers (event_feed drain, game thread); senderPeerSlot keys the mirror. A spawn materializes
// the class at the wire transform inside a ScopedMirrorSpawn and then PARKS it:
// DisableCharacterTicks on the actor and its movement component, so no AI, no anger loop and no
// dash, and SetActorEnableCollision(false), so the killsphere can never overlap-kill a viewing
// peer. Pose messages drive SetActorLocation and SetActorRotation. All three kinds are
// client-relayable, client to host to other clients, and the origin never receives its own send.
void OnSpawnMsg(const coop::net::OwnerEntitySpawnPayload& p, int senderPeerSlot);
void OnPoseMsg(const coop::net::OwnerEntityPosePayload& p, int senderPeerSlot);
void OnDestroyMsg(const coop::net::OwnerEntityDestroyPayload& p, int senderPeerSlot);

// A peer left: destroy every mirror keyed to its slot (DisconnectSlot fanout).
void OnPeerLeftSlot(int slot);

// Session end: destroy ALL mirrors (they are OUR spawned actors -- they must
// not linger into SP) + clear owned tracking. Full-teardown fanout.
void OnDisconnect();

}  // namespace coop::owner_entity_sync
