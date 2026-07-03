// coop/npc_world_enum.h -- HOST-side OFF-INTERCEPTOR NPC enrollment.
//
// Extracted from npc_sync.cpp (K-0, 2026-06-16) per the 800-LOC soft cap; concept widened
// 2026-07-03. ONE domain concept: enrolling host NPCs the PE interceptor can NEVER see,
// through the SAME end state the interceptor+POST reach for a fresh spawn (Npc Element
// alloc + bound live actor + reverse-map entry [+ EntitySpawn broadcast]). Two members:
//   1. The GUObjectArray WALK (RegisterExistingWorldNpcs) -- actors that loaded WITH the
//      level, before the interceptor installed.
//   2. The EX_CALLMATH SPAWN CATCH (InstallExSpawnCatch/DrainPendingExSpawns) -- actors a
//      BP ubergraph spawns via EX_CallMath BeginDeferred, which routes UFunction::Func ONE
//      LAYER BELOW ProcessEvent (docs/COOP_DISPATCH_VISIBILITY.md): the PE interceptor+POST
//      register but never fire. Caught by the ufunction_hook Func-thunk; SOURCE-GATED by
//      the spawning actor's class (FFrame::Object) to the event-swarm spawners
//      (trigger_wispSwarm_C -> wisp_C), so per-peer AMBIENT spawners (ticker_wispSpawner's
//      wisp_C, weight 100 of ~108 in its cooked table) stay local by design -- the same
//      standing decision as the colored wisp siblings. The catch fires PRE-Finish
//      (transform unset), so it only QUEUES; the drain enrolls next pump tick, when
//      FinishSpawningActor has run and the transform is real.
//
// Consumes npc_sync's host-side lifecycle state through its public accessors only
// (GetSession / IsInstalled / IsHostNpcSyncDisabled / IsAllowlistedClass /
// GetNpcIdForActor / MapActorToNpcId) + the shared MirrorManager<Npc> singleton --
// it owns no private npc_sync state. Game thread.

#pragma once

namespace coop::npc_world_enum {

// Why a newly-registered NPC's EntitySpawn carries savePersisted=1 (adopt a local twin) vs 0
// (fresh-spawn a mirror) depends on the CALLER's intent, NOT the actor's has-a-key property:
//  - ConnectEdge:        host world-load init (subsystems). The broadcast (re)reaches a JOINER who
//                        loaded the same save -> it MAY have a local twin of a keyed save object ->
//                        savePersisted = HasSaveKey(obj). [the v75 connect-edge adoption contract]
//  - MidSessionConverge: a kerfur turned ON mid-session (kerfur_convert). The broadcast reaches
//                        ALREADY-CONNECTED peers who have NO twin (they cancelled their own local
//                        turn-on) -> ALWAYS savePersisted=0 -> fresh-spawn now. A turn-on kerfur has
//                        a random UCS-minted key so HasSaveKey is true, but the key is meaningless to
//                        a peer that never loaded it -> routing it into the deferred adoption poll
//                        causes an ~8s pop-in + a class-only false-bind dupe (RCA 2026-06-15). This
//                        matches the runtime-interceptor send, which already hardcodes savePersisted=0
//                        for the same "spawned after the join, no local twin" reason.
enum class NpcEnumOrigin { ConnectEdge, MidSessionConverge };

// HOST-only PRE-EXISTING world-NPC enumeration (2026-06-07, user "two kerfurs on host" -- a kerfur
// already in the loaded save wasn't synced). Walk GUObjectArray for allowlisted-NPC actors that
// EXIST but are NOT yet coop-tracked -- i.e. loaded with the level BEFORE the interceptor installed,
// so they never went through the intercepted BeginDeferred and were never registered. Register each
// as a host Npc Element (alloc + bind the live actor + reverse-map) -- the same end state the
// interceptor+POST observer reach for a fresh spawn, but for an actor that loaded with the level.
// npc_sync::QueueConnectBroadcastForSlot then mirrors them to the joiner + TickPoseStream streams
// them after. Idempotent (skips already-tracked actors). `origin` decides the savePersisted policy
// (see above). Called at the host connect edge (ConnectEdge) AND from the kerfur turn-on converge
// (MidSessionConverge). Returns the count newly registered. Game thread. Cold path: one
// GUObjectArray walk per call.
int RegisterExistingWorldNpcs(NpcEnumOrigin origin);

// Patch the ufunction_hook Func-thunk onto the (already-resolved) BeginDeferredActorSpawnFromClass
// UFunction so EX_CallMath spawns of allowlisted classes from the event-swarm source spawners are
// CAUGHT (queued; see DrainPendingExSpawns). Called by npc_sync::Install once the UFunction + the
// allowlist resolve and the lifecycle observers are live (same gate as the interceptor -- an
// Element enrolled here gets the same K2_DestroyActor close). Idempotent. Game thread.
void InstallExSpawnCatch(void* beginDeferredFn);

// Drain the EX-spawn catch queue: enroll each still-live, still-untracked queued actor as a host
// Npc Element + broadcast EntitySpawn with its CURRENT (post-Finish) transform, savePersisted=0
// (an event-swarm spawn happens after any join; no peer has a local twin). Host-only; call once
// per net-pump tick (TickPoseStream head). Cheap no-op when the queue is empty. Game thread.
void DrainPendingExSpawns();

// Drop any queued-but-undrained EX-spawn entries. Disconnect edge (npc_sync::OnDisconnect):
// a stale queued address must never survive into the next session. Any thread.
void ClearPendingExSpawns();

}  // namespace coop::npc_world_enum
