// harness/autotest_death.cpp -- the native death-chain instrument (VOTVCOOP_RUN_DEATH_TEST), one
// process in two configurations. `mp.py death --session` is a solo host (a session with zero
// clients), the acceptance run: the native death plays out (about 10 s, the black screen at
// +5 s), the level travel is refused at UGameplayStatics::OpenLevel, and the player comes back
// standing at the KPP with the pause menu reachable. `mp.py death` is sessionless, the negative
// control: single player is untouched, so the travel must still happen and the seam refuse
// nothing. Neither needs a second peer. The observation half never fails (the measured
// timeline from a real lethal Add Player Damage to the travel or its refusal, a memory
// profile, the seam's counters); the acceptance half does, asserted in `death_test:` lines and
// never inferred from a module's own log. The hit is the game's own Add Player Damage, so the
// real lethal chain runs; only the trigger is synthetic.

#include "harness/autotest.h"

#include "coop/net/session.h"
#include "coop/config/config.h"
#include "coop/dev/death_write_diff.h"
#include "coop/player/death_revive.h"
#include "coop/player/players_registry.h"
#include "harness/session_runtime.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"

#define PSAPI_VERSION 2   // K32GetProcessMemoryInfo from kernel32 -- no psapi.lib link
#include <psapi.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {

namespace GT = ue_wrap::game_thread;
namespace E = ue_wrap::engine;
namespace V = ue_wrap::vitals;
namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

// The widget the death chain adds at +5 s (Create(blackScreen_C).AddToViewport): the asset
// carries a CanvasPanel, an Image and a brush and no function or ubergraph at all, a static
// full-screen black image that never removes itself. Whoever cancels the travel inherits it.
constexpr const wchar_t* kBlackScreenClass = L"blackScreen_C";

bool WaitDone(const std::shared_ptr<std::atomic<int>>& d, int timeoutMs) {
    for (int i = 0; i < timeoutMs / 5 && d->load() == 0; ++i) ::Sleep(5);
    return d->load() != 0;
}

double RssMb() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!::K32GetProcessMemoryInfo(::GetCurrentProcess(),
                                   reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        return -1.0;
    return static_cast<double>(pmc.WorkingSetSize) / 1048576.0;
}

// One game-thread sample of everything the arms and the timeline read.
struct Sample {
    bool  havePawn = false;
    bool  canRagdoll = true;
    bool  haveCanRagdoll = false;
    bool  isRagdoll = false;
    bool  dead = false;
    bool  haveState = false;
    float health = -1.f;
    bool  blackScreen = false;        // a blackScreen_C OBJECT exists
    // Whether it is on the screen, a different question: RemoveFromParent detaches, so a removed
    // widget is still findable, and "is the black screen gone" must read IsInViewport.
    bool  blackScreenInViewport = false;
    bool  inGameplay = false;   // the live UWorld is still untitled_1 (we did not travel)
    bool  haveWorld = false;
    // Add Player Damage's own early-out terms (gamemode.immortal, isDreaming, dead, startInvinc),
    // so a hit that lands nowhere can name the term.
    bool  startInvinc = false;
    bool  haveStartInvinc = false;
    bool  immortal = false;
    bool  haveImmortal = false;
    // The physics-grabbed actor. The revive's teleport drops the grabbed actor, transforms it and
    // picks it back up; ragdollMode already ran dropGrabObject on the death path, so this should
    // read invalid before a revive teleports, and if it does not, the revive writes someone else's
    // prop.
    bool  grabValid = false;
    bool  haveGrab = false;
    bool  sessionRunning = false;
    // Where the player is. The revive repositions to the coop KPP, and its three-tier fallback
    // reports that a call was dispatched, not that the player moved; the position is the only
    // honest assertion.
    float locX = 0.f, locY = 0.f, locZ = 0.f;
    bool  haveLoc = false;
    // lib.loadLevel's menu prep, read back: pause_mainMenu lives on the screen tree all session, so
    // loadLevel's two writes stick through a cancelled travel and the next ESC shows a loading
    // screen instead of the pause menu.
    int32_t screenSwiIdx = -1;   // in-game value is 1 (ui_menu uber @2445)
    int32_t canvasLoadingVis = -1;  // in-game value is 1 = ESlateVisibility::Collapsed
    // The damage indicator's worst directional accumulator
    // (gamemode.playerInterface.umg_damageIndicator.damage_{up,down,left,right}): a revived player
    // at full health wearing a red screen is death state that outlived the revive.
    float dmgRed = -1.f;
    // dmg_full's live Visibility. Separate from dmgRed: the death branch zeroes the four quadrants
    // and shows dmg_full in the same block, so a reader of the floats alone reports a clean HUD
    // while a full-screen red image is on screen.
    int dmgFullVis = -1;   // ESlateVisibility; 1 = Collapsed = the authored default
    // The second red: Add Player Damage spawns an effect_bloodLoss_C whose post-process and
    // ui_bloodLossBlur wash the whole world red, and a lethal hit pins its duration at the 120 s
    // cap, so it outlives the revive by two minutes unless the revive expires it. Counted as live
    // actors.
    int32_t bloodLossActors = -1;
    float bloodLossTime = -1.f;
    // The effect's own widget: effect_bloodLoss_C removes it in ReceiveDestroyed, so it should die
    // with the actor; counted separately, since an actor gone with the blur still up is the
    // teardown's bug, and both gone with the screen still red is a third source.
    int32_t bloodBlurInViewport = -1;
    double rssMb = -1.0;
};

// An object-pointer property by name off a live object, and whether it points at something live.
bool ReadBpObjectValid(void* obj, const wchar_t* name, bool& outValid) {
    if (!obj || !R::IsLive(obj)) return false;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), name);
    if (off < 0) return false;
    void* target = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(obj) + off);
    outValid = target != nullptr && R::IsLive(target);
    return true;
}

// A BP bool by name off a live object (byte and mask).
bool ReadBpBool(void* obj, const wchar_t* name, bool& out) {
    if (!obj || !R::IsLive(obj)) return false;
    int32_t byteOff = -1; uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(obj), name, byteOff, mask)) return false;
    out = (*(reinterpret_cast<uint8_t*>(obj) + byteOff) & mask) != 0;
    return true;
}

// The worst of the damage indicator's four quadrant accumulators, or -1 if the chain does not
// resolve; the same chain the game's own damage path writes.
float ReadDamageRed() {
    void* gm = R::FindObjectByClass(P::name::GamemodeClass);
    if (!gm || !R::IsLive(gm)) return -1.f;
    const int32_t offUi = R::FindPropertyOffset(R::ClassOf(gm), L"playerInterface");
    if (offUi < 0) return -1.f;
    void* ui = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(gm) + offUi);
    if (!ui || !R::IsLive(ui)) return -1.f;
    const int32_t offInd = R::FindPropertyOffset(R::ClassOf(ui), L"umg_damageIndicator");
    if (offInd < 0) return -1.f;
    void* ind = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(ui) + offInd);
    if (!ind || !R::IsLive(ind)) return -1.f;
    void* cls = R::ClassOf(ind);
    float worst = 0.f;
    for (const wchar_t* n : {L"damage_up", L"damage_down", L"damage_left", L"damage_right"}) {
        const int32_t off = R::FindPropertyOffset(cls, n);
        if (off < 0) return -1.f;
        const float v = *reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(ind) + off);
        if (v > worst) worst = v;
    }
    return worst;
}

