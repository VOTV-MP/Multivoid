// coop/dev/freecam.h -- developer free-flying camera (dev-only).
//
// A debugging tool to fly around the coop scene. Toggle from EITHER the ImGui dev
// menu (Player > Movement > "Freecam") OR the HOME key -- the HOME toggle is kept
// by explicit user request (the one hotkey exception to the F1-menu rule
// [[feedback-dev-features-in-imgui-menu]]). Controls while flying:
//   HOME  -- toggle freecam on/off (also the menu checkbox).
//   WASD  -- move; Space / Ctrl -- up / down; Shift -- move faster.
//   mouse -- look (uses the game's own look, so it's as smooth as the game).
//   wheel -- adjust fly speed; MMB -- bring the real player to the freecam.
//
// Smoothness: look comes from the game's control rotation (no raw-mouse jitter)
// and movement is frame-synced + dt-scaled (driven off the player's per-frame
// tick), so it doesn't stutter regardless of frame rate.

#pragma once

namespace coop::dev::freecam {

// Read multivoid.ini; if [dev] freecam=1 (and master not killed), start the HOME
// toggle + movement driver + mouse-wheel hook threads. No-op otherwise. Call once
// from harness boot. (The menu's SetActive also lazily starts these, so freecam
// works via the menu even when freecam=0 but devkeys=1.)
void Init();

// Menu action (Player > Movement): turn the free camera on/off (the view blends
// to/from a free camera). Lazily starts the driver threads if Init didn't. Safe
// to call off the game thread (the actual enable/disable is posted to it).
void SetActive(bool on);

// Freecam currently on? (for the menu checkbox; reflects HOME-driven toggles
// too). Lock-free.
bool IsActive();

}  // namespace coop::dev::freecam
