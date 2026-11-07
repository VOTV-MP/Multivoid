// coop/dev/mannequin_walker_drill.cpp -- see coop/dev/mannequin_walker_drill.h.
// WHAT IT MEASURES. (1) A client's mannequin spawner is refused at the gate: the refusal counter of spawn_authority's row rises when
// the client runs the spawner's own body. (2) One walker, the host's: the client holds one copy for its key, and while the host's
// walker moves the copy follows it (the drive stream), within kFollowTolCm. (3) is absent, see the .h. (4) The target's gaze: the
// client is walker B's target, its camera on the copy stops the host's walker, and when it looks away the walker moves again within
// the Blueprint's own timer_saw bound plus a margin, the HOST's own camera on it throughout (ruling (a): only the target's gaze
// freezes it) and the host's gaze reads in that window counted as answered with the target's level. Then steps_b: (6) B's hit lands
// on the client, (5) the client leaves, (7) it comes back and is told B's state, (8) the ticker's statue route crosses.
// THE TWO WALKERS. A walker KEEPS its target until it is lost (coop/creatures/walker_target.h), so the first player in range is the
// one taken. Walker A: the client waits at the KPP gate, out of its range, the host walks in, and the host's arrival opens the window
// while the walker is still far. Walker B is born while the host stands at the KPP gate, out of every spawn point's range, and the
// client walks in last: it is the only player in range, so it is B's target. Between them A is destroyed.
// THE PEERS. Both reach the same state through peer_signal (VOTVCOOP_CAN_DIR); a wait is on a state the other peer reached, bounded,
// and a bound that runs out before a verdict is INVALID. The host judges.

#include "coop/dev/mannequin_walker_drill.h"

