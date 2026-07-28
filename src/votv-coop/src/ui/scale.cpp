// ui/scale.cpp -- see ui/scale.h.

#include "ui/scale.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ui::scale {
namespace {

// Reference design height: all ui/ pixel constants were authored at 1080p.
constexpr float kRefHeight = 1080.f;

// Clamp so a tiny window can't shrink the UI unreadable and an 8K screen +
// a max size pref can't explode the atlas (glyph area grows quadratically).
constexpr float kResMin = 0.5f;
constexpr float kResMax = 3.0f;
constexpr float kCombinedMax = 4.0f;

// The user-pref clamp range is OWNED by the ui.scale registry row -- the
// slider (dev_menu) and this clamp read the same [lo, hi]; no local twin.
// Arc 3: the TYPED HANDLE replaces the latched FindRow lookup (compile-bound,
// never null -- the fallback literals retire with the lookup).
float UserMin() {
    return static_cast<float>(coop::config_registry::rows::ui_scale.row->lo);
}
float UserMax() {
    return static_cast<float>(coop::config_registry::rows::ui_scale.row->hi);
}

// All render-thread only (the Present detour thread), like the rest of ui/.
float g_res = 1.0f;      // quantized resolution factor (height / 1080)
float g_user = 1.25f;    // the user's size pref (ini ui.scale; "побольше" default)
float g_scale = 1.25f;   // published combined factor = min(res * user, cap)
bool  g_rebuild = false;
bool  g_prefLoaded = false;

// VIEWPORT DEBOUNCE (2026-07-28, perf audit of the arc-D2 donor). The rebuild is
// edge-triggered off a CONTINUOUS signal -- the live client height -- and that was
// tolerable only while a bake cost 5-16 ms. It now costs 58-80 ms, on the RENDER
// THREAD inside the Present detour, so the whole game stalls for each one.
//
// Two failures, one root. There are fifteen sixth-boundaries inside [0.5, 3.0], so
// a full-range drag used to fire fifteen rebakes back to back (~1 s of freeze);
// and with no hysteresis, a window edge parked ON a boundary re-baked EVERY FRAME
// forever. Neither is a font problem: the atlas is being asked to follow a value
// that is still moving. So the pending factor has to hold still before it counts.
//
// Counted in FRAMES, not milliseconds, because the caller IS the frame and a
// wall-clock hold would tie the answer to the frame rate it is trying to protect
// ([[lesson-readiness-announcements-precede-visible-state]]). An explicit
// RequestRebuild (a font-family click, a scale-slider release) is NOT debounced --
// those are already discrete user acts and must feel instant.
constexpr int kViewportSettleFrames = 12;
float g_pendingRes   = 0.f;
int   g_pendingHeld  = 0;

void Recombine() {
    float s = g_res * g_user;
    if (s > kCombinedMax) s = kCombinedMax;
    if (s != g_scale) {
        g_scale = s;
        g_rebuild = true;
    }
}

}  // namespace

void NoteViewport(float width, float height) {
    (void)width;  // height drives the factor (widescreen shouldn't inflate text)
    if (height <= 0.f) return;
    float raw = height / kRefHeight;
    if (raw < kResMin) raw = kResMin;
    if (raw > kResMax) raw = kResMax;
    // Quantize to sixths: 720/1080/1440/2160 all land exactly.
    float q = std::round(raw * 6.f) / 6.f;
    // HYSTERESIS. Plain rounding flips at the midpoint, so a height sitting on a
    // boundary alternates on sub-pixel noise. Widen the dead zone around the step
    // we are ALREADY on: leaving it costs 1.5x the normal half-step.
    constexpr float kStep = 1.f / 6.f;
    if (std::fabs(raw - g_res) < kStep * 0.75f) q = g_res;

    if (q == g_res) { g_pendingHeld = 0; return; }   // settled where we already are
    if (q != g_pendingRes) { g_pendingRes = q; g_pendingHeld = 1; return; }
    if (++g_pendingHeld < kViewportSettleFrames) return;   // still moving
    g_pendingHeld = 0;
    g_res = q;
    Recombine();
}

float Ui() { return g_scale; }

float UserScale() { return g_user; }

float UserScaleMin() { return UserMin(); }
float UserScaleMax() { return UserMax(); }

void SetUserScale(float s) {
    if (s < UserMin()) s = UserMin();
    if (s > UserMax()) s = UserMax();
    if (s == g_user) return;
    g_user = s;
    Recombine();
}

void LoadUserPrefOnce() {
    if (g_prefLoaded) return;
    g_prefLoaded = true;
    // Typed registry read (arc 2): garbage OR out-of-[row lo,hi] -> the 1.25
    // default ("побольше") + a T10 sweep row. (The old atof path fed 99 into
    // the clamp -> silently 1.75; out-of-range is garbage now, user ruling.)
    SetUserScale(coop::config::ResolveFloat(coop::config_registry::rows::ui_scale));
}

void RequestRebuild() { g_rebuild = true; }

bool ConsumeRebuild() {
    const bool r = g_rebuild;
    g_rebuild = false;
    return r;
}

}  // namespace ui::scale
