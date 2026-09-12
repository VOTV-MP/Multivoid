// ui/boot_warning_dialog.h -- the mod-install problem modal (ImGui overlay surface).
//
// The generic boot-time install-problem modal. Arm() renders it over whatever surface is up
// until the user acknowledges. Its current feeder is server_browser_native's missing-donor
// warning ("the game updated and the mod needs a new release").
//
// A DUPLICATE install does not come here: the loader's predecessor scan and boot's per-process
// mutex refuse the second instance outright, through a plain MessageBox, because a refused
// instance must never install the overlay this dialog renders from.
//
// Threading: Arm() from the boot thread (before the overlay ever presents); IsOpen() and
// Render() are render-thread only. Same ownership shape as end_reason_dialog, but the
// pending text lives HERE (there is no join_progress analogue for boot problems).

#pragma once

#include <string>

namespace ui::boot_warning_dialog {

// Queue a warning for display (boot thread; called once, before first present).
// Non-empty text arms the modal; the user's OK clears it.
void Arm(const std::string& text);

// True while a warning is pending acknowledgement. Cheap peek; render thread.
bool IsOpen();

// Draw the modal this frame. No-ops when nothing pending. Render thread only.
void Render();

// Drop the pending warning without acknowledgement -- the SEH re-fault guard. imgui_overlay's
// __except must clear every surface whose open flag would otherwise re-enter a faulted
// Render() every frame.
void Clear();

}  // namespace ui::boot_warning_dialog
