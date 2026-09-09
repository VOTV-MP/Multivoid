// ui/input_focus.h -- who owns the keyboard right now (window + overlay text).
//
// These are INPUT-ARBITRATION predicates, not configuration. A global GetAsyncKeyState
// hotkey poller gates on BOTH (the dev screenshot watcher is the one that does not):
//   1) IsOurWindowForeground() -- a same-machine host+client pair must not fire both
//      instances when the user presses a hotkey in one window (GetAsyncKeyState is
//      process-global).
//   2) !IsOverlayCapturingText() -- a keystroke typed into an overlay text field (chat input,
//      rebind box) must never ALSO fire a game or voice bind. Pressing T to chat then G used
//      to activate voice: the mic thread's global poll is independent of ImGui eating it.
//
// The overlay-capture flag (io.WantTextInput, or chat open) is published once a frame by the render
// thread and read by pollers on their own threads -- a relaxed atomic, one frame of staleness being
// harmless for a held-key poll. It defaults false until the overlay first publishes. No engine
// access; safe from any thread.

#pragma once

namespace ui::input_focus {

// True ONLY when the current foreground window belongs to our process. Returns
// true if no foreground window query is possible (defensive default).
bool IsOurWindowForeground();

// Publisher (render thread, once per frame) / readers (any thread).
void SetOverlayCapturingText(bool capturing);
bool IsOverlayCapturingText();

}  // namespace ui::input_focus