// Live effect_bloodLoss_C actors and the worst remaining `time`; -1 / -1 means the class is not
// loaded (no bloodLoss this session), a legitimate pre-hit state.
void ReadBloodLoss(int32_t& outCount, float& outWorst) {
    void* cls = R::FindClass(L"effect_bloodLoss_C");
    if (!cls) { outCount = -1; outWorst = -1.f; return; }
    const int32_t offTime = R::FindPropertyOffset(cls, L"time");
    outCount = 0; outWorst = 0.f;
    for (void* a : R::FindObjectsByClass(L"effect_bloodLoss_C")) {
        if (!a || !R::IsLive(a)) continue;
        ++outCount;
        if (offTime >= 0) {
            const float t = *reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(a) + offTime);
            if (t > outWorst) outWorst = t;
        }
    }
}

// Live ui_bloodLossBlur_C widgets on the viewport; -1 means the class is not loaded.
int32_t ReadBloodBlurInViewport() {
    if (!R::FindClass(L"ui_bloodLossBlur_C")) return -1;
    void* userWidgetCls = R::FindClass(P::name::UserWidgetClass);
    void* fnInView = userWidgetCls ? R::FindFunction(userWidgetCls, L"IsInViewport") : nullptr;
    int32_t n = 0;
    for (void* w : R::FindObjectsByClass(L"ui_bloodLossBlur_C")) {
        if (!w || !R::IsLive(w) || !fnInView) continue;
        ue_wrap::ParamFrame f(fnInView);
        if (f.valid() && ue_wrap::Call(w, f) && f.Get<bool>(L"ReturnValue")) ++n;
    }
    return n;
}

// Every UUserWidget-descended object on the viewport, by class name: a probe aimed at a suspect
// cannot find a source nobody thought of, an enumeration can. One array walk, once per run.
std::wstring CensusViewportWidgets() {
    void* userWidgetCls = R::FindClass(P::name::UserWidgetClass);
    if (!userWidgetCls) return L"(UserWidget class unresolved)";
    void* fnInView = R::FindFunction(userWidgetCls, L"IsInViewport");
    if (!fnInView) return L"(IsInViewport unresolved)";
    std::wstring out;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o)) continue;
        void* cls = R::ClassOf(o);
        if (!cls || !R::IsDescendantOfAny(cls, &userWidgetCls, 1)) continue;
        ue_wrap::ParamFrame f(fnInView);
        if (!f.valid() || !ue_wrap::Call(o, f) || !f.Get<bool>(L"ReturnValue")) continue;
        if (!out.empty()) out += L", ";
        out += R::ToString(R::NameOf(cls));
    }
    return out.empty() ? L"(none)" : out;
}

// dmg_full's live Visibility, -1 when unresolved (never a failure: an unresolvable offset is not
// a claim about the game's state). The Tick's death branch sets it Visible while the widget's
// export authors it Collapsed, and the alive path never writes the field: a one-way latch only
// the revive clears, so asserting it makes the fix a regression test.
int ReadDmgFullVisibility() {
    void* gm = R::FindObjectByClass(P::name::GamemodeClass);
    if (!gm || !R::IsLive(gm)) return -1;
    void* uiCls = R::FindClass(L"ui_UI_C");
    void* dmgCls = R::FindClass(L"ui_damageIndicator_C");
    void* wCls = R::FindClass(P::name::WidgetClass);
    if (!uiCls || !dmgCls || !wCls) return -1;
    const int32_t oPI = R::FindPropertyOffset(R::FindClass(P::name::GamemodeClass), L"playerInterface");
    const int32_t oDI = R::FindPropertyOffset(uiCls, L"umg_damageIndicator");
    const int32_t oFull = R::FindPropertyOffset(dmgCls, L"dmg_full");
    const int32_t oVis = R::FindPropertyOffset(wCls, L"Visibility");
    if (oPI < 0 || oDI < 0 || oFull < 0 || oVis < 0) return -1;
    void* ui = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + oPI);
    if (!ui || !R::IsLive(ui)) return -1;
    void* ind = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ui) + oDI);
    if (!ind || !R::IsLive(ind)) return -1;
    void* full = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ind) + oFull);
    if (!full || !R::IsLive(full)) return -1;
    return static_cast<int>(*(reinterpret_cast<uint8_t*>(full) + oVis));
}

// Every live ui_damageIndicator_C instance with its four quadrant values, and which one the
// revive's path (gamemode.playerInterface.umg_damageIndicator) points at: a reader of that one
// object can report 0.00 honestly while a different instance is the one on screen.
std::wstring CensusDamageIndicators() {
    void* cls = R::FindClass(L"ui_damageIndicator_C");
    if (!cls) return L"(class unresolved)";
    const int32_t oU = R::FindPropertyOffset(cls, L"damage_up");
    const int32_t oD = R::FindPropertyOffset(cls, L"damage_down");
    const int32_t oL = R::FindPropertyOffset(cls, L"damage_left");
    const int32_t oR = R::FindPropertyOffset(cls, L"damage_right");
    if (oU < 0 || oD < 0 || oL < 0 || oR < 0) return L"(offsets unresolved)";
    // The one the revive's path points at.
    void* target = nullptr;
    if (void* gm = R::FindObjectByClass(P::name::GamemodeClass)) {
        const int32_t offUi = R::FindPropertyOffset(R::ClassOf(gm), L"playerInterface");
        if (offUi >= 0) {
            void* ui = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(gm) + offUi);
            if (ui && R::IsLive(ui)) {
                const int32_t offInd = R::FindPropertyOffset(R::ClassOf(ui), L"umg_damageIndicator");
                if (offInd >= 0)
                    target = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(ui) + offInd);
            }
        }
    }
    std::wstring out;
    int n = 0;
    for (void* o : R::FindObjectsByClass(L"ui_damageIndicator_C")) {
        if (!o || !R::IsLive(o)) continue;
        ++n;
        auto* b = reinterpret_cast<uint8_t*>(o);
        wchar_t buf[192];
        _snwprintf_s(buf, _TRUNCATE, L" #%d%ls u=%.2f d=%.2f l=%.2f r=%.2f", n,
                     (o == target ? L"(THE ONE WE WRITE)" : L"(NOT ours)"),
                     *reinterpret_cast<float*>(b + oU), *reinterpret_cast<float*>(b + oD),
                     *reinterpret_cast<float*>(b + oL), *reinterpret_cast<float*>(b + oR));
        out += buf;
    }
    // The world-side red sources too: a live redSkyEvent_C, weatherFogController_C or blackFog_C
    // tints the whole scene and has nothing to do with the death.
    for (const wchar_t* c : {L"redSkyEvent_C", L"weatherFogController_C", L"blackFog_C"}) {
        int live = 0;
        for (void* o : R::FindObjectsByClass(c)) if (o && R::IsLive(o)) ++live;
        if (live) { out += L" | WORLD "; out += c; out += L"=" + std::to_wstring(live); }
    }
    return L"instances=" + std::to_wstring(n) + out;
}

