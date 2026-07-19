// ui/boot_warning_dialog.h -- the mod-install problem modal (ImGui overlay surface).
//
// v122 (2026-07-19, the multivoid versioned-artifact rename): when the xinput
// proxy finds MORE THAN ONE payload version file beside the exe (a botched
// manual update -- e.g. multivoid-0.9.0n-120.dll AND multivoid-0.10.0-75.dll,
// or a stale legacy votv-coop.dll), it loads the highest build and hands the
// leftover list to the payload via the MULTIVOID_DUP_FILES env var. The boot
// thread Arm()s this dialog; it renders over whatever surface is up until the
// user acknowledges. Generic on purpose: any future boot-time install problem
// can Arm() it too.
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
