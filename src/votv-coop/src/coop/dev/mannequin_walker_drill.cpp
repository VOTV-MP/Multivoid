// coop/dev/mannequin_walker_drill.cpp -- see coop/dev/mannequin_walker_drill.h.
//
// WHAT IT MEASURES. (1) A client's mannequin spawner is refused at the gate: the refusal counter of spawn_authority's row
// rises when the client runs the spawner's own body. (2) One walker, the host's: the client holds exactly one copy for its key,
// and while the host's walker moves the copy follows it (the drive stream), within kFollowTolCm. (3) The target's gaze: the
// client is the walker's target, its camera turned to the copy stops the host's walker, and when it looks away the walker moves
// again within the Blueprint's own timer_saw bound plus a margin.
// THE TWO WALKERS. A walker KEEPS its target until it is lost (coop/creatures/walker_target.h), so one cannot hand a hunt from
// the host to the client by walking the client nearer. Walker A is hunted by the host while the client stands in its range but
// farther (the host enters the range first: the first player in range is taken, and a later, nearer one is not); its copy, were
// its brain not gated, would go for the client's own player, the other direction. Walker B is born while the host stands at the
// KPP gate, out of every spawn point's range, and the client is the only player in range at its first read: the client is its
// target. Between them A is destroyed, so one walker stands at a time.
// THE PEERS. Both reach the same state through peer_signal (VOTVCOOP_CAN_DIR); a wait is on a state the other peer reached
// (a mark, a read on the game thread), bounded, and a bound that runs out before a verdict is INVALID. The host judges.

#include "coop/dev/mannequin_walker_drill.h"

#include "harness/autotest.h"
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/walker_target.h"
#include "coop/dev/director/director.h"
#include "coop/dev/mannequin_drill.h"
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
namespace WT  = coop::walker_target;
using coop::rig_verdict::Outcome;
using ue_wrap::FRotator;
using ue_wrap::FVector;

constexpr const char* kTag = "mannequin";
constexpr const char* kSpawnerTag = "mannequinSpawner.ReceiveTick";
constexpr uint8_t     kClientSlot = 1;

// The walker's own hunt range (walker_target's kRangeCm, the Blueprint's test at its call end 41), and the margin that makes
// "out of range" mean it: the host at its start stands far beyond it of every spawn point, so this only guards a changed start.
constexpr float kRangeCm = 20000.f;
constexpr float kOutOfRangeMarginCm = 2000.f;
// Where each player stands, in the range the Blueprint hunts through: the host close, the client farther, then the client close
// for its gaze (far enough that the walker cannot reach it before the gaze steps are read).
constexpr float kHostStandoffCm = 12000.f;
constexpr float kClientStandoffCm = 16000.f;
constexpr float kCloseCm = 6000.f;
constexpr float kKppReachCm = 1500.f;

// Bounds, each a readiness wait that ends sooner on its state. timer_saw is the Blueprint's own 10-20 s release.
constexpr int kTimerSawS = 20;
constexpr int kMarginS = 15;
constexpr int kWalkDeadlineS = 400;
constexpr int kWalkWaitS = kWalkDeadlineS + 90;
constexpr int kSpawnBoundS = 30;
constexpr int kTargetBoundS = 30;
constexpr int kMoveBoundS = kTimerSawS + 25;      // from its birth or from a release to its first steps
constexpr int kReleaseBoundS = kTimerSawS + kMarginS;
constexpr int kFreezeBoundS = 15;
constexpr int kViewBoundS = 20;

// The sampling: one reading per kSampleMs. Moving is the walker's displacement over kMovingSamples readings, still the same
// over kStillSamples; the window is how long both peers sample for step 2.
constexpr int    kSampleMs = 100;
constexpr size_t kMovingSamples = 10;
constexpr float  kMovingCm = 60.f;
constexpr size_t kStillSamples = 20;
constexpr float  kStillCm = 30.f;
constexpr double kWindowS = 8.0;
constexpr float  kMinMoveCm = 500.f;
// The copy's distance to where the host's walker stood a short lag before, the nearest of kLagSteps lags of kLagStepS: the
// stream's own delay is no error, a copy that stands or walks elsewhere is. Measured on the first rig run.
constexpr float  kFollowTolCm = 150.f;
constexpr double kLagStepS = 0.05;
constexpr int    kLagSteps = 10;
constexpr float  kAimUpCm = 90.f;   // the mesh's middle, over the actor's origin

int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

// A readiness wait on a state the game thread reads. A judged wait (`abort` false) ends in the caller's check.
template <class Pred>
bool WaitGT(const char* what, int seconds, Pred pred, bool abort = true) {
    return PS::WaitFor(what, seconds, [&] {
        bool ok = false;
        OnGameThread([&] { ok = pred(); });
        return ok;
    }, abort);
}