#include "harness/autotest.h"
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/walker_brain.h"
#include "coop/creatures/walker_gaze_host.h"
#include "coop/creatures/walker_state_client.h"
#include "coop/creatures/walker_target.h"
#include "coop/dev/director/director.h"
#include "coop/dev/mannequin_drill.h"
#include "coop/dev/mannequin_walker_geometry.h"
#include "coop/dev/mannequin_walker_steps_b.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace coop::dev::mannequin_walker_drill {
namespace {

namespace AC  = ue_wrap::ambient_creature;
namespace DIR = coop::director;
namespace E   = ue_wrap::engine;
namespace OI  = ue_wrap::object_index;
namespace P   = ue_wrap::profile;
namespace PS  = harness::autotest::peer_signal;
namespace R   = ue_wrap::reflection;
namespace V   = ue_wrap::vitals;
namespace WGH = coop::walker_gaze_host;
namespace WT  = coop::walker_target;
using coop::rig_verdict::Outcome;
using ue_wrap::FRotator;
using ue_wrap::FVector;

constexpr const char* kSpawnerTag = "mannequinSpawner.ReceiveTick";

// The walker's own hunt range (walker_target's kRangeCm, the Blueprint's test at its call end 41), and the margin that makes
// "out of range" mean it: the host at its start stands far beyond it of every spawn point, so this only guards a changed start.
constexpr float kRangeCm = 20000.f;
constexpr float kOutOfRangeMarginCm = 2000.f;
// Where each player stops. A target enters the walker's range at the bed anchor, the spot the director's stage places a walk at
// (coop/player/shelter_spot, Kind::Bed): a walk to it is placed there at once, with no route to walk, and EnterRange then walks
// the player toward the walker until the walker TAKES it as its target (an event, not a distance: the walker closes on its target
// whatever the route, so a distance chosen beforehand does not hold). Walker A: the client does not walk (it waits at the KPP
// gate, out of range, so it is never the target); the host goes to its bed anchor and steps into range, and its arrival opens the
// window. Walker B: the host back at the KPP gate, out of range, then the client walks to the open spot the host stood at,
// last. The walker closes on its target while the steps run, and a measurement ends when it is within kApproachEndCm: at that
// distance it stands at its target's shoulder, where the game's own frustum test of its bounds holds whatever the yaw.
constexpr float kKppReachCm = 1500.f;

// Bounds, each a readiness wait that ends sooner on its state. timer_saw is the Blueprint's own 10-20 s release.
constexpr int kTimerSawS = 20;
constexpr int kMarginS = 15;
constexpr int kSpawnBoundS = 30;
constexpr int kSlotBoundS = 240;   // a client learns its slot at its join, within the boot and the world load
constexpr int kTargetBoundS = 30;
constexpr int kMoveBoundS = kTimerSawS + 25;      // from its birth or from a release to its first steps
constexpr int kReleaseBoundS = kTimerSawS + kMarginS;
constexpr int kFreezeBoundS = 15;
constexpr int kViewBoundS = 20;
constexpr int kThinkBoundS = 20;   // a copy's graph refusals and its nav pawn's controller, from the copy's birth

// The sampling: one reading per kSampleMs. Moving is the walker's displacement over kMovingSamples readings, still the same
// over kStillSamples; the window is how long both peers sample for step 2.
constexpr int    kSampleMs = 100;
constexpr size_t kMovingSamples = 10;
constexpr float  kMovingCm = 60.f;
constexpr size_t kStillSamples = 20;
constexpr float  kStillCm = 30.f;
constexpr double kWindowS = 8.0;                       // of walking, in step 2's window
constexpr int    kApproachWindowS = kMoveBoundS + 8;   // the most the window runs: the walker's wait to be let go, and kWindowS
constexpr float  kMinMoveCm = 500.f;
constexpr size_t kMinComparedReadings = 20;   // copy readings that overlap the host's window, or step 2 had too little to compare
// The copy's distance to where the host's walker stood a short lag before, the nearest of kLagSteps lags of kLagStepS: the
// stream's own delay is no error, a copy that stands or walks elsewhere is. Measured on the first rig run.
constexpr float  kFollowTolCm = 150.f;
constexpr double kLagStepS = 0.05;
constexpr int    kLagSteps = 10;

double NowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The walker's own view test, as this client's gaze lane judges it.
bool InView(void* copy) {
    void* controller = AC::WalkerGazeController(copy);
    return controller && AC::WalkerInView(copy, controller);
}

// A walker's displacement over its last readings.
struct Track {
    std::vector<FVector> p;
    void  Push(const FVector& v) { p.push_back(v); }
    // From the reading `n` back to the newest; negative while fewer are held.
    float Span(size_t n) const { return p.size() > n ? Dist3(p.back(), p[p.size() - 1 - n]) : -1.f; }
    bool  Moving() const { return Span(kMovingSamples) >= kMovingCm; }
    bool  Still() const { const float s = Span(kStillSamples); return s >= 0.f && s <= kStillCm; }
};

// ---- samples ----

struct Sample {
    double t = 0;
    float  x = 0, y = 0, z = 0;
};

// A peer's readings of the copy as one line: the copy's distance from this peer's player at the first reading and at the last
// (so a copy out of the stream's reach is told from one that is followed badly), then the readings.
std::string Pack(const std::vector<Sample>& v, float startCm, float endCm) {
    char head[48];
    std::snprintf(head, sizeof(head), "%.0f %.0f ", startCm, endCm);
    std::string out = head;
    char b[96];
    for (const Sample& s : v) {
        std::snprintf(b, sizeof(b), "%.6f %.1f %.1f %.1f ", s.t, s.x, s.y, s.z);
        out += b;
    }
    return out;
}

std::vector<Sample> Unpack(const std::string& text, float& startCm, float& endCm) {
    std::vector<Sample> out;
    std::istringstream in(text);
    startCm = endCm = -1.f;
    in >> startCm >> endCm;
    Sample s;
    while (in >> s.t >> s.x >> s.y >> s.z) out.push_back(s);
    return out;
}

// Both peers read their walker every kSampleMs for `seconds`, with the view turned away from it throughout.
template <class ActorOf>
std::vector<Sample> Window(ActorOf actorOf, double seconds) {
    std::vector<Sample> out;
    const double end = NowSec() + seconds;
    while (NowSec() < end) {
        OnGameThread([&] {
            void* w = actorOf();
            FVector at{};
            if (!w || !R::IsLive(w) || !E::TryGetActorLocation(w, at)) return;
            Aim(w, false);
            out.push_back(Sample{NowSec(), at.X, at.Y, at.Z});
        });
        ::Sleep(kSampleMs);
    }
    return out;
}

bool HostAt(const std::vector<Sample>& h, double t, FVector& out) {
    if (h.size() < 2 || t < h.front().t || t > h.back().t) return false;
    for (size_t i = 1; i < h.size(); ++i) {
        if (h[i].t < t) continue;
        const Sample &a = h[i - 1], &b = h[i];
        const double f = b.t > a.t ? (t - a.t) / (b.t - a.t) : 0.0;
        out = FVector{static_cast<float>(a.x + (b.x - a.x) * f), static_cast<float>(a.y + (b.y - a.y) * f),
                      static_cast<float>(a.z + (b.z - a.z) * f)};
        return true;
    }
    return false;
}

struct Follow {
    float  maxCm = 0, meanCm = 0;
    size_t n = 0;
};

// Each copy reading against the host walker's track, at its best lag.
Follow Compare(const std::vector<Sample>& host, const std::vector<Sample>& copy) {
    Follow f;
    double sum = 0;
    for (const Sample& c : copy) {
        float best = -1.f;
        for (int k = 0; k <= kLagSteps; ++k) {
            FVector at{};
            if (!HostAt(host, c.t - k * kLagStepS, at)) continue;
            const float d = Dist3(at, FVector{c.x, c.y, c.z});
            if (best < 0.f || d < best) best = d;
        }
        if (best < 0.f) continue;
        f.maxCm = (std::max)(f.maxCm, best);
        sum += best;
        ++f.n;
    }
    f.meanCm = f.n ? static_cast<float>(sum / static_cast<double>(f.n)) : 0.f;
    return f;
}

float Excursion(const std::vector<Sample>& v) {
    float widest = 0.f;
    for (const Sample& s : v) widest = (std::max)(widest, Dist3(FVector{s.x, s.y, s.z}, FVector{v[0].x, v[0].y, v[0].z}));
    return widest;
}

// The INVALID of a walker that never walked: how far it stands from the player it hunts, and its own `looking` flag.
// A walker already at its target had no room to walk: a defect of this drill's geometry, said as such.
bool NeverWalked(const char* who, float cm, bool looking, int boundS) {
    char why[220];
    if (cm >= 0.f && cm < kApproachEndCm) {
        std::snprintf(why, sizeof(why), "%s reached its target before the window opened (%.0f cm)", who, cm);
        return Invalid(why);
    }
    std::snprintf(why, sizeof(why), "%s never started walking within %d s of its hunted player's view turned away: %.0f cm from "
                  "that player, looking=%d", who, boundS, cm, looking ? 1 : 0);
    return Invalid(why);
}

// ---- red arms ----

// The module a red disables reads its own value of the row; this drill only names which are its own.
bool Is(const char* value) {
    static const std::string s_mode = coop::config::ResolveEnum(::coop::config_registry::rows::mannequin_drill);
    return s_mode == value;
}

// ---- the host ----

// The game's own spawn, asked as MAN-0's instrument asks it: a new walker, its key, and its distance from the host.
bool SpawnOne(void*& walker, std::string& key, float& hostCm) {
    std::vector<void*> had;
    OnGameThread([&] { had = Walkers(); });
    coop::dev::mannequin_drill::SpawnWalker();
    walker = nullptr;
    WaitGT("the game's own spawn put a walker up", kSpawnBoundS, [&] {
        for (void* a : Walkers())
            if (std::find(had.begin(), had.end(), a) == had.end()) {
                walker = a;
                return true;
            }
        return false;
    }, false);
    if (!walker) return Invalid("the game's own spawn gave no walker");
    key.clear();
    WaitGT("the walker has its key", 15, [&] { return !(key = KeyOf(walker)).empty(); }, false);
    if (key.empty()) return Invalid("the walker has no key");
    OnGameThread([&] { hostCm = PlayerTo(walker); });
    UE_LOGI("mannequin-drill: walker '%s' stands %.0f cm from the host", key.c_str(), hostCm);
    return true;
}

// Whether the host's walker hunts `slot`, as walker_target keeps it; the host's view stays turned away from it while asked.
bool Hunts(void* walker, uint8_t slot) {
    Aim(walker, false);
    return WT::TargetOf(walker) == slot;
}

// What the host read of its walker in step 2's window: the readings, the walker's distance from the host at the first and the last,
// whether it walked, and its `looking` flag at the last.
struct Approach {
    std::vector<Sample> samples;
    float               startCm = -1.f;
    float               endCm = -1.f;
    float               clientStartCm = -1.f;   // the client's puppet from the walker, at the first and the last reading
    float               clientEndCm = -1.f;
    bool                moved = false;
    bool                looking = false;
};

// The host's readings of its walker, from the host's arrival: one every kSampleMs, with the view turned away. The walker has been
// hunting since the host crossed its range, and its own timer_saw lets it go within kMoveBoundS of that view turning away, so the
// window runs kApproachWindowS and ends sooner: once the walker has walked kWindowS, or is within kApproachEndCm of the host.
Approach ApproachWindow(void* walker) {
    Approach r;
    Track trk;
    size_t walkedReadings = 0;
    const size_t walkedNeeded = static_cast<size_t>(kWindowS * 1000.0 / kSampleMs);
    const double end = NowSec() + kApproachWindowS;
    bool done = false;
    while (!done && NowSec() < end) {
        OnGameThread([&] {
            FVector at{};
            if (!R::IsLive(walker) || !E::TryGetActorLocation(walker, at)) {
                done = true;
                return;
            }
            Aim(walker, false);
            r.samples.push_back(Sample{NowSec(), at.X, at.Y, at.Z});
            trk.Push(at);
            r.endCm = PlayerTo(walker);
            if (r.startCm < 0.f) r.startCm = r.endCm;
            r.clientEndCm = ClientTo(walker);
            if (r.clientStartCm < 0.f) r.clientStartCm = r.clientEndCm;
            r.looking = AC::ReadWalkerLooking(walker);
            if (trk.Moving()) {
                r.moved = true;
                ++walkedReadings;
            }
            done = (r.endCm >= 0.f && r.endCm < kApproachEndCm) || walkedReadings >= walkedNeeded;
        });
        ::Sleep(kSampleMs);
    }
    return r;
}

bool HostPhaseA(void*& a, std::string& keyA) {
    float hostCm = 0.f;
    if (!SpawnOne(a, keyA, hostCm)) return false;
    if (hostCm <= kRangeCm + kOutOfRangeMarginCm)
        return Invalid("the host stands within the walker's range at its birth: the host must enter it first, from outside");
    PS::Mark("walker_a", keyA);
    if (!AwaitClient("copy_a", 60)) return false;

    // The client waits where it stands, at the KPP gate beside the host's start and out of the walker's range, so it is never A's
    // target; the host walks, staged or not (it is the intended target), and the walker hunts it from the moment it crosses the
    // range.
    // The puppet of the client's slot is spawned a moment after its ready edge: the first read waits for it. Every later read of it
    // (ApproachWindow, ClientFarEnough in phase B) is behind this wait, and an unread value there is only said or judged INVALID.
    float clientCm = -1.f;
    if (!WaitGT("the client's puppet stands readable on the host", kPuppetBoundS, [&] { return (clientCm = ClientTo(a)) >= 0.f; }, false))
        return Invalid("the client's puppet never stood readable on the host");
    UE_LOGI("mannequin-drill: the client stands %.0f cm from walker A", clientCm);
    if (clientCm <= kRangeCm + kOutOfRangeMarginCm) {
        char why[200];
        std::snprintf(why, sizeof(why), "the client stands %.0f cm from walker A, inside its %.0f range plus margin (or unread): it would "
                      "be hunted", clientCm, kRangeCm);
        return Invalid(why);
    }
    FVector bed{};
    if (!BedAnchor(bed)) return Invalid("the host has no bed anchor to walk to");
    if (!WalkTo(bed, kAnchorReachCm, "the host to the bed anchor")) return Invalid("the host's walk to the bed anchor did not arrive");
    if (!EnterRange(a, "the host", [](void* w) { return WT::TargetOf(w) == 0; })) return false;

    // At the host's arrival: its view turns away, its distance from the walker is judged (in range, not at its shoulder), and the
    // window opens: nothing else sits between.
    float   arrivalCm = -1.f;
    FVector spot{};
    OnGameThread([&] {
        Aim(a, false);
        arrivalCm = PlayerTo(a);
        if (void* me = coop::players::Registry::Get().Local()) E::TryGetActorLocation(me, spot);
    });
    if (arrivalCm < kApproachEndCm || arrivalCm >= kRangeCm) {
        char why[200];
        std::snprintf(why, sizeof(why), "the host stands %.0f cm from walker A after stepping into range: not between %.0f and %.0f",
                      arrivalCm, kApproachEndCm, kRangeCm);
        return Invalid(why);
    }
    // Where the host stands is in the spawn point's range and reached over the NavMesh from the bed, so it is outside the house: the
    // client walks there for walker B, whose copy it can then see.
    char spotText[96];
    std::snprintf(spotText, sizeof(spotText), "%.1f %.1f %.1f", spot.X, spot.Y, spot.Z);
    PS::Mark("host_open_spot", spotText);
    PS::Mark("window_go");
    const Approach w = ApproachWindow(a);
    uint8_t target = WT::kNoTarget;
    OnGameThread([&] { target = WT::TargetOf(a); });
    UE_LOGI("mannequin-drill: walker A hunts slot %d; %.0f cm from the host at the window's start, %.0f at its end", static_cast<int>(target),
            w.startCm, w.endCm);
    if (w.startCm < 0.f || w.startCm > kRangeCm) return Invalid("the host is not within walker A's range at its arrival");
    if (!w.moved) {
        if (target != 0) return Invalid("walker A does not hunt the host");
        return NeverWalked("walker A", w.endCm, w.looking, kApproachWindowS);
    }
    std::string text;
    if (!PS::AwaitText("copy_samples_a", text, 90)) return false;
    float copyStartCm = -1.f, copyEndCm = -1.f;
    const std::vector<Sample> copy = Unpack(text, copyStartCm, copyEndCm);
    const std::vector<Sample>& host = w.samples;
    const float moved = Excursion(host);
    UE_LOGI("mannequin-drill: step 2 window: host walker moved %.0f cm over %zu readings, the copy gave %zu; the client stands %.0f cm "
            "(its puppet here, at the window's start, %.0f at its end) and %.0f cm (its own read of the copy, at the start, %.0f at its "
            "end) from the walker", moved, host.size(), copy.size(), w.clientStartCm, w.clientEndCm, copyStartCm, copyEndCm);
    if (moved < kMinMoveCm || copy.empty()) return Invalid("step 2 had no mover to compare: the host walker did not walk in the window");
    const Follow f = Compare(host, copy);
    UE_LOGI("mannequin-drill: step 2 follow: %zu copy readings, worst %.0f cm, mean %.0f cm (tolerance %.0f)", f.n, f.maxCm,
            f.meanCm, kFollowTolCm);
    if (f.n < kMinComparedReadings) return Invalid("step 2 had too few copy readings in the host's window to compare");
    char what[128];
    std::snprintf(what, sizeof(what), "the client's copy follows the host's walker (worst %.0f cm, %zu readings)", f.maxCm, f.n);
    if (!Check(f.maxCm <= kFollowTolCm, "2", what)) return Fail("2", what);
    return true;
}

// Walker B stands at least kApproachEndCm from the client as `when` happens; nearer, the gaze steps are not the measurement.
bool ClientFarEnough(void* b, const char* when) {
    float cm = -1.f;
    OnGameThread([&] { cm = ClientTo(b); });
    UE_LOGI("mannequin-drill: walker B is %.0f cm from the client as %s", cm, when);
    if (cm >= kApproachEndCm) return true;
    char why[200];
    std::snprintf(why, sizeof(why), "walker B is %.0f cm from the client (under %.0f, or unread) before %s", cm, kApproachEndCm, when);
    return Invalid(why);
}

// Walker B, steps 4 to 7 (numbered as in mannequin_walker_drill.h). Step 4: the client is its target; the client's gaze stops it,
// and its look away lets it go again. Then steps 6, 5 and 7 (coop/dev/mannequin_walker_steps_b): its hit lands on the client, the
// client leaves, and the rejoined client is told B's state.
bool HostPhaseB(void* a, void*& b, std::string& keyB, int& clientFails) {
    // Between the walkers: A ends, and the host goes back to the KPP gate, out of every spawn point's range, so that the client
    // is the only player in range at B's first read.
    OnGameThread([&] { AC::Destroy(a); });
    const FVector kpp{P::name::kKPPSpawnX, P::name::kKPPSpawnY, P::name::kKPPSpawnZ};
    if (!WalkTo(kpp, kKppReachCm, "the host back to the KPP gate")) return Invalid("the host's walk back to the KPP gate did not arrive");
    float hostCm = 0.f;
    if (!SpawnOne(b, keyB, hostCm)) return false;
    if (hostCm <= kRangeCm + kOutOfRangeMarginCm)
        return Invalid("the host stands within walker B's range at its birth: the client must be the only player in it");
    PS::Mark("walker_b", keyB);
    if (!AwaitClient("copy_b", 60)) return false;
    if (!PS::Await("client_close", kWalkWaitS)) return false;
    if (!WaitGT("walker B hunts the client", kTargetBoundS, [&] { return Hunts(b, kClientSlot); }, false))
        return Invalid("walker B does not hunt the client");

    // Its release is the Blueprint's own: nobody looks at it yet, and it walks once its timer_saw has let go.
    Track walking;
    const bool moving = WaitGT("walker B walks with the client's view turned away", kMoveBoundS, [&] {
        FVector p{};
        Aim(b, false);
        if (!E::TryGetActorLocation(b, p)) return false;
        walking.Push(p);
        return walking.Moving();
    }, false);
    if (!moving) {
        float cm = -1.f;
        bool  looking = false;
        OnGameThread([&] {
            cm = ClientTo(b);
            looking = AC::ReadWalkerLooking(b);
        });
        return NeverWalked("walker B", cm, looking, kMoveBoundS);
    }
    if (!ClientFarEnough(b, "the gaze would be aimed")) return false;
    PS::Mark("aim");
    if (!PS::Await("sees", kCloseInS + kViewBoundS)) return false;
    if (!ClientFarEnough(b, "the freeze is judged")) return false;

    // The client's camera is on the copy: the host's walker must FREEZE, the Blueprint's own `looking` set and the walker standing
    // still together. A stop without `looking` is a pause between the walker's moves, not a freeze (measured under the
    // no_gaze_route red: the walker stopped with `looking` false), so it does not pass; a `looking` that never comes is this step's FAIL.
    Track standing;
    bool looking = false;
    const bool frozen = WaitGT("walker B freezes while its target looks at it", kFreezeBoundS, [&] {
        FVector p{};
        Aim(b, false);
        if (!E::TryGetActorLocation(b, p)) return false;
        standing.Push(p);
        looking = AC::ReadWalkerLooking(b);
        return looking && standing.Still();
    }, false);
    UE_LOGI("mannequin-drill: step 4 freeze: frozen=%d looking=%d", frozen ? 1 : 0, looking ? 1 : 0);
    const char* const stopWhat = "the host's walker stops while its target's camera is on it";
    if (!Check(frozen, "4", stopWhat)) return Fail("4", stopWhat);

    // The client looks away: the walker moves again when its own timer_saw clears `saw`, within the timer's bound and a margin.
    PS::Mark("look_away");
    if (!PS::Await("away", 60)) return false;
    if (!ClientFarEnough(b, "the release is judged")) return false;
    // The host's own camera is on B for the whole release wait: ruling (a) says only the TARGET's gaze freezes the walker, so a
    // host camera that still reached the walker's `looking` would keep it frozen. Two forms, both built, because the aim alone can
    // prove nothing here: B is born ~63000 cm from the host (measured at its birth in a green run; its distance at the release
    // is read below as hostToB) and the mesh may not be drawn at such a distance (an inference, no draw distance is measured),
    // so the host's own test may be false whatever the aim. The second form needs no sight: the walker's gaze reads, at its pinned sites,
    // are overwritten with the client's level while the client hunts it, and the host's reads in the window are counted.
    uint32_t answered0 = 0, answered1 = 0;
    float    hostToB = -1.f;
    bool     hostSees = false;
    OnGameThread([&] { answered0 = WGH::AnsweredReads(); });
    Track released;
    const bool goes = WaitGT("walker B walks again after its target looks away", kReleaseBoundS, [&] {
        FVector p{};
        Aim(b, true);
        if (!E::TryGetActorLocation(b, p)) return false;
        released.Push(p);
        return released.Moving();
    }, false);
    OnGameThread([&] {
        answered1 = WGH::AnsweredReads();
        hostToB = PlayerTo(b);
        hostSees = InView(b);
        Aim(b, false);   // the host's view goes away again before the hit step
    });
    UE_LOGI("mannequin-drill: step 4 release: the host's camera was on walker B (%.0f cm from the host; in the host's view now=%d), "
            "%u gaze reads answered with the target's level", hostToB, hostSees ? 1 : 0, answered1 - answered0);
    char what[200];
    std::snprintf(what, sizeof(what), "the host's walker moves again within %d s of its target looking away, the host's own camera on it",
                  kReleaseBoundS);
    if (!Check(goes, "4", what)) return Fail("4", what);
    std::snprintf(what, sizeof(what), "the host's gaze reads at the walker's sites are answered with its target's level, not the host's "
                  "camera (%u answered in the release window)", answered1 - answered0);
    if (!Check(answered1 > answered0, "4", what)) return Fail("4", what);
    return steps_b::HostB(b, keyB, clientFails);
}

// ---- the client ----

// The mannequin spawner's own body, run here: its refusal counter must rise (step 1).
bool RunSpawner() {
    void* ticker = nullptr;
    if (void* c = OI::ClassByName(L"ticker_mannequinSpawner_C"))
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) *static_cast<void**>(ctx) = obj;
        }, &ticker);
    void* fn = ticker ? R::FindDispatchFunctionCached(R::ClassOf(ticker), L"ReceiveTick") : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<float>(L"DeltaSeconds", 0.016f);
    return ue_wrap::Call(ticker, f);
}

