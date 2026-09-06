// coop/host_spawn_watcher.h -- mirroring of ambient spawner outputs (the pinecone scare and its
// sibling forage spawners).
//
// THE SEAM. VOTV's ambient spawners (pineconeSpawner_C and kin) materialize their props through
// UGameplayStatics::BeginDeferredActorSpawnFromClass. The spawned actor's own Init() is
// dispatched BP-internally (EX_LocalVirtualFunction -> ProcessInternal) and so BYPASSES our
// ProcessEvent detour -- measured: a forced pinecone spawn fired zero Init POSTs. The
// BeginDeferred CALL, though, is a cross-object GameplayStatics UFunction dispatched THROUGH
// ProcessEvent and therefore observable, so this module registers its OWN POST observer on it
// (each (UFunction, cb) pair gets its own observer-table slot and all of them fire).
//
// WHY A SEPARATE MODULE rather than part of npc_sync: npc_sync owns the SAME BeginDeferred seam
// for the NPC route (allowlist -> EntitySpawn). This is the PROP route (ambient transient props
// -> PropSpawn-by-eid). They coexist as two independent POST observers on the one shared
// UFunction, with zero coupling and no edit to the NPC path.

#pragma once

namespace coop::net { class Session; }

namespace coop::host_spawn_watcher {

// Cache the session pointer (the boot-lifetime Session -- session_holder pattern).
void SetSession(coop::net::Session* session);

// Idempotent: resolve the GameplayStatics spawn UFunctions + offsets + the ambient-prop class
// set, then arm TWO seams. BeginDeferredActorSpawnFromClass POST observer -> the KEYLESS
// ambient set (pinecone/stick/crystal): keyless PropSpawn + death-watch, the transform read
// off the PARAMS, which only a PE observer provides. FinishSpawningActor UFunction::Func
// patch -> the KEYED spawn seam for EVERY dispatch route, including the EX_CallMath spawns a
// PE observer can never see; that callback only ENQUEUES, and DrainPendingSpawns adopts on
// the next tick. Non-fatal per seam, called every net-pump tick until resolved.
//
// OWNER-SYMMETRIC, not host-only: pineconeSpawner measurably anchors at the LOCAL player's
// camera (bytecode: GetPlayerCameraManager -> GetActorLocation plus a 3-10k offset), so every
// peer runs its OWN spawner and broadcasts over the same keyless PropSpawn, a client through
// the host's relay fan-out. The broadcast mirror spawns SIMULATING and falls under its own
// physics, deliberately not synced. Echo protection: a receiver's mirror spawn dispatches
// BeginDeferred through ProcessEvent, so this POST fires INSIDE it, and ScopedMirrorSpawn
// is the guard -- a MarkIncomingSpawn cannot exist before the actor does.
void Install(coop::net::Session* session);

// Drain the FinishSpawningActor pending queue: express (prop_lifecycle::ExpressSpawnedProp)
// every finished keyed Aprop_C spawn that is live, still untracked, and NOT the local hotbar
// hand actor (the hand edge in coop/player/hand_item owns that actor's eventual world release).
// ONE express attempt per entry -- the queue is cleared unconditionally, so a still-unkeyed
// actor falls to the periodic safety census rather than being retried here. Host-side express;
// game thread (net-pump tick).
void DrainPendingSpawns(coop::net::Session* session);

// Per-tick death-watch. PEER-SYMMETRIC, like the broadcaster: each peer watches the ambient
// props IT broadcast, and the list only ever holds its own spawns. When the engine has
// destroyed one (SetLifeSpan expiry or consumption -- a spawner-spawned actor has no observable
// K2_DestroyActor) it broadcasts PropDestroy(eid) so the other peers drop their mirrors. Cheap
// (IsLiveByIndex over a small bounded set). Game thread (net-pump tick).
void TickWatchedProps(coop::net::Session* session);

// Clear per-session state: the death-watch list, the pending queue and the session pointer. The
// POST observer stays registered -- it self-gates on connected() and on not being inside a
// mirror-spawn scope, which is all it needs, since every connected peer broadcasts its own
// ambient spawns. Net disconnect.
void OnDisconnect();

}  // namespace coop::host_spawn_watcher
