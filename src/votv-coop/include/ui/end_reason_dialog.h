// ui/end_reason_dialog.h -- the "could not connect" / "disconnected" modal (ImGui overlay
// surface). Shown over the main-menu server browser when a browser JOIN could not be
// established (a dead or ghost host, the server list unreachable, a refusal), and over the menu
// when a session ended after the join (a kick, a ban, the host quitting, the link lost). The
// NOTICE is owned by coop::join_progress -- Fail, RefuseJoin and NoteDisconnect stash it, this
// surface renders and clears it. Dependency direction is ui -> session (loading_screen reads
// join_progress the same way); join_progress never calls ui. A user CANCEL sets no notice, so
// cancelling a join is silent (no modal). Every notice carries its stable code
// (coop/net/end_reason), shown so a player can paste it into a report.
//
// Threading: IsOpen()/Render() are render-thread only (they peek join_progress's notice under
// its mutex). No engine calls.

#pragma once

namespace ui::end_reason_dialog {

// True while a notice is pending acknowledgement (join_progress holds it). Cheap peek; render
// thread only.
bool IsOpen();

// Draw the modal this frame (over the reopened browser, or the menu). No-ops when nothing is
// pending. The "OK" button acknowledges (clears the notice -> hides the modal). Render thread only.
void Render();

}  // namespace ui::end_reason_dialog