bool CheckRefusal(const char* mark) {
    uint64_t before = 0, after = 0;
    bool ran = false;
    OnGameThread([&] {
        before = coop::spawn_authority::RefusalCount(kSpawnerTag);
        ran = RunSpawner();
        after = coop::spawn_authority::RefusalCount(kSpawnerTag);
    });
    if (!ran) return Invalid("this client has no mannequin spawner to run");
    char what[128];
    std::snprintf(what, sizeof(what), "this client's mannequin spawner body is refused (%llu -> %llu)",
                  static_cast<unsigned long long>(before), static_cast<unsigned long long>(after));
    if (!Check(after > before, "1", what)) return FailedToHost(mark, "1", what);
    return true;
}

// The host's walker `key` has its copy here, once (step 2); a copy that never came is the lane's failure, said at once.
bool AwaitCopy(const std::string& key, const char* label, const char* mark) {
    int n = 0;
    const bool there = WaitGT("the walker's copy stands here", 60, [&] { return ByKey(key, &n) != nullptr; }, false);
    if (!there) {
        UE_LOGW("mannequin-drill: step 2 FAIL walker %s has no copy on this client", label);
        coop::rig_verdict::Say(kTag, Outcome::Fail, "walker %s has no copy on this client", label);
        return false;
    }
    OnGameThread([&] { ByKey(key, &n); });
    char what[128];
    std::snprintf(what, sizeof(what), "this client holds exactly one copy of walker %s for its key (%d)", label, n);
    if (!Check(n == 1, "2", what)) return FailedToHost(mark, "2", what);
    return true;
}

