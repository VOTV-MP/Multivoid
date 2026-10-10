// ui/fonts.h -- overlay font loading (render-thread UI layer).
//
// The stock ImGui default font, ProggyClean at 13 px, has NO Cyrillic glyphs, which is why the chat
// pipeline used to squash everything to ASCII. This module bakes a vendored family -- embedded in
// the DLL as RCDATA: Roboto, JetBrains Mono, Cascadia Code, Regular for every panel and Bold at
// chat size for the chat feed and input -- with Cyrillic glyph ranges, rasterized by
// imgui_freetype, which hints more sharply than stb_truetype at UI sizes.
//
// Fonts are baked at kPx * ui::scale::Ui(), the REAL rasterized size for the live resolution, never
// through io.FontGlobalScale, which stretches the bitmap and blurs it. Load() is re-entrant:
// ui::style::MaybeRescale clears the atlas and re-runs it when the scale or the family changes,
// then invalidates the DX11 device objects so the backend re-bakes. It must run between
// ImGui::CreateContext() and the first NewFrame, since the DX11 backend bakes the atlas lazily on
// frame 1.

#pragma once

#include <cstdint>

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

// GRANULAR font roles: each on-screen surface picks its OWN family, so chat, the net-stats widget,
// the nameplates, the menu and panels, and the release-and-update toast are independent. Menu is
// baked FIRST and so becomes ImGui's default font, which every panel -- F1, scoreboard, admin,
// server browser, loading -- then follows with no per-window push; chat, net, nameplate and toast
// are pushed by their consumers.
//
// Persisted per role as multivoid.ini ui.font.<menu|chat|net|nameplate|toast>, each one of
// fixedsys, roboto, jetbrains or cascadia, and switchable live in F1 > Cosmetics > Interface. The
// defaults are Fixedsys for menu, chat and toast, and Roboto for nameplate and net. Fixedsys is the
// game's own terminal pixel font (FSEX300, font_terminal), taken from the VOTV assets and
// embedded as RCDATA like the rest.
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
const char* RoleLabel(Role r);       // the role's name in the player's language ("Menu / panels" in English)

// The role's family. Render thread (F1 menu). A pane changes it by setting the ui.font.<role> row;
// ApplyRowsIfChanged applies it.
Family RoleFamily(Role r);

// The registry token of a family -- what a pane passes to SetValue.
const char* FamilyToken(Family f);

// Follow the five `ui.font` rows: once, at boot.
void SubscribeRows();

// Render thread, once per frame: when a row changed since the last frame (one atomic exchange),
// re-resolve the five and apply; the atlas rebuild is requested once when any family differed.
void ApplyRowsIfChanged();

// How many times a font row change was applied since boot; any thread. The settings drill's
// readiness.
uint32_t RowsApplies();

// The ImGui context that owned the atlas is being destroyed -- the failed bring-up retry path. Drop
// the cached ImFont* so the NEXT context re-loads instead of handing out a dangling pointer.
void OnContextDestroyed();

}  // namespace ui::fonts
