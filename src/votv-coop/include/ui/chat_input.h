// ui/chat_input.h -- the T-chat input bar (render thread, ImGui).
//
// T (during a coop session, no other surface capturing) opens the bar
// (imgui_overlay's WndProc edge); typing goes to ImGui only (CaptureActive
// swallows game input while open). Enter sends via coop::chat_sync::QueueSend
// and closes; ESC closes WITHOUT sending and falls through to the game (the
// pause menu opens normally -- the user-requested "ESC = chat gone" behavior).

#pragma once

namespace ui::chat_input {

bool IsOpen();
void Open();
void Close();

// Draw the input bar (bottom-left, above the chat feed). Call only while
// IsOpen() from the per-frame ImGui pass.
void Render();

}  // namespace ui::chat_input