// Blendable materials on one FPostProcessSettings, by name and weight: a post-process material is
// the only thing that tints the sky, the stars and the near ground by the same amount while
// leaving UMG alone.
std::wstring BlendablesOf(void* owner, const wchar_t* settingsProp, const wchar_t* label) {
    if (!owner || !R::IsLive(owner)) return L"";
    void* cls = R::ClassOf(owner);
    const int32_t oS = R::FindPropertyOffset(cls, settingsProp);
    void* st = R::PropertyInnerStruct(cls, settingsProp);
    if (oS < 0 || !st) return L"";
    const int32_t oWB = R::FindPropertyOffset(st, L"WeightedBlendables");
    if (oWB < 0) return L"";
    auto* arr = reinterpret_cast<uint8_t*>(owner) + oS + oWB;
    void* data = *reinterpret_cast<void**>(arr);
    const int32_t num = *reinterpret_cast<int32_t*>(arr + 8);
    if (num <= 0) return L"";
    std::wstring out = std::wstring(L" | ") + label + L".blend[";
    for (int32_t i = 0; i < num && i < 8 && data; ++i) {
        auto* el = reinterpret_cast<uint8_t*>(data) + static_cast<size_t>(i) * 16;
        const float w = *reinterpret_cast<float*>(el);
        void* obj = *reinterpret_cast<void**>(el + 8);
        wchar_t b[160];
        _snwprintf_s(b, _TRUNCATE, L"%ls w=%.2f%ls", (i ? L", " : L""), w,
                     (obj && R::IsLive(obj)) ? R::ToString(R::NameOf(obj)).c_str() : L"<null>");
        out += b;
    }
    return out + L"]";
}

// The render state itself, what can tint the scene while leaving UMG untouched. In UE4 four
// things do: a camera fade, a post-process component on the camera, player or gamemode, a
// PostProcessVolume, or scene lighting; the first three are readable by name.
std::wstring CensusRenderState() {
    std::wstring out;
    wchar_t buf[256];

    // 1. The camera fade (SetManualCameraFade leaves these set).
    if (void* pcm = R::FindObjectByClass(L"PlayerCameraManager")) {
        if (R::IsLive(pcm)) {
            void* cls = R::ClassOf(pcm);
            const int32_t oAmt = R::FindPropertyOffset(cls, L"FadeAmount");
            const int32_t oCol = R::FindPropertyOffset(cls, L"FadeColor");
            int32_t oEnB = -1; uint8_t oEnM = 0;
            R::FindBoolProperty(cls, L"bEnableFading", oEnB, oEnM);
            auto* b = reinterpret_cast<uint8_t*>(pcm);
            const float amt = oAmt >= 0 ? *reinterpret_cast<float*>(b + oAmt) : -1.f;
            const bool en = (oEnB >= 0) && ((*(b + oEnB) & oEnM) != 0);
            float r = -1, g = -1, bl = -1;
            if (oCol >= 0) {
                auto* c = reinterpret_cast<float*>(b + oCol);
                r = c[0]; g = c[1]; bl = c[2];
            }
            _snwprintf_s(buf, _TRUNCATE, L"CameraFade{enabled=%d amount=%.2f color=(%.2f,%.2f,%.2f)}",
                         en ? 1 : 0, amt, r, g, bl);
            out += buf;
        }
    }

    // 2/3. The post-process components, by owner.
    auto pp = [&](const wchar_t* ownerCls, const wchar_t* propName) {
        void* o = R::FindObjectByClass(ownerCls);
        if (!o || !R::IsLive(o)) return;
        const int32_t off = R::FindPropertyOffset(R::ClassOf(o), propName);
        if (off < 0) return;
        void* comp = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(o) + off);
        if (!comp || !R::IsLive(comp)) return;
        void* ccls = R::ClassOf(comp);
        const int32_t oW = R::FindPropertyOffset(ccls, L"BlendWeight");
        int32_t oEB = -1; uint8_t oEM = 0;
        R::FindBoolProperty(ccls, L"bEnabled", oEB, oEM);
        auto* cb = reinterpret_cast<uint8_t*>(comp);
        _snwprintf_s(buf, _TRUNCATE, L" | %ls.%ls{enabled=%d weight=%.2f}", ownerCls, propName,
                     (oEB >= 0) ? (((*(cb + oEB)) & oEM) ? 1 : 0) : -1,
                     oW >= 0 ? *reinterpret_cast<float*>(cb + oW) : -1.f);
        out += buf;
    };
    pp(P::name::MainPlayerClass, L"PostProcess_pl");
    pp(P::name::MainPlayerClass, L"PostProcess");
    pp(P::name::GamemodeClass, L"PostProcess");

    // 3b. What is in the player's post-process: enabled and weight say a component is live, not
    // what it does; WeightedBlendables names an injected material, and the colour-grading overrides
    // say whether the scene was graded instead.
    {
        void* mp2 = R::FindObjectByClass(P::name::MainPlayerClass);
        if (mp2 && R::IsLive(mp2)) {
            const int32_t offC = R::FindPropertyOffset(R::ClassOf(mp2), L"PostProcess_pl");
            if (offC >= 0) {
                void* comp = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(mp2) + offC);
                if (comp && R::IsLive(comp)) {
                    void* ccls = R::ClassOf(comp);
                    const int32_t offS = R::FindPropertyOffset(ccls, L"Settings");
                    void* st = R::PropertyInnerStruct(ccls, L"Settings");
                    if (offS >= 0 && st) {
                        auto* sb = reinterpret_cast<uint8_t*>(comp) + offS;
                        const int32_t oWB = R::FindPropertyOffset(st, L"WeightedBlendables");
                        if (oWB >= 0) {
                            // FWeightedBlendables is a TArray of FWeightedBlendable {float Weight;
                            // UObject* Object}, 16 B each; the material is named, since "there is
                            // one" is not an identification.
                            auto* arr = reinterpret_cast<uint8_t*>(sb + oWB);
                            void* data = *reinterpret_cast<void**>(arr);
                            const int32_t num = *reinterpret_cast<int32_t*>(arr + 8);
                            _snwprintf_s(buf, _TRUNCATE, L" | PostProcess_pl.Blendables.Num=%d", num);
                            out += buf;
                            for (int32_t i = 0; i < num && i < 8 && data; ++i) {
                                auto* el = reinterpret_cast<uint8_t*>(data) + static_cast<size_t>(i) * 16;
                                const float w = *reinterpret_cast<float*>(el);
                                void* obj = *reinterpret_cast<void**>(el + 8);
                                out += L" [w=" + std::to_wstring(w) + L" ";
                                out += (obj && R::IsLive(obj))
                                           ? (R::ToString(R::NameOf(obj)) + L" : " + R::ClassNameOf(obj))
                                           : std::wstring(L"<null/dead>");
                                out += L"]";
                            }
                        }
                        // Any colour override that is on is named.
                        for (const auto& f : R::EnumerateStructFields(st)) {
                            if (f.name.rfind(L"bOverride_", 0) != 0) continue;
                            if (f.name.find(L"Color") == std::wstring::npos &&
                                f.name.find(L"Scene") == std::wstring::npos &&
                                f.name.find(L"Tint") == std::wstring::npos) continue;
                            int32_t bo = -1; uint8_t bm = 0;
                            if (!R::FindBoolProperty(st, f.name.c_str(), bo, bm)) continue;
                            if ((*(sb + bo) & bm) == 0) continue;
                            out += L" | ON:" + f.name;
                        }
                    }
                }
            }
        }
    }

    // 3c. The fog: a distance-dependent tint (near grass green, distant trees red) is fog
    // inscattering, not a uniform post-process; daynightCycle owns the inscattering colour and
    // density, so this reads the value they land on.
    for (void* fog : R::FindObjectsByClass(L"ExponentialHeightFogComponent")) {
        if (!fog || !R::IsLive(fog)) continue;
        void* fc = R::ClassOf(fog);
        const int32_t oCol = R::FindPropertyOffset(fc, L"FogInscatteringColor");
        const int32_t oDen = R::FindPropertyOffset(fc, L"FogDensity");
        auto* fb = reinterpret_cast<uint8_t*>(fog);
        float r = -1, g = -1, b2 = -1;
        if (oCol >= 0) { auto* c = reinterpret_cast<float*>(fb + oCol); r = c[0]; g = c[1]; b2 = c[2]; }
        _snwprintf_s(buf, _TRUNCATE, L" | FOG{inscatter=(%.3f,%.3f,%.3f) density=%.4f}",
                     r, g, b2, oDen >= 0 ? *reinterpret_cast<float*>(fb + oDen) : -1.f);
        out += buf;
    }

    // 3d. Blendables on every source, not just the player's.
    if (void* gm2 = R::FindObjectByClass(P::name::GamemodeClass)) {
        const int32_t o = R::FindPropertyOffset(R::ClassOf(gm2), L"PostProcess");
        if (o >= 0) {
            void* c = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(gm2) + o);
            out += BlendablesOf(c, L"Settings", L"gm.PostProcess");
        }
    }
    for (void* v2 : R::FindObjectsByClass(L"PostProcessVolume"))
        out += BlendablesOf(v2, L"Settings", R::ToString(R::NameOf(v2)).c_str());
    // And the camera's own settings, the last stop before the frame.
    if (void* mp3 = R::FindObjectByClass(P::name::MainPlayerClass)) {
        const int32_t o = R::FindPropertyOffset(R::ClassOf(mp3), L"Camera");
        if (o >= 0) {
            void* cam = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(mp3) + o);
            out += BlendablesOf(cam, L"PostProcessSettings", L"Camera");
        }
    }

    // 3e. Which branch lit the Bad Sun: daynightCycle's new-day block spawns it every day when the
    // game instance's mode is b7, otherwise on a 0.1%-per-day roll behind the badsun achievement.
    if (void* gi = R::FindObjectByClass(P::name::GameInstanceClass)) {
        if (R::IsLive(gi)) {
            const uint8_t mode = *(reinterpret_cast<uint8_t*>(gi) + P::off::mainGameInstance_GameMode);
            _snwprintf_s(buf, _TRUNCATE, L" | GameInstance.gamemode=b%u%ls", (unsigned)mode,
                         mode == 7 ? L" (== b7: Bad Sun spawns EVERY new day)" : L"");
            out += buf;
        }
    }

    // 4. Every PostProcessVolume with a non-zero blend.
    int vols = 0, hot = 0;
    for (void* v : R::FindObjectsByClass(L"PostProcessVolume")) {
        if (!v || !R::IsLive(v)) continue;
        ++vols;
        const int32_t oW = R::FindPropertyOffset(R::ClassOf(v), L"BlendWeight");
        if (oW >= 0 && *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(v) + oW) > 0.01f) ++hot;
    }
    _snwprintf_s(buf, _TRUNCATE, L" | PostProcessVolumes=%d (blend>0: %d)", vols, hot);
    out += buf;
    for (void* v : R::FindObjectsByClass(L"PostProcessVolume")) {
        if (!v || !R::IsLive(v)) continue;
        void* vc = R::ClassOf(v);
        const int32_t oW = R::FindPropertyOffset(vc, L"BlendWeight");
        int32_t oUB = -1; uint8_t oUM = 0;
        R::FindBoolProperty(vc, L"bUnbound", oUB, oUM);
        auto* vb = reinterpret_cast<uint8_t*>(v);
        _snwprintf_s(buf, _TRUNCATE, L" [%ls w=%.2f unbound=%d", R::ToString(R::NameOf(v)).c_str(),
                     oW >= 0 ? *reinterpret_cast<float*>(vb + oW) : -1.f,
                     (oUB >= 0) ? (((*(vb + oUB)) & oUM) ? 1 : 0) : -1);
        out += buf;
        // The colour grading: the weight says a volume contributes, only these say what, and a
        // scene-wide uniform red with the UI untouched is what a graded volume looks like.
        const int32_t oS = R::FindPropertyOffset(vc, L"Settings");
        void* vst = R::PropertyInnerStruct(vc, L"Settings");
        if (oS >= 0 && vst) {
            auto* sb2 = reinterpret_cast<uint8_t*>(v) + oS;
            for (const wchar_t* f : {L"ColorGain", L"ColorOffset", L"ColorSaturation",
                                     L"ColorContrast", L"ColorGamma"}) {
                int32_t bo = -1; uint8_t bm = 0;
                std::wstring ov = L"bOverride_"; ov += f;
                const bool on = R::FindBoolProperty(vst, ov.c_str(), bo, bm) &&
                                ((*(sb2 + bo) & bm) != 0);
                if (!on) continue;
                const int32_t off2 = R::FindPropertyOffset(vst, f);
                if (off2 < 0) continue;
                auto* c = reinterpret_cast<float*>(sb2 + off2);  // FVector4
                _snwprintf_s(buf, _TRUNCATE, L" %ls=(%.3f,%.3f,%.3f,%.3f)", f, c[0], c[1], c[2], c[3]);
                out += buf;
            }
        }
        out += L"]";
    }
    return out.empty() ? L"(nothing readable)" : out;
}

