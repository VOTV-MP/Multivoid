// coop/player/death_revive.cpp -- see the header for the design and the bounds, and
// docs/DEATH_ARC.md for the RE the whole thing rests on.

#include "coop/player/death_revive.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"
#include "coop/session/teleport_client.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/reflected_offset.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/level_travel.h"

#include <windows.h>

#include <atomic>
#include <cmath>

namespace coop::death_revive {
namespace {

namespace E = ue_wrap::engine;
namespace LT = ue_wrap::engine::level_travel;
namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;
namespace V = ue_wrap::vitals;
namespace RO = ue_wrap::reflected_offset;

// The widget the death chain adds at +5 s (mainPlayer uber @4353,
// `Create(blackScreen_C).AddToViewport(0)`). `[V]` it has NO function or ubergraph export
// at all -- the level travel is what disposes of it today, so a cancelled travel inherits a
// permanent black screen unless we remove it. `[V]` `RemoveFromParent` works on it
// (`inViewport 1 -> 0`, measured 2026-08-31) and DETACHES rather than destroys, so the
// completion test is `IsInViewport`, never findability
// ([[lesson-removefromparent-detaches-a-widget-it-does-not-destroy-it]]).
constexpr const wchar_t* kBlackScreenClass = L"blackScreen_C";

// `ui_menu_C.screenSwi`'s in-game value. `[V]` the widget sets it itself at uber @2445
// (`screenSwi.SetActiveWidgetIndex(1)`, gated on `GetCurrentLevelName != 'menu'`), and
// `lib.loadLevel` @333 stomps it to 0 on the way to the menu. So 1 is "the value the widget
// sets for itself in gameplay", not a guess.
constexpr int32_t kScreenSwitcherInGameIndex = 1;

// ESlateVisibility::Collapsed. `[V]` `canvas_loading` (ui_menu export 83) carries an
// EXPLICIT serialized `Visibility = Collapsed`, and `ui_menu`'s ubergraph writes it NOWHERE
// -- the only writer in the game is `lib.loadLevel` @219, which sets it Visible. So the
// restore value is a constant read off the asset. A stateful "sample it while alive and put
// that back" was designed and DELETED: it had a hole (what if we never sampled).
constexpr uint8_t kSlateCollapsed = 1;

// How close to the KPP the player must land for the reposition to count. `harness.cpp`'s own
// teleport self-check uses 200 uu for the same question; the teleport either lands or falls
// through three tiers, so this is a real assertion, not a tolerance for drift.
constexpr float kAtKppToleranceCm = 300.f;

// ---- the veto's inputs, published by Tick, read by the detour -----------------------------
// Every one of these is a plain value: the veto may not dispatch, allocate or lock.
std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<void*>    g_pawn{nullptr};
std::atomic<int32_t>  g_pawnIdx{-1};
std::atomic<int32_t>  g_deadByte{-1};
std::atomic<uint8_t>  g_deadMask{0};
std::atomic<bool>     g_armed{false};          // this death is ours to answer
std::atomic<bool>     g_revivePending{false};  // the detour refused a travel; the pump owes a revive
std::atomic<uint64_t> g_vetoAtMsAtomic{0};     // when it refused -- stamped IN the veto (see Watchdog)

std::atomic<bool>     g_installed{false};    // written from the timeline tick AND the pump
std::atomic<bool>     g_installDone{false};  // install attempted (success or permanent failure)

// ---- pump-side state (game thread only) ---------------------------------------------------
bool  g_wasDead = false;
bool  g_lastReviveOk = false;
bool  g_reviveRanThisDeath = false;
uint64_t g_vetoAtMs = 0;

// The revive is six synchronous writes on ONE tick, so past a couple of ticks it is FAILURE,
// not slowness. It is load-bearing rather than belt-and-braces: while `dead` is true the
// player can neither pause nor quit (uber @21777), so during this window there is NO MANUAL
// EXIT. The deadline IS the exit.
constexpr uint64_t kReviveDeadlineMs = 3000;

// The WATCHDOG's deadline, deliberately looser than the pump's. The pump-side deadline answers
// "the revive ran and did not finish"; this one answers "the revive never ran at all", which
// can only be caused by something larger going wrong, so it allows a slow world-load or a
// stalled frame to recover on its own first.
constexpr uint64_t kWatchdogDeadlineMs = 8000;

// Cached verbs. Resolved together, once, and the arm decision is gated on ALL of them --
// "veto only if the revive is provably possible" (DEATH_ARC section 3.1). Refusing a travel
// we cannot answer strands the player; letting it proceed costs a menu trip.
struct Verbs {
    bool  resolved = false;
    void* removeFromParent = nullptr;  // UWidget::RemoveFromParent
    void* isInViewport = nullptr;      // UUserWidget::IsInViewport
    void* setVisibility = nullptr;     // UWidget::SetVisibility
    void* setActiveIdx = nullptr;      // UWidgetSwitcher::SetActiveWidgetIndex
    int32_t offPauseMenu = -1;         // mainGamemode_C.pause_mainMenu
    int32_t offCanvasLoading = -1;     // ui_menu_C.canvas_loading
    int32_t offScreenSwi = -1;         // ui_menu_C.screenSwi
    // The damage indicator's four directional accumulators. `[V]` `Add Player Damage`
    // @4269 does `VictoryFloatPlusEquals(gamemode.playerInterface.umg_damageIndicator
    // .damage_{up,down,left,right}, damage/maxHealth*4)` -- it ACCUMULATES, choosing the
    // quadrant from the hit's direction. So the hit that killed the player leaves red on
    // the screen, and after a revive at full health that is DEATH STATE LEAKING PAST THE
    // REVIVE: the same class as the black screen, found the same way (the user saw it).
    int32_t offPlayerInterface = -1;   // mainGamemode_C.playerInterface  (ui_UI_C)
    int32_t offDamageIndicator = -1;   // ui_UI_C.umg_damageIndicator     (ui_damageIndicator_C)
    int32_t offDmgUp = -1, offDmgDown = -1, offDmgLeft = -1, offDmgRight = -1;
    // The two writes the CANCELLED TRAVEL leaves behind. See ReconcileCancelledTravel().
    int32_t offDmgFull = -1;        // ui_damageIndicator_C.dmg_full  (UImage*)
    int32_t offTravelOption = -1;   // mainGameInstance_C.NewVar_1    (FString)
};
Verbs g_verbs;

// `FindFunction` is EXACT-OWNER (no SuperStruct climb), so every engine verb resolves off the
// class that DECLARES it -- `Widget` for RemoveFromParent/SetVisibility, `UserWidget` for
// IsInViewport, `WidgetSwitcher` for SetActiveWidgetIndex. Resolving them off the BP class
// returns null and makes the whole seam look unavailable (laptop.cpp:128-131's lesson).
bool ResolveVerbs() {
    if (g_verbs.resolved) return true;
    Verbs v;
    void* widgetCls = R::FindClass(P::name::WidgetClass);
    void* userWidgetCls = R::FindClass(P::name::UserWidgetClass);
    void* switcherCls = R::FindClass(L"WidgetSwitcher");
    void* gmCls = R::FindClass(P::name::GamemodeClass);
    void* menuCls = R::FindClass(L"ui_menu_C");
    if (widgetCls) {
        v.removeFromParent = R::FindFunction(widgetCls, L"RemoveFromParent");
        v.setVisibility = R::FindFunction(widgetCls, P::name::WidgetSetVisibilityFn);
    }
    if (userWidgetCls) v.isInViewport = R::FindFunction(userWidgetCls, L"IsInViewport");
    if (switcherCls)   v.setActiveIdx = R::FindFunction(switcherCls, L"SetActiveWidgetIndex");
    if (gmCls) {
        v.offPauseMenu = R::FindPropertyOffset(gmCls, L"pause_mainMenu");
        v.offPlayerInterface = R::FindPropertyOffset(gmCls, L"playerInterface");
    }
    if (void* uiCls = R::FindClass(L"ui_UI_C"))
        v.offDamageIndicator = R::FindPropertyOffset(uiCls, L"umg_damageIndicator");
    if (void* dmgCls = R::FindClass(L"ui_damageIndicator_C")) {
        v.offDmgUp = R::FindPropertyOffset(dmgCls, L"damage_up");
        v.offDmgDown = R::FindPropertyOffset(dmgCls, L"damage_down");
        v.offDmgLeft = R::FindPropertyOffset(dmgCls, L"damage_left");
        v.offDmgRight = R::FindPropertyOffset(dmgCls, L"damage_right");
    }
    if (menuCls) {
        v.offCanvasLoading = R::FindPropertyOffset(menuCls, L"canvas_loading");
        v.offScreenSwi = R::FindPropertyOffset(menuCls, L"screenSwi");
    }
    if (void* dmgCls2 = R::FindClass(L"ui_damageIndicator_C"))
        v.offDmgFull = R::FindPropertyOffset(dmgCls2, L"dmg_full");
    if (void* giCls = R::FindClass(P::name::GameInstanceClass))
        v.offTravelOption = R::FindPropertyOffset(giCls, L"NewVar_1");
    const bool ok = v.removeFromParent && v.isInViewport && v.setVisibility &&
                    v.setActiveIdx && v.offPauseMenu >= 0 && v.offCanvasLoading >= 0 &&
                    v.offScreenSwi >= 0;
    if (!ok) return false;  // classes load with gameplay; retry next tick, quietly
    v.resolved = true;
    g_verbs = v;
    UE_LOGI("death_revive: revive verbs resolved (remove=%p inViewport=%p setVis=%p "
            "setIdx=%p pause_mainMenu=0x%X canvas_loading=0x%X screenSwi=0x%X)",
            v.removeFromParent, v.isInViewport, v.setVisibility, v.setActiveIdx,
            v.offPauseMenu, v.offCanvasLoading, v.offScreenSwi);
    return true;
}

// ---- THE VETO. DATA ONLY. Runs inside the OpenLevel detour. -------------------------------
//
// Every early return is "let the travel proceed", which is the fail-CLOSED direction: a
// player who reaches the menu is recoverable, a player stranded in a world we refused to
// leave for a reason we could not evaluate is not.
bool VetoOpenLevel(void* /*worldContextObject*/, uint64_t /*levelName*/, bool /*bAbsolute*/) {
    if (!g_armed.load(std::memory_order_acquire)) return false;

    // The session test, and the reason no authorship bracket exists: our own flees call
    // Session::Stop() BEFORE travelling, so a flee's travel arrives with running() == false
    // and passes through here untouched.
    coop::net::Session* s = g_session.load(std::memory_order_relaxed);
    if (!s || !s->running()) return false;

    // O(1) GUObjectArray slot read -- no dispatch, no lock, and it rejects a recycled or
    // PendingKill slot without ever dereferencing the pawn's own (possibly freed) memory.
    void* pawn = g_pawn.load(std::memory_order_acquire);
    const int32_t idx = g_pawnIdx.load(std::memory_order_relaxed);
    if (!pawn || !R::IsLiveByIndex(pawn, idx)) return false;

    const int32_t off = g_deadByte.load(std::memory_order_relaxed);
    const uint8_t mask = g_deadMask.load(std::memory_order_relaxed);
    if (off < 0 || mask == 0) return false;
    if ((*(reinterpret_cast<uint8_t*>(pawn) + off) & mask) == 0)
        return false;  // not a death travel -- a quit, a save load, the backrooms

    g_revivePending.store(true, std::memory_order_release);
    // Stamp the clock HERE, in the veto, not in the pump. The watchdog below has to be able to
    // recover from "we refused a travel and then the pump never came back", and a deadline
    // armed by the pump cannot bound a failure of the pump. Data-only: one tick count.
    g_vetoAtMsAtomic.store(::GetTickCount64(), std::memory_order_release);
    return true;  // CANCEL: SetClientTravel never runs, so no travel is ever requested
}

// ---- the revive's individual writes -------------------------------------------------------

bool CallNoArgs(void* obj, void* fn) {
    if (!obj || !fn || !R::IsLive(obj)) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(obj, f);
}

// Read `IsInViewport` off a widget. `have` distinguishes "false" from "could not ask".
bool ReadInViewport(void* widget, bool& out) {
    if (!widget || !g_verbs.isInViewport || !R::IsLive(widget)) return false;
    ue_wrap::ParamFrame f(g_verbs.isInViewport);
    if (!f.valid() || !ue_wrap::Call(widget, f)) return false;
    out = f.Get<bool>(L"ReturnValue");
    return true;
}

// Detach the death's black screen. Returns true once it is provably off the viewport --
// which includes the case where there is none to remove (an early revive, before +5 s).
bool ClearBlackScreen() {
    void* w = R::FindObjectByClass(kBlackScreenClass);
    if (!w || !R::IsLive(w)) return true;  // nothing on screen
    CallNoArgs(w, g_verbs.removeFromParent);
    bool inViewport = true;
    if (!ReadInViewport(w, inViewport)) return false;  // cannot verify -> not done
    return !inViewport;
}

// Undo `lib.loadLevel`'s menu prep. NOT cosmetic: `[V]` `pause_mainMenu` is
// `AddToViewport(3)`'d once at gamemode init and lives on the screen tree all session, merely
// Collapsed -- so `loadLevel`'s two writes stick, and a revived player who presses ESC gets a
// LOADING SCREEN where the pause menu should be.
bool RestoreMenuPrep() {
    void* gm = R::FindObjectByClass(P::name::GamemodeClass);
    if (!gm || !R::IsLive(gm)) return false;
    void* menu = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(gm) +
                                                 g_verbs.offPauseMenu);
    if (!menu || !R::IsLive(menu)) return false;
    auto* menuBytes = reinterpret_cast<uint8_t*>(menu);
    void* canvas = *reinterpret_cast<void* const*>(menuBytes + g_verbs.offCanvasLoading);
    void* switcher = *reinterpret_cast<void* const*>(menuBytes + g_verbs.offScreenSwi);
    if (!canvas || !switcher || !R::IsLive(canvas) || !R::IsLive(switcher)) return false;

