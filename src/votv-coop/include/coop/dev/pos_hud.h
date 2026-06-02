// coop/dev/pos_hud.h -- developer on-screen pos + camera readout (dev-only).
//
// A small left-middle UMG overlay that shows the local player's world position
// and the camera rotation in real time. Useful when staging scenes, picking
// spawn anchors, or sanity-checking the freecam / pose sync.
//
// Driven by the ImGui dev menu (Player > HUD > "Position / camera readout").
// The legacy F2 hotkey was RETIRED 2026-06-02 (RULE [[feedback-dev-features-in-
// imgui-menu]]). The overlay is a separate in-world UMG widget so it stays on
// screen while you move with the menu closed.
//
// The overlay is HitTestInvisible (never steals input) and refreshes ~10 Hz
// (numeric readouts don't need per-frame updates). The widget lives off the
// GameInstance, so it survives level loads -- the toggle just hides/shows it.

#pragma once

namespace coop::dev::pos_hud {

// Menu action (Player > HUD): show/hide the overlay. On first show it lazily
// creates the widget + starts a ~10 Hz refresh-pump thread. Safe to call off the
// game thread (the show/hide is posted to it). Idempotent.
void SetVisible(bool on);

// Current visibility (for the menu checkbox). Lock-free.
bool IsVisible();

}  // namespace coop::dev::pos_hud
