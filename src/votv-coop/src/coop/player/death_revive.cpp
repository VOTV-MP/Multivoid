// coop/player/death_revive.cpp -- the coop death: the native death runs to completion, the level
// travel it ends in is refused, and the player is revived in place. The design and the bounds
// are in the header.

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

// The widget the death chain adds at +5 s. It has no function or ubergraph export; the level
// travel is what disposes of it, so a cancelled travel keeps a permanent black screen unless it
// is removed. RemoveFromParent detaches rather than destroys, so the completion test is
// IsInViewport, never findability.
constexpr const wchar_t* kBlackScreenClass = L"blackScreen_C";

// ui_menu_C.screenSwi's in-game value: the widget sets 1 for itself outside the menu level, and
// lib.loadLevel stomps it to 0 on the way to the menu.
constexpr int32_t kScreenSwitcherInGameIndex = 1;

// ESlateVisibility::Collapsed: canvas_loading is authored Collapsed, and the only writer in the
// game is lib.loadLevel, which sets it Visible, so the restore value is a constant off the asset.
constexpr uint8_t kSlateCollapsed = 1;

// How close to the KPP the player must land for the reposition to count. The teleport either
// lands or falls through its tiers, so this is an assertion, not a drift tolerance.
constexpr float kAtKppToleranceCm = 300.f;

// The veto's inputs, published by Tick and read by the detour: plain values only, since the veto
// may not dispatch, allocate or lock.
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

// Pump-side state, game thread only.
bool  g_wasDead = false;
bool  g_lastReviveOk = false;
bool  g_reviveRanThisDeath = false;
uint64_t g_vetoAtMs = 0;

// The revive is six synchronous writes on one tick, so past a few ticks it is failure, not
// slowness. While dead the player can neither pause nor quit, so this window has no manual
// exit; the deadline is the exit.
constexpr uint64_t kReviveDeadlineMs = 3000;

// The watchdog's deadline, looser than the pump's: the pump's answers "the revive ran and did
// not finish", this one "the revive never ran", which needs something larger to have gone
// wrong, so a slow world load gets time to recover first.
constexpr uint64_t kWatchdogDeadlineMs = 8000;

// Cached verbs, resolved together once; the arm decision is gated on all of them. A travel
// refused without a possible revive strands the player; one let through costs a menu trip.
struct Verbs {
    bool  resolved = false;
    void* removeFromParent = nullptr;  // UWidget::RemoveFromParent
    void* isInViewport = nullptr;      // UUserWidget::IsInViewport
    void* setVisibility = nullptr;     // UWidget::SetVisibility
    void* setActiveIdx = nullptr;      // UWidgetSwitcher::SetActiveWidgetIndex
    int32_t offPauseMenu = -1;         // mainGamemode_C.pause_mainMenu
    int32_t offCanvasLoading = -1;     // ui_menu_C.canvas_loading
    int32_t offScreenSwi = -1;         // ui_menu_C.screenSwi
    // The damage indicator's four directional accumulators. Add Player Damage accumulates
    // damage/maxHealth*4 into the quadrant of the hit, so the killing hit leaves red on screen past
    // a revive to full health.
    int32_t offPlayerInterface = -1;   // mainGamemode_C.playerInterface  (ui_UI_C)
    int32_t offDamageIndicator = -1;   // ui_UI_C.umg_damageIndicator     (ui_damageIndicator_C)
    int32_t offDmgUp = -1, offDmgDown = -1, offDmgLeft = -1, offDmgRight = -1;
    // The two writes the cancelled travel leaves behind. See ReconcileCancelledTravel().
    int32_t offDmgFull = -1;        // ui_damageIndicator_C.dmg_full  (UImage*)
    int32_t offTravelOption = -1;   // mainGameInstance_C.NewVar_1    (FString)
};
Verbs g_verbs;

// FindFunction is exact-owner, so each engine verb resolves off the class that declares it:
// Widget for RemoveFromParent and SetVisibility, UserWidget for IsInViewport, WidgetSwitcher
// for SetActiveWidgetIndex. Off the BP class they return null.
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

