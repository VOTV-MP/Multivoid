// harness/autotest/death_state_probe.cpp -- the readers behind harness/autotest/death_state_probe.h.
// Every function here runs on the game thread and reads UObject state by reflection; none of them
// writes anything and none of them decides anything, so a drill can read the same state before it
// acts, after it acts, and on a cadence in between without the reads meaning three things. What
// each field is and why it is worth a line is in the header, beside the field.

#include "harness/autotest/death_state_probe.h"

#include "coop/player/players_registry.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/game_mode.h"

namespace harness::autotest {
namespace {

namespace E = ue_wrap::engine;
namespace V = ue_wrap::vitals;
namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

// The widget the death chain adds at +5 s (Create(blackScreen_C).AddToViewport): the asset
// carries a CanvasPanel, an Image and a brush and no function or ubergraph at all, a static
// full-screen black image that never removes itself. Whoever cancels the travel inherits it.
constexpr const wchar_t* kBlackScreenClass = L"blackScreen_C";

// A BP bool by name off a live object (byte and mask).
bool ReadBpBool(void* obj, const wchar_t* name, bool& out) {
    if (!obj || !R::IsLive(obj)) return false;
    int32_t byteOff = -1; uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(obj), name, byteOff, mask)) return false;
    out = (*(reinterpret_cast<uint8_t*>(obj) + byteOff) & mask) != 0;
    return true;
}

// An object-pointer property by name off a live object, and whether it points at something live.
bool ReadBpObjectValid(void* obj, const wchar_t* name, bool& outValid) {
    if (!obj || !R::IsLive(obj)) return false;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), name);
    if (off < 0) return false;
    void* target = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(obj) + off);
    outValid = target != nullptr && R::IsLive(target);
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

}  // namespace

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
    // One walk, three readers: FindObjectsByClass is a full GUObjectArray pass and this function
    // asked for the same set three times.
    const auto ppVolumes = R::FindObjectsByClass(L"PostProcessVolume");
    for (void* v2 : ppVolumes)
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
            const int mode = ue_wrap::game_mode::ReadFrom(gi);
            _snwprintf_s(buf, _TRUNCATE, L" | GameInstance.gamemode=b%d%ls", mode,
                         mode == 7 ? L" (== b7: Bad Sun spawns EVERY new day)" : L"");
            out += buf;
        }
    }

    // 4. Every PostProcessVolume with a non-zero blend.
    int vols = 0, hot = 0;
    for (void* v : ppVolumes) {
        if (!v || !R::IsLive(v)) continue;
        ++vols;
        const int32_t oW = R::FindPropertyOffset(R::ClassOf(v), L"BlendWeight");
        if (oW >= 0 && *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(v) + oW) > 0.01f) ++hot;
    }
    _snwprintf_s(buf, _TRUNCATE, L" | PostProcessVolumes=%d (blend>0: %d)", vols, hot);
    out += buf;
    for (void* v : ppVolumes) {
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

DeathSnapshot ReadDeathState() {
    DeathSnapshot s;
    void* mp = coop::players::Registry::Get().Local();
    if (mp && R::IsLive(mp)) {
        s.havePawn = true;
        s.haveCanRagdoll = E::ReadMainPlayerCanRagdoll(mp, s.canRagdoll);
        s.haveState = E::ReadMainPlayerRagdollState(mp, s.isRagdoll, s.dead);
        s.haveStartInvinc = ReadBpBool(mp, L"startInvinc", s.startInvinc);
        s.haveGrab = ReadBpObjectValid(mp, L"grabbing_actor", s.grabValid);
        ue_wrap::FVector at{};
        s.haveLoc = E::TryGetActorLocation(mp, at);
        s.locX = at.X; s.locY = at.Y; s.locZ = at.Z;
    }
    ReadMenuPrep(s.screenSwiIdx, s.canvasLoadingVis);
    s.dmgRed = ReadDamageRed();
    s.dmgFullVis = ReadDmgFullVisibility();
    ReadBloodLoss(s.bloodLossActors, s.bloodLossTime);
    s.bloodBlurInViewport = ReadBloodBlurInViewport();
    if (void* gm = R::FindObjectByClass(P::name::GamemodeClass))
        s.haveImmortal = ReadBpBool(gm, L"immortal", s.immortal);
    float hp = -1.f;
    if (V::Read(V::Field::Health, &hp)) s.health = hp;
    // EVERY live blackScreen_C, OR-ed. FindObjectByClass is first-by-index with no liveness and
    // no world filter, so after one menu-to-gameplay cycle a departed world's instance sits at a
    // lower index and answers for the live one -- and "is the black screen up" is a term two
    // drills assert on.
    for (void* bs : R::FindObjectsByClass(kBlackScreenClass)) {
        if (!bs || !R::IsLive(bs)) continue;
        s.blackScreen = true;
        void* userWidgetCls = R::FindClass(P::name::UserWidgetClass);
        void* fnInView = userWidgetCls ? R::FindFunction(userWidgetCls, L"IsInViewport") : nullptr;
        if (!fnInView) break;
        ue_wrap::ParamFrame f(fnInView);
        if (f.valid() && ue_wrap::Call(bs, f) && f.Get<bool>(L"ReturnValue")) {
            s.blackScreenInViewport = true;
            break;
        }
    }
    // The gameplay world's leaf name contains "ntitled" (untitled_1.Untitled_1); the menu and
    // loading worlds do not.
    if (void* w = R::FindObjectByClass(P::name::WorldClass)) {
        s.haveWorld = true;
        s.inGameplay = R::ToString(R::NameOf(w)).find(L"ntitled") != std::wstring::npos;
    }
    return s;
}

}  // namespace harness::autotest
