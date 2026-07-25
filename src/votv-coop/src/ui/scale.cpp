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
    // Quantize to sixths: 720/1080/1440/2160 all land exactly, and a windowed
    // drag-resize crosses a step (and re-bakes the atlas) only a few times.
    const float q = std::round(raw * 6.f) / 6.f;
    if (q != g_res) {
        g_res = q;
        Recombine();
    }
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