// The veto, data only, inside the OpenLevel detour. Every early return lets the travel proceed,
// the fail-closed direction: a player at the menu is recoverable, a player stranded in a world
// we refused to leave for a reason we could not evaluate is not.
bool VetoOpenLevel(void* /*worldContextObject*/, uint64_t /*levelName*/, bool /*bAbsolute*/) {
    if (!g_armed.load(std::memory_order_acquire)) return false;

    // The session test, which is why no authorship bracket exists: our own flees call Session::Stop
    // before travelling, so a flee's travel arrives with running() false and passes.
    coop::net::Session* s = g_session.load(std::memory_order_relaxed);
    if (!s || !s->running()) return false;

    // A slot read: no dispatch, no lock, and a recycled or PendingKill slot is rejected without
    // touching the pawn's memory.
    void* pawn = g_pawn.load(std::memory_order_acquire);
    const int32_t idx = g_pawnIdx.load(std::memory_order_relaxed);
    if (!pawn || !R::IsLiveByIndex(pawn, idx)) return false;

    const int32_t off = g_deadByte.load(std::memory_order_relaxed);
    const uint8_t mask = g_deadMask.load(std::memory_order_relaxed);
    if (off < 0 || mask == 0) return false;
    if ((*(reinterpret_cast<uint8_t*>(pawn) + off) & mask) == 0)
        return false;  // not a death travel -- a quit, a save load, the backrooms

    g_revivePending.store(true, std::memory_order_release);
    // The clock is stamped in the veto, not the pump: the watchdog must bound "a travel refused and
    // the pump never came back", and a deadline armed by the pump cannot bound a failure of the
    // pump.
    g_vetoAtMsAtomic.store(::GetTickCount64(), std::memory_order_release);
    return true;  // CANCEL: SetClientTravel never runs, so no travel is ever requested
}

// The revive's individual writes.

bool CallNoArgs(void* obj, void* fn) {
    if (!obj || !fn || !R::IsLive(obj)) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(obj, f);
}

// IsInViewport off a widget; the return distinguishes "false" from "could not ask".
bool ReadInViewport(void* widget, bool& out) {
    if (!widget || !g_verbs.isInViewport || !R::IsLive(widget)) return false;
    ue_wrap::ParamFrame f(g_verbs.isInViewport);
    if (!f.valid() || !ue_wrap::Call(widget, f)) return false;
    out = f.Get<bool>(L"ReturnValue");
    return true;
}

// Detach the black screen. True once it is provably off the viewport, including when there is
// none yet (a revive before +5 s).
bool ClearBlackScreen() {
    void* w = R::FindObjectByClass(kBlackScreenClass);
    if (!w || !R::IsLive(w)) return true;  // nothing on screen
    CallNoArgs(w, g_verbs.removeFromParent);
    bool inViewport = true;
    if (!ReadInViewport(w, inViewport)) return false;  // cannot verify -> not done
    return !inViewport;
}

// Undo lib.loadLevel's menu prep. pause_mainMenu is added to the viewport once at gamemode init
// and stays on the screen tree all session, merely Collapsed, so loadLevel's two writes stick,
// and a revived player pressing ESC would get a loading screen instead of the pause menu.
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

// Reconcile what the cancelled travel would have made moot: the OpenLevel we refuse is how the
// game disposes of every one-way write the death made. Every persistent write of the death
// episode (Add Player Damage, ragdollMode, the ubergraph death chain, lib_C::loadLevel) was
// enumerated and classified; three are undone by nothing, and two are handled here.
// [1] ui_damageIndicator_C.dmg_full.Visibility: the Tick's death branch sets it Visible while
// the widget authors it Collapsed, and the alive path never writes it, so once dead for a single
// tick a full-screen red image draws over everything for good.
// [2] mainGameInstance_C.NewVar_1: the death's loadLevel stores its menu option on the game
// instance, which outlives every level, and the next level's gamemode reads and clears it.
// Vetoed, it stays armed, and one branch off it reaches lib_C::end while paused; "" is the
// game's own reset value.
// The third, gameInstance.subArea := None, is left alone: the revive puts the player at the KPP,
// outdoors, where None is correct, and the trigger volumes re-establish it.
// Best-effort: a failure here is cosmetic or latent, never a reason to strand the player, so it
// is not a term of the revive's conjunction.
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

// Clear the damage indicator's four accumulators. Best-effort: not in the arm gate and not in
// the conjunction, since a player wearing red is alive and playable, and a flee to the menu over
// it would be worse. The acceptance test asserts it, so a regression stays visible. Returns the
// largest value found, or a negative number if the chain did not resolve.
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
    // The value returned is read after the write: a completion test must not read back its own
    // write, and here the write is the whole operation. The caller reporting what it cleared gets
    // the pre-write value too.
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

