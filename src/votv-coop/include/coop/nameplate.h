// coop/nameplate.h -- floating nickname labels above remote players.
//
// Gameplay layer (principle 7). Owns the set of remote players to label and the
// per-frame draw logic (MTA nametag shape: project the head world point to
// screen, fade with distance, draw centered outlined text). It draws through
// ue_wrap::hud only -- no engine memory access, no UFunction marshaling here.

#pragma once

namespace coop {

class RemotePlayer;

namespace nameplate {

// Register a remote player to be labelled -- spawns a 3D text label above its
// head. Game thread.
void Register(RemotePlayer* player);

// Stop labelling a player and destroy its label (call before destroying it).
// Game thread.
void Unregister(RemotePlayer* player);

// Per-frame-ish update: reposition each label above its player's head and
// billboard it to face the local player. Call periodically from the game thread
// (the harness posts this every ~50 ms). Cheap; no-op if no labels.
void Update();

}  // namespace nameplate
}  // namespace coop
