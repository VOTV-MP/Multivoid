// coop/dev/spawn_npc.h -- DEV/TEST: spawn a kerfurOmega NPC on demand.
//
// Gameplay/dev layer. The ONLY programmatic NPC-spawn trigger in the mod -- VOTV NPCs spawn only
// from in-game purchase or scripted events, so without this the NPC-sync paths (host
// AllocAndInstall + broadcast, client mirror Install) have no autonomous-test coverage. Two
// triggers: the ImGui dev menu (Content > Entities > "Spawn kerfurOmega"), for hands-on use; and a
// trigger FILE named by env VOTVCOOP_SPAWN_TRIGGER, which spawns once and deletes itself when it
// appears. A test harness creates that file AFTER all peers connect, because NPC EntitySpawn is not
// part of the connect-edge replay. The actual spawn is npc_sync::DevSpawnNpcInFront, run on the
// game thread.

#pragma once

namespace coop::dev::spawn_npc {

// Start the trigger-FILE watcher thread, IF env VOTVCOOP_SPAWN_TRIGGER is set
// (autonomous tests). No-op otherwise -- the menu is the hands-on spawn path.
// Gated by [dev] enabled (master). Call once from harness boot.
void Init();

// Menu action (Content > Entities): spawn a kerfurOmega_C in front of the local
// player (host-side sync path). Safe to call off the game thread (the spawn is
// posted to it).
void SpawnKerfurOmega();

// Test-spawn the increment-1 npc_sync allowlist additions in front of the host --
// THE reliable mirror test (the F1 wisps/ventCrawler events don't reliably spawn a
// catchable creature: wisps arms an overlap box; the eventer's ventCrawler spawn
// uses EX_CallMath -> bypasses our interceptor + lands in a far vent). See
// spawn_npc.cpp PostSpawnClass. Host-only (dev_gate); safe off the game thread.
void SpawnKillerWisp();
void SpawnVentCrawler();

// OWNER-ENTITY lane test (F1 > Content > Entities): spawn an eyer_C in front of the local player.
// eyer is NOT npc-allowlisted by design -- the owner_entity_sync BeginDeferred observer catches it
// as a LOCALLY-OWNED entity and announces it, so every peer materializes a brain-parked,
// collision-off display mirror. Host-only trigger (dev_gate); the client-owned direction is
// exercised by the native ticker roll.
void SpawnEyer();

// Killer Wisp cross-peer-kill TEST: spawn a killerwisp ON the first connected client puppet, so it
// acquires that PUPPET as its Target and exercises the host-detect -> neutralize ->
// WispGrab/WispTear -> client ragdoll-death + tear path (the wisp normally grabs whoever is
// nearest, which is the host). Host-only (dev_gate); no-op if no client is connected. Safe off the
// game thread (posted to it).
void SpawnKillerWispOnClient();

}  // namespace coop::dev::spawn_npc
