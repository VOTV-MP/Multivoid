// harness/autotest_hud_tint.cpp -- SP-solo HUD RED-TINT discriminator.
//
// A red wash over the screen survives a frame whose health is 100, dead 0, quadrants 0, with no
// bloodLoss actors and fade, fog, blendables and grading all clean. The remaining candidate is
// `ui_damageIndicator_C`, a child of `ui_UI_C` that every viewport-widget census missed: its
// `dmg_tunnel` draws a translucent full-screen material every frame, and a cooked material's
// graph is stripped, so whether it is really transparent at alpha 0 cannot be read statically.
//
// The probe settles that by writing the one field that separates the hypotheses and looking at
// the frame: arm A baseline, arm B the whole widget collapsed, arm C only `dmg_tunnel`, arm D
// only `dmg_full`, each logging READY and restoring before the next. B runs first and is
// decisive -- red surviving it excludes the indicator entirely, worth as much as a positive at
// the same cost of one run -- and C and D only narrow a positive B. Every arm logs the state it
// reads, so the screenshots can be re-judged later without re-running it.

#include "harness/autotest.h"

#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;
namespace GT = ue_wrap::game_thread;
namespace E = ue_wrap::engine;
namespace V = ue_wrap::vitals;

// ESlateVisibility. Visible=0 is what the death branch writes (`b0`); Collapsed=1 is
// what `dmg_full` is authored as and what this probe writes to take an image out of
// the draw. Collapsed (not Hidden) so the image also stops occupying layout.
constexpr uint8_t kVisible   = 0;
constexpr uint8_t kCollapsed = 1;

// The seven images of ui_damageIndicator_C, in the order the ubergraph touches them.
// dmg_tunnel and dmg_full are the two this probe is about; the rest are logged so a
// reader can see they were where the disassembly says they were.
const wchar_t* const kImageNames[] = {
    L"dmg_up", L"dmg_down", L"dmg_left", L"dmg_right",
    L"dmg_tunnel", L"dmg_full", L"dmg_heal",
};
constexpr int kImageCount = static_cast<int>(sizeof(kImageNames) / sizeof(kImageNames[0]));
constexpr int kIdxTunnel = 4;
constexpr int kIdxFull   = 5;

struct Verbs {
    bool resolved = false;
    void* setVisibility = nullptr;   // UWidget::SetVisibility
    int32_t offVisibility = -1;      // UWidget::Visibility (byte)
    int32_t offPlayerInterface = -1; // mainGamemode_C.playerInterface (ui_UI_C)
    int32_t offDamageIndicator = -1; // ui_UI_C.umg_damageIndicator (ui_damageIndicator_C)
    int32_t offImage[kImageCount] = {-1, -1, -1, -1, -1, -1, -1};
    int32_t offDmg[4] = {-1, -1, -1, -1};  // damage_up/down/left/right floats
};
Verbs g_v;

bool Resolve() {
    if (g_v.resolved) return true;
    Verbs v;
    void* widgetCls = R::FindClass(P::name::WidgetClass);
    void* gmCls = R::FindClass(P::name::GamemodeClass);
    void* uiCls = R::FindClass(L"ui_UI_C");
    void* dmgCls = R::FindClass(L"ui_damageIndicator_C");
    if (!widgetCls || !gmCls || !uiCls || !dmgCls) return false;

    // Resolve SetVisibility + Visibility off the ENGINE `Widget` class -- the class that
    // DECLARES them. FindFunction does not climb SuperStruct, so asking ui_damageIndicator_C
    // for them returns null (the same trap recorded in death_revive.cpp).
    v.setVisibility = R::FindFunction(widgetCls, P::name::WidgetSetVisibilityFn);
    v.offVisibility = R::FindPropertyOffset(widgetCls, L"Visibility");
    v.offPlayerInterface = R::FindPropertyOffset(gmCls, L"playerInterface");
    v.offDamageIndicator = R::FindPropertyOffset(uiCls, L"umg_damageIndicator");
    for (int i = 0; i < kImageCount; ++i)
        v.offImage[i] = R::FindPropertyOffset(dmgCls, kImageNames[i]);
    v.offDmg[0] = R::FindPropertyOffset(dmgCls, L"damage_up");
    v.offDmg[1] = R::FindPropertyOffset(dmgCls, L"damage_down");
    v.offDmg[2] = R::FindPropertyOffset(dmgCls, L"damage_left");
    v.offDmg[3] = R::FindPropertyOffset(dmgCls, L"damage_right");

    if (!v.setVisibility || v.offVisibility < 0 || v.offPlayerInterface < 0 ||
        v.offDamageIndicator < 0)
        return false;
    for (int i = 0; i < kImageCount; ++i)
        if (v.offImage[i] < 0) return false;

    v.resolved = true;
    g_v = v;
    UE_LOGI("hudtint: verbs resolved (setVis=%p Visibility=0x%X playerInterface=0x%X "
            "umg_damageIndicator=0x%X)",
            v.setVisibility, v.offVisibility, v.offPlayerInterface, v.offDamageIndicator);
    return true;
}

