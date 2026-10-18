// coop/creatures/npc_world_enum.h -- HOST-side OFF-INTERCEPTOR NPC enrollment.
//
// ONE domain concept: enrolling host NPCs the PE interceptor can NEVER see,
// through the SAME end state the interceptor and its POST reach for a fresh spawn
// (Npc Element alloc + bound live actor + reverse-map entry, plus the EntitySpawn
// broadcast). Two members: the GUObjectArray WALK, for actors that loaded WITH the
// level before the interceptor installed, and the EX_CALLMATH SPAWN CATCH, for
// actors a Blueprint ubergraph spawns via EX_CallMath BeginDeferred -- which
// routes UFunction::Func one layer BELOW ProcessEvent
// (docs/coop-dispatch-visibility.md), so the interceptor and its POST register but
// never fire.
//
// Consumes npc_sync's host-side lifecycle state through its public accessors only,
// plus the shared MirrorManager<Npc> singleton; it owns no private npc_sync state.
// Game thread.

#pragma once

namespace coop::npc_world_enum {

// HOST-only PRE-EXISTING world-NPC enumeration: a kerfur already in the loaded save was never
// synced, because it loaded with the level BEFORE the interceptor installed and so never went
// through the intercepted BeginDeferred. Walk GUObjectArray for allowlisted-NPC actors that
// EXIST but are NOT yet coop-tracked, and register each as a host Npc Element (alloc + bind the
// live actor + reverse-map) -- the same end state the interceptor and its POST reach for a fresh
// spawn. Each one's EntitySpawn goes to the peers already connected and NOT to `joiningSlot`:
// npc_sync::QueueConnectBroadcastForSlot then mirrors every tracked NPC to the joiner, so the
// joiner gets the spawn once, from there; TickPoseStream streams them after. Idempotent (skips
// already-tracked actors). Called at the host connect edge, the one caller, with the joiner's
// slot. Returns the count newly registered. Game thread. Cold path: one GUObjectArray walk per
// call.
int RegisterExistingWorldNpcs(int joiningSlot);

// Patch the ufunction_hook Func-thunk onto the (already-resolved)
// BeginDeferredActorSpawnFromClass UFunction so EX_CallMath spawns of allowlisted classes
// are CAUGHT (queued; see DrainPendingExSpawns). SOURCE-GATED by the spawning actor's class
// (FFrame::Object) to the spawners whose output is host-authoritative and mirrored: the
// wisp event swarm, the piramid chain, the ambient sky-wisp ticker -- which anchors at
// absolute map coordinates rather than around a player, so the host rolls it and the
// client's own ticker is cancelled in the spawn authority -- the sell gun's coin mint,
// whose deferred spawn is bytecode-internal and invisible to the world-actor interceptor,
// and the story-event spawners (the eventer, the gray controller, the super egger and the
// balls follower) whose event-creature output mints inside their own graphs.
// The catch fires PRE-Finish, with the transform still unset, which is why it only QUEUES;
// the drain enrolls next pump tick, when FinishSpawningActor has run and the transform is
// real. Called by npc_sync::Install once the UFunction and the allowlist resolve and the
// lifecycle observers are live (the same gate as the interceptor -- an Element enrolled
// here gets the same K2_DestroyActor close). Idempotent. Game thread.
void InstallExSpawnCatch();

// Drain the EX-spawn catch queue: enroll each still-live, still-untracked queued actor as a host
// Npc Element + broadcast EntitySpawn with its CURRENT (post-Finish) transform, savePersisted=0
// (an event-swarm spawn happens after any join; no peer has a local twin). Host-only; call once
// per net-pump tick (TickPoseStream head). Cheap no-op when the queue is empty. Game thread.
void DrainPendingExSpawns();

// Drop any queued-but-undrained EX-spawn entries. Disconnect edge (npc_sync::OnDisconnect):
// a stale queued address must never survive into the next session. Any thread.
void ClearPendingExSpawns();

}  // namespace coop::npc_world_enum
