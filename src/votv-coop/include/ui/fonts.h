// ui/fonts.h -- overlay font loading (render-thread UI layer).
//
// The stock ImGui default font (ProggyClean 13 px) has NO Cyrillic glyphs,
// which is why the chat pipeline historically ASCII-squashed everything. This
// module bakes a vendored family (embedded in the DLL as RCDATA -- Roboto,
// JetBrains Mono, Cascadia Code; Regular for every panel, Bold at chat size
// for the chat feed/input) with Cyrillic glyph ranges, rasterized by
// imgui_freetype (sharper hinting than stb_truetype at UI sizes).
//
// RESOLUTION SCALE (2026-07-04): fonts are baked at kPx * ui::scale::Ui() --
// the REAL rasterized size for the live resolution. Never io.FontGlobalScale
// (bitmap stretch = blur). Load() is re-entrant: imgui_overlay::MaybeRescale
// re-runs it (it clears the atlas first) when the scale or family changes,
// then invalidates the DX11 device objects so the backend re-bakes.
//
// Family is a user pref: votv-coop.ini ui.font = roboto | jetbrains |
// cascadia (default roboto -- user 2026-07-04 after comparing all three);
// switch live in F1 > Cosmetics > Interface.
//
// Load() must run between ImGui::CreateContext() and the first NewFrame (the
// DX11 backend bakes the atlas lazily on frame 1).

#pragma once

struct ImFont;

namespace ui::fonts {

// Base text sizes in px AT 1080p (the ui::scale reference). The baked size is
// this times ui::scale::Ui().
inline constexpr float kUiPx   = 16.f;
inline constexpr float kChatPx = 18.f;

enum class Family : int { JetBrainsMono = 0, Roboto = 1, CascadiaCode = 2 };
inline constexpr int kFamilyCount = 3;

// (Re)bake the overlay fonts into the shared atlas at the current scale +
// family. Clears the atlas first. Call only BETWEEN frames (bring-up, or the
// MaybeRescale window before NewFrame).
void Load();

// The chat font (bold, chat size) or nullptr if no TTF loaded (use ImGui::GetFont()).
ImFont* Chat();

// The px the chat font was actually baked at (kChatPx * scale at bake time).
// Drawing AddText at this size renders the crisp 1:1 rasterization.
float ChatPx();

Family      CurrentFamily();
const char* FamilyLabel(Family f);   // UI label ("JetBrains Mono", ...)

// Switch the overlay family: persists votv-coop.ini ui.font and requests the
// atlas rebuild (applies next frame). Render thread (F1 menu).
void SetFamily(Family f);

// The ImGui context that owned the atlas is being destroyed (failed bring-up
// retry path): drop the cached ImFont* so the NEXT context re-loads instead
// of handing out a dangling pointer (audit 2026-07-04 item 1c).
void OnContextDestroyed();

}  // namespace ui::fonts