    bool ok = true;
    {
        ue_wrap::ParamFrame f(g_verbs.setVisibility);
        if (!f.valid()) return false;
        f.Set<uint8_t>(L"InVisibility", kSlateCollapsed);
        ok = ue_wrap::Call(canvas, f) && ok;
    }
    {
        ue_wrap::ParamFrame f(g_verbs.setActiveIdx);
        if (!f.valid()) return false;
        f.Set<int32_t>(L"Index", kScreenSwitcherInGameIndex);
        ok = ue_wrap::Call(switcher, f) && ok;
    }
    return ok;
}

// Clear the damage indicator's four directional accumulators.
//
// DELIBERATELY BEST-EFFORT: it is NOT in the arm gate and NOT in the revive's conjunction.
// A player wearing red is alive, standing, at the KPP and can open the menu; failing the
// conjunction over it would flee to the main menu, which is a far worse outcome than a few
// seconds of red. The ACCEPTANCE instrument is stricter than this and asserts it, so a
// regression is still visible -- the runtime's give-up threshold and the test's bar are
// different questions and are allowed to differ.
//
// Returns the largest value it found, or a negative number if the chain did not resolve, so
// the caller can report what it actually saw rather than just "tried".
// ---- RECONCILE WHAT THE CANCELLED TRAVEL WOULD HAVE MADE MOOT ----------------------------
//
// The `OpenLevel` we refuse is not just a camera move: it is how vanilla VOTV DISPOSES of
// every one-way write the death made. The game never needs an undo because the world and the
// HUD are about to be destroyed and rebuilt. We keep them, so the disposal is ours.
//
// This is deliberately NOT "clear the artifact the user noticed". Twice now a red artifact was
// found by the USER LOOKING AT THE SCREEN and answered by appending one more named field to
// this file (11.2 item 7 records the first). The third time it is a CENSUS instead: every
// function of the death EPISODE was disassembled from the cooked pak and every persistent
// write enumerated -- `Add Player Damage`, `ragdollMode`, the ubergraph death chain, and
// `lib_C::loadLevel` -- then each classified as "the alive path restores it", "this revive
// already restores it", or OWED. The full table is docs/DEATH_ARC.md 11.3b. It came back with
// exactly THREE writes nothing undoes, and two of them are handled here:
//
//   [1] `ui_damageIndicator_C.dmg_full.Visibility` -- `[V]` the Tick's death branch does
//       `@2292 SetVisibility(b0 = Visible)` while the widget's EXPORT authors it Collapsed,
//       and the ALIVE path never writes that field at all. A one-way latch: once `dead` has
//       been true for a single tick, a full-screen `inst_dmg_center` (mat_dmgInd, unlit
//       translucent, alpha 1.0) draws over everything FOREVER. This is the red screen the
//       user reported, and it is the only part of that report that survives a revive.
//
//   [2] `mainGameInstance_C.NewVar_1` -- `[V]` the death calls
//       `lib_C::loadLevel('menu', 'PQXYyeofZ8cr5rJD4YXLVw', ...)`, whose @138 stores that
//       magic option ON THE GAME INSTANCE, which outlives every level. It is the DEATH SIGNAL
//       the next level reads: `mainGamemode` uber @12232 compares it against that exact
//       literal and @13245 clears it. Our veto means it is set and NEVER consumed, so it sits
//       armed for the rest of the process -- and @12322's branch off it reaches
//       `lib_C::end(self)` when the game is paused. Clearing it to "" is precisely what the
//       travel would have caused, and it is the game's own reset value.
//
// The THIRD write is `gameInstance.subArea := 'None'` (uber @4489 and loadLevel @52) and it is
// deliberately LEFT ALONE: the revive teleports the player to the KPP, which is outdoors, so
// `None` is the CORRECT sub-area for where they now are, and the game's own trigger volumes
// re-establish it on the next building they enter. Restoring the pre-death value would be the
// bug. Recorded here so the next reader does not "fix" it.
//
// Best-effort by design: a failure here is cosmetic-or-latent, never a reason to strand the
// player, so this does NOT enter the revive's completion conjunction (11.2's lesson -- gating
// on a screen artifact threw a LIVING player to the menu one run in four).
void ReconcileCancelledTravel() {
    if (ReconcileDisabled()) {
        UE_LOGI("death_revive: RECONCILE SUPPRESSED (VOTVCOOP_DEATH_NO_RECONCILE=1) -- the "
                "death's un-disposed writes are being LEFT for the write-diff instrument to "
                "find. This is the negative-control arm; the travel is still refused and the "
                "player is still revived.");
        return;
    }

    // [1] the HUD latch.
    if (g_verbs.offPlayerInterface >= 0 && g_verbs.offDamageIndicator >= 0 &&
        g_verbs.offDmgFull >= 0 && g_verbs.setVisibility) {
        void* gm = R::FindObjectByClass(P::name::GamemodeClass);
        if (gm && R::IsLive(gm)) {
            void* ui = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) +
                                                 g_verbs.offPlayerInterface);
            if (ui && R::IsLive(ui)) {
                void* ind = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ui) +
                                                     g_verbs.offDamageIndicator);
                if (ind && R::IsLive(ind)) {
                    void* full = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ind) +
                                                          g_verbs.offDmgFull);
                    if (full && R::IsLive(full)) {
                        ue_wrap::ParamFrame f(g_verbs.setVisibility);
                        if (f.valid()) {
                            f.Set<uint8_t>(L"InVisibility", kSlateCollapsed);
                            UE_LOGI("death_revive: dmg_full -> Collapsed (its authored default; "
                                    "the death branch's one-way SetVisibility latch) ok=%d",
                                    ue_wrap::Call(full, f) ? 1 : 0);
                        }
                    }
                }
            }
        }
    }

    // [2] the death signal the cancelled travel never delivered.
    if (g_verbs.offTravelOption >= 0) {
        void* gi = R::FindObjectByClass(P::name::GameInstanceClass);
        if (gi && R::IsLive(gi)) {
            const bool ok = ue_wrap::field_io::WriteFStringField(gi, g_verbs.offTravelOption, L"");
            UE_LOGI("death_revive: gameInstance.NewVar_1 cleared (the 'PQXYyeofZ8cr5rJD4YXLVw' "
                    "death option a vetoed travel leaves armed) ok=%d", ok ? 1 : 0);
        }
    }
}

