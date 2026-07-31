// ui/style.h -- the ImGui STYLE at the current UI factor: who owns it, and when it
// is re-derived.
//
// Extracted from ui/imgui_overlay.cpp 2026-07-31 (the modular file-size rule: that
// file crossed the 800 LOC soft cap). The seam is deliberately narrow -- the overlay
// owns the swap chain, the hooks and the surfaces; this owns exactly one question:
// what does the style look like at `ui::scale::Ui()`, and what has to be rebuilt when
// that factor moves.

#pragma once

struct HWND__;
using HWND = HWND__*;

namespace ui::style {

// Re-derive the WHOLE style from defaults at the current UI factor.
//
// ONE owner, and that is load-bearing: `ImGuiStyle::ScaleAllSizes` is cumulative and
// lossy, so the only correct usage is reset-then-scale, never scale-again -- and it
// also needs a correction applied afterwards (see the .cpp). Two call sites each
// remembering to re-apply that correction is how the cursor bug survived for months.
//
// Render thread (it touches ImGui state), and only outside a frame.
void Rebuild();

// Poll the client rect, and if the resulting UI factor changed, re-bake the fonts,
// drop the backend's device objects and Rebuild(). No-ops when nothing moved, so it
// is safe to call every frame -- which is how it is called.
void MaybeRescale(HWND hwnd);

}  // namespace ui::style
