// coop/npc_world_enum.h -- HOST-side pre-existing world-NPC enumeration.
//
// Extracted from npc_sync.cpp (K-0, 2026-06-16) per the 800-LOC soft cap: npc_sync.cpp
// had grown to 942 LOC. Owns the host-only GUObjectArray walk that registers NPC actors
// which loaded WITH the level (before the BeginDeferred interceptor installed) so they
// reach a fresh joiner. Reaches the SAME end state the interceptor+POST observer reach
// for a fresh spawn (Npc Element alloc + bound live actor + reverse-map entry), but for
// an actor that loaded with the level.
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

}  // namespace coop::npc_world_enum
