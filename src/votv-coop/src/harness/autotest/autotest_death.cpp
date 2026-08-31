// harness/autotest_death.cpp -- the NATIVE DEATH CHAIN instrument
// (VOTVCOOP_RUN_DEATH_TEST). One process, TWO CONFIGURATIONS, and they measure
// different things because they EXCLUDE each other -- and since the arc landed they
// assert DIFFERENT, EQUALLY REAL CONTRACTS:
//
//   `mp.py death --session`  -- SOLO HOST, and the arc's ACCEPTANCE run. `running()` is
//                               true from `Start()` with zero clients
//                               (session_start.cpp:234), so this is a real coop session
//                               by the user's own definition (2026-08-31: "Solo host in
//                               a session a a coop session... Single player is when
//                               playing solo game in solo save, no session"). Here the
//                               whole native death must play out (~10 s, black screen at
//                               +5 s), the level travel must be REFUSED at
//                               UGameplayStatics::OpenLevel, and the player must come
//                               back standing at the KPP with the pause menu reachable.
//
//   `mp.py death`            -- SESSIONLESS, and it is the discriminator's NEGATIVE
//                               CONTROL, not a lesser run. The user's decision is that
//                               single player is untouched, so the travel MUST still
//                               happen and the seam must refuse NOTHING. Without this
//                               arm a fix that cancelled every travel would pass.
//
// Neither configuration needs a second peer; the whole chain is local BP. What neither
// can show is the OBSERVER's screen -- that is a two-peer run.
//
// IT REPORTS TWO DIFFERENT THINGS, AND ONLY ONE OF THEM CAN FAIL.
//
// 1. OBSERVATION (never fails). The measured timeline of VOTV's own death, from a
//    real lethal `Add Player Damage` to the level travel (or its refusal), plus a memory
//    profile across it and the travel seam's own counters. This is the RE doc's bytecode
//    chain (research/findings/world-systems/votv-player-death-chain-RE-2026-08-31.md)
//    confronted with the running game: `dead := true` -> +5 s blackScreen_C -> +5 s
//    loadLevel('menu') -> OpenLevel. A disagreement here is a finding.
//
// 2. ACCEPTANCE (fails). docs/DEATH_ARC.md's contract, per configuration. The arms are
//    asserted in `death_test:` lines and NEVER inferred from a module's own log -- a
//    module logging "REVIVE OK" is the subject speaking about itself.
//
// M0 -- THE MEASUREMENT THE ARC HINGES ON. `net_pump.cpp`'s death policy has
// asserted since 2026-06-01, from "4 hands-on + an autonomous probe arc", that
// "the balloon is VOTV's OWN possessed-ragdoll leak in the GAMEPLAY world
// (~165 MB/s to OOM)" and that therefore the only cure is to leave the world.
// The arc does the opposite -- it keeps the player in the world through the whole
// ten seconds and past it -- so if that rate is real the design is dead on
// arrival. The claim has no surviving finding doc and predates the v122
// no-passive-mint root fix; it is inherited, not measured, so this instrument
// re-measures it as a DIFFERENTIAL: the RSS slope across the dead window against
// the slope across an equally long ALIVE window immediately before it, on the
// same process, same save, same frame rate. A shared baseline drift cancels.
//
// The hit is the game's own `Add Player Damage`, so it walks the real lethal
// chain (Add Player Damage -> kill() -> ragdollMode(...,death=true) -> fallen ->
// the two RetriggerableDelays); only the trigger is synthetic.

#include "harness/autotest.h"

#include "coop/net/session.h"
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

