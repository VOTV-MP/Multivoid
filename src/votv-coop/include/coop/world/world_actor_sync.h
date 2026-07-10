// coop/world_actor_sync.h -- HOST-AUTHORITATIVE mirror of NON-Character event actors (B3b, 2026-06-17).
//
// The "sync all events" arc (research/findings/votv-all-events-coop-sync-classification-2026-06-17.md,
// channel B3 / the B3b split) + the design doc votv-b3b-worldactor-mirror-design-2026-06-17.md.
// Mirrors the ~14 non-Character event actors (gray saucers, Rozital mothership, ariral ships, sky UFO,
// space jellyfish, firetank, ...) that fly in on events. The Character-only NPC mirror (npc_sync +
// npc_pose_drive) can't replicate them: it drives pos + YAW-ONLY rotation + DriveCharacterMovement
// (CMC@0x288, ACharacter-only). So this is a SELF-CONTAINED sibling of npc_sync -- its own
// coop::element::WorldActor element (transform-only, FULL rotation), its own MirrorManager<WorldActor>,
// its own wire (WorldActorSpawn/WorldActorDestroy reliable + MsgType::WorldActorPose unreliable batch),
// and its own SECOND interceptor on BeginDeferredActorSpawnFromClass (the WorldActor allowlist is
// DISJOINT from npc_sync's, so the two interceptors are conflict-free -- game_thread.h multi-interceptor
// support). It does NOT extend the kerfur-coupled Npc, and does NOT touch npc_sync.
//
// HOST-AUTHORITATIVE (clients run a dormant event scheduler -- time_sync pins their TimeScale=0 -- so
// they never fire an event themselves; the host is the sole event producer):
//   - host interceptor: an allowlisted spawn -> allocate a WorldActor Element + broadcast WorldActorSpawn
//     (reuse EntitySpawnPayload, savePersisted=0). Lets the spawn proceed (returns false).
//   - host POST observer (same UFunction): bind the returned AActor* + actor->eid reverse map.
//   - host K2_DestroyActor PRE observer: broadcast WorldActorDestroy + release the Element.
//   - host TickPoseStream (~sendHz): read each live WA's transform -> publish ONE WorldActorPose batch.
//   - client interceptor: SUPPRESS an allowlisted local spawn (defense-in-depth -- the dormant scheduler
//     shouldn't fire one anyway) unless the bypass slot matches a wire-received materialization.
//   - client OnWorldActorSpawn: materialize a transform-only mirror (BeginDeferred + FinishSpawning),
//     park it (SetActorTickEnabled(false) -- generic, no CMC read), Install into MirrorManager<WorldActor>.
//   - client OnWorldActorDestroy: K2_DestroyActor the mirror + drop the Element.
//   - client TickClientWorldActors: drain the latest WorldActorPose batch + drive every mirror's interp.
//   - host QueueConnectBroadcastForSlot: re-send WorldActorSpawn (+ current transform) for every live WA
//     to a fresh joiner so it mirrors actors that spawned before it joined.

#pragma once

namespace coop::net {
class Session;
struct EntitySpawnPayload;
struct EntityDestroyPayload;
}  // namespace coop::net

namespace coop::world_actor_sync {

// Resolve the allowlist classes + the BeginDeferred spawn path + register the host PRE/POST/interceptor.
// Idempotent + retries (the WA BP classes load with the gameplay level, like the NPC classes). Caches
// the session for the receivers. Called every NetPumpTick from subsystems::Install.
void Install(coop::net::Session* session);

// True once Install's attempt completed (it stops retrying). Mirrors npc_sync::IsInstalled.
bool IsInstalled();

// Teardown: drain WorldActor mirrors (K2 the client ones; release-only the host ones) + clear host
// reverse-map + bypass slot. Game thread.
void OnDisconnect();

// HOST: read each live allowlisted WA's transform each tick + publish ONE WorldActorPose batch for the
// net thread to fan out (so client mirrors MOVE). Also the pose-walk DEAD-RETIRE seam (2026-07-04): a
// BOUND actor that reads dead closes its lifecycle here (retire + WorldActorDestroy broadcast) because
// event-end SELF K2_DestroyActor calls are PE-invisible to the destroy PRE observer (the piramid END).
// No-op on a client / a solo peer. Game thread (driven from subsystems::TickGameplay beside
// npc_sync::TickPoseStream).
void TickPoseStream();

// CLIENT: drain the latest WorldActorPose batch -> open an interp window per WA (SetTargetPose), then
// advance + drive EVERY live mirror (element::WorldActor::Tick). No-op on the host. Game thread.
void TickClientWorldActors();

// HOST: re-send WorldActorSpawn (class + CURRENT transform) for every already-spawned WA to a freshly-
// connected client so a joiner mirrors actors that spawned BEFORE it joined. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// CLIENT receiver: materialize a transform-only mirror for a host WorldActorSpawn. Game-thread (the
// event_feed dispatcher GT::Post's it; UFunction calls inside are game-thread only).
void OnWorldActorSpawn(const coop::net::EntitySpawnPayload& payload);

// CLIENT receiver: tear down the mirror for a host WorldActorDestroy. Game thread.
void OnWorldActorDestroy(const coop::net::EntityDestroyPayload& payload);

// HOST off-interceptor enroll for an ALREADY-SPAWNED allowlisted WA the PE interceptor could
// not see (an EX_CallMath BeginDeferred -- e.g. piramidSpawner_C's runTrigger outputs, caught
// by npc_world_enum's source-gated Func-thunk and drained here next pump tick, post-Finish so
// the transform is real). Reaches the same end state as interceptor+POST: WorldActor Element +
// actor bind + reverse-map + WorldActorSpawn broadcast. Dedups against the interceptor path via
// the reverse map. Returns the element id, or 0 if not enrolled (gates/allowlist/alloc). Game
// thread (UFunction-free, but the reverse map + element store expect the pump context).
unsigned int HostEnrollExSpawn(void* actor);

}  // namespace coop::world_actor_sync