// The live ui_damageIndicator_C instance, via the gamemode's HUD. Returns null until
// the gameplay world has built the HUD.
void* IndicatorGT() {
    void* gm = R::FindObjectByClass(P::name::GamemodeClass);
    if (!gm || !R::IsLive(gm)) return nullptr;
    void* ui = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_v.offPlayerInterface);
    if (!ui || !R::IsLive(ui)) return nullptr;
    void* ind = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ui) + g_v.offDamageIndicator);
    if (!ind || !R::IsLive(ind)) return nullptr;
    return ind;
}

bool SetVisGT(void* widget, uint8_t vis) {
    if (!widget || !R::IsLive(widget)) return false;
    ue_wrap::ParamFrame f(g_v.setVisibility);
    if (!f.valid()) return false;
    f.Set<uint8_t>(L"InVisibility", vis);
    return ue_wrap::Call(widget, f);
}

// Log every image's live Visibility + the player numbers that drive them. This is the
// record that lets the screenshots be re-judged later without another run.
void CensusGT(const char* tag) {
    void* ind = IndicatorGT();
    if (!ind) { UE_LOGW("hudtint: [%s] no live ui_damageIndicator_C", tag); return; }
    uint8_t* base = reinterpret_cast<uint8_t*>(ind);

    const uint8_t indVis = *(base + g_v.offVisibility);
    char line[512];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "hudtint: [%s] indicator vis=%u |", tag, indVis);
    for (int i = 0; i < kImageCount && n > 0; ++i) {
        void* img = *reinterpret_cast<void**>(base + g_v.offImage[i]);
        const int vis = (img && R::IsLive(img))
                            ? static_cast<int>(*(reinterpret_cast<uint8_t*>(img) + g_v.offVisibility))
                            : -1;
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " %ls=%d", kImageNames[i], vis);
    }
    UE_LOGI("%s", line);

    float hp = -1.f, maxHp = -1.f;
    V::Read(V::Field::Health, &hp);
    V::Read(V::Field::MaxHealth, &maxHp);
    float d[4] = {-1.f, -1.f, -1.f, -1.f};
    for (int i = 0; i < 4; ++i)
        if (g_v.offDmg[i] >= 0) d[i] = *reinterpret_cast<float*>(base + g_v.offDmg[i]);
    // tunnel alpha is what the Tick WOULD write this frame, from the same inputs.
    const float tunnelAlpha = (hp >= 0.f) ? (1.0f - hp / 100.0f) : -1.f;
    UE_LOGI("hudtint: [%s] health=%.2f maxHealth=%.2f -> tunnel alpha would be %.3f | "
            "quadrants up=%.2f down=%.2f left=%.2f right=%.2f",
            tag, hp, maxHp, tunnelAlpha, d[0], d[1], d[2], d[3]);
}

// Post `fn` to the game thread and wait for it. false => the task faulted or timed out.
bool RunGT(void (*fn)()) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, fn] { fn(); done->store(1); });
    for (int i = 0; i < 800 && done->load() == 0; ++i) ::Sleep(5);
    return done->load() != 0;
}

// ---- the arms ----------------------------------------------------------------------
// Each is a bare void() so RunGT can post it; they read g_v/IndicatorGT on the game thread.

// The AUTHORED visibilities, captured at the baseline arm and replayed by ArmRestoreAll.
// Restoring to a hardcoded constant is the same mistake this whole investigation is about:
// the first run wrote Visible(0) back over an indicator the game had authored
// HitTestInvisible(3), which would have silently handed the HUD mouse hit-testing it never
// had. Capture what was there; put back exactly that.
int g_origIndicatorVis = -1;
int g_origImageVis[kImageCount] = {-1, -1, -1, -1, -1, -1, -1};

void ArmCensusBaseline() {
    void* ind = IndicatorGT();
    if (ind) {
        uint8_t* base = reinterpret_cast<uint8_t*>(ind);
        g_origIndicatorVis = *(base + g_v.offVisibility);
        for (int i = 0; i < kImageCount; ++i) {
            void* img = *reinterpret_cast<void**>(base + g_v.offImage[i]);
            if (img && R::IsLive(img))
                g_origImageVis[i] = *(reinterpret_cast<uint8_t*>(img) + g_v.offVisibility);
        }
    }
    CensusGT("A-baseline");
}

void ArmCollapseIndicator() {
    void* ind = IndicatorGT();
    UE_LOGI("hudtint: arm B -- collapsing the WHOLE umg_damageIndicator (ok=%d)",
            SetVisGT(ind, kCollapsed) ? 1 : 0);
    CensusGT("B-noindicator");
}

void ArmRestoreIndicatorCollapseTunnel() {
    void* ind = IndicatorGT();
    if (ind && g_origIndicatorVis >= 0) SetVisGT(ind, static_cast<uint8_t>(g_origIndicatorVis));
    uint8_t* base = reinterpret_cast<uint8_t*>(ind);
    void* tunnel = base ? *reinterpret_cast<void**>(base + g_v.offImage[kIdxTunnel]) : nullptr;
    UE_LOGI("hudtint: arm C -- indicator restored, collapsing dmg_tunnel only (ok=%d)",
            SetVisGT(tunnel, kCollapsed) ? 1 : 0);
    CensusGT("C-notunnel");
}