// The widget the death chain adds at +5 s (mainPlayer uber @4353,
// Create(blackScreen_C).AddToViewport(0)). Measured 2026-08-31 from the asset
// itself: blackScreen_C's name table carries a CanvasPanel, an Image and a
// SlateBrush and NO function or ubergraph export at all -- it is a static
// full-screen black image that never removes itself. That is why it is worth
// observing: whoever cancels the travel inherits it.
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
    // ...and whether it is actually ON THE SCREEN. These are different questions and the
    // difference is the whole point: `RemoveFromParent` DETACHES, so a removed widget is
    // STILL FINDABLE ([[lesson-removefromparent-detaches-a-widget-it-does-not-destroy-it]]).
    // Any "is the black screen gone" assertion must read IsInViewport.
    bool  blackScreenInViewport = false;
    bool  inGameplay = false;   // the live UWorld is still untitled_1 (we did not travel)
    bool  haveWorld = false;
    // Add Player Damage's own early-out terms (uber @659:
    // gamemode.immortal || isDreaming || dead || startInvinc -> return). Read so
    // a hit that lands NOWHERE can say WHY instead of just failing: the retired
    // KO test's first run lost its lethal hit here (health 100.00 -> 100.00) and
    // the verdict alone could not name the term.
    bool  startInvinc = false;
    bool  haveStartInvinc = false;
    bool  immortal = false;
    bool  haveImmortal = false;
    // `grabbing_actor` -- the physics-grabbed actor. Sampled because the revive's
    // teleport is NOT position-only: `teleportWObackrooms` @10-@449 drops the
    // grabbed actor, transforms it, and `pickupObjectDirect`s it back (@435/@847/
    // @909). `ragdollMode` already ran `dropGrabObject()` on the death's common
    // path, so this SHOULD read invalid by the time a revive teleports -- but that
    // is an inference, and if it is wrong the revive performs a second
    // shared-world write on someone else's prop.
    bool  grabValid = false;
    bool  haveGrab = false;
    bool  sessionRunning = false;
    // Where the player is. The revive repositions to the coop KPP (USER 2026-08-31: "Кпп"),
    // and `ApplyLocally` has a THREE-TIER fallback whose report says a call was dispatched,
    // not that the player moved -- so the position is the only honest assertion.
    float locX = 0.f, locY = 0.f, locZ = 0.f;
    bool  haveLoc = false;
    // `lib.loadLevel`'s menu prep, read back. NOT cosmetic and NOT belt-and-braces: it is the
    // arm that catches a SILENTLY LOST CAPABILITY, which is exactly the shape of the retired
    // KO lane's H1 (a header promising a thing that had zero call sites). `pause_mainMenu`
    // lives on the screen tree all session, so `loadLevel`'s two writes stick through a
    // cancelled travel and the next ESC shows a LOADING SCREEN instead of the pause menu.
    int32_t screenSwiIdx = -1;   // in-game value is 1 (ui_menu uber @2445)
    int32_t canvasLoadingVis = -1;  // in-game value is 1 = ESlateVisibility::Collapsed
    // The damage indicator's worst directional accumulator
    // (gamemode.playerInterface.umg_damageIndicator.damage_{up,down,left,right}). A revived
    // player at full health wearing a red screen is death state that outlived the revive.
    float dmgRed = -1.f;
    // The SECOND red, and a different mechanism: `Add Player Damage` @3414 spawns an
    // `effect_bloodLoss_C` whose PostProcess + ui_bloodLossBlur wash the whole WORLD red.
    // `[V]` any lethal hit pins its duration at the 120 s cap, so it outlives the revive by
    // two minutes unless the revive expires it. Counted as live actors, not as a float,
    // because "is one still standing" is the question.
    int32_t bloodLossActors = -1;
    float bloodLossTime = -1.f;
    // The effect's OWN widget. `[V]` `effect_bloodLoss_C` carries `widgetBlur` / `setBlur` /
    // `AddToViewport` / `RemoveFromParent` / `ReceiveDestroyed`, so the blur SHOULD die with
    // the actor -- but "should" is what the user's screenshot disagreed with, so it gets
    // counted separately. If the actor is gone and this is not, the actor's teardown is the
    // bug; if both are gone and the screen is still red, there is a THIRD source.
    int32_t bloodBlurInViewport = -1;
    double rssMb = -1.0;
};

// Read an object-pointer property by name off a live object and report whether it
// points at something live. Test-local.
bool ReadBpObjectValid(void* obj, const wchar_t* name, bool& outValid) {
    if (!obj || !R::IsLive(obj)) return false;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), name);
    if (off < 0) return false;
    void* target = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(obj) + off);
    outValid = target != nullptr && R::IsLive(target);
    return true;
}

// Read a plain BP bool by name off a live object (byte+mask, same shape as the
// engine's canRagdoll accessor). Test-local: nothing in the mod needs these.
bool ReadBpBool(void* obj, const wchar_t* name, bool& out) {
    if (!obj || !R::IsLive(obj)) return false;
    int32_t byteOff = -1; uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(obj), name, byteOff, mask)) return false;
    out = (*(reinterpret_cast<uint8_t*>(obj) + byteOff) & mask) != 0;
    return true;
}

