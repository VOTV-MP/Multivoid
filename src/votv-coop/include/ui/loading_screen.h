// ui/loading_screen.h -- the CLIENT connecting/loading screen.
//
// The "connecting/loading" game state: while a client join is in progress, VOTV's menu
// widgets are hidden (coop::multiplayer_menu fades ui_menu_C to opacity 0 + HitTestInvisible)
// so only the 3D menu background remains -- the clean menu canvas -- and this surface draws a
// CENTERED progress bar + status text + a Cancel button over it. The menu + its music keep
// playing underneath; we just hide the interactive widgets. It is NOT a full-screen opaque
// cover. The connect log/errors stream in parallel via ui/console.
//
// INTERACTIVE (the Cancel button), so the overlay includes it in CaptureActive() while open.
// A thin renderer over coop::join_progress (principle 7: that module owns the join state
// machine; this draws a Snapshot() of it and forwards Cancel to it). No Open() -- the
// lifecycle is driven by network events through join_progress, not by a UI action.

#pragma once

namespace ui::loading_screen {

// Drawn each frame by the overlay while a join is active. Render thread. No-op otherwise.
void Render();

// True while a client join is in progress (== coop::join_progress::Active()). The overlay
// uses this to decide whether to Render() + include it in AnyOpen()/CaptureActive().
bool IsOpen();

// Force the screen down (resets join_progress). Called from the overlay's SEH fault handler
// + Shutdown() so a faulted/torn-down overlay can't leave the join state stuck.
void Close();

}  // namespace ui::loading_screen
