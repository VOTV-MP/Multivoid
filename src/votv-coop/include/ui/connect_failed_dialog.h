// ui/connect_failed_dialog.h -- the "could not connect" modal (ImGui overlay surface).
//
// A small modal shown over the main-menu server browser when a browser JOIN could
// not be established (dead/ghost host timeout, master unreachable, bad address). The
// failure REASON is owned by coop::join_progress -- Fail() stashes it when it wins the
// abort, this surface renders + clears it. Dependency direction is ui -> session
// (loading_screen reads join_progress the same way); join_progress never calls ui.
//
// A user CANCEL sets no reason, so cancelling a join is silent (no modal). Born
// 2026-07-16: a timed-out browser connect used to leak a false "Remote player left
// the game" toast into the menu with no explanation of why the join failed.
//
// Threading: IsOpen()/Render() are render-thread only (they peek join_progress's
// reason under its mutex). No engine calls.

#pragma once

namespace ui::connect_failed_dialog {

// True while a connect-failure reason is pending acknowledgement (join_progress holds
// it). Cheap peek; render thread only.
bool IsOpen();

// Draw the modal this frame (over the reopened browser). No-ops when nothing pending.
// The "OK" button acknowledges (clears the reason -> hides the modal). Render thread only.
void Render();

}  // namespace ui::connect_failed_dialog