// Walk gamemode -> pause_mainMenu -> {canvas_loading, screenSwi} and read back the two values
// `lib.loadLevel` stomps on the way to the menu. Test-local; the mod's own restore lives in
// coop/player/death_revive.cpp. Every engine verb resolves off its DECLARING class, because
// `FindFunction` is exact-owner and does not climb SuperStruct.
// The worst of the damage indicator's four quadrant accumulators, or -1 if the chain does
// not resolve. Test-local, and it walks the SAME chain the game's own damage path writes.
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

// How many live effect_bloodLoss_C actors there are and the worst remaining `time`.
// -1/-1 means the class is not loaded at all (no bloodLoss has ever been added this session),
// which is a legitimate pre-hit state and NOT a resolution failure.
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

// Count live `ui_bloodLossBlur_C` widgets that are actually ON the viewport. -1 = the class
// is not loaded (none has ever existed this session).
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

// EVERY UUserWidget-descended object currently ON the viewport, by class name.
//
// This exists because three successive TARGETED probes each measured their own target clear
// while the user was still looking at a red screen. A probe aimed at what you already suspect
// cannot find a source you have not thought of; an enumeration can. One full GUObjectArray
// walk, run ONCE at the end of the run, so the cost is irrelevant.
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

// EVERY live `ui_damageIndicator_C` INSTANCE with its four quadrant values.
//
// The reader used by the revive and by D10 walks ONE path -- gamemode.playerInterface
// .umg_damageIndicator -- and reports that object's values. If more than one instance exists,
// that reader can report 0.00 with perfect honesty while a DIFFERENT instance is the one on
// screen. Counting instances is the question "am I even looking at the right object", which
// no amount of re-reading the same pointer can answer.
std::wstring CensusDamageIndicators() {
    void* cls = R::FindClass(L"ui_damageIndicator_C");
    if (!cls) return L"(class unresolved)";
    const int32_t oU = R::FindPropertyOffset(cls, L"damage_up");
    const int32_t oD = R::FindPropertyOffset(cls, L"damage_down");
    const int32_t oL = R::FindPropertyOffset(cls, L"damage_left");
    const int32_t oR = R::FindPropertyOffset(cls, L"damage_right");
    if (oU < 0 || oD < 0 || oL < 0 || oR < 0) return L"(offsets unresolved)";
    // which one the revive's path points at, so a mismatch is visible rather than inferred
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
    // ...and the WORLD-side red sources, because the pre-hit screenshot proved the tint is
    // present BEFORE any damage: a live redSkyEvent_C / weatherFogController_C / blackFog_C
    // tints the whole scene and has nothing to do with the death.
    for (const wchar_t* c : {L"redSkyEvent_C", L"weatherFogController_C", L"blackFog_C"}) {
        int live = 0;
        for (void* o : R::FindObjectsByClass(c)) if (o && R::IsLive(o)) ++live;
        if (live) { out += L" | WORLD "; out += c; out += L"=" + std::to_wstring(live); }
    }
    return L"instances=" + std::to_wstring(n) + out;
}

// THE RENDER STATE ITSELF -- what can actually tint the scene while leaving UMG untouched.
//
// Written after four subsystem probes each measured clean while the user still saw red, and
// after a pre-hit screenshot proved the tint predates the death entirely. Enumerating
// SUBSYSTEMS asks "is my suspect guilty"; this asks "what is the renderer actually doing",
// which is the question the screen answers.
//
// In UE4 exactly four things tint the world but not the UI: a camera FADE, a post-process on
// the camera/player/gamemode component, a PostProcessVolume, or scene lighting. The first
// three are readable by name.
// Blendable materials on ONE FPostProcessSettings, by name and weight. A post-process
// MATERIAL is the only thing that can tint the sky, the stars and the near ground by the same
// amount while leaving UMG alone, so every settings struct in the pipeline gets asked -- not
// just the one I happened to suspect.
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