// Step 2c: the copy thinks nothing. Its nav pawn (the walker's `pawn`, a childPawn_C) has no controller, and the gate has refused
// entries of the walker's graph, within a bound. With the gate off (no_brain_gate) the pawn has its AI controller and nothing is
// refused. The copy's word goes to the host at `mark`, "ok" or the failure.
bool CheckThinksNothing(const std::string& key, const char* mark) {
    uint32_t refused = 0;
    bool     pawnRead = false, noController = false;
    WaitGT("the copy's graph is refused and its nav pawn has no controller", kThinkBoundS, [&] {
        void* c = ByKey(key);
        if (!c) return false;
        const int32_t off = R::FindPropertyOffset(R::ClassOf(c), L"pawn");
        void* pawn = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<uint8_t*>(c) + off) : nullptr;
        pawnRead = pawn && R::IsLive(pawn);
        noController = pawnRead && !E::GetController(pawn);
        refused = coop::walker_brain::RefusedCalls();
        return refused > 0 && noController;
    }, false);
    char what[160];
    std::snprintf(what, sizeof(what), "this client's copy of walker A thinks nothing (nav pawn controller=%s, %u entries refused)",
                  noController ? "none" : pawnRead ? "SET" : "unread", refused);
    if (!Check(refused > 0 && noController, "2", what)) return FailedToHost(mark, "2", what);
    PS::Mark(mark, "ok");
    return true;
}

