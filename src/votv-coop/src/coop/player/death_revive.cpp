// coop/player/death_revive.cpp -- the coop run-ending: the native ending runs to completion, the
// travel it ends in is cancelled by coop/player/run_end_travel, and the player is revived in
// place. The design and the bounds are in the header.

#include "coop/player/death_revive.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"
#include "coop/session/teleport_client.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <string>

namespace coop::death_revive {
namespace {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;
namespace V = ue_wrap::vitals;

// The widget the death chain adds at +5 s. It has no function or ubergraph export; the level
// travel is what disposes of it, so a cancelled travel keeps a permanent black screen unless it
// is removed. RemoveFromParent detaches rather than destroys, so the completion test is
// IsInViewport, never findability.
constexpr const wchar_t* kBlackScreenClass = L"blackScreen_C";

// ESlateVisibility::Collapsed: ui_damageIndicator_C authors dmg_full Collapsed and only the
// death branch of its Tick ever writes Visible, so the restore value is a constant off the asset.
constexpr uint8_t kSlateCollapsed = 1;

// How close to the KPP the player must land for the reposition to count. The teleport either
// lands or falls through its tiers, so this is an assertion, not a drift tolerance.
constexpr float kAtKppToleranceCm = 300.f;

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<int32_t>  g_deadByte{-1};
std::atomic<uint8_t>  g_deadMask{0};
std::atomic<bool>     g_armed{false};          // this death is ours to answer
std::atomic<bool>     g_revivePending{false};  // the seam cancelled a travel; the pump owes a revive
std::atomic<uint64_t> g_cancelAtMsAtomic{0};   // when it cancelled -- stamped AT the cancel (see Watchdog)
std::atomic<bool>     g_verbsResolved{false};  // ResolveVerbs succeeded (read off the game thread)
std::atomic<bool>     g_seamReady{false};      // run_end_travel can cancel a run-ending travel

// The ending that asked to travel, for the revive's line. Written at the cancel and read on the
// next pump task, both on the game thread.
std::wstring g_pendingAuthorClass;
bool         g_pendingAuthorIsLocalPawn = false;

// The readiness window. Every field here is ATOMIC, and not because two threads compete for it
// in the steady state -- Tick owns them all and Tick is game-thread only -- but because the RESET
// is not on that thread: `OnSessionStart` is called straight out of `StartCoopSession`, which the
// harness runs on the TimelineThread (`harness/session_runtime.cpp:362`), so a Stop/Start can zero
// these while an already-dequeued pump composite is still in Tick. Relaxed is enough: nothing is
// published through them, the drills only read the span, and what is being bought is a whole read
// rather than an ordering. The four per-term stamps exist so the window's own line says WHICH term
// the wait was spent on -- a total alone cannot be acted on, and this is the number a fix moves.
// The module's OLDER per-session fields (`g_wasDead`, `g_cancelAtMs`, `g_reviveRanThisDeath`,
// `g_screenCleanupLeft`, `g_pendingAuthorClass`) are plain and are reset down that same path: the
// same race, pre-dating this window, and its root is that a game-thread module's per-session reset
// is invoked from the session's thread rather than posted. Named here, not quietly inherited.
std::atomic<uint64_t>  g_pawnFirstMs{0};
std::atomic<long long> g_armReadyAfterPawnMs{-1};
std::atomic<long long> g_tSeam{-1}, g_tVerbs{-1}, g_tSession{-1}, g_tDeadOff{-1};

// Pump-side state, game thread only.
bool  g_wasDead = false;
bool  g_lastReviveOk = false;
bool  g_reviveRanThisDeath = false;
uint64_t g_cancelAtMs = 0;

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
    // The damage indicator's four directional accumulators. Add Player Damage accumulates
    // damage/maxHealth*4 into the quadrant of the hit, so the killing hit leaves red on screen past
    // a revive to full health.
    int32_t offPlayerInterface = -1;   // mainGamemode_C.playerInterface  (ui_UI_C)
    int32_t offDamageIndicator = -1;   // ui_UI_C.umg_damageIndicator     (ui_damageIndicator_C)
    int32_t offDmgUp = -1, offDmgDown = -1, offDmgLeft = -1, offDmgRight = -1;
    // The one write the run ending leaves behind that nothing else disposes of. See
    // ReconcileRunEndLatches().
    int32_t offDmgFull = -1;        // ui_damageIndicator_C.dmg_full  (UImage*)
};
Verbs g_verbs;

// FindFunction is exact-owner, so each engine verb resolves off the class that declares it:
// Widget for RemoveFromParent and SetVisibility, UserWidget for IsInViewport. Off the BP class
// they return null.
bool ResolveVerbs() {
    if (g_verbs.resolved) return true;
    // Throttled while unresolved: each attempt is three FindClass calls, and a MISS walks every
    // UObject slot and is never cached, so an unthrottled retry is a full-array scan per frame for
    // as long as the widget classes are not loaded. The pump is posted behind a 16 ms sleep, so
    // ~60 Hz, and 60 attempts apart is about a second.
    static uint32_t sThrottle = 0;
    if ((sThrottle++ % 60) != 0) return false;
    Verbs v;
    void* widgetCls = R::FindClass(P::name::WidgetClass);
    void* userWidgetCls = R::FindClass(P::name::UserWidgetClass);
    void* gmCls = R::FindClass(P::name::GamemodeClass);
    if (widgetCls) {
        v.removeFromParent = R::FindFunction(widgetCls, L"RemoveFromParent");
        v.setVisibility = R::FindFunction(widgetCls, P::name::WidgetSetVisibilityFn);
    }
    if (userWidgetCls) v.isInViewport = R::FindFunction(userWidgetCls, L"IsInViewport");
    if (gmCls) v.offPlayerInterface = R::FindPropertyOffset(gmCls, L"playerInterface");
    if (void* uiCls = R::FindClass(L"ui_UI_C"))
        v.offDamageIndicator = R::FindPropertyOffset(uiCls, L"umg_damageIndicator");
    if (void* dmgCls = R::FindClass(L"ui_damageIndicator_C")) {
        v.offDmgUp = R::FindPropertyOffset(dmgCls, L"damage_up");
        v.offDmgDown = R::FindPropertyOffset(dmgCls, L"damage_down");
        v.offDmgLeft = R::FindPropertyOffset(dmgCls, L"damage_left");
        v.offDmgRight = R::FindPropertyOffset(dmgCls, L"damage_right");
    }
    if (void* dmgCls2 = R::FindClass(L"ui_damageIndicator_C"))
        v.offDmgFull = R::FindPropertyOffset(dmgCls2, L"dmg_full");
    const bool ok = v.removeFromParent && v.isInViewport && v.setVisibility;
    if (!ok) return false;  // classes load with gameplay; retry next tick, quietly
    v.resolved = true;
    g_verbs = v;
    g_verbsResolved.store(true, std::memory_order_release);
    UE_LOGI("death_revive: revive verbs resolved (remove=%p inViewport=%p setVis=%p "
            "playerInterface=0x%X dmg_full=0x%X)",
            v.removeFromParent, v.isInViewport, v.setVisibility,
            v.offPlayerInterface, v.offDmgFull);
    return true;
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

// The one-way writes a cancelled run-ending leaves behind. In the August build the cut was at
// `OpenLevel`, so `lib_C::loadLevel` had already run and its four writes had to be undone here.
// The cut is now the `loadLevel` body itself, so those writes never happen: the menu prep
// (`canvas_loading`, `screenSwi`) and the death signal (`NewVar_1`, whose armed-and-never-consumed
// branch reached `lib_C::end` while paused) are disposed of at the root rather than reconciled.
// What is left is the one latch the ENDING itself writes:
// [1] ui_damageIndicator_C.dmg_full.Visibility: the Tick's death branch sets it Visible while the
// widget authors it Collapsed, and the alive path never writes it, so once dead for a single tick
// a full-screen red image draws over everything for good.
// gameInstance.subArea := None is still left alone: the revive puts the player at the KPP,
// outdoors, where None is correct, and the trigger volumes re-establish it.
// Best-effort: a failure here is cosmetic or latent, never a reason to strand the player, so it
// is not a term of the revive's conjunction.
void ReconcileRunEndLatches() {
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
        void* gm = ue_wrap::world_singleton::Gamemode();
        if (gm) {
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
}

// The pause four of the five run-endings set before their delay (gameover_C, SKEL_C, birch_C,
// npc_angryErieFlesh_C all call SetGamePaused(true); the death chain never pauses). The travel
// they asked for is what used to undo it, through mainGamemode::transition's own
// SetGamePaused(false) -- and that hop no longer runs. coop/session/pause_guard owns this axis in
// the steady state, but its scope is Session::connected(), so a host with no client connected --
// which this lane has always counted as a session -- would be left in a paused world behind a
// black screen. One shot, here, as this episode's residue; the invariant stays pause_guard's.
void ClearRunEndPause() {
    if (!E::IsGamePaused()) return;
    const bool ok = E::SetGamePaused(false);
    UE_LOGI("death_revive: the run-ending paused the world and the travel that would have "
            "un-paused it was cancelled -- un-paused ok=%d", ok ? 1 : 0);
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
    void* gm = ue_wrap::world_singleton::Gamemode();
    if (!gm) return -1.f;
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

// False when the pawn's location cannot be read.
bool DistanceToKpp(void* pawn, float& out) {
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(pawn, at)) return false;
    const float dx = at.X - P::name::kKPPSpawnX;
    const float dy = at.Y - P::name::kKPPSpawnY;
    const float dz = at.Z - P::name::kKPPSpawnZ;
    out = std::sqrt(dx * dx + dy * dy + dz * dz);
    return true;
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

    // 1c. The latches the ending leaves that nothing disposes of. Best-effort; not a conjunction
    //     term.
    ReconcileRunEndLatches();
    // 1d. And the pause an ending set, which the cancelled travel no longer undoes.
    ClearRunEndPause();
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

    // 5. Only now the flag. (The menu-prep restore that used to sit here is gone with the writes
    //    it undid: cancelling the loadLevel body means they are never made.)
    const bool deadCleared = ClearDeadFlag(pawn);

    // The conjunction. The position is readable in the same frame as the write: the game's own
    // teleportWObackrooms reads the location back one statement after setting it.
    bool isRagdoll = false, deadNow = true;
    const bool haveState = E::ReadMainPlayerRagdollState(pawn, isRagdoll, deadNow);
    float hp = -1.f;
    const bool haveHp = V::Read(V::Field::Health, &hp);
    float dist = -1.f;   // -1: unread, never at the KPP
    const bool distRead = DistanceToKpp(pawn, dist);
    // The positional term is load-bearing: ApplyLocally has a tiered fallback, so its report says a
    // call succeeded, not that the player moved.
    const bool atKpp = distRead && dist <= kAtKppToleranceCm;

    // The conjunction is about the player, not the screen. With blackCleared as a term, one run in
    // four saw the same-frame IsInViewport read after RemoveFromParent still true, and a living
    // player was thrown to the main menu over a widget that had not detached yet. The screen
    // artifacts are retried on the following pump ticks instead.
    const bool ok = vitalsOk && wokeUp && teleReported &&
                    deadCleared && haveState && !isRagdoll && !deadNow && haveHp && hp > 0.f &&
                    atKpp;

    UE_LOGI("death_revive: REVIVE %s after a run-ending authored by %ls -- vitals=%d wake=%d "
            "tele=%d deadClr=%d "
            "| screen (retried, NOT a gate): black=%d dmgRed=%.2f bloodLoss=%.0f actor(s) @%.1fs "
            "| readback: ragdoll=%d dead=%d hp=%.1f distKPP=%.0f cm (tol %.0f)",
            ok ? "OK" : "FAILED",
            g_pendingAuthorClass.empty() ? L"(unrecorded)" : g_pendingAuthorClass.c_str(),
            vitalsOk ? 1 : 0, wokeUp ? 1 : 0,
            teleReported ? 1 : 0, deadCleared ? 1 : 0, blackCleared ? 1 : 0, redBefore, bloodActors, bloodTimeBefore,
            haveState ? (isRagdoll ? 1 : 0) : -1, haveState ? (deadNow ? 1 : 0) : -1,
            haveHp ? hp : -1.f, dist, kAtKppToleranceCm);
    (void)session;
    return ok;
}

// How many pump ticks after a revive the screen cleanup is re-attempted, and how many ticks
// apart. The measured case needs ONE attempt; each is a no-op once the artifact is gone, and the
// run stops the moment all three read clear. The stride is not cosmetic: an attempt is two full
// GUObjectArray walks (a FindObjectByClass miss walks every slot, and FindObjectsByClass always
// does), so the stuck case -- an effect actor whose own tick never destroys it -- used to spend
// 120 consecutive pump ticks doing ~240 walks of a ~293k array. Same wall-clock window, an
// eighth of the walks.
constexpr int kScreenCleanupTicks = 120;
constexpr int kScreenCleanupStride = 8;
int g_screenCleanupLeft = 0;

// Re-attempt the three screen artifacts until they are provably gone; true when nothing is
// left. Idempotent, one class lookup per artifact once clear.
bool TickScreenCleanup() {
    const bool black = ClearBlackScreen();
    const float red = ClearDamageIndicator();
    const float bloodActors = ExpireBloodLoss();
    return black && red <= 0.f && bloodActors <= 0.f;
}

// The failure exit. The flee's teardown includes Session::Stop, which is exactly what stands the
// seam down (its session test), and the flee travels through mainGamemode::transition rather than
// loadLevel, so it never reaches the seam at all.
void GiveUpAndLeave(coop::net::Session& session, const char* why) {
    g_armed.store(false, std::memory_order_release);
    g_revivePending.store(false, std::memory_order_release);
    g_cancelAtMsAtomic.store(0, std::memory_order_release);
    UE_LOGW("death_revive: %s -- falling back to the main-menu flee", why);
    coop::net_pump::FleeToMainMenuOnDeath(session, why);
}

}  // namespace

void Install(coop::net::Session* session) {
    // The cached Session is always refreshed: the arm and ReviveAvailable read it.
    g_session.store(session, std::memory_order_release);
}

bool ReviveAvailable() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return g_verbsResolved.load(std::memory_order_acquire) &&
           g_deadByte.load(std::memory_order_relaxed) >= 0 && s && s->running();
}

void NoteRunEndSeamReady(bool ready) { g_seamReady.store(ready, std::memory_order_release); }

long long ArmReadyAfterPawnMs() { return g_armReadyAfterPawnMs.load(std::memory_order_acquire); }

void NoteRunEndCancelled(void* author, const wchar_t* authorClass, bool authorIsLocalPawn) {
    (void)author;
    g_pendingAuthorClass = authorClass ? authorClass : L"";
    g_pendingAuthorIsLocalPawn = authorIsLocalPawn;
    g_revivePending.store(true, std::memory_order_release);
    // The clock is stamped at the cancel, not by the pump: the watchdog must bound "a travel was
    // cancelled and the pump never came back", and a deadline armed by the pump cannot bound a
    // failure of the pump.
    g_cancelAtMsAtomic.store(::GetTickCount64(), std::memory_order_release);
}

void OnSessionStart() {
    // Not the game thread (see the window's own note): every field it touches here is atomic.
    g_pawnFirstMs.store(0, std::memory_order_relaxed);
    g_armReadyAfterPawnMs.store(-1, std::memory_order_release);
    for (std::atomic<long long>* s : {&g_tSeam, &g_tVerbs, &g_tSession, &g_tDeadOff})
        s->store(-1, std::memory_order_relaxed);
    g_armed.store(false, std::memory_order_release);
    g_revivePending.store(false, std::memory_order_release);
    g_cancelAtMsAtomic.store(0, std::memory_order_release);
    g_wasDead = false;
    g_reviveRanThisDeath = false;
    g_lastReviveOk = false;
    g_cancelAtMs = 0;
    g_screenCleanupLeft = 0;
    g_pendingAuthorClass.clear();
    g_pendingAuthorIsLocalPawn = false;
}

void Tick(coop::net::Session& session, void* localPawn) {
    Install(&session);  // idempotent; also refreshes the cached Session pointer

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

    // The arm's terms, read ONCE and used twice -- by the window latch just below and by the edge
    // decision further down. Two readings of "can this death be answered" could disagree across
    // the ticks between them, and the instrument that reports the window would then be measuring
    // something other than the decision it describes.
    const bool seamReady = g_seamReady.load(std::memory_order_acquire);
    const bool sessionLive = session.running();
    const bool deadOffsetKnown = g_deadByte.load(std::memory_order_relaxed) >= 0;
    const bool armReady = seamReady && verbsOk && sessionLive && deadOffsetKnown;

    // THE WINDOW. Every term above is a readiness term -- the seam's watch and its two author
    // classes, the revive's verbs, a live session, the dead flag's offset -- and none of them is
    // true the instant a player can first die. A death inside that span is NOT armed, so
    // net_pump's flee tears the coop state down and travels to the menu, which is the whole of
    // the symptom a field report describes. It is no role asymmetry -- neither Install nor Tick
    // tests the role -- but a client reaches its own pawn later than a host does, so a client
    // meets the window more often. It is stamped once per session and logged, which puts the
    // number in a field log where no drill can go.
    if (localPawn && g_pawnFirstMs.load(std::memory_order_relaxed) == 0)
        g_pawnFirstMs.store(::GetTickCount64(), std::memory_order_relaxed);
    const uint64_t pawnAt = g_pawnFirstMs.load(std::memory_order_relaxed);
    if (pawnAt != 0 && g_armReadyAfterPawnMs.load(std::memory_order_relaxed) < 0) {
        const long long now = static_cast<long long>(::GetTickCount64() - pawnAt);
        auto stamp = [now](std::atomic<long long>& slot, bool term) {
            if (term && slot.load(std::memory_order_relaxed) < 0)
                slot.store(now, std::memory_order_relaxed);
        };
        stamp(g_tSeam, seamReady);
        stamp(g_tVerbs, verbsOk);
        stamp(g_tSession, sessionLive);
        stamp(g_tDeadOff, deadOffsetKnown);
        if (armReady) {
            g_armReadyAfterPawnMs.store(now, std::memory_order_release);
            UE_LOGI("death_revive: the arm is READY +%lld ms after the local pawn first ticked "
                    "(seam +%lld, verbs +%lld, session +%lld, deadOffset +%lld) -- a death before "
                    "that point could not have been answered and the pump's flee would have ended "
                    "the session, so the largest term is what a fix has to move",
                    now, g_tSeam.load(std::memory_order_relaxed),
                    g_tVerbs.load(std::memory_order_relaxed),
                    g_tSession.load(std::memory_order_relaxed),
                    g_tDeadOff.load(std::memory_order_relaxed));
        }
    }

    bool isRagdoll = false, dead = false;
    const bool haveState = localPawn && E::ReadMainPlayerRagdollState(localPawn, isRagdoll, dead);

    // The arm decision, on the rising edge of dead: about ten seconds before the travel, and the
    // same decision as whether net_pump flees. It answers ONLY for the death chain -- the five
    // run-endings that never set `dead` reach the seam without ever passing here, and the pump
    // has no flee to stand down for them.
    if (haveState && dead && !g_wasDead) {
        g_reviveRanThisDeath = false;
        if (armReady) {
            g_armed.store(true, std::memory_order_release);
            UE_LOGI("death_revive: local death ARMED -- the native death runs to completion "
                    "(~10 s: sound, black screen at +5 s) and its travel will be cancelled");
        } else {
            g_armed.store(false, std::memory_order_release);
            UE_LOGW("death_revive: local death NOT armed (seam=%d verbs=%d session=%d deadOff=%d) "
                    "-- net_pump's flee handles this death",
                    seamReady ? 1 : 0, verbsOk ? 1 : 0,
                    session.running() ? 1 : 0,
                    g_deadByte.load(std::memory_order_relaxed));
        }
    }
    if (haveState && !dead && g_wasDead) {
        // The death ended; disarmed so the pump's flee owns the next one if the seam has gone.
        g_armed.store(false, std::memory_order_release);
        g_cancelAtMs = 0;
    }
    if (haveState) g_wasDead = dead;

    // The deferred revive.
    if (g_revivePending.load(std::memory_order_acquire)) {
        if (g_cancelAtMs == 0) {
            g_cancelAtMs = ::GetTickCount64();
            UE_LOGI("death_revive: the run-ending travel was cancelled -- the world is kept; "
                    "reviving on this pump task");
        }
        if (!localPawn || !R::IsLive(localPawn)) {
            if (::GetTickCount64() - g_cancelAtMs > kReviveDeadlineMs)
                GiveUpAndLeave(session, "no local pawn to revive after the travel was cancelled");
            return;
        }
        g_revivePending.store(false, std::memory_order_release);
        g_reviveRanThisDeath = true;
        g_lastReviveOk = RunRevive(session, localPawn);
        if (g_lastReviveOk) {
            g_armed.store(false, std::memory_order_release);
            g_cancelAtMs = 0;
            g_cancelAtMsAtomic.store(0, std::memory_order_release);
            g_screenCleanupLeft = kScreenCleanupTicks;
        } else {
            GiveUpAndLeave(session, "the revive did not complete its conjunction");
        }
        return;
    }

    // The screen artifacts, retried until gone; after the pending-revive block and outside every
    // gate above it, since it must keep running once the death is over.
    if (g_screenCleanupLeft > 0 &&
        ((kScreenCleanupTicks - g_screenCleanupLeft) % kScreenCleanupStride) == 0) {
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

    // The deadline: an armed death still holding dead this long after the cancelled travel is a
    // failure, and the player has no manual exit while dead, so this is their exit.
    if (g_cancelAtMs != 0 && haveState && dead && g_reviveRanThisDeath &&
        ::GetTickCount64() - g_cancelAtMs > kReviveDeadlineMs) {
        GiveUpAndLeave(session, "still dead past the revive deadline");
    }
}

void Watchdog() {
    // The one failure this lane cannot have: a travel cancelled, then nothing. The revive runs
    // from Tick, which net_pump calls only while a session runs and the local death is unhandled;
    // if that stops between the cancel and the revive, the pending flag is never consumed, the
    // pump's deadline never starts, and the player is stranded in a world we refused to leave,
    // with no pause menu. A failure of Tick cannot be covered inside Tick, nor inside the same
    // posted composite, so this runs on the thread that posts it.
    if (!g_revivePending.load(std::memory_order_acquire)) return;
    const uint64_t at = g_cancelAtMsAtomic.load(std::memory_order_acquire);
    if (at == 0) return;
    if (::GetTickCount64() - at <= kWatchdogDeadlineMs) return;

    // The flag is taken first so this fires once; the flee is posted to the game thread, since
    // every op inside it is game-thread-only.
    g_revivePending.store(false, std::memory_order_release);
    g_cancelAtMsAtomic.store(0, std::memory_order_release);
    g_armed.store(false, std::memory_order_release);
    UE_LOGE("death_revive: WATCHDOG -- a run-ending travel was cancelled %llu ms ago and no "
            "revive ever ran (the pump stopped ticking between the cancel and the revive). "
            "Leaving the world rather than stranding a player with no pause menu.",
            static_cast<unsigned long long>(kWatchdogDeadlineMs));
    ue_wrap::game_thread::Post([] {
        if (coop::net::Session* s = g_session.load(std::memory_order_acquire))
            coop::net_pump::FleeToMainMenuOnDeath(*s, "revive never ran after a cancelled travel");
    });
}

bool ArmedForThisDeath() { return g_armed.load(std::memory_order_acquire); }
bool LastReviveSucceeded() { return g_lastReviveOk; }

bool ReconcileDisabled() {
    static const bool s = (coop::config::ReadEnv("VOTVCOOP_DEATH_NO_RECONCILE") == "1");
    return s;
}

}  // namespace coop::death_revive
