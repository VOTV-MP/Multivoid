// harness/autotest_death.cpp -- the NATIVE DEATH CHAIN instrument
// (VOTVCOOP_RUN_DEATH_TEST). SOLO and SESSIONLESS on purpose: it is launched with
// no net role, so `net_pump`'s local-death flee (gated on a live session) never
// fires and the game's own death is allowed to play out end to end. Nothing about
// the measurement needs a second peer -- the whole chain is local BP.
//
// IT REPORTS TWO DIFFERENT THINGS, AND ONLY ONE OF THEM CAN FAIL.
//
// 1. OBSERVATION (never fails). The measured timeline of VOTV's own death, from a
//    real lethal `Add Player Damage` to the level travel, plus a memory profile
//    across it. This is the RE doc's bytecode chain
//    (research/findings/world-systems/votv-player-death-chain-RE-2026-08-31.md)
//    confronted with the running game: `dead := true` -> +5 s blackScreen_C ->
//    +5 s loadLevel('menu') -> OpenLevel. A disagreement here is a finding.
//
// 2. ACCEPTANCE (fails). docs/DEATH_ARC.md's contract: the whole native death is
//    allowed to run, the travel is cancelled at `UGameplayStatics::OpenLevel`, and
//    the player is revived in place of it. NONE OF THAT IS BUILT YET, so arms
//    D3/D4/D5 are EXPECTED RED and `mp.py death` exits non-zero. That is the
//    point: DEATH_ARC section 8 requires an instrument that fails on the build
//    that lacks the fix, because the retired KO lane's test passed while three
//    HIGH defects were live in the very lane it certified.
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

#include "coop/player/players_registry.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"

#define PSAPI_VERSION 2   // K32GetProcessMemoryInfo from kernel32 -- no psapi.lib link
#include <psapi.h>

#include <atomic>
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
    bool  blackScreen = false;
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
    double rssMb = -1.0;
};