bool ClientBody() {
    if (!HostPuppetUp()) return Invalid("this client's world or the host's puppet never came up");
    if (!harness::autotest::meet::WalkToHost(kTag)) return Invalid("this client could not reach the host");
    if (!CheckRefusal("client_ready")) return false;
    PS::Mark("client_ready", "ok");

    // Walker A: the host hunts it. This client walks first and stops outside its range, so it is never its target.
    std::string keyA;
    if (!PS::AwaitText("walker_a", keyA, 240) || !AwaitCopy(keyA, "A", "copy_a") || !CheckThinksNothing(keyA, "copy_a")) return false;
    // It does not walk: it stands at the KPP gate, out of range, with its view turned away from the copy.
    void* copy = nullptr;
    OnGameThread([&] { Aim(ByKey(keyA), false); });
    if (!PS::Await("window_go", kWalkWaitS)) return false;
    float copyStartCm = -1.f, copyEndCm = -1.f;
    OnGameThread([&] { copyStartCm = PlayerTo(ByKey(keyA)); });
    const std::vector<Sample> samples = Window([&] { return ByKey(keyA); }, kApproachWindowS);
    OnGameThread([&] { copyEndCm = PlayerTo(ByKey(keyA)); });
    PS::Mark("copy_samples_a", Pack(samples, copyStartCm, copyEndCm));
    FVector at{};

    // Walker B: this client is its target. It walks to the host's open spot, last, with its view turned away; then its camera goes on
    // the copy, and off it.
    std::string keyB;
    if (!PS::AwaitText("walker_b", keyB, 2 * kWalkWaitS) || !AwaitCopy(keyB, "B", "copy_b")) return false;
    PS::Mark("copy_b", "ok");
    // It walks to the spot the host stood at for walker A: in range of the spawn point and outside the house, where the copy can be
    // seen (a bed anchor stands inside it, behind walls).
    std::string spotText;
    if (!PS::AwaitText("host_open_spot", spotText, 60)) return false;
    std::istringstream spotIn(spotText);
    spotIn >> at.X >> at.Y >> at.Z;
    if (!WalkTo(at, kAnchorReachCm, "this client to the host's open spot")) return Invalid("this client's walk to the host's open spot did not arrive");
    // The copy's target as the host's WalkerState delivered it: this client's own slot, from inside the walker's range.
    const auto takesMe = [](void* c) {
        return coop::walker_state_client::TargetOfCopy(c) == coop::players::Registry::Get().LocalPeerId() && PlayerTo(c) <= kInRangeCm;
    };
    const bool taken = WaitGT("walker B takes this client", kHuntGraceS, [&] {
        copy = ByKey(keyB);
        return copy && takesMe(copy);
    }, false);
    // Not taken from the spot (the spawn point is farther from it than the host's walker A was): the step-in walk of the host's
    // own phase, toward the copy until it takes this client.
    if (!taken && !EnterRange(copy, "this client", [](void* c) {
            return coop::walker_state_client::TargetOfCopy(c) == coop::players::Registry::Get().LocalPeerId();
        }))
        return false;
    float arrivalCm = -1.f;
    OnGameThread([&] {
        copy = ByKey(keyB);
        arrivalCm = PlayerTo(copy);
        Aim(copy, false);
    });
    UE_LOGI("mannequin-drill: this client stands %.0f cm from walker B's copy after the host's open spot, taken at once=%d", arrivalCm,
            taken ? 1 : 0);
    if (arrivalCm < kApproachEndCm || arrivalCm >= kRangeCm) {
        char why[200];
        std::snprintf(why, sizeof(why), "this client stands %.0f cm from walker B's copy at the host's open spot: not between %.0f "
                      "and %.0f", arrivalCm, kApproachEndCm, kRangeCm);
        return Invalid(why);
    }
    PS::Mark("client_close");
    if (!PS::Await("aim", 240)) return false;
    // The bed anchor can stand inside the house, and a copy behind a wall is not rendered: the client closes in until it is seen.
    float cm = -1.f;
    bool  lastSeen = false;
    const bool seen = CloseUntilSeen([&] { return ByKey(keyB); }, &InViewReportingParts, kCloseInS, cm, lastSeen);
    UE_LOGI("mannequin-drill: this client aimed at walker B's copy, %.0f cm away: in view=%d", cm, seen ? 1 : 0);
    if (!seen) {
        std::string parts;
        OnGameThread([&] {
            if (void* c = ByKey(keyB)) parts = ViewPartsOf(c);
        });
        char why[1100];
        std::snprintf(why, sizeof(why), "walker B's copy never came into this client's view within %d s: %.0f cm away, last view read %d; %s",
                      kCloseInS, cm, lastSeen ? 1 : 0, parts.c_str());
        return Invalid(why);
    }
    PS::Mark("sees");
    if (!PS::Await("look_away", 120)) return false;
    const bool away = WaitGT("walker B's copy leaves this client's view", kViewBoundS, [&] {
        void* c = ByKey(keyB);
        if (!c) return true;
        Aim(c, false);
        return !InView(c);
    }, false);
    if (!away) return Invalid("walker B's copy stayed in this client's view when turned away");
    PS::Mark("away");
    return steps_b::ClientB(keyB);
}

