// harness/autotest/autotest_death.cpp -- the native death-chain instrument
// (VOTVCOOP_RUN_DEATH_TEST), one process in two configurations. `mp.py death --session` is a solo
// host (a session with zero clients), the acceptance run: the native death plays out (about 10 s,
// the black screen at +5 s), the run-ending travel is cancelled at lib_C::loadLevel, and the
// player comes back standing at the KPP with the pause menu reachable. `mp.py death` is
// sessionless, the negative control: single player is untouched, so the travel must still happen
// and the seam refuse nothing. Neither needs a second peer. The observation half never fails (the
// measured timeline from a real lethal Add Player Damage to the travel or its refusal, a memory
// profile, the seam's counters); the acceptance half does, asserted in `death_test:` lines and
// never inferred from a module's own log. The hit is the game's own Add Player Damage, so the
// real lethal chain runs; only the trigger is synthetic.

#include "harness/autotest.h"

#include "harness/autotest/death_state_probe.h"

#include "coop/net/session.h"
#include "coop/config/config.h"
#include "coop/dev/death_write_diff.h"
#include "coop/player/death_revive.h"
#include "coop/player/run_end_travel.h"
#include "ue_wrap/core/script_gate.h"
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

namespace RET = coop::player::run_end_travel;

namespace GT = ue_wrap::game_thread;
namespace E = ue_wrap::engine;
namespace V = ue_wrap::vitals;
namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;


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

