// ue_wrap/devices/window_canvas.h -- engine access for the base's big bay window (Ad_window_C),
// whose dirt is a 1645x512 render target that a sponge wipes clean one dab at a time. Principle-7
// wrapper: reflection offsets, the UFunctions and the draw; no network or coop state
// (coop::window_stroke_sync owns those).
//
// A stroke is d_window_C::cleanPhys(sponge, Hit): it maps the hit to a texture pixel, stores the
// dab's top-left corner in the field `A`, opens the window's canvas session through its `Canvas`
// event (sets `cv`; the session closes itself 0.25 s after the last dab) and draws the sponge's
// brush material with UCanvas::K2_DrawMaterial(dynmat, A, (size,size), (0,0), (1,1), 0, (.5,.5)).
// cleanPhys is a script function reached through EX_LocalVirtualFunction, so no hook that carries
// arguments sees it; the stroke is observed at its native draw instead. A K2_DrawMaterial whose
// calling frame is a d_window_C with `cv` set is a stroke: the window's only other draw, the
// periodic dirt splotch, refuses to run while `cv` is set. The window is a singleton without a
// save Key. External code must never call setDraw/endDraw: the session belongs to `Canvas`.

#pragma once

namespace ue_wrap::window_canvas {

// One dab, in the render target's pixel space.
struct Dab {
    float x = 0.f, y = 0.f;  // top-left corner (the window's field A)
    float size = 0.f;        // edge length
    float opac = 0.f;        // brush material scalar "opac"
    float col = 0.f;         // brush material scalar "col" (0 = paint clean)
};

// Resolve d_window_C (fields A, canv, cv; the Canvas event) and UCanvas::K2_DrawMaterial.
// Idempotent; false while the window's Blueprint class is not loaded. Game thread.
bool EnsureResolved();

// Class-pure: obj is a d_window_C. False if unresolved.
bool IsWindow(void* obj);

// UCanvas::K2_DrawMaterial, for the stroke observer's Func post-hook. Null if unresolved.
void* DrawMaterialFn();

// True iff `sourceObject` -- the frame that issued a K2_DrawMaterial -- is a window with an open
// stroke session. Raw field reads only, so it is safe inside the Func hook.
bool IsStrokeDraw(void* sourceObject);

// The last stroke's top-left corner (field A). Raw field read.
bool ReadStrokeCorner(void* window, float& x, float& y);

// The brush the player's held sponge (or mop) paints with: its size and the brush material's
// current opac/col, which the sponge rewrites right before each dab. False when the player holds
// no sponge. Game thread (reads the material parameters through UFunctions).
bool ReadHeldBrush(void* mainPlayer, float& size, float& opac, float& col);

// Signal playback owns the world's render-target canvas; the window refuses to draw meanwhile.
bool IsSignalPlaying();

// Replay one dab on `window`: open or join the session through Canvas, then draw with a brush
// material instance this module owns. False when anything is unresolved. Game thread.
bool DrawDab(void* window, const Dab& d);

// Release the owned brush material (session teardown). Game thread.
void ReleaseBrush();

}  // namespace ue_wrap::window_canvas
