// ui/voice_panel.h -- the voice-chat settings window. Opened with the V key
// (game-focus edge in the overlay WndProc). V opens voice settings and is
// independent of the tilde scoreboard. Rendered by imgui_overlay as its own
// interactive surface (it joins the input-capture set); the open state latches
// until V / the window's X closes it.
//
// Setters write atomics (safe from the render thread); device/mode changes
// rewrite multivoid.ini then RequestDevicesRestart() -- the reopen happens on
// the next game tick, never here. Render thread only.

#pragma once

namespace ui::voice_panel {

void Toggle();
void Close();
bool IsOpen();
void Render();

}  // namespace ui::voice_panel