float ClearDamageIndicator(float* outBefore = nullptr) {
    if (g_verbs.offPlayerInterface < 0 || g_verbs.offDamageIndicator < 0 ||
        g_verbs.offDmgUp < 0 || g_verbs.offDmgDown < 0 ||
        g_verbs.offDmgLeft < 0 || g_verbs.offDmgRight < 0)
        return -1.f;
    void* gm = R::FindObjectByClass(P::name::GamemodeClass);
    if (!gm || !R::IsLive(gm)) return -1.f;
    void* ui = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(gm) +
                                               g_verbs.offPlayerInterface);
    if (!ui || !R::IsLive(ui)) return -1.f;
    void* ind = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(ui) +
                                                g_verbs.offDamageIndicator);
    if (!ind || !R::IsLive(ind)) return -1.f;
    auto* base = reinterpret_cast<uint8_t*>(ind);
    const int32_t offs[4] = {g_verbs.offDmgUp, g_verbs.offDmgDown,
                             g_verbs.offDmgLeft, g_verbs.offDmgRight};
    // Returns the worst value found AFTER the write, for the same reason ExpireBloodLoss
    // counts actors: a cleanup's completion test must not read back its own write. Here the
    // write IS the whole operation (there is no owning actor to wait on), so post-write is
    // both honest and immediate -- but the caller that reports what it CLEARED wants the
    // pre-write value, so both are returned.
    float before = 0.f;
    for (int32_t off : offs) {
        float* f = reinterpret_cast<float*>(base + off);
        if (*f > before) before = *f;
        *f = 0.f;
    }
    if (outBefore) *outBefore = before;
    float after = 0.f;
    for (int32_t off : offs) {
        const float v = *reinterpret_cast<const float*>(base + off);
        if (v > after) after = v;
    }
    return after;
}

