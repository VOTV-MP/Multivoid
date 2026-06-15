// ui/toast.h -- transient top-center overlay notifications (the v59 launch
// version toast; generic for future one-line notices).
//
// Push() is thread-safe (the version check completes on an HTTP worker);
// Render() runs on the render thread inside the ImGui frame (imgui_overlay
// calls it last so toasts sit above every other surface). A toast expires
// after its per-push duration; expired entries are dropped lazily in Render.
// No input capture -- the window is click-through (NoInputs), the player
// keeps playing/menuing under it.

#pragma once

#include <string>

namespace ui::toast {

// Queue a toast line. `seconds` = visible duration; `warn` tints the text
// amber (the OUTDATED case) instead of the default soft white.
void Push(const std::string& text, float seconds, bool warn);

// Draw all live toasts (top-center stack). Call once per ImGui frame.
void Render();

}  // namespace ui::toast