// Expire the death's blood-loss effect. Add Player Damage also adds the bloodLoss effect, which
// spawns an effect_bloodLoss_C actor (it creates the ui_bloodLossBlur_C widget, the red wash
// over the world, independent of the damage indicator) tracked in
// gamemode.effects. Any lethal hit pins its duration at the 120 s cap at full strength, so
// every death leaves two minutes of red on a player restored to full health. The actor's own
// destroy verb is not called: time is set to 0 and the effect's own tick runs its natural
// expiry, so the gamemode's effect lists cannot desynchronise. Only bloodLoss: the death adds
// exactly this one effect, and the others are not the revive's to clear.
float ExpireBloodLoss(float* outWorstTime = nullptr) {
    void* cls = R::FindClass(L"effect_bloodLoss_C");
    if (!cls) return -1.f;
    static int32_t sOffTime = -2;
    if (sOffTime == -2) sOffTime = R::FindPropertyOffset(cls, L"time");
    if (sOffTime < 0) return -1.f;
    // Returns the live actor count, not the time: a version that returned the worst time seen
    // before zeroing read back its own write on the next call and declared the cleanup complete
    // while the actor and its blur widget could still stand. The actor's own tick
    // destroys it (its ReceiveDestroyed removes the blur widget), so only its absence answers.
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

// The dead write. The game has no revive (dead := false exists nowhere in its bytecode), so this
// is the one value only we author. It goes last: while dead the player is invulnerable (Add
// Player Damage early-outs) and cannot quit, so the game's own flag protects the revive window.
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

// The revive: six writes, then the conjunction, each verified by reading the world back rather
// than by the return of the call meant to cause it. A six-write sequence cannot be pre-proven;
// what is cheaply resolvable is pre-flighted, the rest is read back, and the player leaves for
// the menu if the world disagrees.
bool RunRevive(coop::net::Session& session, void* pawn) {
    if (!pawn || !R::IsLive(pawn)) return false;

    // 1. The black screen, first because it is what the player is staring at; removing it does not
    //    perturb the chain.
    const bool blackCleared = ClearBlackScreen();

    // 1b. The damage indicator: an artifact the travel used to dispose of. Best-effort.
    float redBefore = 0.f;
    ClearDamageIndicator(&redBefore);

    // 1c. What the cancelled travel would have disposed of. Best-effort; not a conjunction term.
    ReconcileCancelledTravel();
    float bloodTimeBefore = 0.f;
    const float bloodActors = ExpireBloodLoss(&bloodTimeBefore);

    // 2. Health, before dead is cleared: while dead the player takes no damage, so the restore
    //    cannot race a hit. Full maxHealth, since a native death would have reloaded the save and
    //    there is no prior value to inherit; the game's own regen resumes from here.
    float maxHp = 100.f;
    if (!V::Read(V::Field::MaxHealth, &maxHp) || maxHp <= 0.f) maxHp = 100.f;
    const bool vitalsOk = V::Write(V::Field::Health, maxHp);

    // 3. Stand up. forceWakeup is the unconditional one: movement mode, capsule collision, camera
    //    and mesh re-attach, physics off, input on, isRagdoll false (which re-opens the pause gate)
    //    and control rotation; it never calls fallen(), so it cannot re-arm the chain. Not
    //    forceGetUp (a latent delay that re-reads dead) and not wakeup (refuses while dead).
    const bool wokeUp = E::ForceMainPlayerWakeup(pawn);

    // 4. Reposition to the KPP, not spawnLocation, which is just the transform at level start and
    //    can be kilometres from the corpse after a long session. The same landmark the client
    //    teleport uses on join.
    const bool teleReported = coop::teleport_client::ApplyLocally(
        {P::name::kKPPSpawnX, P::name::kKPPSpawnY, P::name::kKPPSpawnZ, 0.f, 0.f, 0.f});

    // 5. The menu prep loadLevel left behind.
    const bool menuRestored = RestoreMenuPrep();

    // 6. Only now the flag.
    const bool deadCleared = ClearDeadFlag(pawn);

    // The conjunction. The position is readable in the same frame as the write: the game's own
    // teleportWObackrooms reads the location back one statement after setting it.
    bool isRagdoll = false, deadNow = true;
    const bool haveState = E::ReadMainPlayerRagdollState(pawn, isRagdoll, deadNow);
    float hp = -1.f;
    const bool haveHp = V::Read(V::Field::Health, &hp);
    const float dist = DistanceToKpp(pawn);
    // The positional term is load-bearing: ApplyLocally has a tiered fallback, so its report says a
    // call succeeded, not that the player moved.
    const bool atKpp = dist <= kAtKppToleranceCm;

    // The conjunction is about the player, not the screen. With blackCleared as a term, one run in
    // four saw the same-frame IsInViewport read after RemoveFromParent still true, and a living
    // player was thrown to the main menu over a widget that had not detached yet. The screen
    // artifacts are retried on the following pump ticks instead.
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

// How many pump ticks after a revive the screen cleanup is re-attempted. The measured case
// needed one extra tick; each attempt is a no-op once the artifact is gone, and it stops when
// all three read clear.
constexpr int kScreenCleanupTicks = 120;
int g_screenCleanupLeft = 0;

// Re-attempt the three screen artifacts until they are provably gone; true when nothing is
// left. Idempotent, one class lookup per artifact once clear.
bool TickScreenCleanup() {
    const bool black = ClearBlackScreen();
    const float red = ClearDamageIndicator();
    const float bloodActors = ExpireBloodLoss();
    return black && red <= 0.f && bloodActors <= 0.f;
}

// The failure exit. The flee's teardown includes Session::Stop, which is exactly what stands
// the veto down (its first term is a live session), so the travel it then authors passes
// through this seam with no special case.
void GiveUpAndLeave(coop::net::Session& session, const char* why) {
    g_armed.store(false, std::memory_order_release);
    g_revivePending.store(false, std::memory_order_release);
    g_vetoAtMsAtomic.store(0, std::memory_order_release);
    UE_LOGW("death_revive: %s -- falling back to the main-menu flee", why);
    coop::net_pump::FleeToMainMenuOnDeath(session, why);
}

}  // namespace

void Install(coop::net::Session* session) {
    // The cached Session is always refreshed: the veto reads it.
    g_session.store(session, std::memory_order_release);
    // Both outcomes latch: this runs on the unconditional timeline tick, and a permanently failing
    // install (a stale signature) must not retry forever.
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

    // The dead byte and mask, resolved once (the layout is stable per game build) through
    // FindBoolProperty rather than the plain-byte read the sender path uses: this side writes it,
    // and a masked field shares its byte.
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

    // The arm decision, on the rising edge of dead: about ten seconds before the travel, and the
    // same decision as whether net_pump flees.
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
        // The death ended; disarmed so a later unrelated travel is never refused by a stale arm.
        g_armed.store(false, std::memory_order_release);
        g_vetoAtMs = 0;
    }
    if (haveState) g_wasDead = dead;

    // The deferred revive.
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

    // The screen artifacts, retried until gone; after the pending-revive block and outside every
    // gate above it, since it must keep running once the death is over.
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

    // The deadline: an armed death still holding dead this long after the refused travel is a
    // failure, and the player has no manual exit while dead, so this is their exit.
    if (g_vetoAtMs != 0 && haveState && dead && g_reviveRanThisDeath &&
        ::GetTickCount64() - g_vetoAtMs > kReviveDeadlineMs) {
        GiveUpAndLeave(session, "still dead past the revive deadline");
    }
}

void Watchdog() {
    // The one failure this lane cannot have: a travel refused, then nothing. The revive runs from
    // Tick, which net_pump calls only while a session runs and the local death is unhandled; if
    // that stops between the veto and the revive, the pending flag is never consumed, the pump's
    // deadline never starts, and the player is dead in a world we refused to leave, with no pause
    // menu. A failure of Tick cannot be covered inside Tick, so this lives on the unconditional
    // timeline tick.
    if (!g_revivePending.load(std::memory_order_acquire)) return;
    const uint64_t at = g_vetoAtMsAtomic.load(std::memory_order_acquire);
    if (at == 0) return;
    if (::GetTickCount64() - at <= kWatchdogDeadlineMs) return;

    // The flag is taken first so this fires once; the flee is posted to the game thread, since
    // every op inside it is game-thread-only.
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
unsigned long long TravelsSeen() { return LT::SeenCount(); }
bool LastReviveSucceeded() { return g_lastReviveOk; }

bool ReconcileDisabled() {
    static const bool s = (coop::config::ReadEnv("VOTVCOOP_DEATH_NO_RECONCILE") == "1");
    return s;
}

}  // namespace coop::death_revive
