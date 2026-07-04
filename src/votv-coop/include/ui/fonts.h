// ui/fonts.h -- overlay font loading (render-thread UI layer).
//
// The stock ImGui default font (ProggyClean 13 px) has NO Cyrillic glyphs, which
// is why the chat pipeline historically ASCII-squashed everything ('?' for every
// Russian char). This module makes vendored Roboto (assets/fonts, Apache 2.0,
// deployed next to the DLL) the DEFAULT font of the WHOLE overlay -- Regular for
// every panel, Bold at chat size for the chat feed/input -- both with Cyrillic
// glyph ranges + 2x oversampling (the chat-imgui-samp recipe). Fallback chain
// when the deployed files are missing: Windows Tahoma/Segoe UI; last resort =
// the ImGui default (ASCII-only, pre-2026-07-04 look).
//
// Load() must run between ImGui::CreateContext() and the first NewFrame (the
// DX11 backend bakes the atlas lazily on first frame).

#pragma once

struct ImFont;

namespace ui::fonts {

// Overlay base text size (all panels) + chat text size in px.
inline constexpr float kUiPx   = 16.f;
inline constexpr float kChatPx = 18.f;

// Load the overlay fonts into the shared atlas. Call once during overlay init.
void Load();

// The chat font (bold, kChatPx) or nullptr if no TTF loaded (use ImGui::GetFont()).
ImFont* Chat();

// The ImGui context that owned the atlas is being destroyed (failed bring-up
// retry path): drop the latch so the NEXT context re-loads instead of handing
// out a dangling ImFont* (audit 2026-07-04 item 1c).
void OnContextDestroyed();

}  // namespace ui::fonts