// Expire the death's BLOOD-LOSS effect.
//
// `[V]` `Add Player Damage` @3414 also calls `lib_C::addEffect('bloodLoss', strength, time,
// ...)` -> `mainGamemode::addEffect`, which SPAWNS an `effect_bloodLoss_C` actor carrying
// `time` / `maxTime` / `strength` and tracks it in `gamemode.effects` + `effects_names`.
// `[V]` that actor owns a `PostProcessComponent` and a `ui_bloodLossBlur_C` widget -- it is
// the red wash over the whole world, and it is a SECOND, independent red from the damage
// indicator's quadrants.
//
// `[V]` THE DURATION IS PINNED AT ITS CAP BY ANY LETHAL HIT. @2838: time =
// FClamp(Lerp(10,5,maxHealth/100) * (dmg/5 + 0.01) * 1.5, 0, 120) and strength =
// FClamp(Lerp(1,0.2,maxHealth/100) * (dmg/5 + 0.01), 0, 1). At maxHealth 100 a mere 100
// damage already gives 150 -> clamped 120 SECONDS at strength 1.0. So every death leaves two
// full minutes of red on a player the revive has just restored to full health -- which is
// not cosmetic: a revive that maxes health and keeps BLOOD LOSS is incoherent.
//
// We do NOT call the actor's own `destroy` verb, whose body has not been read. We write
// `time := 0` and let the effect's OWN ReceiveTick run the expiry path it runs every time an
// effect ends naturally -- so `gamemode.effects` / `effects_names` cannot desynchronise,
// because nothing unusual happens. Same best-effort status as the damage indicator.
//
// SCOPE, deliberately narrow: ONLY `bloodLoss`. The death adds exactly this one effect; food
// poisoning, LSD, sleepiness and the rest are unrelated to dying and clearing them would be
// the revive helping itself to state it did not author.
float ExpireBloodLoss(float* outWorstTime = nullptr) {
    void* cls = R::FindClass(L"effect_bloodLoss_C");
    if (!cls) return -1.f;
    static int32_t sOffTime = -2;
    if (sOffTime == -2) sOffTime = R::FindPropertyOffset(cls, L"time");
    if (sOffTime < 0) return -1.f;
    // RETURNS THE LIVE ACTOR COUNT, NOT THE TIME -- and that distinction was a real defect.
    // The first version returned the worst `time` it saw BEFORE zeroing, so on the very next
    // call it returned 0 *because this function had written 0*, and the retry loop declared
    // "cleanup complete" one tick later while the actor -- with its PostProcess and its
    // `ui_bloodLossBlur` widget -- could still be standing. A completion test that reads back
    // its own write proves nothing. The actor's own tick is what destroys it (and its
    // `ReceiveDestroyed` is what calls `RemoveFromParent` on the blur widget), so the honest
    // question is "is one still alive", and only the actor's absence answers it.
    int live = 0;
    float worstTime = 0.f;
    for (void* a : R::FindObjectsByClass(L"effect_bloodLoss_C")) {
        if (!a || !R::IsLive(a)) continue;
        ++live;
        float* t = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(a) + sOffTime);
        if (*t > worstTime) worstTime = *t;
        *t = 0.f;
    }
    if (outWorstTime) *outWorstTime = worstTime;
    return static_cast<float>(live);
}

