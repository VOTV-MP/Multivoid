// harness/autotest/autotest_slip.cpp -- does a ZERO-DAMAGE ragdoll re-enter the death chain when
// the pawn's `dead` is already set? Field reports say a player who was walking around lands on
// the main menu and takes every peer with them; one slipped on a banana peel, another stood near
// an explosion. The bytecode says why, and the claim is one branch:
//
//   prop_bananaHusk_C::steppedOn -> player.punch(..., damage = 0, ...)   twice
//   mainPlayer::punch            -> if (!isRagdoll) ragdollMode(true, false, false)
//   mainPlayer::ragdollMode      -> ... -> fallen(death = false)   [Label_595 branch only]
//   mainPlayer::fallen           -> @39848: if !(death) goto .L39685
//                                   @39685: if !(this.dead) goto .L39704  <- the only guard
//                                   @39699: .L37478 = deathEnd; dead := true; +5 s blackScreen;
//                                           +5 s lib_C::loadLevel("menu", ..., this)
//
// So the damage is irrelevant: it is a RAGDOLL bug, and a fall, a wisp, a punch and an explosion
// all enter that same fallen(false).

#include "harness/autotest.h"

#include "harness/autotest/death_state_probe.h"
#include "harness/autotest/gt_task.h"
#include "harness/autotest/slip_drill.h"
#include "harness/session_runtime.h"

#include "coop/net/session.h"
#include "coop/player/death_revive.h"
#include "coop/player/players_registry.h"
#include "coop/player/run_end_travel.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <memory>