void Check(bool ok, const char* step, const char* what) {
    UE_LOGI("mannequin-drill: step %s %s %s", step, ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

bool Invalid(const char* why) {
    UE_LOGW("mannequin-drill: INVALID -- %s", why);
    coop::rig_verdict::Say(kTag, Outcome::Invalid, "%s", why);
    return false;
}

float Dist3(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double NowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A key is a GUID's text, so each wide character is one ASCII one.
std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (const wchar_t c : w) s.push_back(static_cast<char>(c));
    return s;
}

// ---- the walkers, game thread ----

std::vector<void*> Walkers() {
    std::vector<void*> v;
    AC::ListWalkers(v);
    return v;
}

std::string KeyOf(void* walker) { return Narrow(ue_wrap::prop::GetInteractableKeyString(walker)); }

// The walker of this key among the live ones, and how many carry it.
void* ByKey(const std::string& key, int* count = nullptr) {
    void* found = nullptr;
    int n = 0;
    for (void* w : Walkers())
        if (KeyOf(w) == key) {
            found = w;
            ++n;
        }
    if (count) *count = n;
    return found;
}

// The game's ticker_foodSleeper_C drops a player under 20 food into a ragdoll at random; a ragdoll turns no view and walks nowhere.
void KeepFed() {
    V::Write(V::Field::Food, 100.f);
    V::Write(V::Field::Sleep, 100.f);
}

// This player's view on `walker`: toward it, or turned the other way, so that it is not in this view (the camera behind it).
void Aim(void* walker, bool toward) {
    KeepFed();
    void* pawn = coop::players::Registry::Get().Local();
    FVector at{};
    if (!pawn || !walker || !R::IsLive(walker) || !E::TryGetActorLocation(walker, at)) return;
    FRotator rot = DIR::LookAt(E::GetCameraLocation(), FVector{at.X, at.Y, at.Z + kAimUpCm});
    if (!toward) {
        rot.Yaw += 180.f;
        rot.Pitch = 0.f;
    }
    while (rot.Yaw > 180.f) rot.Yaw -= 360.f;
    while (rot.Yaw < -180.f) rot.Yaw += 360.f;
    E::SetControlRotation(E::GetController(pawn), rot);
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

std::string Pack(const std::vector<Sample>& v) {
    std::string out;
    char b[96];
    for (const Sample& s : v) {
        std::snprintf(b, sizeof(b), "%.6f %.1f %.1f %.1f ", s.t, s.x, s.y, s.z);
        out += b;
    }
    return out;
}

std::vector<Sample> Unpack(const std::string& text) {
    std::vector<Sample> out;
    std::istringstream in(text);
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

// ---- the director's walk ----

// The bot director walks this player, staged at a known-good anchor when the route is long (the director's stage); true when it
// arrived within `reachCm`.
bool WalkTo(const FVector& to, float reachCm, const char* what) {
    std::shared_ptr<DIR::BackgroundWalk> walk;
    OnGameThread([&] { walk = DIR::StartBackgroundWalk(to, reachCm, kWalkDeadlineS); });
    const bool done = PS::WaitFor(what, kWalkWaitS, [&] { return walk->state.load() != 0; }, false);
    if (!done) DIR::CancelBackgroundWalk(*walk);
    UE_LOGI("mannequin-drill: walk '%s' %s", what, done && walk->state.load() == 1 ? "arrived" : "did not arrive");
    return done && walk->state.load() == 1;
}

float PlayerTo(void* actor) {
    void* me = coop::players::Registry::Get().Local();
    FVector a{}, b{};
    return me && actor && E::TryGetActorLocation(me, a) && E::TryGetActorLocation(actor, b) ? Dist3(a, b) : -1.f;
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

bool HostPhaseA(void*& a, std::string& keyA) {
    float hostCm = 0.f;
    if (!SpawnOne(a, keyA, hostCm)) return false;
    if (hostCm <= kRangeCm + kOutOfRangeMarginCm)
        return Invalid("the host stands within the walker's range at its birth: the host must enter it first, from outside");
    PS::Mark("walker_a", keyA);
    if (!PS::Await("copy_a", 60)) return false;

    // The host enters the range first: it is the first player in it, so the host is the hunted one, and the client enters
    // after and stays farther.
    FVector at{};
    OnGameThread([&] { E::TryGetActorLocation(a, at); });
    if (!WalkTo(at, kHostStandoffCm, "the host toward walker A")) return Invalid("the host's walk toward the walker did not arrive");
    float within = 0.f;
    OnGameThread([&] { Aim(a, false); within = PlayerTo(a); });
    if (within < 0.f || within > kRangeCm) return Invalid("the host is not within the walker's range after its walk");
    PS::Mark("host_in");
    if (!PS::Await("client_in", kWalkWaitS)) return false;
    if (!WaitGT("walker A hunts the host", kTargetBoundS, [&] { return Hunts(a, 0); }, false))
        return Invalid("walker A does not hunt the host");

    // It walks once its own timer_saw has let go (the host's view is turned away, and nobody else's counts).
    Track trk;
    const bool moving = WaitGT("walker A walks with the host's view turned away", kMoveBoundS, [&] {
        FVector p{};
        Aim(a, false);
        if (!E::TryGetActorLocation(a, p)) return false;
        trk.Push(p);
        return trk.Moving();
    }, false);
    if (!moving) return Invalid("walker A never walked with the host's view turned away");
    PS::Mark("window_go");
    const std::vector<Sample> host = Window([&] { return a; }, kWindowS);
    std::string text;
    if (!PS::AwaitText("copy_samples_a", text, 60)) return false;
    const std::vector<Sample> copy = Unpack(text);
    const float moved = Excursion(host);
    UE_LOGI("mannequin-drill: step 2 window: host walker moved %.0f cm over %zu readings, the copy gave %zu", moved, host.size(), copy.size());
    if (moved < kMinMoveCm || copy.empty()) return Invalid("step 2 had no mover to compare: the host walker did not walk in the window");
    const Follow f = Compare(host, copy);
    UE_LOGI("mannequin-drill: step 2 follow: %zu copy readings, worst %.0f cm, mean %.0f cm (tolerance %.0f)", f.n, f.maxCm,
            f.meanCm, kFollowTolCm);
    char what[128];
    std::snprintf(what, sizeof(what), "the client's copy follows the host's walker (worst %.0f cm, %zu readings)", f.maxCm, f.n);
    Check(f.n > 0 && f.maxCm <= kFollowTolCm, "2", what);
    return true;
}

// Step 3, on walker B: the client is its target; the client's gaze stops it, and its look away lets it go again.
bool HostPhaseB(void* a, void*& b, std::string& keyB) {
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
    if (!PS::Await("copy_b", 60)) return false;
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
    if (!moving) return Invalid("walker B never walked with the client's view turned away");
    PS::Mark("aim");
    if (!PS::Await("sees", 60)) return false;

    // The client's camera is on the copy: the host's walker must stand still, with its own `looking` set.
    Track standing;
    const bool stopped = WaitGT("walker B stops while its target looks at it", kFreezeBoundS, [&] {
        FVector p{};
        Aim(b, false);
        if (!E::TryGetActorLocation(b, p)) return false;
        standing.Push(p);
        return standing.Still();
    }, false);
    bool looking = false;
    OnGameThread([&] { looking = AC::ReadWalkerLooking(b); });
    UE_LOGI("mannequin-drill: step 3 freeze: stopped=%d looking=%d", stopped ? 1 : 0, looking ? 1 : 0);
    Check(stopped, "3", "the host's walker stops while its target's camera is on it");
    if (stopped && !looking) return Invalid("walker B stood still without its looking flag set: it stopped for another reason");

    // The client looks away: the walker moves again when its own timer_saw clears `saw`, within the timer's bound and a margin.
    PS::Mark("look_away");
    if (!PS::Await("away", 60)) return false;
    Track released;
    const bool goes = WaitGT("walker B walks again after its target looks away", kReleaseBoundS, [&] {
        FVector p{};
        Aim(b, false);
        if (!E::TryGetActorLocation(b, p)) return false;
        released.Push(p);
        return released.Moving();
    }, false);
    char what[160];
    std::snprintf(what, sizeof(what), "the host's walker moves again within %d s of its target looking away", kReleaseBoundS);
    Check(goes, "3", what);
    return true;
}

// ---- the client ----

bool WorldReady() {
    return WaitGT("this client's world", 240, [] {
        return coop::players::Registry::Get().Local() && coop::net_pump::HasAnnouncedWorldReady();
    });
}

bool HostPuppetUp() {
    return WorldReady() && WaitGT("the host's puppet", 60, [] {
        coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
        return p && p->valid();
    });
}

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

bool CheckRefusal() {
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
    Check(after > before, "1", what);
    return true;
}

// The host's walker `key` has its copy here, once (step 2); a copy that never came is the lane's failure, said at once.
bool AwaitCopy(const std::string& key, const char* label) {
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
    Check(n == 1, "2", what);
    return true;
}

bool ClientBody() {
    if (!HostPuppetUp()) return Invalid("this client's world or the host's puppet never came up");
    if (!harness::autotest::meet::WalkToHost(kTag)) return Invalid("this client could not reach the host");
    if (!CheckRefusal()) return false;
    PS::Mark("client_ready");

    // Walker A: the host hunts it, and this client comes into its range after the host, farther than the host.
    std::string keyA;
    if (!PS::AwaitText("walker_a", keyA, 240) || !AwaitCopy(keyA, "A")) return false;
    PS::Mark("copy_a");
    if (!PS::Await("host_in", kWalkWaitS)) return false;
    FVector at{};
    void* copy = nullptr;
    OnGameThread([&] {
        copy = ByKey(keyA);
        if (copy) E::TryGetActorLocation(copy, at);
    });
    if (!copy) return Invalid("walker A's copy is gone before this client's walk");
    if (!WalkTo(at, kClientStandoffCm, "this client toward walker A")) return Invalid("this client's walk toward walker A did not arrive");
    OnGameThread([&] { Aim(ByKey(keyA), false); });
    PS::Mark("client_in");
    if (!PS::Await("window_go", 240)) return false;
    const std::vector<Sample> samples = Window([&] { return ByKey(keyA); }, kWindowS);
    PS::Mark("copy_samples_a", samples.empty() ? std::string("none") : Pack(samples));

    // Walker B: this client is its target. It walks close, turned away; then its camera goes on the copy, and off it.
    std::string keyB;
    if (!PS::AwaitText("walker_b", keyB, 2 * kWalkWaitS) || !AwaitCopy(keyB, "B")) return false;
    PS::Mark("copy_b");
    copy = nullptr;
    OnGameThread([&] {
        copy = ByKey(keyB);
        if (copy) E::TryGetActorLocation(copy, at);
    });
    if (!copy) return Invalid("walker B's copy is gone before this client's walk");
    if (!WalkTo(at, kCloseCm, "this client toward walker B")) return Invalid("this client's walk toward walker B did not arrive");
    OnGameThread([&] { Aim(ByKey(keyB), false); });
    PS::Mark("client_close");
    if (!PS::Await("aim", 240)) return false;
    float cm = -1.f;
    const bool seen = WaitGT("walker B's copy is in this client's view", kViewBoundS, [&] {
        void* c = ByKey(keyB);
        if (!c) return false;
        Aim(c, true);
        cm = PlayerTo(c);
        return InView(c);
    }, false);
    UE_LOGI("mannequin-drill: this client aimed at walker B's copy, %.0f cm away: in view=%d", cm, seen ? 1 : 0);
    if (!seen) return Invalid("walker B's copy never came into this client's view when aimed at");
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
    return true;
}

// A peer's fails count as it left it in its done signal; text that is no count is a fail.
int ParseFails(const std::string& text) {
    char* end = nullptr;
    const long n = std::strtol(text.c_str(), &end, 10);
    return end != text.c_str() && n >= 0 ? static_cast<int>(n) : 1;
}

void RunClient() {
    const bool ran = ClientBody();
    PS::Mark("client_done", std::to_string(ran ? g_fails : g_fails + 1));  // a client that stopped short is no pass
    UE_LOGI("mannequin-drill: DONE on the client fails=%d", g_fails);
}

void RunHost() {
    void* a = nullptr;
    void* b = nullptr;
    std::string keyA, keyB;
    const bool ran = PS::Await("client_ready", 480) && HostPhaseA(a, keyA) && HostPhaseB(a, b, keyB);
    PS::Mark("host_done");
    UE_LOGI("mannequin-drill: DONE on the host fails=%d ran=%d", g_fails, ran ? 1 : 0);
    if (!ran) return;  // the verdict, INVALID or FAIL, was said where the drill stopped
    std::string clientText;
    const int clientFails = PS::AwaitText("client_done", clientText, 120) ? ParseFails(clientText) : 1;
    if (g_fails == 0 && clientFails == 0) coop::rig_verdict::Say(kTag, Outcome::Pass, "host=%d client=%d", g_fails, clientFails);
    else coop::rig_verdict::Say(kTag, Outcome::Fail, "host=%d client=%d", g_fails, clientFails);
}

}  // namespace

bool Armed() {
    static const bool armed = Is("walker") || Is("no_row") || Is("no_brain_gate") || Is("no_drive_claim") ||
                              Is("no_gaze_route") || Is("no_level");
    return armed;
}

DWORD WINAPI Thread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    if (harness::autotest::IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace coop::dev::mannequin_walker_drill
