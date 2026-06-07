// ui/console.h -- the in-game console (a general feature for all players).
//
// A streaming log view + command input. It subscribes to the mod's log stream (via
// ue_wrap::log::SetSink) so it mirrors everything the mod logs -- connect progress, errors,
// and general output -- on screen, colored by level. It AUTO-SHOWS while a client join is in
// progress (coop::join_progress::Active()) so the player sees the connect log/errors next to
// the centered loading screen; otherwise it is user-toggled.
//
// The command input is the foundation for player/host commands (typed here, dispatched to
// the session) -- for now it handles a couple of LOCAL commands (help, clear); networked
// host/client commands come with the command subsystem.

#pragma once

namespace ui::console {

// Register the logger sink so the console starts capturing the mod log. Call once at boot
// (before/around the rest of the UI). Idempotent.
void Init();
// Unregister the sink (overlay teardown).
void Shutdown();

// Draw the console each frame while open (render thread). No-op when closed.
void Render();

// Open while user-toggled OR while a client join is in progress (auto-show).
bool IsOpen();
void Open();
void Close();
void Toggle();

}  // namespace ui::console
