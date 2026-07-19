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
// Family is a PER-ROLE user pref: multivoid.ini ui.font.<menu|chat|net|
// nameplate|toast> = fixedsys | roboto | jetbrains | cascadia (per-role
// defaults below; menu/chat/toast = fixedsys -- VOTV's own terminal pixel
// font, 2026-07-09); switch live in F1 > Cosmetics > Interface. fixedsys is
// the game's own font (FSEX300 = font_terminal), bundled from the VOTV
// assets. (The old GLOBAL ui.font key is dead -- per-role only.)
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

enum class Family : int { JetBrainsMono = 0, Roboto = 1, CascadiaCode = 2, Fixedsys = 3 };
inline constexpr int kFamilyCount = 4;

// Nameplate base text size (px at 1080p) -- the up-close size; the plate scales
// DOWN with distance from here (hud::kNickPx mirrors this).
inline constexpr float kNameplatePx = 16.f;

// GRANULAR font roles (2026-07-09): each on-screen surface picks its OWN family, so
// chat, the net-stats widget, the nameplates, the menu/panels and our release/update
// toast are independent. Menu is baked FIRST -> it is ImGui's default font, so every
// panel (F1, scoreboard, admin, server browser, loading) follows it with no per-window
// push; Chat/Net/Nameplate/Toast are pushed by their consumers. Persisted per role as
// multivoid.ini ui.font.<menu|chat|net|nameplate|toast>. Per-role DEFAULTS (user
// 2026-07-09): menu/chat/toast = Fixedsys (VOTV); nameplate/net = Roboto.
enum class Role : int { Menu = 0, Chat = 1, Net = 2, Nameplate = 3, Toast = 4 };
inline constexpr int kRoleCount = 5;

// (Re)bake the overlay fonts into the shared atlas at the current scale + the
// per-role families. Clears the atlas first. Call only BETWEEN frames (bring-up,
// or the MaybeRescale window before NewFrame).
void Load();

// The baked ImFont* for a role (nullptr only if the whole atlas failed -> the
// caller uses ImGui::GetFont()). PxFor = the px it was baked at (draw AddText at
// this size for the crisp 1:1 rasterization).
ImFont* FontFor(Role r);
float   PxFor(Role r);

const char* FamilyLabel(Family f);   // "JetBrains Mono", ...
const char* RoleLabel(Role r);       // "Menu / panels", "Chat", "Net stats", "Nameplates"

// Per-role family get/set. SetRoleFamily persists ui.font.<role> and requests the
// atlas rebuild (applies next frame). Render thread (F1 menu).
Family RoleFamily(Role r);
void   SetRoleFamily(Role r, Family f);

// The ImGui context that owned the atlas is being destroyed (failed bring-up
// retry path): drop the cached ImFont* so the NEXT context re-loads instead
// of handing out a dangling pointer (audit 2026-07-04 item 1c).
void OnContextDestroyed();

}  // namespace ui::fonts
