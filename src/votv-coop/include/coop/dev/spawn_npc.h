// coop/dev/spawn_npc.h -- DEV/TEST: spawn a kerfurOmega NPC on demand.
//
// Gameplay/dev layer. The ONLY programmatic NPC-spawn trigger in the mod --
// VOTV NPCs spawn only from in-game purchase / scripted events, so without this
// the NPC-sync paths (host AllocAndInstall + broadcast, client mirror Install)
// have no autonomous-test coverage. Two triggers:
//   * F7 (hands-on, [dev] devkeys=1, foreground-gated) -- spawn a kerfur.
//   * a trigger FILE named by env VOTVCOOP_SPAWN_TRIGGER -- when the file
//     appears it spawns once + deletes it. mp.py's npctest creates the file
//     AFTER all peers connect, so the broadcast reaches every client (NPC
//     EntitySpawn is not part of the connect-edge replay).
// The actual spawn is npc_sync::DevSpawnNpcInFront, run on the game thread.

#pragma once

namespace coop::dev::spawn_npc {

// Start the F7 + trigger-file watcher thread. Gated by [dev] enabled (master)
// AND ([dev] devkeys for F7, OR VOTVCOOP_SPAWN_TRIGGER set for the file watch).
void Init();

}  // namespace coop::dev::spawn_npc