std::wstring CensusRenderState() {
    std::wstring out;
    wchar_t buf[256];

    // 1. APlayerCameraManager fade -- SetManualCameraFade leaves these set.
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

    // 2/3. the post-process components, by their owner.
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

    // 3b. WHAT IS ACTUALLY IN THE PLAYER'S POST-PROCESS. `enabled=1 weight=1.00` says a
    //     component is live, not what it does. `WeightedBlendables.Num` names an INJECTED
    //     material -- the only way a component tints the world red -- and the colour-grading
    //     overrides say whether someone graded the scene instead.
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
                            // FWeightedBlendables{ TArray<FWeightedBlendable> Array }, and
                            // FWeightedBlendable{ float Weight; UObject* Object } = 16 B.
                            // NAME the material: "there is one" is not an identification, and
                            // a uniform scene-wide tint with UI untouched can only be this.
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
                        // any colour-ish override that is ON is worth naming
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

    // 3c. THE FOG. The frames are distance-dependent -- near grass still green, distant trees
    //     deep red -- which is fog inscattering, not a uniform post-process multiply.
    //     `daynightCycle` owns SetFogInscatteringColor / SetFogDensity / enable_fog and
    //     `newsky` owns fog_color_A/B, so this reads the value they land on.
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

    // 3d. blendables on EVERY source, not just the player's.
    if (void* gm2 = R::FindObjectByClass(P::name::GamemodeClass)) {
        const int32_t o = R::FindPropertyOffset(R::ClassOf(gm2), L"PostProcess");
        if (o >= 0) {
            void* c = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(gm2) + o);
            out += BlendablesOf(c, L"Settings", L"gm.PostProcess");
        }
    }
    for (void* v2 : R::FindObjectsByClass(L"PostProcessVolume"))
        out += BlendablesOf(v2, L"Settings", R::ToString(R::NameOf(v2)).c_str());
    // and the camera's own settings, the last stop before the frame
    if (void* mp3 = R::FindObjectByClass(P::name::MainPlayerClass)) {
        const int32_t o = R::FindPropertyOffset(R::ClassOf(mp3), L"Camera");
        if (o >= 0) {
            void* cam = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(mp3) + o);
            out += BlendablesOf(cam, L"PostProcessSettings", L"Camera");
        }
    }

    // 3e. WHICH BRANCH LIT THE MEGASUN. `[V]` daynightCycle's new-day block spawns the Bad Sun
    //     either because `GameInstance.gamemode == b7` (deterministic, every day) or on a
    //     0.1%/day roll gated on the `badsun` achievement. Reading the mode says which -- and
    //     saves asking a question that is measurable.
    if (void* gi = R::FindObjectByClass(P::name::GameInstanceClass)) {
        if (R::IsLive(gi)) {
            const uint8_t mode = *(reinterpret_cast<uint8_t*>(gi) + P::off::mainGameInstance_GameMode);
            _snwprintf_s(buf, _TRUNCATE, L" | GameInstance.gamemode=b%u%ls", (unsigned)mode,
                         mode == 7 ? L" (== b7: Bad Sun spawns EVERY new day)" : L"");
            out += buf;
        }
    }

    // 4. every PostProcessVolume in the world with a non-zero blend.
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
        // THE COLOUR GRADING -- the field I kept not reading. Weight says a volume is
        // contributing; only these say WHAT it contributes, and a scene-wide uniform red
        // with UI untouched is exactly what a graded volume looks like.
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