namespace harness::autotest {
namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace RET = coop::player::run_end_travel;

constexpr const wchar_t* kPeelClass = L"prop_bananaHusk_C";

struct Tally {
    int pass = 0;
    int fail = 0;
    int vacuous = 0;
};
Tally g_v;

void Check(bool ok, const char* name, const char* why) {
    (ok ? g_v.pass : g_v.fail)++;
    if (ok) UE_LOGI("[SLIP] PASS %s -- %s", name, why);
    else    UE_LOGE("[SLIP] FAIL %s -- %s", name, why);
}

// A check whose PREMISE did not hold. It is neither a pass nor a failure of the thing it names,
// and counting it either way is how an instrument grades itself green: a run where nothing
// dispatched satisfies every negative assertion in this file.
void Inconclusive(const char* name, const char* why) {
    ++g_v.vacuous;
    UE_LOGW("[SLIP] INCONCLUSIVE %s -- %s", name, why);
}

// A snapshot plus whether the game thread actually produced it. A timed-out read returns a
// default-constructed DeathSnapshot, which reads as "not dead, not ragdolled, no black screen" --
// exactly the shape of a clean control -- so every caller must consult `landed` before asserting
// anything, and above all before asserting a negative.
struct Reading {
    DeathSnapshot s;
    bool landed = false;
};

Reading Snap() {
    Reading r;
    auto out = std::make_shared<DeathSnapshot>();
    r.landed = RunOnGameThread([out] { *out = ReadDeathState(); });
    if (!r.landed) {
        UE_LOGW("[SLIP] a state read did not reach the game thread inside its budget -- that "
                "sample is UNREAD, not clean");
        return r;
    }
    r.s = *out;
    // sessionRunning is the caller's field by design (the probe reads UObject state and nothing
    // else), and this drill's preconditions rest on it: the seam judges nothing without a session.
    r.s.sessionRunning = harness::session_runtime::Session().running();
    return r;
}

void LogState(const char* when, const Reading& r) {
    if (!r.landed) { UE_LOGW("[SLIP] %s -- UNREAD (the game thread did not answer)", when); return; }
    const DeathSnapshot& s = r.s;
    UE_LOGI("[SLIP] %s -- dead=%d ragdoll=%d(read=%d) canRagdoll=%d hp=%.1f black=%d "
            "inGameplay=%d session=%d", when, s.dead ? 1 : 0, s.isRagdoll ? 1 : 0,
            s.haveState ? 1 : 0, s.canRagdoll ? 1 : 0, s.health,
            s.blackScreenInViewport ? 1 : 0, s.inGameplay ? 1 : 0, s.sessionRunning ? 1 : 0);
}

// The faithful trigger: a real peel through its own steppedOn, so the peel's guard (upright and
// nearly still) and both of its zero-damage punches run. `hit` is left as the frame built it --
// the ubergraph stores it and never reads it.
bool DispatchSteppedOn(void* peel, void* player) {
    if (!peel || !R::IsLive(peel) || !player || !R::IsLive(player)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(peel), L"steppedOn");
    if (!fn) { UE_LOGW("[SLIP] %ls::steppedOn did not resolve", kPeelClass); return false; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    if (!f.SetRaw(L"player", &player, sizeof(player))) {
        UE_LOGW("[SLIP] steppedOn has no `player` parameter -- the game changed under this drill");
        return false;
    }
    return ue_wrap::Call(peel, f);
}

// The peel's own second call with the peel taken out of the picture. The claim under test is about
// a zero-damage ragdoll, not about banana physics, so a peel that landed on its side must not be
// able to make the run inconclusive. Impulses stay zero: `punch` reaches ragdollMode before it uses
// any of them, and a drill that also throws the player makes its own reading harder.
bool DispatchPunchZeroDamage(void* pawn) {
    if (!pawn || !R::IsLive(pawn)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(pawn), L"punch");
    if (!fn) { UE_LOGW("[SLIP] mainPlayer::punch did not resolve"); return false; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<float>(L"damage", 0.f);
    return ue_wrap::Call(pawn, f);
}

enum class Trigger { None, Peel, Punch };

const char* TriggerName(Trigger t) {
    switch (t) {
        case Trigger::Peel:  return "the peel's own steppedOn";
        case Trigger::Punch: return "the direct zero-damage punch";
        case Trigger::None:  break;
    }
    return "NOTHING -- no trigger dispatched";
}

// Slip the local player, and report WHICH trigger did it -- judged by the ragdoll the player is in
// afterwards, never by the dispatch returning true. steppedOn returns true whether or not the
// peel's guard let the punches run, so a call-return fallback would skip the punch exactly when it
// is needed.
Trigger SlipLocalPlayer(ue_wrap::CachedObjRef& peel) {
    if (peel.Alive()) {
        void* p = peel.Get();
        RunOnGameThread([p] {
            void* pawn = coop::players::Registry::Get().Local();
            if (pawn) DispatchSteppedOn(p, pawn);
        });
        ::Sleep(500);
        const Reading r = Snap();
        if (r.landed && r.s.haveState && r.s.isRagdoll) return Trigger::Peel;
        UE_LOGI("[SLIP] the peel's guard did not let the punches run (not flat, or not yet still) "
                "-- driving its own punch instead");
    }
    auto sent = std::make_shared<bool>(false);
    const bool landed = RunOnGameThread([sent] {
        void* pawn = coop::players::Registry::Get().Local();
        if (pawn) *sent = DispatchPunchZeroDamage(pawn);
    });
    return (landed && *sent) ? Trigger::Punch : Trigger::None;
}

// Watch for `ms`, stamping when each thing the death chain does first happened, in milliseconds
// after the call; -1 means it never happened inside the window. `landedSamples` is the guard on
// every negative read off this: a window of unread samples looks exactly like a quiet one.
struct Watch {
    long long ragdoll = -1, dead = -1, black = -1, travel = -1;
    int landedSamples = 0, totalSamples = 0;
    Reading last;
};
constexpr int kSampleMs = 250;

Watch WatchFor(int ms) {
    Watch w;
    const uint64_t t0 = ::GetTickCount64();
    for (uint64_t now = t0; now - t0 < static_cast<uint64_t>(ms); now = ::GetTickCount64()) {
        const Reading r = Snap();
        ++w.totalSamples;
        const long long dt = static_cast<long long>(::GetTickCount64() - t0);
        if (r.landed) {
            ++w.landedSamples;
            const DeathSnapshot& s = r.s;
            if (s.haveState && s.isRagdoll && w.ragdoll < 0) w.ragdoll = dt;
            if (s.haveState && s.dead && w.dead < 0) w.dead = dt;
            if (s.blackScreenInViewport && w.black < 0) w.black = dt;
            if (s.haveWorld && !s.inGameplay && w.travel < 0) w.travel = dt;
            w.last = r;
        }
        ::Sleep(kSampleMs);
    }
    return w;
}

// Undo everything this drill wrote, on EVERY exit. Without it a run that does not reach a revive
// leaves `dead` latched true on a walking player, and that is not merely untidy: death_revive arms
// on the RISING edge of `dead` and disarms on the falling one, so a latched flag means the next
// real death produces no edge, never arms, and net_pump's !ArmedForThisDeath() branch tears the
// session down. The drill would leave the process unable to survive a death. The peel goes too:
// E::SpawnActor runs the keyed spawn seam, which is OWNER-SYMMETRIC (host_spawn_watcher.h), so an
// abandoned fixture is adopted and broadcast to the other peer.
void TearDown(ue_wrap::CachedObjRef& peel) {
    auto report = std::make_shared<int>(0);   // bit 0 = dead cleared, 1 = woken, 2 = peel destroyed
    void* p = peel.Get();
    const bool landed = RunOnGameThread([report, p] {
        if (void* pawn = coop::players::Registry::Get().Local()) {
            if (E::WriteMainPlayerDead(pawn, false)) *report |= 1;
            if (E::ForceMainPlayerWakeup(pawn)) *report |= 2;
        }
        if (p && R::IsLive(p) && E::DestroyActor(p)) *report |= 4;
    });
    peel.Reset();
    if (!landed) {
        UE_LOGE("[SLIP] TEARDOWN DID NOT RUN -- the game thread never took it. `dead` may still be "
                "set on the local player, which would stop the next real death from arming");
        return;
    }
    UE_LOGI("[SLIP] teardown: deadCleared=%d woken=%d peelDestroyed=%d",
            (*report & 1) ? 1 : 0, (*report & 2) ? 1 : 0, (*report & 4) ? 1 : 0);
}

void Verdict() {
    UE_LOGI("[SLIP] SEAM -- watch=%d travelsSeen=%llu menuTravels=%llu cancelled=%llu "
            "lastReviveOk=%d", RET::WatchInstalled() ? 1 : 0, RET::TravelsSeen(),
            RET::MenuTravelsSeen(), RET::TravelsCancelled(),
            coop::death_revive::LastReviveSucceeded() ? 1 : 0);
    // A vacuous row is not a pass: PASS with any INCONCLUSIVE row would be the drill grading
    // itself on checks whose premise never held.
    const bool ok = g_v.fail == 0 && g_v.vacuous == 0;
    UE_LOGI("[SLIP] VERDICT %s (%d passed, %d failed, %d inconclusive)",
            ok ? "PASS" : (g_v.fail ? "FAIL" : "INCONCLUSIVE"), g_v.pass, g_v.fail, g_v.vacuous);
    UE_LOGI("[SLIP] DONE");
}

}  // namespace

// The drill asserts that branch rather than waiting for a log nobody will send: it slips the local
// player twice, once with `dead` false (the control, which must only ragdoll) and once with it
// true. Solo host, or a client linked to one (`mp.py slip --client`) -- the second half of the
// question, whether our run-ending seam refuses the travel the chain asks for, needs a session.
void RunSlipDrill() {
    const bool isClient = IsClientRole();
    UE_LOGI("[SLIP] starting as the %s (waiting 60 s: world, possession, a live session)",
            isClient ? "CLIENT" : "HOST");
    ::Sleep(60000);

    ue_wrap::CachedObjRef peel;

    // Preconditions. A player who cannot ragdoll cannot slip, and one already dead or already down
    // makes both arms meaningless. A client must also be LINKED, not merely running: the whole
    // point of that arm is a session that exists to be torn down.
    Reading r;
    bool ready = false, linked = false;
    for (int i = 0; i < 60 && !ready; ++i) {
        r = Snap();
        linked = !isClient || harness::session_runtime::Session().connected();
        ready = r.landed && r.s.havePawn && r.s.haveState && r.s.haveCanRagdoll && r.s.canRagdoll &&
                r.s.health > 0.f && !r.s.dead && !r.s.isRagdoll && r.s.inGameplay &&
                r.s.sessionRunning && linked;
        if (!ready) ::Sleep(1000);
    }
    LogState("pre-drill", r);
    if (isClient) UE_LOGI("[SLIP] client link: connected=%d", linked ? 1 : 0);
    if (!ready) {
        UE_LOGW("[SLIP] VERDICT INCONCLUSIVE -- preconditions never met (see the line above)");
        UE_LOGI("[SLIP] DONE");
        return;
    }

    // The seam is a PRECONDITION, not a row to report. With the watch down, arm B arms `dead` and
    // fires a chain whose loadLevel("menu") nothing will refuse -- the run really does end and, on
    // the client arm, takes that peer out of the session. A drill may not cause the defect it is
    // measuring.
    if (!RET::WatchInstalled()) {
        UE_LOGW("[SLIP] VERDICT INCONCLUSIVE -- lib_C::loadLevel is NOT watched, so arm B would "
                "travel for real. Refusing to arm a death this drill cannot catch");
        UE_LOGI("[SLIP] DONE");
        return;
    }
    UE_LOGI("[SLIP] seam ready: lib_C::loadLevel is watched, so arm B's travel will be judged");

    // One peel, spawned above the player's feet so it drops the last few centimetres and settles
    // flat. Both arms reuse it; the first slip kicks it away (the ubergraph's last statement) and
    // three seconds is enough to come to rest again. Held as a CachedObjRef, not a bare pointer:
    // it lives across ~30 s and two arms, and the world may destroy it in between.
    {
        auto spawned = std::make_shared<void*>(nullptr);
        RunOnGameThread([spawned] {
            void* pawn = coop::players::Registry::Get().Local();
            void* cls = R::FindClass(kPeelClass);
            if (!pawn || !cls) return;
            ue_wrap::FVector at{};
            if (!E::TryGetActorLocation(pawn, at)) {
                UE_LOGW("[SLIP] no peel -- the player's location could not be read");
                return;
            }
            at.Z += 40.f;
            *spawned = E::SpawnActor(cls, at);
        });
        if (*spawned) {
            void* got = *spawned;
            RunOnGameThread([&peel, got] { peel.Set(got); });
        }
    }
    ::Sleep(3000);
    UE_LOGI("[SLIP] %ls %s", kPeelClass,
            peel.Alive() ? "spawned at the player's feet and settled"
                         : "is not in this cook or did not spawn -- the punch carries both arms");

    // ---- ARM A: the control. A slip with `dead` FALSE must ragdoll and do nothing else. ----
    UE_LOGI("[SLIP] ARM A (control): slipping a player whose `dead` is FALSE");
    const Trigger ta = SlipLocalPlayer(peel);
    UE_LOGI("[SLIP] arm A trigger: %s", TriggerName(ta));
    const Watch a = WatchFor(9000);
    UE_LOGI("[SLIP] arm A TIMELINE (ms, %d ms sampling; %d/%d samples landed) -- ragdoll=%lld "
            "dead=%lld black=%lld travel=%lld",
            kSampleMs, a.landedSamples, a.totalSamples, a.ragdoll, a.dead, a.black, a.travel);
    LogState("arm A end", a.last);

    // The ragdoll may already be up when the window opens: SlipLocalPlayer waits 500 ms to judge
    // which trigger fired, so a 0 ms stamp means "before the first sample", not "instantaneous".
    const bool aFired = ta != Trigger::None && a.ragdoll >= 0;
    Check(aFired, "B1 slip-ragdolled",
          aFired ? "the zero-damage trigger put the player into a ragdoll -- punch reached "
                   "ragdollMode, so the hops above the branch are live"
                 : "the player never ragdolled: the trigger did not reach ragdollMode, and arm B "
                   "would prove nothing about the branch under it");
    // Only assertable once the trigger is known to have fired AND the window was actually read.
    if (!aFired || a.landedSamples == 0) {
        Inconclusive("B2 control-quiet",
                     !aFired ? "nothing ragdolled, so a quiet window says nothing about the guard"
                             : "no sample in the window reached the game thread");
    } else {
        const bool quiet = a.dead < 0 && a.black < 0 && a.travel < 0;
        Check(quiet, "B2 control-quiet",
              quiet ? "an ordinary slip set no `dead`, showed no black screen and travelled "
                      "nowhere: the guard at @39685 holds while `dead` is false"
                    : "an ordinary slip started the death chain -- the defect is NOT gated on "
                      "`dead`, and this drill's whole model is wrong");
    }

    // ---- ARM B: the defect. `dead` true on a player who is standing, then the same slip. ----
    // forceWakeup and not ragdollMode(false,...), whose `wakeup` refuses while dead -- and
    // emphatically not anything that calls `fallen`, which would arm the chain we are about to test.
    auto armReport = std::make_shared<int>(0);   // bit 0 = stood up, 1 = dead written
    const bool armLanded = RunOnGameThread([armReport] {
        void* pawn = coop::players::Registry::Get().Local();
        if (!pawn) return;
        if (E::ForceMainPlayerWakeup(pawn)) *armReport |= 1;
        if (E::WriteMainPlayerDead(pawn, true)) *armReport |= 2;
    });
    ::Sleep(2000);
    const Reading mid = Snap();
    LogState("arm B armed", mid);

    const bool armedOk = armLanded && (*armReport & 2) && mid.landed && mid.s.dead;
    const bool standing = armLanded && (*armReport & 1) && mid.landed && !mid.s.isRagdoll;
    Check(armedOk, "C1 armed-dead-set",
          armedOk ? "`dead` reads TRUE on a player still on their feet -- the state the field "
                    "reports describe"
                  : "the `dead` write did not hold, so arm B tests nothing");
    Check(standing, "C2 standing",
          standing ? "the player is out of the ragdoll, so punch will call ragdollMode again"
                   : "the player is STILL ragdolled -- punch returns early and the trigger cannot "
                     "fire");
    if (!armedOk || !standing) {
        Inconclusive("D* arm-B", "the arm's own preconditions did not hold; no chain was provoked");
        TearDown(peel);
        Verdict();
        return;
    }

    UE_LOGI("[SLIP] ARM B (defect): slipping the same player with `dead` TRUE");
    const unsigned long long cancelledBefore = RET::TravelsCancelled();
    const unsigned long long menuSeenBefore  = RET::MenuTravelsSeen();
    const Trigger tb = SlipLocalPlayer(peel);
    UE_LOGI("[SLIP] arm B trigger: %s", TriggerName(tb));
    // Past the chain's own 10 s (deathEnd, +5 s black screen, +5 s travel), with room for the
    // revive that follows a cancel.
    const Watch b = WatchFor(22000);
    const unsigned long long cancelled = RET::TravelsCancelled() - cancelledBefore;
    const unsigned long long menuSeen  = RET::MenuTravelsSeen() - menuSeenBefore;
    // `dead=` is suppressed here: this drill set it before the window opened, so its stamp would be
    // our own write reported beside genuine chain events.
    UE_LOGI("[SLIP] arm B TIMELINE (ms, %d ms sampling; %d/%d samples landed) -- ragdoll=%lld "
            "black=%lld travel=%lld [the chain predicts black~5000, travel~10000]",
            kSampleMs, b.landedSamples, b.totalSamples, b.ragdoll, b.black, b.travel);
    LogState("arm B end", b.last);

    // THE ROW THIS DRILL EXISTS FOR. A black screen five seconds after a zero-damage slip is the
    // death chain; nothing else in this game does that.
    const bool fired = b.black >= 0;
    if (b.landedSamples == 0) {
        Inconclusive("D1 chain-fired", "no sample in the window reached the game thread");
    } else {
        Check(fired, "D1 chain-fired",
              fired ? "the death chain FIRED on a zero-damage ragdoll: `fallen(false)` re-enters at "
                      "@39685 when `dead` is already set, so the peel, the explosion and every "
                      "other ragdoll cause are one defect"
                    : "no black screen -- the chain did NOT fire, and the @39685 reading this drill "
                      "rests on is wrong");
    }
    // Every row below reads the chain's CONSEQUENCES, so each is vacuous without it: a world that
    // never travelled and a player who was never dead satisfy them all.
    if (!fired) {
        Inconclusive("D2 travel-refused", "no chain fired, so no travel was there to refuse");
        Inconclusive("D3 world-kept", "no chain fired, so surviving proves nothing");
        Inconclusive("D4 revived", "no chain fired, so there was no episode to revive from");
    } else {
        // The PAIR, as autotest_runend asserts it: `cancelled` alone cannot tell our seam refusing
        // THIS travel from some other travel being refused in the same window.
        const bool refused = menuSeen >= 1 && cancelled >= 1;
        Check(refused, "D2 travel-refused",
              refused ? "the seam SAW this menu travel and REFUSED it: inside a session the defect "
                        "is already covered, and the field reports are about a peer where it is not"
                      : "the travel was not both seen and cancelled by the seam -- it reaches a "
                        "player");
        const bool kept = b.travel < 0 && b.last.landed && b.last.s.inGameplay;
        Check(kept, "D3 world-kept",
              kept ? "the world survived the chain: nobody went to the menu"
                   : "the world CHANGED -- the run ended for every peer in the session");
        // The black screen is a term here, not a row of its own: the revive clears it best-effort,
        // and a player standing alive behind one is not revived in any sense a player would accept.
        const bool revived = b.last.landed && b.last.s.haveState && !b.last.s.dead &&
                             b.last.s.health > 0.f && !b.last.s.blackScreenInViewport &&
                             coop::death_revive::LastReviveSucceeded();
        Check(revived, "D4 revived",
              revived ? "the revive ran and the player is alive, `dead` cleared and the screen "
                        "clear: the episode was disposed of"
                      : "the player is dead, unreadable, still behind the black screen, or the "
                        "revive itself reported failure");
    }

    TearDown(peel);
    Verdict();
}

DWORD WINAPI SlipDrillThread(LPVOID) {
    RunSlipDrill();
    return 0;
}

}  // namespace harness::autotest
