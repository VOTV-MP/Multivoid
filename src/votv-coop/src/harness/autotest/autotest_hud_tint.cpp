// harness/autotest_hud_tint.cpp -- SP-solo HUD RED-TINT discriminator.
//
// WHY THIS EXISTS. The user has reported a red wash over the screen across several
// sessions ("красная пелена", "a red filled hud - the damage screen effect"). Two
// hours of targeted probes each measured their own target clean while the user still
// saw red -- the exact shape of [[lesson-a-symptom-needs-a-baseline-before-it-needs-a-
// hypothesis]]. docs/DEATH_ARC.md 11.3a then recorded a live read that EXCLUDES the
// obvious suspects from the pre-hit frame: `health=100.00`, `dead=0`, quadrants
// `0.00`, bloodLoss actors `0` -- and the frame was STILL red, on a New Game, on both
// RHIs, with fade/fog/blendables/grading all read clean. That red is still unexplained.
//
// The 2026-08-31 pak disassembly of `ui_damageIndicator_C` gives a candidate that no
// previous probe could see, because the widget is a CHILD of `ui_UI_C` and every prior
// census enumerated VIEWPORT widgets and never descended:
//
//   * `dmg_tunnel` carries NO `Visibility` property in its export, so it takes
//     UWidget's class default (Visible) and draws `mat_tunnel` -- an MD_UI,
//     BLEND_Translucent material -- FULL SCREEN, EVERY FRAME, FOREVER. Its Tick sets
//     `alpha := 1 - saveSlot.health/100` (divisor is the LITERAL 100). At full health
//     alpha is 0, but a cooked material's graph is STRIPPED, so whether `mat_tunnel`
//     is actually transparent at alpha=0 CANNOT be read statically. That is the whole
//     question this probe answers.
//   * `dmg_full` is authored `Collapsed` and is turned Visible by the Tick's DEATH
//     branch (`@2292 SetVisibility(b0)`) -- which nothing in the game ever undoes,
//     because vanilla always tears the world down. That is a real one-way latch and it
//     is ours now that we keep the world; but it is NOT what a pre-hit `dead=0` frame
//     can be showing, so it is a SEPARATE red from the one above.
//
// So this probe does not "check the damage indicator". It DISCRIMINATES, by writing
// the one field that separates the hypotheses and looking at the resulting frame:
//
//   arm A  baseline                      -> HUDTINT A-BASELINE READY
//   arm B  whole umg_damageIndicator collapsed -> HUDTINT B-NOINDICATOR READY
//   arm C  restore; only dmg_tunnel collapsed  -> HUDTINT C-NOTUNNEL READY
//   arm D  restore; only dmg_full collapsed    -> HUDTINT D-NOFULL READY
//   restore everything, log DONE.
//
// Arm B is the decisive one and it is deliberately FIRST after the baseline: if the
// red survives the whole widget being collapsed, the damage indicator is excluded
// ENTIRELY and the next hunt starts somewhere else -- which is worth exactly as much
// as a positive result and costs the same single run. Arms C/D only narrow a positive B.
//
// The probe also LOGS the live state every arm reads (each image's pointer + its real
// `Visibility` byte, plus health/maxHealth/dead/the four quadrant floats), because the
// numbers are what let a future reader re-judge the screenshots without re-running.
//
// SOLO + role-agnostic; no session, no connection. Gated by env
// VOTVCOOP_RUN_HUD_TINT_PROBE=1; launch `mp.py hudtint` (which captures one PNG per
// arm). Throwaway diagnostic -- NOT a shipping path; it only ever writes `Visibility`
// and it restores every value it touched before it exits.

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