// The `dead` write. The game has NO revive -- `dead := false` exists nowhere in its bytecode
// -- so this is the one value in the whole sequence that only we can author. It goes LAST for
// a measured POSITIVE reason: while `dead` is true the player is invulnerable (`Add Player
// Damage` @659 early-outs) and cannot quit (@21777), so the game's own flag protects the
// revive window. That structurally forecloses the retired KO lane's H2 class -- a hand-rolled
// immunity racing a stale health value.
bool ClearDeadFlag(void* pawn) {
    const int32_t off = g_deadByte.load(std::memory_order_relaxed);
    const uint8_t mask = g_deadMask.load(std::memory_order_relaxed);
    if (!pawn || !R::IsLive(pawn) || off < 0 || mask == 0) return false;
    uint8_t* p = reinterpret_cast<uint8_t*>(pawn) + off;
    *p &= static_cast<uint8_t>(~mask);
    return (*p & mask) == 0;  // read BACK: wrote != holds
}

float DistanceToKpp(void* pawn) {
    const ue_wrap::FVector at = E::GetActorLocation(pawn);
    const float dx = at.X - P::name::kKPPSpawnX;
    const float dy = at.Y - P::name::kKPPSpawnY;
    const float dz = at.Z - P::name::kKPPSpawnZ;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---- the revive ---------------------------------------------------------------------------
//
// Six writes, then the CONJUNCTION -- every one verified by read-back rather than by the
// return value of the call that was supposed to cause it. A six-write sequence cannot be
// pre-proven; the honest split is pre-flight what is cheaply resolvable, verify the rest by
// reading the world back, and leave for the menu if the world disagrees.
bool RunRevive(coop::net::Session& session, void* pawn) {
    if (!pawn || !R::IsLive(pawn)) return false;

    // 1. the black screen. First because it is what the player is staring at, and `[V]`
    //    removing it does not perturb the chain (travel landed at 10 703 ms with it removed
    //    vs 10 672 ms with it left alone).
    const bool blackCleared = ClearBlackScreen();

    // 1b. the damage indicator. Same class as the black screen: an artifact of the death
    //     that the level travel used to dispose of. Best-effort (see the function).
    float redBefore = 0.f;
    ClearDamageIndicator(&redBefore);

    // 1b. dispose of what the cancelled travel would have disposed of -- the census, not the
    //     artifact the user happened to see. Best-effort; NOT a conjunction term.
    ReconcileCancelledTravel();
    float bloodTimeBefore = 0.f;
    const float bloodActors = ExpireBloodLoss(&bloodTimeBefore);

    // 2. health, BEFORE `dead` is cleared -- while dead the player takes no damage, so this
    //    is the window in which restoring it cannot race an incoming hit. Full maxHealth is a
    //    product choice: a native death would have reloaded the save, so there is no prior
    //    value to inherit, and the game's own regen (+dt/6, uber @56667) resumes from here.
    float maxHp = 100.f;
    if (!V::Read(V::Field::MaxHealth, &maxHp) || maxHp <= 0.f) maxHp = 100.f;
    const bool vitalsOk = V::Write(V::Field::Health, maxHp);

    // 3. stand up. `forceWakeup` is the UNCONDITIONAL one (uber @25800): movement mode,
    //    capsule collision, camera re-attach, SetSimulatePhysics(false), EnableInput, mesh
    //    re-attach, `isRagdoll := false` (@26497 -- this is what re-opens the pause gate), and
    //    SetControlRotation. `[V]` it never calls `fallen()`, so it cannot re-arm the chain.
    //    NOT `forceGetUp` (a 0.2 s latent delay, and it lands in @39685 which re-reads `dead`)
    //    and NOT `wakeup` (refuses while dead, @26584).
    const bool wokeUp = E::ForceMainPlayerWakeup(pawn);

    // 4. reposition. USER decision 2026-08-31: the КПП, not `spawnLocation` -- which `[V]` is
    //    just `GetTransform()` at level start (uber @34642) and can be kilometres from the
    //    corpse after a long session. This is the same landmark the client teleport already
    //    uses on join.
    const bool teleReported = coop::teleport_client::ApplyLocally(
        {P::name::kKPPSpawnX, P::name::kKPPSpawnY, P::name::kKPPSpawnZ, 0.f, 0.f, 0.f});

    // 5. the menu prep `loadLevel` left behind.
    const bool menuRestored = RestoreMenuPrep();

    // 6. and only now the flag.
    const bool deadCleared = ClearDeadFlag(pawn);

    // ---- the conjunction ------------------------------------------------------------------
    // `[V]` the position is readable in the SAME frame as the write -- the game's own
    // `teleportWObackrooms` reads `K2_GetActorLocation()` back one statement after its
    // `K2_SetActorLocation` (@980 -> @1020) and stores it in `lastLoc`.
    bool isRagdoll = false, deadNow = true;
    const bool haveState = E::ReadMainPlayerRagdollState(pawn, isRagdoll, deadNow);
    float hp = -1.f;
    const bool haveHp = V::Read(V::Field::Health, &hp);
    const float dist = DistanceToKpp(pawn);
    // The positional term is LOAD-BEARING, not belt-and-braces: `ApplyLocally` has a
    // three-tier fallback, so its report says a call succeeded, not that the player moved.
    const bool atKpp = dist <= kAtKppToleranceCm;

    // THE CONJUNCTION IS ABOUT THE PLAYER, NOT ABOUT THE SCREEN -- corrected 2026-08-31 by a
    // run that failed. `blackCleared` used to be a term here, and on 1 run in 4 the
    // same-frame `IsInViewport` read after `RemoveFromParent` still came back TRUE, so the
    // conjunction refused and the player was thrown to the MAIN MENU (ending the lobby)
    // because a WIDGET had not detached yet. That trade is upside down: a black screen on a
    // living player is bad, being ejected from the world is worse, and it is exactly the
    // failure mode this arc exists to remove. The screen artifacts are now RETRIED on the
    // following pump ticks (TickScreenCleanup) instead of being a pass/fail gate.
    const bool ok = vitalsOk && wokeUp && teleReported && menuRestored &&
                    deadCleared && haveState && !isRagdoll && !deadNow && haveHp && hp > 0.f &&
                    atKpp;

    UE_LOGI("death_revive: REVIVE %s -- vitals=%d wake=%d tele=%d menu=%d deadClr=%d "
            "| screen (retried, NOT a gate): black=%d dmgRed=%.2f bloodLoss=%.0f actor(s) @%.1fs "
            "| readback: ragdoll=%d dead=%d hp=%.1f distKPP=%.0f cm (tol %.0f)",
            ok ? "OK" : "FAILED", vitalsOk ? 1 : 0, wokeUp ? 1 : 0,
            teleReported ? 1 : 0, menuRestored ? 1 : 0, deadCleared ? 1 : 0, blackCleared ? 1 : 0, redBefore, bloodActors, bloodTimeBefore,
            haveState ? (isRagdoll ? 1 : 0) : -1, haveState ? (deadNow ? 1 : 0) : -1,
            haveHp ? hp : -1.f, dist, kAtKppToleranceCm);
    (void)session;
    return ok;
}

// How many pump ticks after a revive to keep re-attempting the screen cleanup. The measured
// case needed one extra tick; this is generous because each attempt is a no-op the moment the
// artifact is gone, and the whole thing stops as soon as all three read clear.
constexpr int kScreenCleanupTicks = 120;
int g_screenCleanupLeft = 0;

// Re-attempt the three screen artifacts until they are provably gone. Returns true when
// there is nothing left to do. Called from Tick after a successful revive; it is idempotent
// and each call is one class lookup per artifact once they are clear.
bool TickScreenCleanup() {
    const bool black = ClearBlackScreen();
    const float red = ClearDamageIndicator();
    const float bloodActors = ExpireBloodLoss();
    return black && red <= 0.f && bloodActors <= 0.f;
}

// The failure exit. `FleeToMainMenu`'s body IS the backstop -- all ten teardown ops INCLUDING
// `Session::Stop()`, and Stop() is exactly what makes our own veto stand down (its first term
// is a live session), so the travel it then authors passes straight through this seam. No
// disarm, no special case: the invariant does the work.
void GiveUpAndLeave(coop::net::Session& session, const char* why) {
    g_armed.store(false, std::memory_order_release);
    g_revivePending.store(false, std::memory_order_release);
    g_vetoAtMsAtomic.store(0, std::memory_order_release);
    UE_LOGW("death_revive: %s -- falling back to the main-menu flee", why);
    coop::net_pump::FleeToMainMenuOnDeath(session, why);
}

}  // namespace

