// coop/dev/spawn_npc.h -- DEV/TEST: spawn a kerfurOmega NPC on demand.
//
// Gameplay/dev layer. The ONLY programmatic NPC-spawn trigger in the mod --
// VOTV NPCs spawn only from in-game purchase / scripted events, so without this
// the NPC-sync paths (host AllocAndInstall + broadcast, client mirror Install)
// have no autonomous-test coverage. Two triggers:
//   * the ImGui dev menu (Game > Entities > "Spawn kerfurOmega") -- hands-on.
//     The legacy F7 hotkey was RETIRED 2026-06-02 (RULE [[feedback-dev-features-
//     in-imgui-menu]]).
//   * a trigger FILE named by env VOTVCOOP_SPAWN_TRIGGER -- when the file
//     appears it spawns once + deletes it. mp.py's npctest creates the file
//     AFTER all peers connect, so the broadcast reaches every client (NPC
//     EntitySpawn is not part of the connect-edge replay).
// The actual spawn is npc_sync::DevSpawnNpcInFront, run on the game thread.

#pragma once

namespace coop::dev::spawn_npc {

// Start the trigger-FILE watcher thread, IF env VOTVCOOP_SPAWN_TRIGGER is set
// (autonomous tests). No-op otherwise -- the menu is the hands-on spawn path.
// Gated by [dev] enabled (master). Call once from harness boot.
void Init();

// Menu action (Game > Entities): spawn a kerfurOmega_C in front of the local
// player (host-side sync path). Safe to call off the game thread (the spawn is
// posted to it).
void SpawnKerfurOmega();

}  // namespace coop::dev::spawn_npc