// Read a plain BP bool by name off a live object (byte+mask, same shape as the
// engine's canRagdoll accessor). Test-local: nothing in the mod needs these.
bool ReadBpBool(void* obj, const wchar_t* name, bool& out) {
    if (!obj || !R::IsLive(obj)) return false;
    int32_t byteOff = -1; uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(obj), name, byteOff, mask)) return false;
    out = (*(reinterpret_cast<uint8_t*>(obj) + byteOff) & mask) != 0;
    return true;
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
        }
        if (void* gm = R::FindObjectByClass(P::name::GamemodeClass))
            out->haveImmortal = ReadBpBool(gm, L"immortal", out->immortal);
        float hp = -1.f;
        if (V::Read(V::Field::Health, &hp)) out->health = hp;
        out->blackScreen = R::FindObjectByClass(kBlackScreenClass) != nullptr;
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
            "startInvinc=%d(read=%d) immortal=%d(read=%d) dead=%d inGameplay=%d rss=%.1f MB",
            s.havePawn ? 1 : 0, s.canRagdoll ? 1 : 0, s.haveCanRagdoll ? 1 : 0, s.health,
            s.startInvinc ? 1 : 0, s.haveStartInvinc ? 1 : 0,
            s.immortal ? 1 : 0, s.haveImmortal ? 1 : 0, s.dead ? 1 : 0, s.inGameplay ? 1 : 0,
            s.rssMb);
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
    // 10x max health so it is lethal under any armor/isStrong scaling (the BP
    // multiplies by 0.75 when strong; nothing scales it UP).
    float maxHp = 100.f;
    { auto done = std::make_shared<std::atomic<int>>(0);
      auto mh = std::make_shared<float>(100.f);
      GT::Post([done, mh] { float v = 100.f; if (V::Read(V::Field::MaxHealth, &v)) *mh = v; done->store(1); });
      WaitDone(done, 8000);
      maxHp = *mh; }
    const float lethal = (maxHp > 0.f ? maxHp : 100.f) * 10.f;
    const float hpBefore = s.health;

    auto hitDone = std::make_shared<std::atomic<int>>(0);
    auto hitOk = std::make_shared<int>(0);
    GT::Post([hitDone, hitOk, lethal] {
        void* mp = coop::players::Registry::Get().Local();
        if (mp && R::IsLive(mp) && E::InvokeAddPlayerDamage(mp, lethal)) *hitOk = 1;
        hitDone->store(1);
    });
    WaitDone(hitDone, 8000);
    const uint64_t tHit = ::GetTickCount64();
    UE_LOGI("death_test: delivered Add Player Damage(%.0f) (health was %.2f, invoke=%s)",
            lethal, hpBefore, *hitOk ? "ok" : "FAILED");

    // ---- observe the chain ---------------------------------------------------
    MemWindow dead;
    long long tDead = -1, tRagdoll = -1, tBlack = -1, tTravel = -1;
    bool sawZeroHealth = false;
    Sample last = s;
    for (uint64_t now = tHit; now - tHit < static_cast<uint64_t>(kDeadWindowMs);
         now = ::GetTickCount64()) {
        Sample p = Probe();
        const long long dt = static_cast<long long>(::GetTickCount64() - tHit);
        if (p.haveState && p.dead && tDead < 0) tDead = dt;
        if (p.haveState && p.isRagdoll && tRagdoll < 0) tRagdoll = dt;
        if (p.blackScreen && tBlack < 0) tBlack = dt;
        if (p.haveWorld && !p.inGameplay && tTravel < 0) tTravel = dt;
        if (p.health <= 0.f && p.health >= -0.5f) sawZeroHealth = true;
        // Only the in-world part of the run is a memory measurement; once the
        // travel starts, RSS is dominated by the teardown + the new level.
        if (tTravel < 0) dead.Add(p.rssMb);
        last = p;
        ::Sleep(kSampleMs);
    }
    dead.ms = (tTravel > 0 ? static_cast<uint64_t>(tTravel) : static_cast<uint64_t>(kDeadWindowMs));

    UE_LOGI("death_test: TIMELINE (ms after the hit) -- dead=%lld ragdoll=%lld blackScreen=%lld "
            "travel=%lld  [RE predicts dead~0, blackScreen~5000, travel~10000]",
            tDead, tRagdoll, tBlack, tTravel);
    UE_LOGI("death_test: DEAD window memory -- %.1f -> %.1f MB over %llu ms (%.2f MB/s, peak %.1f); "
            "ALIVE control %.2f MB/s; DIFFERENTIAL %.2f MB/s",
            dead.firstMb, dead.lastMb, static_cast<unsigned long long>(dead.ms),
            dead.SlopeMbPerSec(), dead.peakMb, alive.SlopeMbPerSec(),
            dead.SlopeMbPerSec() - alive.SlopeMbPerSec());

    // ---- ACCEPTANCE ----------------------------------------------------------
    // D1/D2 are the falsifiers: without them, "the world survived" would pass on a
    // run where the hit simply never landed.
    Verdict("D1 death-ran", tDead >= 0 && (sawZeroHealth || tRagdoll >= 0),
            tDead >= 0 ? "dead=true was observed -- the lethal chain really started"
                       : "dead never became true; the hit did not kill, so nothing below "
                         "means anything");
    Verdict("D2 ritual-played", tBlack >= 0,
            tBlack >= 0 ? "blackScreen_C reached the viewport -- the native death was allowed "
                          "to play out"
                        : "no blackScreen_C ever appeared; the chain did not reach uber @4353");
    Verdict("D3 world-survived", tTravel < 0,
            tTravel < 0 ? "no level travel inside the window -- the world was kept"
                        : "the level travel ran: the world was torn down and the player is in "
                          "the main menu (EXPECTED until the OpenLevel seam lands)");
    Verdict("D4 revived", last.haveState && !last.dead,
            (last.haveState && !last.dead)
                ? "dead is false again -- something cleared it"
                : "dead is still true (or unreadable): no revive happened (EXPECTED until the "
                  "arc lands)");
    Verdict("D5 standing", last.havePawn && last.haveState && !last.isRagdoll && last.health > 1.f,
            (last.havePawn && last.haveState && !last.isRagdoll && last.health > 1.f)
                ? "the player is up, off the ragdoll, with positive health"
                : "the player is not standing with health (EXPECTED until the arc lands)");
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
