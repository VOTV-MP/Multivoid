// ui/voice_panel.h -- the voice-chat settings window. Opened with the V key
// (game-focus edge in the overlay WndProc; user direction 2026-06-12: V opens
// voice settings, independent of the tilde scoreboard). Rendered by
// imgui_overlay as its own interactive surface (it joins the input-capture
// set); the open state latches until V / the window's X closes it.
//
// Setters write atomics (safe from the render thread); device/mode changes
// rewrite votv-coop.ini then RequestDevicesRestart() -- the reopen happens on
// the next game tick, never here. Render thread only.

#pragma once

namespace ui::voice_panel {

void Toggle();
void Close();
bool IsOpen();
void Render();

}  // namespace ui::voice_panel
