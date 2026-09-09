// ui/input_focus.h -- who owns the keyboard right now (window + overlay text).
//
// These are INPUT-ARBITRATION predicates, not configuration, which is why they do not live
// with the ini rows. Every global GetAsyncKeyState hotkey poller gates on BOTH:
//   1) IsOurWindowForeground() -- a same-machine host+client pair must not fire both
//      instances when the user presses a hotkey in one window (GetAsyncKeyState is
//      process-global).
//   2) !IsOverlayCapturingText() -- a keystroke typed into an overlay text field (chat
//      input, rebind box) must never ALSO fire a game or voice bind. Pressing T to chat and
//      then G used to activate voice, because the mic thread's global GetAsyncKeyState(G)
//      poll is independent of ImGui eating the 'g' as text.
//
// The overlay-capture flag is published once per frame by the render thread and read by
// pollers on their own threads -- a relaxed atomic, since one frame of staleness is harmless
// for a held-key poll. It defaults false (keys live) until the overlay first publishes.

#pragma once

namespace ui::input_focus {

// True ONLY when the current foreground window belongs to our process. Returns
// true if no foreground window query is possible (defensive default).
bool IsOurWindowForeground();

// Publisher (render thread, once per frame) / readers (any thread).
void SetOverlayCapturingText(bool capturing);
bool IsOverlayCapturingText();

}  // namespace ui::input_focus