// One snapshot, taken on the game thread and handed back to this worker. The readers live in
// harness/autotest/death_state_probe.cpp; the two fields below are the caller's own instant, not
// the task's -- neither reads UObject state, and the RSS sample belongs to the cadence that took
// it, not to whenever the game thread got round to the task.
DeathSnapshot Probe() {
    auto done = std::make_shared<std::atomic<int>>(0);
    auto out = std::make_shared<DeathSnapshot>();
    GT::Post([done, out] { *out = ReadDeathState(); done->store(1); });
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
            "death contract is the ACCEPTANCE half");

    // A pawn that can be killed: canRagdoll true, no invincibility term set, in the gameplay
    // world. Inside a session the arm's readiness is a precondition too, and on a client the link
    // as well: a death before the arm is ready measures net_pump's FLEE, not the seam this run is
    // about. That window is death_revive's own measurement and it logs the span; here it is only
    // waited out, because the two are different questions and a run that cannot tell them apart
    // has answered neither.
    const bool isClient = IsClientRole();
    DeathSnapshot s;
    bool ready = false, linked = false;
    long long armWindowMs = -1;
    for (int i = 0; i < 120 && !ready; ++i) {
        s = Probe();
        linked = !isClient || harness::session_runtime::Session().connected();
        armWindowMs = coop::death_revive::ArmReadyAfterPawnMs();
        // The sessionless control has no arm to wait for, and requiring one would make the arm
        // that proves single player untouched unrunnable.
        const bool armReady = !s.sessionRunning || armWindowMs >= 0;
        ready = s.havePawn && s.haveState && s.haveCanRagdoll && s.health > 0.f &&
                s.canRagdoll && !s.dead && !s.startInvinc && !s.immortal && s.inGameplay &&
                linked && armReady;
        if (!ready) ::Sleep(1000);
    }
    UE_LOGI("death_test: pre-hit state -- role=%s havePawn=%d canRagdoll=%d(read=%d) health=%.2f "
            "startInvinc=%d(read=%d) immortal=%d(read=%d) dead=%d inGameplay=%d "
            "sessionRunning=%d connected=%d armReadyAfterPawn=%lld ms grabValid=%d(read=%d) "
            "rss=%.1f MB",
            isClient ? "CLIENT" : "HOST",
            s.havePawn ? 1 : 0, s.canRagdoll ? 1 : 0, s.haveCanRagdoll ? 1 : 0, s.health,
            s.startInvinc ? 1 : 0, s.haveStartInvinc ? 1 : 0,
            s.immortal ? 1 : 0, s.haveImmortal ? 1 : 0, s.dead ? 1 : 0, s.inGameplay ? 1 : 0,
            s.sessionRunning ? 1 : 0, linked ? 1 : 0, armWindowMs,
            s.grabValid ? 1 : 0, s.haveGrab ? 1 : 0, s.rssMb);
    if (!ready) {
        UE_LOGW("death_test: VERDICT INCONCLUSIVE -- preconditions never met (see the pre-hit "
                "state line above; a held canRagdoll, a set startInvinc/immortal, no gameplay "
                "world, a client that never linked, or an arm that never became ready would "
                "each swallow the run)");
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
    DeathSnapshot last = s;
    for (uint64_t now = tHit; now - tHit < static_cast<uint64_t>(kDeadWindowMs);
         now = ::GetTickCount64()) {
        DeathSnapshot p = Probe();
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

    UE_LOGI("death_test: SEAM -- watching=%d gateEnabled=%d travelsSeen=%llu menuTravels=%llu "
            "cancelled=%llu lastReviveOk=%d sessionRunning=%d (the watch is process-wide; the "
            "SESSION is what gates the verdict, and the gate's enable is session-scoped)",
            RET::WatchInstalled() ? 1 : 0,
            ue_wrap::script_gate::IsEnabled() ? 1 : 0,
            RET::TravelsSeen(), RET::MenuTravelsSeen(), RET::TravelsCancelled(),
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
                tTravel < 0 ? "no level travel inside the window -- the travel was cancelled and "
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
        if (!last.haveLoc)
            _snprintf_s(at, sizeof(at), _TRUNCATE, "no location could be read back from the pawn");
        else
            _snprintf_s(at, sizeof(at), _TRUNCATE,
                        "%.0f cm from the coop KPP (%.0f,%.0f,%.0f) -- delta (%.0f,%.0f,%.0f), "
                        "horiz %.0f vert %.0f -- read back from the pawn, not inferred from the "
                        "teleport's return", dist,
                        P::name::kKPPSpawnX, P::name::kKPPSpawnY, P::name::kKPPSpawnZ,
                        dx, dy, dz, std::sqrt(dx * dx + dy * dy), dz);
        Verdict("D7 at-KPP", last.haveLoc && dist <= 500.f, at);
        // D8 asserted that the revive had RESTORED the menu prep loadLevel stomps. The cut is the
        // loadLevel body now, so those writes are never made and there is nothing to restore: the
        // old assertion would pass whatever happened, which is the same shape as a seam that never
        // saw the travel satisfying "refused nothing". It asserts the invariant that replaced it --
        // the prep was never disturbed at all -- and the reading stays the player-facing one.
        char mp[192];
        _snprintf_s(mp, sizeof(mp), _TRUNCATE,
                    "screenSwi=%d (want 1, ui_menu's own in-game value) canvas_loading vis=%d "
                    "(want 1 = Collapsed, the asset's serialized value) -- untouched, because the "
                    "cancelled body never writes them; wrong here means ESC shows a LOADING SCREEN",
                    last.screenSwiIdx, last.canvasLoadingVis);
        Verdict("D8 menu-untouched",
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
        // The field report's own claim, and the arm that sees it: the session must still be there
        // afterwards. What that report describes -- coop state torn down, a travel to the menu,
        // the host left serving nobody -- is net_pump's flee, which fires exactly when the arm did
        // not. A run that revives the player but loses the session has not answered it. The role
        // split is the pump's own (`net_pump.cpp`): a host is still hosting through every
        // connection state, a client's link is what it has.
        coop::net::Session& sess = harness::session_runtime::Session();
        const bool sessionLive = isClient ? sess.connected() : sess.running();
        char sv[256];
        _snprintf_s(sv, sizeof(sv), _TRUNCATE,
                    "role=%s -- the session is %s (running=%d connected=%d) and the arm was ready "
                    "%lld ms after the pawn; the pump's local-death flee is what takes this away, "
                    "and it fires exactly when the arm did not (its own line is NOT quoted here: "
                    "counting that marker in a log is how the flee is detected, and a verdict "
                    "that spells it plants a hit in every passing run)",
                    isClient ? "CLIENT" : "HOST", sessionLive ? "KEPT" : "GONE",
                    sess.running() ? 1 : 0, sess.connected() ? 1 : 0, armWindowMs);
        Verdict("D14 session-kept", sessionLive, sv);
    } else {
        // Single player, no session: the contract is that nothing of ours acts.
        Verdict("D3 sp-untouched", tTravel >= 0,
                tTravel >= 0 ? "the level travel ran, as vanilla VOTV does -- single player is "
                               "not touched by the arc (the veto's first term is a live session)"
                             : "NO travel happened without a session: the veto fired outside "
                               "coop, which breaks the user's single-player guarantee");
        Verdict("D4 sp-no-revive", !(last.haveState && !last.dead && last.health > 1.f),
                "nothing revived the player, which is correct with no session");
        // Refusing nothing is only half the claim: a seam that never saw the travel refuses
        // nothing either, and D3 above has already established that a travel ran. So the pass
        // needs both terms -- the detour saw it, and let it through.
        // Refusing nothing is only half the claim: a seam that never saw the travel refuses
        // nothing either. The first re-base of this check asked whether the seam SAW the travel,
        // and that rested on something it should not have: with no session the gate's enable is
        // this lane's to withhold, so the callback runs at all only because two OTHER consumers
        // leave the gate enabled in solo. Fixing THAT would have flipped this check silently, on a
        // change that has nothing to do with it. So the verdict is asked DIRECTLY: JudgeMenuTravel is the
        // seam's own classification, and with no session it must answer RunNoSession whatever the
        // gate is doing. That is the single-player guarantee resting on the test itself.
        const RET::Judgement j = RET::JudgeMenuTravel(R::FindObjectByClass(P::name::GamemodeClass));
        const bool refusesToAct = j == RET::Judgement::RunNoSession;
        const bool seamQuiet = RET::TravelsCancelled() == 0;
        Verdict("D5 seam-quiet", refusesToAct && seamQuiet,
                !refusesToAct ? "the seam's own verdict does NOT stand down without a session -- "
                                "single player is one enabled gate away from being judged"
                              : seamQuiet
                                ? "the verdict answers RunNoSession and nothing was cancelled: "
                                  "single player is untouched by the test itself"
                                : "the seam CANCELLED a travel with no session running");
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