// Every live actor descending from effect_C, by class name, plus the gamemode's own effects_names
// array: the two halves of VOTV's effect system, which can disagree. effect_C's base carries a
// PostProcessComponent, so a world-only tint is one of these or a stale gamemode row.
std::wstring CensusEffects() {
    std::wstring out;
    void* effectCls = R::FindClass(L"effect_C");
    if (effectCls) {
        const int32_t n = R::NumObjects();
        for (int32_t i = 0; i < n; ++i) {
            void* o = R::ObjectAt(i);
            if (!o || !R::IsLive(o)) continue;
            void* cls = R::ClassOf(o);
            if (!cls || !R::IsDescendantOfAny(cls, &effectCls, 1)) continue;
            std::wstring nm = R::ToString(R::NameOf(cls));
            if (nm.rfind(L"Default__", 0) == 0) continue;  // CDOs are not on screen
            if (!out.empty()) out += L", ";
            out += L"actor:" + nm;
        }
    } else {
        out += L"(effect_C unresolved)";
    }
    // The gamemode's parallel bookkeeping.
    if (void* gm = R::FindObjectByClass(P::name::GamemodeClass)) {
        if (R::IsLive(gm)) {
            const int32_t off = R::FindPropertyOffset(R::ClassOf(gm), L"effects_names");
            if (off >= 0) {
                struct FNameArr { void* data; int32_t num; int32_t max; };
                auto* a = reinterpret_cast<FNameArr*>(reinterpret_cast<uint8_t*>(gm) + off);
                out += L" | gamemode.effects_names.Num=" + std::to_wstring(a->num);
                // FName is 8 bytes {ComparisonIndex, Number}; each is rendered through the engine's
                // own ToString, so a stale row is named, not counted.
                for (int32_t i = 0; i < a->num && i < 16 && a->data; ++i) {
                    const auto& fn = *reinterpret_cast<const R::FName*>(
                        reinterpret_cast<uint8_t*>(a->data) + static_cast<size_t>(i) * 8);
                    out += L" [" + R::ToString(fn) + L"]";
                }
            } else {
                out += L" | (effects_names offset unresolved)";
            }
        }
    }
    return out.empty() ? L"(none)" : out;
}

