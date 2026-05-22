// dev/freecam.h -- developer free-flying camera (dev-only, ini-gated).
//
// A debugging tool to fly around the coop scene. Gated by votv-coop.ini
// ([dev] freecam=1); OFF by default so shipping players can't cheat with it.
//
// Controls (only when enabled in the ini):
//   HOME  -- toggle freecam on/off (view blends to/from a free camera).
//   WASD  -- move; Space / Ctrl -- up / down; Shift -- move faster.
//   mouse -- look (uses the game's own look, so it's as smooth as the game).
//   MMB   -- teleport the real player to the freecam's current position.
//
// Smoothness: look comes from the game's control rotation (no raw-mouse jitter)
// and movement is frame-synced + dt-scaled (driven off the player's per-frame
// tick), so it doesn't stutter regardless of frame rate.

#pragma once

namespace dev::freecam {

// Read votv-coop.ini; if [dev] freecam=1, start the HOME/MMB watcher and install
// the per-frame movement hook. No-op (and logs "disabled") otherwise. Call once
// after the game-thread dispatcher is live.
void Init();

}  // namespace dev::freecam