void ArmRestoreTunnelCollapseFull() {
    void* ind = IndicatorGT();
    uint8_t* base = reinterpret_cast<uint8_t*>(ind);
    if (base) {
        if (g_origImageVis[kIdxTunnel] >= 0)
            SetVisGT(*reinterpret_cast<void**>(base + g_v.offImage[kIdxTunnel]),
                     static_cast<uint8_t>(g_origImageVis[kIdxTunnel]));
        void* full = *reinterpret_cast<void**>(base + g_v.offImage[kIdxFull]);
        UE_LOGI("hudtint: arm D -- dmg_tunnel restored, collapsing dmg_full only (ok=%d)",
                SetVisGT(full, kCollapsed) ? 1 : 0);
    }
    CensusGT("D-nofull");
}

// Replay the visibilities captured at the baseline arm -- exactly those, not a guess.
void ArmRestoreAll() {
    void* ind = IndicatorGT();
    if (ind && g_origIndicatorVis >= 0)
        SetVisGT(ind, static_cast<uint8_t>(g_origIndicatorVis));
    uint8_t* base = reinterpret_cast<uint8_t*>(ind);
    if (base) {
        for (int i = 0; i < kImageCount; ++i) {
            if (g_origImageVis[i] < 0) continue;
            void* img = *reinterpret_cast<void**>(base + g_v.offImage[i]);
            SetVisGT(img, static_cast<uint8_t>(g_origImageVis[i]));
        }
    }
    CensusGT("E-restored");
}

}  // namespace

DWORD WINAPI HudTintProbeThread(LPVOID /*arg*/) {
    UE_LOGI("hudtint: probe armed (solo; discriminates the red wash between dmg_tunnel, "
            "dmg_full and 'not the damage indicator at all')");

    // Settle until the gameplay world has a HUD. The classes load with the map, so a
    // failure here is "not yet", not "broken" -- retry quietly, then give up loudly.
    bool ready = false;
    for (int i = 0; i < 120 && !ready; ++i) {
        auto done = std::make_shared<std::atomic<int>>(0);
        auto ok = std::make_shared<std::atomic<int>>(0);
        GT::Post([done, ok] {
            if (Resolve() && IndicatorGT() != nullptr) ok->store(1);
            done->store(1);
        });
        for (int j = 0; j < 200 && done->load() == 0; ++j) ::Sleep(5);
        ready = ok->load() != 0;
        if (!ready) ::Sleep(500);
    }
    if (!ready) {
        UE_LOGE("hudtint: ui_damageIndicator_C never resolved -- probe INCONCLUSIVE "
                "(no verdict; do NOT read the screenshots as evidence)");
        UE_LOGI("hudtint: DONE");
        ue_wrap::log::Flush();
        return 0;
    }

    // SETTLE before the baseline. Readiness here means "the HUD object graph resolved",
    // which on a SAVE load happens while the screen is still black -- the first run of
    // this probe on s_test_screens2 captured a fully black arm-A frame and would have
    // been read as "not red" by a reader comparing PNGs. A baseline that has not rendered
    // the world yet cannot serve as the control for any of the three arms after it.
    ::Sleep(12000);

    // Arm A -- baseline. The screenshot the other three are compared against.
    RunGT(&ArmCensusBaseline);
    UE_LOGI("HUDTINT A-BASELINE READY");
    ue_wrap::log::Flush();
    ::Sleep(2500);

    // Arm B -- the decisive one. Whole widget out of the draw.
    RunGT(&ArmCollapseIndicator);
    UE_LOGI("HUDTINT B-NOINDICATOR READY");
    ue_wrap::log::Flush();
    ::Sleep(2500);

    // Arm C -- narrow a positive B to the always-on full-screen image.
    RunGT(&ArmRestoreIndicatorCollapseTunnel);
    UE_LOGI("HUDTINT C-NOTUNNEL READY");
    ue_wrap::log::Flush();
    ::Sleep(2500);

    // Arm D -- the death latch, which on a live un-dead player should change NOTHING
    // (it is authored Collapsed already). A visible change here would mean something
    // made it Visible without a death, which is itself worth knowing.
    RunGT(&ArmRestoreTunnelCollapseFull);
    UE_LOGI("HUDTINT D-NOFULL READY");
    ue_wrap::log::Flush();
    ::Sleep(2500);

    RunGT(&ArmRestoreAll);
    UE_LOGI("hudtint: DONE -- compare A vs B first: if B is still red the damage "
            "indicator is EXCLUDED and the red is elsewhere; if B is clean, C says "
            "whether dmg_tunnel (always-on, mat_tunnel at alpha=1-health/100) is it");
    ue_wrap::log::Flush();
    return 0;
}

}  // namespace harness::autotest
