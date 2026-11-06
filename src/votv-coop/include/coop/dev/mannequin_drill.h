// coop/dev/mannequin_drill.h -- [dev] spawn a native walking mannequin; the baseline drill.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::mannequin_drill {

// Spawn one native walking mannequin the way the game's own spawner does: every spawn point is
// shown for a moment (prepareSpawn), and half a second later the points are asked in turn to spawn
// the walker (wMannequinSpawn_C.spawn) until one accepts; a point on screen refuses. All of that is
// the game's. The game shuffles its points; this instrument asks the nearest to the local player
// first. The walker is the game's real one: it opens doors, is saved with the world and returns angry after a load.
// Refused on a client in a session (dev_gate), while an earlier request is still running, and
// when five walkers are alive (the game itself culls above four). Any thread.
void SpawnWalker();

// The frame tail's entry (harness::pump::TickFrameTail): a single atomic read when no spawn is
// pending. Game thread.
void TickFrame();

// The [dev] mannequin_drill row's baseline phases (`spawn`, `red`); a single enum read for any other value, the two-peer walker
// drill's (coop/dev/mannequin_walker_drill.h) included. Game thread.
void Tick(coop::net::Session* session);

// The local session's last link dropped: the client's samples start over. The host's half runs
// once per process and is not re-armed.
void OnDisconnect();

}  // namespace coop::dev::mannequin_drill