void Install(coop::net::Session* session) {
    // Always refresh the cached Session -- the veto reads it, and the harness's session
    // object outlives any one session but the pointer is what the veto's first term needs.
    g_session.store(session, std::memory_order_release);
    // Latch BOTH outcomes. This runs on the unconditional 60 Hz timeline tick, so a
    // permanently-failing install (a stale signature) must not re-attempt forever.
    if (g_installDone.exchange(true, std::memory_order_acq_rel)) return;
    if (!LT::Install()) return;  // logged there; ArmedForThisDeath() stays false forever
    LT::SetVeto(&VetoOpenLevel);
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("death_revive: travel veto published -- a coop death now keeps the world "
            "(single-player is untouched: the veto's first term is a live session, so with "
            "no session the seam is armed and refuses nothing)");
}

void OnSessionStart() {
    g_armed.store(false, std::memory_order_release);
    g_revivePending.store(false, std::memory_order_release);
    g_vetoAtMsAtomic.store(0, std::memory_order_release);
    g_wasDead = false;
    g_reviveRanThisDeath = false;
    g_lastReviveOk = false;
    g_vetoAtMs = 0;
    g_screenCleanupLeft = 0;
}

void Tick(coop::net::Session& session, void* localPawn) {
    Install(&session);  // idempotent; also refreshes the cached Session pointer

    // Publish the veto's pawn every tick from state the caller validated this tick.
    if (localPawn) {
        g_pawn.store(localPawn, std::memory_order_release);
        g_pawnIdx.store(R::InternalIndexOf(localPawn), std::memory_order_relaxed);
    } else {
        g_pawn.store(nullptr, std::memory_order_release);
        g_pawnIdx.store(-1, std::memory_order_relaxed);
    }

    // The `dead` byte+mask. Property LAYOUT is stable for a game build, so this resolves once
    // -- but it resolves through FindBoolProperty (byte + MASK) rather than the plain-byte
    // read the sender path uses, because we WRITE it and a masked field shares its byte.
    if (g_deadByte.load(std::memory_order_relaxed) < 0 && localPawn && R::IsLive(localPawn)) {
        int32_t b = -1; uint8_t m = 0;
        if (R::FindBoolProperty(R::ClassOf(localPawn), L"dead", b, m)) {
            g_deadMask.store(m, std::memory_order_relaxed);
            g_deadByte.store(b, std::memory_order_release);
        }
    }

    const bool verbsOk = ResolveVerbs();

    bool isRagdoll = false, dead = false;
    const bool haveState = localPawn && E::ReadMainPlayerRagdollState(localPawn, isRagdoll, dead);

    // ---- the arm decision, on the `dead` RISING EDGE ---------------------------------------
    // Ten seconds before the travel, and it is the SAME decision as "should net_pump flee?".
    if (haveState && dead && !g_wasDead) {
        const bool canRevive = g_installed.load(std::memory_order_acquire) && verbsOk &&
                               session.running() &&
                               g_deadByte.load(std::memory_order_relaxed) >= 0;
        g_reviveRanThisDeath = false;
        if (canRevive) {
            g_armed.store(true, std::memory_order_release);
            UE_LOGI("death_revive: local death ARMED -- the native death runs to completion "
                    "(~10 s: sound, black screen at +5 s) and the level travel will be refused");
        } else {
            g_armed.store(false, std::memory_order_release);
            UE_LOGW("death_revive: local death NOT armed (seam=%d verbs=%d session=%d deadOff=%d) "
                    "-- net_pump's flee handles this death",
                    g_installed.load(std::memory_order_acquire) ? 1 : 0, verbsOk ? 1 : 0,
                    session.running() ? 1 : 0,
                    g_deadByte.load(std::memory_order_relaxed));
        }
    }
    if (haveState && !dead && g_wasDead) {
        // The death ended (revived, or the flag cleared some other way). Disarm so a later
        // unrelated travel is never refused by a stale arm.
        g_armed.store(false, std::memory_order_release);
        g_vetoAtMs = 0;
    }
    if (haveState) g_wasDead = dead;

    // ---- the deferred revive ---------------------------------------------------------------
    if (g_revivePending.load(std::memory_order_acquire)) {
        if (g_vetoAtMs == 0) {
            g_vetoAtMs = ::GetTickCount64();
            UE_LOGI("death_revive: level travel REFUSED at UGameplayStatics::OpenLevel -- the "
                    "world is kept; reviving on this pump task");
        }
        if (!localPawn || !R::IsLive(localPawn)) {
            if (::GetTickCount64() - g_vetoAtMs > kReviveDeadlineMs)
                GiveUpAndLeave(session, "no local pawn to revive after the travel was refused");
            return;
        }
        g_revivePending.store(false, std::memory_order_release);
        g_reviveRanThisDeath = true;
        g_lastReviveOk = RunRevive(session, localPawn);
        if (g_lastReviveOk) {
            g_armed.store(false, std::memory_order_release);
            g_vetoAtMs = 0;
            g_vetoAtMsAtomic.store(0, std::memory_order_release);
            g_screenCleanupLeft = kScreenCleanupTicks;
        } else {
            GiveUpAndLeave(session, "the revive did not complete its conjunction");
        }
        return;
    }

    // The screen artifacts, retried until gone. Deliberately AFTER the pending-revive block
    // and outside every gate above it: it must keep running once the death is over.
    if (g_screenCleanupLeft > 0) {
        if (TickScreenCleanup()) {
            UE_LOGI("death_revive: screen cleanup complete (black screen, damage indicator and "
                    "bloodLoss all clear) after %d of %d retry ticks",
                    kScreenCleanupTicks - g_screenCleanupLeft + 1, kScreenCleanupTicks);
            g_screenCleanupLeft = 0;
        } else if (--g_screenCleanupLeft == 0) {
            UE_LOGW("death_revive: screen cleanup did NOT finish in %d ticks -- the player is "
                    "alive and playable but something red or black may still be on screen. "
                    "This is deliberately NOT a reason to leave the world.",
                    kScreenCleanupTicks);
        }
    }

    // The deadline. A revive is six synchronous writes on ONE tick, so an armed death still
    // holding `dead` this long after the travel was refused is a failure, not slowness -- and
    // the player has no manual exit while `dead` is set, so this IS their exit.
    if (g_vetoAtMs != 0 && haveState && dead && g_reviveRanThisDeath &&
        ::GetTickCount64() - g_vetoAtMs > kReviveDeadlineMs) {
        GiveUpAndLeave(session, "still dead past the revive deadline");
    }
}

