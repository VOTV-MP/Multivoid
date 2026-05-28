// coop/dev/pos_hud.h -- developer on-screen pos + camera readout (dev-only, ini-gated).
//
// A small left-middle UMG overlay that shows the local player's world position
// and the camera rotation in real time. Useful when staging scenes, picking
// spawn anchors, or sanity-checking the freecam / pose sync.
//
// Gated by votv-coop.ini ([dev] posinfo=1); OFF by default. While enabled:
//   F2 -- toggle the overlay on/off (the default is OFF on launch so the
//         readout doesn't clutter screenshots).
//
// The overlay is HitTestInvisible (never steals input) and refreshes ~10 Hz
// (numeric readouts don't need per-frame updates). The widget lives off the
// GameInstance, so it survives level loads -- the toggle just hides/shows it.

#pragma once

namespace coop::dev::pos_hud {

// Read votv-coop.ini; if [dev] posinfo=1, start the F2 watcher. No-op otherwise.
// Call once after the game-thread dispatcher is live.
void Init();

}  // namespace coop::dev::pos_hud