// gamemode -> pause_mainMenu -> {canvas_loading, screenSwi}: the two values lib.loadLevel
// stomps on the way to the menu. Every engine verb resolves off its declaring class, since
// FindFunction is exact-owner and does not climb SuperStruct. The mod's own restore is in
// coop/player/death_revive.cpp.
void ReadMenuPrep(int32_t& outSwiIdx, int32_t& outCanvasVis) {
    void* gm = R::FindObjectByClass(P::name::GamemodeClass);
    if (!gm || !R::IsLive(gm)) return;
    const int32_t offMenu = R::FindPropertyOffset(R::ClassOf(gm), L"pause_mainMenu");
    if (offMenu < 0) return;
    void* menu = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(gm) + offMenu);
    if (!menu || !R::IsLive(menu)) return;
    void* menuCls = R::ClassOf(menu);
    const int32_t offCanvas = R::FindPropertyOffset(menuCls, L"canvas_loading");
    const int32_t offSwi = R::FindPropertyOffset(menuCls, L"screenSwi");
    auto* bytes = reinterpret_cast<uint8_t*>(menu);
    if (offSwi >= 0) {
        void* swi = *reinterpret_cast<void* const*>(bytes + offSwi);
        void* cls = R::FindClass(L"WidgetSwitcher");
        void* fn = cls ? R::FindFunction(cls, L"GetActiveWidgetIndex") : nullptr;
        if (swi && fn && R::IsLive(swi)) {
            ue_wrap::ParamFrame f(fn);
            if (f.valid() && ue_wrap::Call(swi, f)) outSwiIdx = f.Get<int32_t>(L"ReturnValue");
        }
    }
    if (offCanvas >= 0) {
        void* canvas = *reinterpret_cast<void* const*>(bytes + offCanvas);
        void* cls = R::FindClass(P::name::WidgetClass);
        void* fn = cls ? R::FindFunction(cls, L"GetVisibility") : nullptr;
        if (canvas && fn && R::IsLive(canvas)) {
            ue_wrap::ParamFrame f(fn);
            if (f.valid() && ue_wrap::Call(canvas, f))
                outCanvasVis = static_cast<int32_t>(f.Get<uint8_t>(L"ReturnValue"));
        }
    }
}

Sample Probe() {
    auto done = std::make_shared<std::atomic<int>>(0);
    auto out = std::make_shared<Sample>();
    GT::Post([done, out] {
        void* mp = coop::players::Registry::Get().Local();
        if (mp && R::IsLive(mp)) {
            out->havePawn = true;
            out->haveCanRagdoll = E::ReadMainPlayerCanRagdoll(mp, out->canRagdoll);
            out->haveState = E::ReadMainPlayerRagdollState(mp, out->isRagdoll, out->dead);
            out->haveStartInvinc = ReadBpBool(mp, L"startInvinc", out->startInvinc);
            out->haveGrab = ReadBpObjectValid(mp, L"grabbing_actor", out->grabValid);
            const ue_wrap::FVector at = E::GetActorLocation(mp);
            out->locX = at.X; out->locY = at.Y; out->locZ = at.Z;
            out->haveLoc = true;
        }
        ReadMenuPrep(out->screenSwiIdx, out->canvasLoadingVis);
        out->dmgRed = ReadDamageRed();
        out->dmgFullVis = ReadDmgFullVisibility();
        ReadBloodLoss(out->bloodLossActors, out->bloodLossTime);
        out->bloodBlurInViewport = ReadBloodBlurInViewport();
        if (void* gm = R::FindObjectByClass(P::name::GamemodeClass))
            out->haveImmortal = ReadBpBool(gm, L"immortal", out->immortal);
        float hp = -1.f;
        if (V::Read(V::Field::Health, &hp)) out->health = hp;
        if (void* bs = R::FindObjectByClass(kBlackScreenClass)) {
            out->blackScreen = true;
            void* userWidgetCls = R::FindClass(P::name::UserWidgetClass);
            void* fnInView = userWidgetCls ? R::FindFunction(userWidgetCls, L"IsInViewport") : nullptr;
            if (fnInView && R::IsLive(bs)) {
                ue_wrap::ParamFrame f(fnInView);
                if (f.valid() && ue_wrap::Call(bs, f))
                    out->blackScreenInViewport = f.Get<bool>(L"ReturnValue");
            }
        }
        // The gameplay world's leaf name contains "ntitled" (untitled_1.Untitled_1); the menu and
        // loading worlds do not.
        if (void* w = R::FindObjectByClass(P::name::WorldClass)) {
            out->haveWorld = true;
            out->inGameplay = R::ToString(R::NameOf(w)).find(L"ntitled") != std::wstring::npos;
        }
        done->store(1);
    });
    WaitDone(done, 8000);
    out->rssMb = RssMb();
    out->sessionRunning = harness::session_runtime::Session().running();
    return *out;
}

int g_pass = 0, g_fail = 0;

void Verdict(const char* arm, bool ok, const char* detail) {
    if (ok) { ++g_pass; UE_LOGI("death_test: %s PASS -- %s", arm, detail); }
    else    { ++g_fail; UE_LOGW("death_test: %s FAIL -- %s", arm, detail); }
}

// A window's memory profile: first and last RSS and the slope between them.
struct MemWindow {
    double firstMb = -1.0, lastMb = -1.0, peakMb = -1.0;
    uint64_t ms = 0;
    double SlopeMbPerSec() const {
        if (firstMb < 0 || lastMb < 0 || ms == 0) return 0.0;
        return (lastMb - firstMb) * 1000.0 / static_cast<double>(ms);
    }
    void Add(double mb) {
        if (mb < 0) return;
        if (firstMb < 0) firstMb = mb;
        lastMb = mb;
        if (mb > peakMb) peakMb = mb;
    }
};

// How far the dead window's RSS slope may exceed the alive window's before it is a balloon: an
// order of magnitude under the inherited ~165 MB/s claim and an order over normal streaming
// churn, so neither answer is a coin flip.
constexpr double kBalloonMbPerSec = 20.0;

// How long to watch, alive and then dead; the dead window outlasts the chain's own 10 s so the
// travel or its absence is inside the observation.
constexpr int kAliveWindowMs = 10000;
constexpr int kDeadWindowMs  = 22000;
// The write-diff's noise floor must cover the whole span it grades: the dead window plus the hit
// post, the loop's overrun and the diff latency, so longer than kDeadWindowMs.
constexpr int kNoiseFloorWindowMs = 26000;
constexpr int kSampleMs      = 250;

}  // namespace