void Watchdog() {
    // THE ONE FAILURE THIS ARC CANNOT BE ALLOWED TO HAVE: a travel refused, and then nothing.
    //
    // The revive runs from `Tick`, which net_pump calls only inside
    // `if (g_netLocal.Raw() && !g_localDeathHandled)`, itself inside a session-gated block,
    // itself only reached while `session.running()`. If ANY of those stops between the veto and
    // the revive, the pending flag is never consumed, the pump-side deadline (armed in `Tick`)
    // is never even started, and the player is left DEAD IN A WORLD WE REFUSED TO LEAVE -- with
    // no manual exit at all, because `[V]` a dead player cannot open the pause menu (mainPlayer
    // uber @21777 ORs `dead` into the deny). No amount of care inside `Tick` can cover a failure
    // OF `Tick`, which is why this lives on the unconditional timeline tick instead.
    if (!g_revivePending.load(std::memory_order_acquire)) return;
    const uint64_t at = g_vetoAtMsAtomic.load(std::memory_order_acquire);
    if (at == 0) return;
    if (::GetTickCount64() - at <= kWatchdogDeadlineMs) return;

    // Take the flag first so this fires once, then hand the flee to the game thread -- we are
    // on the timeline thread here and every op inside the flee is game-thread-only.
    g_revivePending.store(false, std::memory_order_release);
    g_vetoAtMsAtomic.store(0, std::memory_order_release);
    g_armed.store(false, std::memory_order_release);
    UE_LOGE("death_revive: WATCHDOG -- a level travel was refused %llu ms ago and no revive ever "
            "ran (the pump stopped ticking between the veto and the revive). Leaving the world "
            "rather than stranding a dead player who cannot even open the pause menu.",
            static_cast<unsigned long long>(kWatchdogDeadlineMs));
    ue_wrap::game_thread::Post([] {
        if (coop::net::Session* s = g_session.load(std::memory_order_acquire))
            coop::net_pump::FleeToMainMenuOnDeath(*s, "revive never ran after a refused travel");
    });
}

bool ArmedForThisDeath() { return g_armed.load(std::memory_order_acquire); }
bool SeamInstalled() { return LT::IsInstalled(); }
unsigned long long TravelsRefused() { return LT::VetoCount(); }
bool LastReviveSucceeded() { return g_lastReviveOk; }

bool ReconcileDisabled() {
    static const bool s = (coop::config::ReadEnv("VOTVCOOP_DEATH_NO_RECONCILE") == "1");
    return s;
}

}  // namespace coop::death_revive