// The client's first life ends in steps_b::ClientB, which leaves the session for the rig's rejoin (the rig kills this process); the
// rejoined life is steps_b::RunRejoin.
void RunClient() {
    const bool ran = ClientBody();
    UE_LOGI("mannequin-drill: DONE on the client's first life fails=%d ran=%d", FailCount(), ran ? 1 : 0);
}

void RunHost() {
    void* a = nullptr;
    void* b = nullptr;
    std::string keyA, keyB;
    int clientFails = 1;
    const bool ran = AwaitClient("client_ready", 480) && HostPhaseA(a, keyA) && HostPhaseB(a, b, keyB, clientFails);
    PS::Mark("host_done");
    UE_LOGI("mannequin-drill: DONE on the host fails=%d ran=%d", FailCount(), ran ? 1 : 0);
    if (!ran) return;  // the verdict, INVALID or FAIL, was said where the drill stopped
    if (FailCount() == 0 && clientFails == 0) coop::rig_verdict::Say(kTag, Outcome::Pass, "host=%d client=%d", FailCount(), clientFails);
    else coop::rig_verdict::Say(kTag, Outcome::Fail, "host=%d client=%d", FailCount(), clientFails);
}

}  // namespace

bool Armed() {
    static const bool armed = Is("walker") || Is("no_row") || Is("no_brain_gate") || Is("no_drive_claim") ||
                              Is("no_gaze_route") || Is("no_level") || Is("no_loss") || Is("no_delivery") ||
                              Is("no_join_state");
    return armed;
}