DWORD WINAPI DeathTestThread(LPVOID) {
    UE_LOGI("death_test: armed -- a LETHAL Add Player Damage runs VOTV's native death "
            "chain to completion; the timeline + memory are OBSERVED, and the "
            "docs/DEATH_ARC.md contract is the ACCEPTANCE half");

    // A pawn that can be killed: canRagdoll true, no invincibility term set, in the gameplay world.
    Sample s;
    bool ready = false;
    for (int i = 0; i < 120 && !ready; ++i) {
        s = Probe();
        ready = s.havePawn && s.haveState && s.haveCanRagdoll && s.health > 0.f &&
                s.canRagdoll && !s.dead && !s.startInvinc && !s.immortal && s.inGameplay;
        if (!ready) ::Sleep(1000);
    }
    UE_LOGI("death_test: pre-hit state -- havePawn=%d canRagdoll=%d(read=%d) health=%.2f "
            "startInvinc=%d(read=%d) immortal=%d(read=%d) dead=%d inGameplay=%d "
            "sessionRunning=%d grabValid=%d(read=%d) rss=%.1f MB",
            s.havePawn ? 1 : 0, s.canRagdoll ? 1 : 0, s.haveCanRagdoll ? 1 : 0, s.health,
            s.startInvinc ? 1 : 0, s.haveStartInvinc ? 1 : 0,
            s.immortal ? 1 : 0, s.haveImmortal ? 1 : 0, s.dead ? 1 : 0, s.inGameplay ? 1 : 0,
            s.sessionRunning ? 1 : 0, s.grabValid ? 1 : 0, s.haveGrab ? 1 : 0, s.rssMb);
    if (!ready) {
        UE_LOGW("death_test: VERDICT INCONCLUSIVE -- preconditions never met (see the pre-hit "
                "state line above; a held canRagdoll, a set startInvinc/immortal, or no "
                "gameplay world would each swallow the run)");
        UE_LOGI("death_test: DONE");
        return 0;
    }

    // The alive control window: the same cadence, reads and frame load with the player standing
    // still; whatever drifts here the dead window may drift. The write-diff's noise floor rides it,
    // since whatever moves while the player stands (animated hints, radar points, pooled log rows,
    // ticking floats) is world churn, never death; without it the death diff reads about 300
    // changed cells and buries its own signal.
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        GT::Post([done] {
            coop::dev::death_write_diff::ResetNoiseFloor();
            coop::dev::death_write_diff::Snapshot();
            done->store(1);
        });
        WaitDone(done, 30000);
    }

    MemWindow alive;
    {
        const uint64_t t0 = ::GetTickCount64();
        for (uint64_t now = t0; now - t0 < static_cast<uint64_t>(kAliveWindowMs);
             now = ::GetTickCount64()) {
            alive.Add(Probe().rssMb);
            ::Sleep(kSampleMs);
        }
        alive.ms = ::GetTickCount64() - t0;
    }
    UE_LOGI("death_test: ALIVE control window -- %.1f -> %.1f MB over %llu ms (%.2f MB/s, peak %.1f)",
            alive.firstMb, alive.lastMb, static_cast<unsigned long long>(alive.ms),
            alive.SlopeMbPerSec(), alive.peakMb);

    {
        auto done = std::make_shared<std::atomic<int>>(0);
        auto n = std::make_shared<int>(0);
        GT::Post([done, n] {
            *n = coop::dev::death_write_diff::DiffAndLog("alive control 1", /*learnNoise=*/true);
            done->store(1);
        });
        WaitDone(done, 30000);
        if (*n < 0) UE_LOGW("death_test: write-diff floor pass 1 did NOT run (see death_diff)");
    }

    // A second, longer control stretch, off by default. The floor must cover at least as long as
    // the span it grades (the 22 s dead window plus the hit post and the diff latency), or a cell
    // with a period inside that gap (an autosave timer, a world clock, an NPC state machine) is
    // reported as death-attributable forever; coverage is the longest single stretch, not a sum. It
    // costs 26 s of the player standing still per run, so the default run pays nothing and names
    // the residual (periods between about 10 s and the graded span are uncovered); `mp.py death
    // --deep-floor` buys the coverage when the residual is being classified.
    const bool deepFloor = (coop::config::ReadEnv("VOTVCOOP_DEATH_DEEP_FLOOR") == "1");
    if (deepFloor) {
        auto done = std::make_shared<std::atomic<int>>(0);
        GT::Post([done] { coop::dev::death_write_diff::Snapshot(); done->store(1); });
        WaitDone(done, 30000);
        const uint64_t t0 = ::GetTickCount64();
        while (::GetTickCount64() - t0 < static_cast<uint64_t>(kNoiseFloorWindowMs))
            ::Sleep(kSampleMs);
        auto done2 = std::make_shared<std::atomic<int>>(0);
        auto n = std::make_shared<int>(0);
        GT::Post([done2, n] {
            *n = coop::dev::death_write_diff::DiffAndLog("alive control 2", /*learnNoise=*/true);
            done2->store(1);
        });
        WaitDone(done2, 30000);
        if (*n < 0) UE_LOGW("death_test: write-diff floor pass 2 did NOT run (see death_diff)");
    }

    // The write-diff's before instant, here and not on the `dead` edge: Add Player Damage writes
    // the four damage quadrants before `dead` exists, so a snapshot armed on the flag is already
    // late. The drill controls the trigger and snapshots right before delivering the hit; no
    // production seam is implied. See coop/dev/death_write_diff.h.
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        auto n = std::make_shared<int>(0);
        GT::Post([done, n] { *n = coop::dev::death_write_diff::Snapshot(); done->store(1); });
        WaitDone(done, 30000);
        // A silent no-op here would leave the VERDICT line identical to a run where the instrument
        // worked; not a gate, but not invisible.
        if (*n < 0) UE_LOGW("death_test: write-diff PRE-HIT SNAPSHOT did NOT run -- the death "
                            "diff below is meaningless (see death_diff lines)");
    }

    // The lethal hit: 2x max health. Add Player Damage accumulates damage/maxHealth*4 into one of
    // the damage indicator's four directional floats, so a 10x hit put forty units of red on the
    // screen, a wash that outlived the revive and read as a bug in the arc; the only scaling on the
    // path is SelectFloat(0.75, 1.0, isStrong), never upward, so 2x is lethal with a 100% margin
    // and puts a realistic 8 units in the quadrant. A synthetic trigger must stay inside the range
    // the game produces, or it measures its own exaggeration.
    float maxHp = 100.f;
    { auto done = std::make_shared<std::atomic<int>>(0);
      auto mh = std::make_shared<float>(100.f);
      GT::Post([done, mh] { float v = 100.f; if (V::Read(V::Field::MaxHealth, &v)) *mh = v; done->store(1); });
      WaitDone(done, 8000);
      maxHp = *mh; }
    const float lethal = (maxHp > 0.f ? maxHp : 100.f) * 2.f;
    const float hpBefore = s.health;

    auto hitDone = std::make_shared<std::atomic<int>>(0);
    auto hitOk = std::make_shared<int>(0);
    GT::Post([hitDone, hitOk, lethal] {
        void* mp = coop::players::Registry::Get().Local();
        // blood=true: it gates the addEffect('bloodLoss') block, one of the two reds the revive
        // must clear; without it D11 passes while testing nothing.
        if (mp && R::IsLive(mp) && E::InvokeAddPlayerDamage(mp, lethal, /*blood=*/true)) *hitOk = 1;
        hitDone->store(1);
    });
    WaitDone(hitDone, 8000);
    const uint64_t tHit = ::GetTickCount64();
    UE_LOGI("death_test: delivered Add Player Damage(%.0f, blood=true) (health was %.2f, invoke=%s)",
            lethal, hpBefore, *hitOk ? "ok" : "FAILED");

    // The chain, observed.
    MemWindow dead;
    long long tDead = -1, tRagdoll = -1, tBlack = -1, tTravel = -1;
    long long tGrabCleared = -1, tBlackGone = -1;
    // When each red source left the screen; the revive runs at about +10 s, and anything much later
    // is a source it is not reaching.
    long long tRedGone = -1, tBloodGone = -1, tBlurGone = -1;
    bool sawZeroHealth = false;
    Sample last = s;
    for (uint64_t now = tHit; now - tHit < static_cast<uint64_t>(kDeadWindowMs);
         now = ::GetTickCount64()) {
        Sample p = Probe();
        const long long dt = static_cast<long long>(::GetTickCount64() - tHit);
        if (p.haveState && p.dead && tDead < 0) tDead = dt;
        if (p.haveState && p.isRagdoll && tRagdoll < 0) tRagdoll = dt;
        if (p.blackScreenInViewport && tBlack < 0) tBlack = dt;
        if (tBlack >= 0 && p.blackScreenInViewport) tBlackGone = -1;
        else if (tBlack >= 0 && tBlackGone < 0) tBlackGone = dt;
        if (p.haveWorld && !p.inGameplay && tTravel < 0) tTravel = dt;
        if (p.haveGrab && !p.grabValid && tGrabCleared < 0) tGrabCleared = dt;
        // Stamped only after each red has been seen up, so "never appeared" is not "cleared".
        if (p.dmgRed > 0.05f) tRedGone = -1; else if (tRedGone < 0 && tDead >= 0) tRedGone = dt;
        if (p.bloodLossActors > 0) tBloodGone = -1; else if (tBloodGone < 0 && tDead >= 0) tBloodGone = dt;
        if (p.bloodBlurInViewport > 0) tBlurGone = -1; else if (tBlurGone < 0 && tDead >= 0) tBlurGone = dt;
        if (p.health <= 0.f && p.health >= -0.5f) sawZeroHealth = true;
        // Only the in-world part is a memory measurement; once the travel starts, RSS is the
        // teardown and the new level.
        if (tTravel < 0) dead.Add(p.rssMb);
        last = p;
        ::Sleep(kSampleMs);
    }
    dead.ms = (tTravel > 0 ? static_cast<uint64_t>(tTravel) : static_cast<uint64_t>(kDeadWindowMs));

    UE_LOGI("death_test: TIMELINE (ms after the hit) -- dead=%lld ragdoll=%lld blackScreen=%lld "
            "blackGone=%lld travel=%lld grabCleared=%lld  [RE predicts dead~0, "
            "blackScreen~5000, travel~10000; with the arc armed, travel should read -1 and "
            "blackGone should land just past 10000 -- the revive is what removes it]",
            tDead, tRagdoll, tBlack, tBlackGone, tTravel, tGrabCleared);
    // The after instant: the revive has run, so whatever still differs from the pre-hit snapshot is
    // a write the death made and nothing disposed of. VOTVCOOP_DEATH_NO_RECONCILE=1 is the red arm
    // (a large delta); the pair is the reading.
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        auto n = std::make_shared<int>(0);
        GT::Post([done, n, deepFloor] {
            *n = coop::dev::death_write_diff::DiffAndLog(
                coop::death_revive::ReconcileDisabled()
                    ? (deepFloor ? "reconcile OFF, deep floor" : "reconcile OFF, SHALLOW floor")
                    : (deepFloor ? "reconcile ON, deep floor" : "reconcile ON, SHALLOW floor"));
            // Tens of MB, released before the balloon verdict: an instrument that inflates the
            // number it is measured beside measures itself.
            coop::dev::death_write_diff::Release();
            done->store(1);
        });
        WaitDone(done, 30000);
        if (*n < 0) UE_LOGW("death_test: write-diff DEATH DIFF did NOT run (see death_diff)");
    }
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        auto census = std::make_shared<std::wstring>();
        GT::Post([done, census] { *census = CensusViewportWidgets(); done->store(1); });
        WaitDone(done, 15000);
        UE_LOGI("death_test: VIEWPORT WIDGETS at end of run -- %ls", census->c_str());
    }
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        auto census = std::make_shared<std::wstring>();
        GT::Post([done, census] { *census = CensusEffects(); done->store(1); });
        WaitDone(done, 15000);
        UE_LOGI("death_test: EFFECTS at end of run -- %ls", census->c_str());
    }
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        auto census = std::make_shared<std::wstring>();
        GT::Post([done, census] { *census = CensusDamageIndicators(); done->store(1); });
        WaitDone(done, 15000);
        UE_LOGI("death_test: DAMAGE INDICATORS -- %ls", census->c_str());
    }
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        auto census = std::make_shared<std::wstring>();
        GT::Post([done, census] { *census = CensusRenderState(); done->store(1); });
        WaitDone(done, 15000);
        UE_LOGI("death_test: RENDER STATE -- %ls", census->c_str());
    }
    UE_LOGI("death_test: HUD -- damage-indicator worst quadrant: pre-hit %.2f, post %.2f; "
            "bloodLoss actors pre-hit %d post %d (worst time %.1f s), blur widgets in viewport "
            "post %d. CLEARED AT (ms after the hit): quadrants=%lld bloodLoss=%lld blur=%lld "
            "(the revive runs at ~10000; a later stamp is a source the revive is not reaching, "
            "and -1 means STILL ON SCREEN at the end of the run). None of these is cleared by "
            "the game -- the level travel used to dispose of them.",
            s.dmgRed, last.dmgRed, s.bloodLossActors, last.bloodLossActors, last.bloodLossTime,
            last.bloodBlurInViewport, tRedGone, tBloodGone, tBlurGone);
    UE_LOGI("death_test: GRAB -- pre-hit haveGrab=%d grabValid=%d; post haveGrab=%d grabValid=%d "
            "(ragdollMode's dropGrabObject should leave this INVALID before any revive teleports)",
            s.haveGrab ? 1 : 0, s.grabValid ? 1 : 0, last.haveGrab ? 1 : 0, last.grabValid ? 1 : 0);
    UE_LOGI("death_test: DEAD window memory -- %.1f -> %.1f MB over %llu ms (%.2f MB/s, peak %.1f); "
            "ALIVE control %.2f MB/s; DIFFERENTIAL %.2f MB/s",
            dead.firstMb, dead.lastMb, static_cast<unsigned long long>(dead.ms),
            dead.SlopeMbPerSec(), dead.peakMb, alive.SlopeMbPerSec(),
            dead.SlopeMbPerSec() - alive.SlopeMbPerSec());

    UE_LOGI("death_test: SEAM -- installed=%d travelsRefused=%llu lastReviveOk=%d "
            "sessionRunning=%d (the seam is process-wide; the SESSION is what gates the veto)",
            coop::death_revive::SeamInstalled() ? 1 : 0,
            coop::death_revive::TravelsRefused(),
            coop::death_revive::LastReviveSucceeded() ? 1 : 0,
            last.sessionRunning ? 1 : 0);

    // Acceptance. The two configurations assert two contracts: the sessionless run is the negative
    // control (single player is untouched, so a sessionless death must still travel), and without
    // it a fix that cancelled every travel would pass.
    const bool inCoopSession = last.sessionRunning || s.sessionRunning;

    // D1 and D2 are the falsifiers in both configurations: without them "the world survived" passes
    // on a run where the hit never landed.
    Verdict("D1 death-ran", tDead >= 0 && (sawZeroHealth || tRagdoll >= 0),
            tDead >= 0 ? "dead=true was observed -- the lethal chain really started"
                       : "dead never became true; the hit did not kill, so nothing below "
                         "means anything");
    Verdict("D2 ritual-played", tBlack >= 0,
            tBlack >= 0 ? "blackScreen_C reached the viewport -- the native death was allowed "
                          "to play out"
                        : "no blackScreen_C ever appeared; the chain did not reach uber @4353");

    if (inCoopSession) {
        Verdict("D3 world-survived", tTravel < 0,
                tTravel < 0 ? "no level travel inside the window -- OpenLevel was refused and "
                              "the world was kept"
                            : "the level travel ran: the world was torn down and the player is "
                              "in the main menu. The veto did not fire.");
        Verdict("D4 revived", last.haveState && !last.dead,
                (last.haveState && !last.dead)
                    ? "dead is false again -- the revive cleared the flag the game never clears"
                    : "dead is still true (or unreadable): no revive happened");
        Verdict("D5 standing", last.havePawn && last.haveState && !last.isRagdoll &&
                               last.health > 1.f,
                (last.havePawn && last.haveState && !last.isRagdoll && last.health > 1.f)
                    ? "the player is up, off the ragdoll, with positive health"
                    : "the player is not standing with health");
        // The positional arm: ApplyLocally reports a dispatched call, not a moved player.
        const float dx = last.locX - P::name::kKPPSpawnX;
        const float dy = last.locY - P::name::kKPPSpawnY;
        const float dz = last.locZ - P::name::kKPPSpawnZ;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        // The delta, not only the distance: a run once landed the teleport exactly and the player
        // drifted afterwards, and a scalar cannot tell a fall from a walk from a slide.
        char at[256];
        _snprintf_s(at, sizeof(at), _TRUNCATE,
                    "%.0f cm from the coop KPP (%.0f,%.0f,%.0f) -- delta (%.0f,%.0f,%.0f), "
                    "horiz %.0f vert %.0f -- read back from the pawn, not inferred from the "
                    "teleport's return", dist,
                    P::name::kKPPSpawnX, P::name::kKPPSpawnY, P::name::kKPPSpawnZ,
                    dx, dy, dz, std::sqrt(dx * dx + dy * dy), dz);
        Verdict("D7 at-KPP", last.haveLoc && dist <= 500.f, at);
        // The silently-lost-capability arm: pause_mainMenu survives the session on the screen tree,
        // so loadLevel's prep sticks through a cancelled travel.
        char mp[192];
        _snprintf_s(mp, sizeof(mp), _TRUNCATE,
                    "screenSwi=%d (want 1, ui_menu's own in-game value) canvas_loading vis=%d "
                    "(want 1 = Collapsed, the asset's serialized value) -- if either is wrong, "
                    "ESC shows a LOADING SCREEN instead of the pause menu",
                    last.screenSwiIdx, last.canvasLoadingVis);
        Verdict("D8 menu-restored",
                last.screenSwiIdx == 1 && last.canvasLoadingVis == 1, mp);
        // blackScreen_C has no script of its own, so the level travel was the only thing that ever
        // disposed of it; with the travel refused, only the revive removes it, and a permanent
        // black screen is what a player would see without this step.
        char bs[192];
        _snprintf_s(bs, sizeof(bs), _TRUNCATE,
                    "reached the viewport at %lld ms and left at %lld ms (IsInViewport, not "
                    "findability -- RemoveFromParent DETACHES, it does not destroy)",
                    tBlack, tBlackGone);
        // The HUD red, the same class as the black screen (a death artifact the travel used to
        // dispose of), gets its own arm; the runtime clears it best-effort, this bar is stricter.
        char red[192];
        _snprintf_s(red, sizeof(red), _TRUNCATE,
                    "worst damage_{up,down,left,right} = %.2f (want ~0; the death's own hit "
                    "accumulates damage/maxHealth*4 into one quadrant and nothing in the game "
                    "clears it, because the level travel used to)", last.dmgRed);
        Verdict("D10 hud-clear", last.dmgRed >= 0.f && last.dmgRed <= 0.05f, red);
        // D13, the image D10 is blind to: the death branch zeroes the four quadrants and shows
        // dmg_full in the same block, so D10 reports a clean HUD with a full-screen red image on
        // screen. Want 1, Collapsed, the authored value; -1 (unresolved) is not a failure.
        char full[224];
        _snprintf_s(full, sizeof(full), _TRUNCATE,
                    "dmg_full Visibility = %d (want 1 = Collapsed, its authored default; the "
                    "Tick's death branch @2292 sets it Visible and the ALIVE path never writes "
                    "the field, so only the revive can clear this one-way latch)",
                    last.dmgFullVis);
        Verdict("D13 dmgfull-collapsed", last.dmgFullVis != 0, full);
        // The second red, one arm per mechanism: they fail independently, and one "is the screen
        // red" arm could not say which to fix.
        char blood[224];
        _snprintf_s(blood, sizeof(blood), _TRUNCATE,
                    "%d live effect_bloodLoss_C, worst time=%.1f s (want 0 actors; any lethal "
                    "hit pins the duration at the 120 s cap, so without the revive expiring it "
                    "the world stays washed red for two minutes after a full-health revive)",
                    last.bloodLossActors, last.bloodLossTime);
        Verdict("D11 bloodloss-expired", last.bloodLossActors == 0, blood);
        char blur[224];
        _snprintf_s(blur, sizeof(blur), _TRUNCATE,
                    "%d ui_bloodLossBlur_C widgets still ON the viewport, cleared at %lld ms "
                    "(the effect actor's ReceiveDestroyed is what RemoveFromParent's it, so a "
                    "widget outliving the actor means the teardown did not run)",
                    last.bloodBlurInViewport, tBlurGone);
        Verdict("D12 blur-gone", last.bloodBlurInViewport <= 0, blur);
        Verdict("D9 black-screen-cleared",
                tBlack >= 0 && tBlackGone > 0 && !last.blackScreenInViewport, bs);
    } else {
        // Single player, no session: the contract is that nothing of ours acts.
        Verdict("D3 sp-untouched", tTravel >= 0,
                tTravel >= 0 ? "the level travel ran, as vanilla VOTV does -- single player is "
                               "not touched by the arc (the veto's first term is a live session)"
                             : "NO travel happened without a session: the veto fired outside "
                               "coop, which breaks the user's single-player guarantee");
        Verdict("D4 sp-no-revive", !(last.haveState && !last.dead && last.health > 1.f),
                "nothing revived the player, which is correct with no session");
        Verdict("D5 seam-quiet", coop::death_revive::TravelsRefused() == 0,
                coop::death_revive::TravelsRefused() == 0
                    ? "the travel seam refused nothing in a sessionless run"
                    : "the seam REFUSED a travel with no session running");
    }

    const double diff = dead.SlopeMbPerSec() - alive.SlopeMbPerSec();
    Verdict("D6 no-balloon", diff < kBalloonMbPerSec,
            diff < kBalloonMbPerSec
                ? "the dead window's RSS slope is within the alive control -- the inherited "
                  "'~165 MB/s possessed-ragdoll leak' is NOT reproduced here"
                : "the dead window ballooned well past the alive control -- staying in the "
                  "world after death costs memory, and the arc must answer that");

    UE_LOGI("death_test: VERDICT %s (%d pass / %d fail) -- health %.2f -> %.2f",
            g_fail == 0 ? "PASS" : "FAIL", g_pass, g_fail, hpBefore, last.health);
    UE_LOGI("death_test: DONE");
    return 0;
}

}  // namespace harness::autotest
