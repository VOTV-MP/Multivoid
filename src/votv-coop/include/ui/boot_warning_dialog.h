// ui/boot_warning_dialog.h -- the mod-install problem modal (ImGui overlay surface).
//
// Born v122 (2026-07-19) as the xinput proxy's duplicate-payload popup; that
// feeder retired with the proxy lane at UE4SS_ARC WP-2 commit 3 (duplicate
// installs are now cppmod_entry.cpp's predecessor/dup-mutex REFUSE MessageBox
// -- a refused instance must never install the overlay this dialog renders
// from). The dialog stays as the generic boot-time install-problem modal;
// current feeder: server_browser_native's missing-donor warning ("the game
// updated and the mod needs a new release"). Arm() renders it over whatever
// surface is up until the user acknowledges.
//
// Threading: Arm() from the boot thread (before the overlay ever presents);
// IsOpen()/Render() render-thread only. Same ownership shape as
// connect_failed_dialog, but the pending text lives HERE (there is no
// join_progress analogue for boot problems).

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

// Drop the pending warning without acknowledgement -- the SEH re-fault guard
// (imgui_overlay's __except must clear every surface whose open flag would
// otherwise re-enter a faulted Render() every frame; audit 2026-07-19).
void Clear();

}  // namespace ui::boot_warning_dialog