DWORD WINAPI Thread(LPVOID /*arg*/) {
    // The drill is the host's and slot 1's. A third peer (mp.py --observer, there so that slot 1's leave is not the session's
    // last and the host runs the slot's own leave) takes no part: it would join after the marks it needs and say INVALID, or
    // write a client's marks (measured: a third peer did write them). It knows its slot only once joined, so it waits for that before peer_signal.
    if (harness::autotest::IsClientRole()) {
        uint8_t slot = coop::players::kPeerIdUnknown;
        for (int i = 0; i < kSlotBoundS * 10; ++i) {
            slot = coop::players::Registry::Get().LocalPeerId();
            if (slot != coop::players::kPeerIdUnknown) break;
            ::Sleep(100);
        }
        if (slot != kClientSlot) {
            UE_LOGI("mannequin-drill: this client is slot %d, not the drill's slot %d -- it takes no part", static_cast<int>(slot),
                    static_cast<int>(kClientSlot));
            return 0;
        }
    }
    if (!PS::Begin(kTag)) return 0;
    if (!harness::autotest::IsClientRole()) RunHost();
    else if (steps_b::IsRejoinedLife()) steps_b::RunRejoin();
    else RunClient();
    return 0;
}

}  // namespace coop::dev::mannequin_walker_drill
