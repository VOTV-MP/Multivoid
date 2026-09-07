// ui/scale.h -- the ONE owner of the overlay's resolution-scale axis.
//
// Every pixel constant in ui/ was authored on a 1080p screen, so on 1440p and 4K the whole
// overlay -- fonts, windows, paddings -- shrank relative to the screen. Ui() is the
// proportional factor clientHeight / 1080, quantized to sixths so the standard heights land
// exactly (720p = 2/3, 1080p = 1, 1440p = 4/3, 2160p = 2) and a windowed drag-resize does not
// re-bake the font atlas every frame. ui::style::MaybeRescale() polls the client rect each
// frame and performs the atlas/style rebuild when ConsumeRebuild() fires.
//
// All state is render-thread-only (the Present detour thread), like the rest of the overlay.

#pragma once

namespace ui::scale {

// Feed the current client-area size (render thread, once per frame). Marks a
// rebuild when the quantized factor changes.
void NoteViewport(float width, float height);

// Current combined scale factor (resolution factor x the player's size pref, capped at 4.0).
// Stable within a frame. Three consumers read it, all on the render thread: ui::fonts::Load()
// bakes the atlas at px * Ui(), a real rasterized size and NOT io.FontGlobalScale, which
// stretches the 1x bitmap and blurs; ImGuiStyle::ScaleAllSizes(Ui()) runs after a style reset,
// for paddings and spacing; and every explicit pixel constant in ui/ goes through S().
float Ui();

// The player's "UI size" preference (multivoid.ini ui.scale, default 1.25). Multiplies the
// resolution factor; the F1 > Cosmetics > Interface slider drives it live.
float UserScale();
void  SetUserScale(float s);   // clamps to the ui.scale registry row's [lo, hi]
void  LoadUserPrefOnce();      // read ui.scale from the ini (bring-up, latched)
// The pref clamp range -- owned by the ui.scale registry row (arc 2); the F1
// slider consumes these so the slider and the clamp can never diverge.
float UserScaleMin();
float UserScaleMax();

// Scale a 1080p-authored pixel constant to the live resolution.
inline float S(float px) { return px * Ui(); }

// A consumer other than the viewport (the F1 font-family switch) wants the
// atlas/style rebuilt on the next frame.
void RequestRebuild();

// True exactly once after NoteViewport/RequestRebuild flagged a change; the
// caller (ui::style::MaybeRescale) then re-bakes fonts + rescales style.
bool ConsumeRebuild();

}  // namespace ui::scale