// EVERY live actor descending from `effect_C`, by class name, plus the gamemode's own
// `effects_names` array -- the two halves of VOTV's effect system, which can disagree.
//
// The SCREENSHOT settled what three property probes could not: the tint covers the WORLD and
// not the HUD, so it is a POST-PROCESS, and `effect_C`'s base carries a `PostProcessComponent`.
// `effect_bloodLoss_C` measured absent, so either another effect class is up, or the gamemode
// is still holding a row for one that is gone. Enumerate both rather than guess again.
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
    // the gamemode's parallel bookkeeping
    if (void* gm = R::FindObjectByClass(P::name::GamemodeClass)) {
        if (R::IsLive(gm)) {
            const int32_t off = R::FindPropertyOffset(R::ClassOf(gm), L"effects_names");
            if (off >= 0) {
                struct FNameArr { void* data; int32_t num; int32_t max; };
                auto* a = reinterpret_cast<FNameArr*>(reinterpret_cast<uint8_t*>(gm) + off);
                out += L" | gamemode.effects_names.Num=" + std::to_wstring(a->num);
                // FName is 8 bytes {ComparisonIndex, Number}; render each through the engine's
                // own FName::ToString so a stale row is NAMED, not just counted.
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
        // The gameplay world's leaf name contains "ntitled" (untitled_1.Untitled_1);
        // the menu / loading worlds do not. Same discriminator the menu-travel probe
        // settled on.
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

// A window's memory profile: first/last RSS and the slope between them.
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

// How far the dead window's RSS slope may exceed the alive window's before M0 is
// called a balloon. The inherited claim is ~165 MB/s; ordinary VOTV drift while
// simply standing still is the control this subtracts. 20 MB/s is an order of
// magnitude under the claim and an order of magnitude over normal streaming
// churn, so neither answer is a coin flip.
constexpr double kBalloonMbPerSec = 20.0;

// How long to watch, alive and then dead. The alive window is the control; the
// dead window has to outlast the chain's own 10 s so the travel (or its absence)
// is inside the observation.
constexpr int kAliveWindowMs = 10000;
constexpr int kDeadWindowMs  = 22000;
constexpr int kSampleMs      = 250;

}  // namespace

DWORD WINAPI DeathTestThread(LPVOID) {
    UE_LOGI("death_test: armed -- a LETHAL Add Player Damage runs VOTV's native death "
            "chain to completion; the timeline + memory are OBSERVED, and the "
            "docs/DEATH_ARC.md contract is the ACCEPTANCE half");

    // Wait for a pawn that can actually be killed. canRagdoll must be TRUE: the
    // retired KO lane held it shut for the session, and a run in which anything
    // still holds it measures the gate, not the death.
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

    // ---- the ALIVE control window -------------------------------------------
    // The same cadence, the same reads, the same frame load, with the player
    // simply standing there. Whatever this drifts is what the dead window is
    // allowed to drift.
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

    // ---- deliver the lethal hit ---------------------------------------------
    // 2x max health. It USED to be 10x, and that was a real instrument defect rather than
    // caution: `[V]` `Add Player Damage` @4269 ACCUMULATES `damage/maxHealth*4` into one of
    // the damage indicator's four directional floats, so a 10x hit put FORTY units of red on
    // the screen -- a full-screen red wash that outlived the revive and read as a bug in the
    // arc (the user saw it and asked). The only scaling anywhere on the path is
    // `SelectFloat(0.75, 1.0, isStrong)` (@856) -- damage is NEVER scaled UP -- so 2x is
    // lethal with a 100% margin and puts a realistic 8 units in the quadrant instead of 40.
    // A synthetic trigger has to stay inside the range the game itself produces, or it
    // measures its own exaggeration.
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
        // blood=TRUE. `[V]` @2784 gates the `addEffect('bloodLoss', ...)` block on it, and
        // that effect is one of the two reds the revive has to clear -- so a hit without it
        // makes D11 pass while testing nothing (it did exactly that on the 15:09 run:
        // `0 live effect_bloodLoss_C` because none was ever created).
        if (mp && R::IsLive(mp) && E::InvokeAddPlayerDamage(mp, lethal, /*blood=*/true)) *hitOk = 1;
        hitDone->store(1);
    });
    WaitDone(hitDone, 8000);
    const uint64_t tHit = ::GetTickCount64();
    UE_LOGI("death_test: delivered Add Player Damage(%.0f, blood=true) (health was %.2f, invoke=%s)",
            lethal, hpBefore, *hitOk ? "ok" : "FAILED");

    // ---- observe the chain ---------------------------------------------------
    MemWindow dead;
    long long tDead = -1, tRagdoll = -1, tBlack = -1, tTravel = -1;
    long long tGrabCleared = -1, tBlackGone = -1;
    // When each red source actually left the screen. The revive runs at ~+10 s; anything
    // materially later than that is a source the revive is not reaching.
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
        // Only stamp AFTER each red has been seen up, so "never appeared" is not "cleared".
        if (p.dmgRed > 0.05f) tRedGone = -1; else if (tRedGone < 0 && tDead >= 0) tRedGone = dt;
        if (p.bloodLossActors > 0) tBloodGone = -1; else if (tBloodGone < 0 && tDead >= 0) tBloodGone = dt;
        if (p.bloodBlurInViewport > 0) tBlurGone = -1; else if (tBlurGone < 0 && tDead >= 0) tBlurGone = dt;
        if (p.health <= 0.f && p.health >= -0.5f) sawZeroHealth = true;
        // Only the in-world part of the run is a memory measurement; once the
        // travel starts, RSS is dominated by the teardown + the new level.
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

    // ---- ACCEPTANCE ----------------------------------------------------------
    //
    // THE TWO CONFIGURATIONS ASSERT DIFFERENT, EQUALLY REAL CONTRACTS. That is not a
    // convenience: the SESSIONLESS run is the discriminator's NEGATIVE CONTROL. The user's
    // decision is that single player is untouched ("Gate of course, we only work in coop,
    // single player games are not touched by us"), so a sessionless death MUST still travel
    // -- and without this arm a fix that cancelled EVERY travel would pass.
    const bool inCoopSession = last.sessionRunning || s.sessionRunning;

    // D1/D2 are the falsifiers, and they hold in BOTH configurations: without them "the world
    // survived" would pass on a run where the hit simply never landed.
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
        // The positional arm. `ApplyLocally` reports that a call was dispatched, not that the
        // player moved -- three-tier fallback -- so this is the only honest test of step 5.
        const float dx = last.locX - P::name::kKPPSpawnX;
        const float dy = last.locY - P::name::kKPPSpawnY;
        const float dz = last.locZ - P::name::kKPPSpawnZ;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        char at[192];
        _snprintf_s(at, sizeof(at), _TRUNCATE,
                    "%.0f cm from the coop KPP (%.0f,%.0f,%.0f) -- read back from the pawn, "
                    "not inferred from the teleport's return", dist,
                    P::name::kKPPSpawnX, P::name::kKPPSpawnY, P::name::kKPPSpawnZ);
        Verdict("D7 at-KPP", last.haveLoc && dist <= 500.f, at);
        // The silently-lost-capability arm (H1's shape). `pause_mainMenu` survives the whole
        // session on the screen tree, so loadLevel's prep sticks through a cancelled travel.
        char mp[192];
        _snprintf_s(mp, sizeof(mp), _TRUNCATE,
                    "screenSwi=%d (want 1, ui_menu's own in-game value) canvas_loading vis=%d "
                    "(want 1 = Collapsed, the asset's serialized value) -- if either is wrong, "
                    "ESC shows a LOADING SCREEN instead of the pause menu",
                    last.screenSwiIdx, last.canvasLoadingVis);
        Verdict("D8 menu-restored",
                last.screenSwiIdx == 1 && last.canvasLoadingVis == 1, mp);
        // `blackScreen_C` has NO script of its own -- `[V]` its whole asset carries no
        // function or ubergraph export -- so the level travel is the only thing that ever
        // disposed of it. With the travel refused, the ONLY way it leaves the screen is the
        // revive removing it. A permanent black screen is what a player would actually see
        // if this step were missing, which makes it the most visible arm here.
        char bs[192];
        _snprintf_s(bs, sizeof(bs), _TRUNCATE,
                    "reached the viewport at %lld ms and left at %lld ms (IsInViewport, not "
                    "findability -- RemoveFromParent DETACHES, it does not destroy)",
                    tBlack, tBlackGone);
        // The user found this one by LOOKING at a run: revived at the KPP, full health, and
        // the whole HUD washed red. It is the same class as the black screen -- an artifact
        // of the death that the level travel used to dispose of -- so it gets the same
        // treatment and its own arm. The runtime treats the clear as best-effort (a red
        // screen is not worth fleeing to the menu over); this bar is deliberately stricter.
        char red[192];
        _snprintf_s(red, sizeof(red), _TRUNCATE,
                    "worst damage_{up,down,left,right} = %.2f (want ~0; the death's own hit "
                    "accumulates damage/maxHealth*4 into one quadrant and nothing in the game "
                    "clears it, because the level travel used to)", last.dmgRed);
        Verdict("D10 hud-clear", last.dmgRed >= 0.f && last.dmgRed <= 0.05f, red);
        // The second red. ONE arm per mechanism, because they fail independently and a
        // single "is the screen red" arm could not say which to fix.
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
        // SINGLE PLAYER (no session). The contract is that we do NOTHING.
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
