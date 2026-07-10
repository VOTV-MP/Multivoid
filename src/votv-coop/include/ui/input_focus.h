// ui/input_focus.h -- who owns the keyboard right now (window + overlay text).
//
// Extracted from coop/session/ini_config (2026-07-10 placement #5): these are
// INPUT-ARBITRATION predicates, not configuration. Every global GetAsyncKeyState
// hotkey poller gates on BOTH:
//   1) IsOurWindowForeground() -- a same-machine host+client pair must not fire
//      both instances when the user presses a hotkey in one window
//      (GetAsyncKeyState is process-global).
//   2) !IsOverlayCapturingText() -- a keystroke typed into an overlay text field
//      (chat input, rebind box) must never ALSO fire a game/voice bind. Born
//      2026-07-09: pressing T to chat then G activated voice, because the mic
//      thread's global GetAsyncKeyState(G) poll is independent of ImGui eating
//      the 'g' as text.
//
// The overlay-capture flag is published once per frame by the render thread
// (io.WantTextInput || chat open) and read by pollers on their own threads --
// a relaxed atomic (one independent bool; one-frame staleness is harmless for
// a held-key poll). Defaults false (keys live) until the overlay first
// publishes. No engine access; safe from any thread.

#pragma once

namespace ui::input_focus {

// True ONLY when the current foreground window belongs to our process. Returns
// true if no foreground window query is possible (defensive default).
bool IsOurWindowForeground();

// Publisher (render thread, once per frame) / readers (any thread).
void SetOverlayCapturingText(bool capturing);
bool IsOverlayCapturingText();

}  // namespace ui::input_focus
