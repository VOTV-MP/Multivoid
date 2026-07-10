// ui/scale.cpp -- see ui/scale.h.

#include "ui/scale.h"

#include "coop/config/config.h"

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
constexpr float kUserMin = 0.75f;
constexpr float kUserMax = 1.75f;
constexpr float kCombinedMax = 4.0f;

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

void SetUserScale(float s) {
    if (s < kUserMin) s = kUserMin;
    if (s > kUserMax) s = kUserMax;
    if (s == g_user) return;
    g_user = s;
    Recombine();
}

void LoadUserPrefOnce() {
    if (g_prefLoaded) return;
    g_prefLoaded = true;
    const std::string v = coop::config::ReadIniValue("ui.scale", "1.25");
    const float s = static_cast<float>(std::atof(v.c_str()));
    if (s > 0.f) SetUserScale(s);
    else Recombine();  // malformed ini value: publish the default combination
}

void RequestRebuild() { g_rebuild = true; }

bool ConsumeRebuild() {
    const bool r = g_rebuild;
    g_rebuild = false;
    return r;
}

}  // namespace ui::scale
