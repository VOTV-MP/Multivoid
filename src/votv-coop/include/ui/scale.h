// ui/scale.h -- the ONE owner of the overlay's resolution-scale axis.
//
// Every pixel constant in ui/ was authored on a 1080p screen; on 1440p/4K the
// whole overlay (fonts, windows, paddings) shrank relative to the screen (user
// 2026-07-04: "на маленьком экране норм, на большом всё мелкое"). Ui() is the
// proportional factor clientHeight / 1080, quantized to sixths so the standard
// heights land exactly (720p=2/3, 1080p=1, 1440p=4/3, 2160p=2) and a windowed
// drag-resize doesn't re-bake the font atlas every frame.
//
// The factor feeds THREE consumers, all on the render thread:
//   1. ui::fonts::Load() bakes the atlas at px * Ui() (real rasterized size --
//      NOT io.FontGlobalScale, which stretches the 1x bitmap and blurs);
//   2. ImGuiStyle::ScaleAllSizes(Ui()) after a style reset (paddings/spacing);
//   3. every explicit pixel constant in ui/ goes through S().
// imgui_overlay::MaybeRescale() polls the client rect each frame and performs
// the atlas/style rebuild when ConsumeRebuild() fires.
//
// All state is render-thread-only (the Present detour thread), like the rest
// of the overlay UI state.

#pragma once

namespace ui::scale {

// Feed the current client-area size (render thread, once per frame). Marks a
// rebuild when the quantized factor changes.
void NoteViewport(float width, float height);

// Current combined scale factor (resolution factor x the user's size pref,
// capped at 4.0). Stable within a frame.
float Ui();

// The user's "UI size" pref (votv-coop.ini ui.scale, default 1.25 -- user
// 2026-07-04: "все менюшки и тексты ПОБОЛЬШЕ"). Multiplies the resolution
// factor; the F1 > Cosmetics > Interface slider drives it live.
float UserScale();
void  SetUserScale(float s);   // clamps to [0.75, 1.75]; requests the rebuild
void  LoadUserPrefOnce();      // read ui.scale from the ini (bring-up, latched)

// Scale a 1080p-authored pixel constant to the live resolution.
inline float S(float px) { return px * Ui(); }

// A consumer other than the viewport (the F1 font-family switch) wants the
// atlas/style rebuilt on the next frame.
void RequestRebuild();

// True exactly once after NoteViewport/RequestRebuild flagged a change; the
// caller (imgui_overlay::MaybeRescale) then re-bakes fonts + rescales style.
bool ConsumeRebuild();

}  // namespace ui::scale
