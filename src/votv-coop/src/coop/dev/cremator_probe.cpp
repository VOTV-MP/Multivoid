// coop/dev/cremator_probe.cpp -- see coop/dev/cremator_probe.h.

#include "coop/dev/cremator_probe.h"

#include "cremator_probe_detail.h"  // co-located private header (src tree, not include/)

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/interactables/cremator_occupancy.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/devices/cremator.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_nav.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::cremator_probe {
namespace detail {

Mode ModeOf() {
    static const Mode m = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::cremator_probe);
        return v == "1"          ? Mode::Local
             : v == "placeonly"  ? Mode::PlaceOnly
             : v == "host"       ? Mode::Host
                                 : Mode::Off;
    }();
    return m;
}

}  // namespace detail

namespace {

namespace E = ue_wrap::engine;
namespace V = ue_wrap::vitals;
namespace C = ue_wrap::cremator;
using namespace detail;

enum class Step : uint8_t { Start, Walk, StepIn, Lever, Run, WaitMove, PlacedOnly, Over };

constexpr float kPullbackAtS = 40.f;
constexpr float kSurvivedAtS = 90.f;
constexpr float kPlaceOnlyEndS = 120.f;
constexpr int kLinesAfterDeath = 10;
// The NavMesh route to the firebox ends 103 cm from its centre (measured): the firebox interior is off the
// NavMesh, so a walk to a smaller reach runs into its deadline. The last stretch is the step-in leg's.
constexpr float kFireboxReachCm = 120.f;
// A tick bound on an input leg that acts every tick; stepping over the lip takes far fewer.
constexpr int kStepInMaxTicks = 240;
// The walk's budget is the harness's meet walk's (harness/autotest/meet.cpp): twice a walk's pace over the
// longer of the route and the line, plus a margin for the director's stuck episodes and the doors on the way.
constexpr float kPaceCmPerSec = 300.f;
constexpr int kRouteMarginSec = 60;
// The place-only client's pawn counts as moved by the host's order past this distance from where it stood at join.
constexpr float kMovedCm = 300.f;
constexpr float kNoMoveFailS = 300.f;

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<int> g_stepped{0};  // the begin-overlap saw the local pawn enter the probe's cremator

Step g_step = Step::Start;
ue_wrap::CachedObjRef g_cremator;
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
ue_wrap::FVector g_fireboxAt{};
float g_routeCm = 0.f;
float g_lastToFireboxCm = -1.f;  // the route's last point to the firebox centre; -1 with no route
uint64_t g_startMs = 0;  // the lever's pull, or the placement in the place-only arm
uint64_t g_nextSampleMs = 0;
int g_prevAnim = -1;
bool g_pulledBack = false;
bool g_died = false;
int g_linesLeftAfterDeath = 0;
int g_stepTicks = 0;
ue_wrap::FVector g_joinedAt{};  // the place-only pawn's position when it was ready, the move's reference

// The cremator's begin-overlap on the client arms: the local pawn entering the probe's cremator.
void OnLocalOverlap(void* cremator, void* other) {
    if (cremator != g_cremator.Get()) return;
    if (other && other == coop::players::Registry::Get().Local()) g_stepped.store(1, std::memory_order_release);
}

void Fail(const char* why) {
    UE_LOGW("cremator-probe: FAIL %s", why);
    UE_LOGI("cremator-probe: DONE");
    g_step = Step::Over;
}

float Between(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The client finds the nearest firebox, opens the cremator's door, measures the route and starts the director's
// walk into it; WalkTick then waits for the walk's result.
void StartTick(void* local) {
    // The predicate behind `rig: READY joined`: after the join's own start-point placement, which
    // would otherwise move the player straight back out of the firebox.
    if (!coop::net_pump::HasAnnouncedWorldReady() ||
        coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle) return;
    ue_wrap::FVector pawnAt{};
    if (!E::TryGetActorLocation(local, pawnAt)) return;
    if (ModeOf() == Mode::PlaceOnly) {
        // The host's /cremate order moves this pawn; nothing here walks, opens or steps.
        g_joinedAt = pawnAt;
        g_startMs = coop::net::NowMs();
        UE_LOGI("cremator-probe: placeonly waiting for the host's move");
        g_step = Step::WaitMove;
        return;
    }
    if (!C::EnsureResolved()) { Fail("no cremator_C class"); return; }

    const C::Nearest n = C::FindNearestFirebox(pawnAt);
    if (!n.actor) { Fail("no cremator with a firebox is loaded"); return; }

    ue_wrap::FRotator rot{};
    if (!E::TryGetActorRotation(n.actor, rot)) { Fail("the cremator's rotation is unreadable"); return; }
    const std::wstring key = ue_wrap::prop::GetActorSaveKeyString(n.actor);
    UE_LOGI("cremator-probe: cremator key='%ls' firebox=(%.0f,%.0f,%.0f) yaw=%.1f pawn=(%.0f,%.0f,%.0f)",
            key.c_str(), n.at.X, n.at.Y, n.at.Z, rot.Yaw, pawnAt.X, pawnAt.Y, pawnAt.Z);

    // The door is shut at rest, and the firebox is behind it.
    bool opened = false;
    if (!C::OpenDoor(n.actor, opened)) { Fail("the door's open() did not run"); return; }
    UE_LOGI("cremator-probe: DOOR opened=%d", opened ? 1 : 0);

    // The walk is bounded by the NavMesh route, not the line (meet.cpp's reasoning).
    std::vector<ue_wrap::FVector> pts;
    float route = 0.f;
    if (E::FindNavPath(local, pawnAt, n.at, pts))
        for (size_t i = 1; i < pts.size(); ++i) route += Between(pts[i - 1], pts[i]);
    const float line = Between(pawnAt, n.at);
    g_lastToFireboxCm = pts.empty() ? -1.f : Between(pts.back(), n.at);
    g_routeCm = route;
    g_fireboxAt = n.at;
    UE_LOGI("cremator-probe: ROUTE line=%.0f route=%.0f points=%d lastToFirebox=%.0f", line, route,
            static_cast<int>(pts.size()), g_lastToFireboxCm);

    const float way = route > line ? route : line;
    const int budget = static_cast<int>(way / kPaceCmPerSec * 2.f) + kRouteMarginSec;
    g_walk = coop::director::StartBackgroundWalk(n.at, kFireboxReachCm, budget);
    UE_LOGI("cremator-probe: walking into the firebox, %d s budget", budget);
    g_cremator.Set(n.actor);
    g_step = Step::Walk;
}

void WalkTick() {
    if (!g_walk) { Fail("the walk was not started"); return; }
    const int state = g_walk->state.load();
    if (state == 0) return;
    if (state != 1) {
        char why[160];
        std::snprintf(why, sizeof(why), "walk %s lastToFirebox=%.0f", g_walk->goal.failReason, g_lastToFireboxCm);
        coop::rig_verdict::Say("cremator-probe", coop::rig_verdict::Outcome::Invalid, "the walk failed: %s", why);
        Fail(why);
        return;
    }
    void* local = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    const float dist = local && E::TryGetActorLocation(local, at) ? Between(at, g_fireboxAt) : -1.f;
    UE_LOGI("cremator-probe: WALKED in route=%.0f distToFirebox=%.0f", g_routeCm, dist);
    g_walk.reset();
    g_stepTicks = 0;
    UE_LOGI("cremator-probe: STEPIN start dist=%.0f", dist);
    g_step = Step::StepIn;
}

// The walk ends on the NavMesh's edge, short of the firebox: the player steps over the lip with the input a human
// gives, until the cremator's own begin-overlap says it is in.
void StepInTick(void* local) {
    ue_wrap::FVector at{};
    if (!local || !E::TryGetActorLocation(local, at)) return;
    const float dist = Between(at, g_fireboxAt);
    if (g_stepped.load(std::memory_order_acquire)) {
        UE_LOGI("cremator-probe: STEPPED in dist=%.0f ticks=%d", dist, g_stepTicks);
        g_step = Step::Lever;
        return;
    }
    if (g_stepTicks >= kStepInMaxTicks) {
        char why[96];
        std::snprintf(why, sizeof(why), "stepin dist=%.0f ticks=%d", dist, kStepInMaxTicks);
        Fail(why);
        return;
    }
    ue_wrap::FVector dir{g_fireboxAt.X - at.X, g_fireboxAt.Y - at.Y, 0.f};
    const float h = std::sqrt(dir.X * dir.X + dir.Y * dir.Y);
    if (h > 1.f) { dir.X /= h; dir.Y /= h; }
    E::AddMovementInput(local, dir, 1.0f, true);
    ++g_stepTicks;
}

void LogSample(void* local, float t, const char* edge) {
    void* cremator = g_cremator.Get();
    float hp = -1.f, burn = -1.f, windup = -1.f;
    V::Read(V::Field::Health, &hp);
    V::Read(V::Field::BurningTime, &burn);
    C::ReadWindup(cremator, windup);
    const C::Machine m = C::ReadMachine(cremator);
    bool ragdoll = false, dead = false;
    const int deadBit = local && E::ReadMainPlayerRagdollState(local, ragdoll, dead) ? (dead ? 1 : 0) : -1;
    ue_wrap::FVector pos{};
    if (!local || !E::TryGetActorLocation(local, pos)) pos = {};
    UE_LOGI("cremator-probe: T=%.0f hp=%.1f burn=%.1f active=%d anim=%d windup=%.3f closed=%d doorOpened=%d "
            "actors=%d health0=%.1f dead=%d pos=(%.0f,%.0f,%.0f)%s%s",
            t, hp, burn, Flag(cremator, C::ReadActive), Flag(cremator, C::ReadAnim), windup, Flag(cremator, C::ReadClosed),
            Flag(cremator, C::ReadDoorOpened), m.actors, m.health0, deadBit, pos.X, pos.Y, pos.Z, edge ? " edge=" : "",
            edge ? edge : "");
    if (deadBit == 1 && !g_died) {
        g_died = true;
        g_linesLeftAfterDeath = kLinesAfterDeath;
        UE_LOGI("cremator-probe: DIED T=%.0f", t);
    }
}

void LeverTick(void* local) {
    void* cremator = g_cremator.Get();
    if (!cremator) { Fail("the cremator died before the lever"); return; }
    bool opened = false;
    if (!C::ShutDoor(cremator, opened)) { Fail("the open door's close() did not run"); return; }
    if (!C::CallUseLever(cremator)) { Fail("useLever did not run"); return; }
    g_startMs = coop::net::NowMs();
    g_nextSampleMs = g_startMs + kSampleMs;
    UE_LOGI("cremator-probe: LEVER doorWasOpened=%d", opened ? 1 : 0);
    g_prevAnim = Flag(cremator, C::ReadAnim);
    LogSample(local, 0.f, "lever");
    g_step = Step::Run;
}

void RunTick(void* local) {
    const uint64_t now = coop::net::NowMs();
    const float t = static_cast<float>(now - g_startMs) / 1000.f;

    // The handle's finish is the edge of `anim` going false; a 1 s sample can straddle it, so it is read every tick.
    const int anim = Flag(g_cremator.Get(), C::ReadAnim);
    if (g_prevAnim == 1 && anim == 0) LogSample(local, t, "handle-finish");
    g_prevAnim = anim;

    if (now < g_nextSampleMs) return;
    g_nextSampleMs = now + kSampleMs;

    // The pull-back goes before the sample so the line of the second it ran in already shows its effect's start.
    if (t >= kPullbackAtS && !g_pulledBack && !g_died) {
        g_pulledBack = true;
        UE_LOGI("cremator-probe: PULLBACK ran=%d", C::CallUseLever(g_cremator.Get()) ? 1 : 0);
    }
    const bool wasDead = g_died;
    LogSample(local, t, nullptr);
    if (wasDead) {
        if (--g_linesLeftAfterDeath <= 0) {
            UE_LOGI("cremator-probe: DONE");
            g_step = Step::Over;
        }
        return;
    }
    if (!g_died && t >= kSurvivedAtS) {
        UE_LOGI("cremator-probe: SURVIVED");
        UE_LOGI("cremator-probe: DONE");
        g_step = Step::Over;
    }
}

// The place-only arm, before the move: the client touches nothing and waits for the host's teleport to apply.
void WaitMoveTick(void* local) {
    const uint64_t now = coop::net::NowMs();
    ue_wrap::FVector at{};
    if (local && E::TryGetActorLocation(local, at) && Between(at, g_joinedAt) > kMovedCm) {
        UE_LOGI("cremator-probe: placeonly MOVED to=(%.0f,%.0f,%.0f)", at.X, at.Y, at.Z);
        g_startMs = now;
        g_nextSampleMs = now + kSampleMs;
        g_step = Step::PlacedOnly;
        return;
    }
    if (static_cast<float>(now - g_startMs) / 1000.f >= kNoMoveFailS) Fail("placeonly no move");
}

// The place-only arm after the move: the client says what happens to it, touching nothing.
void PlacedOnlyTick(void* local) {
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextSampleMs) return;
    g_nextSampleMs = now + kSampleMs;
    const float t = static_cast<float>(now - g_startMs) / 1000.f;
    float hp = -1.f, burn = -1.f;
    V::Read(V::Field::Health, &hp);
    V::Read(V::Field::BurningTime, &burn);
    bool ragdoll = false, dead = false;
    const bool haveDead = local && E::ReadMainPlayerRagdollState(local, ragdoll, dead);
    ue_wrap::FVector pos{};
    if (!local || !E::TryGetActorLocation(local, pos)) pos = {};
    UE_LOGI("cremator-probe: placeonly T=%.0f hp=%.1f burn=%.1f dead=%d pos=(%.0f,%.0f,%.0f)", t, hp, burn,
            haveDead ? (dead ? 1 : 0) : -1, pos.X, pos.Y, pos.Z);
    if (haveDead && dead) {
        UE_LOGI("cremator-probe: DIED T=%.0f", t);
    } else if (t < kPlaceOnlyEndS) {
        return;
    } else {
        UE_LOGI("cremator-probe: SURVIVED");
    }
    UE_LOGI("cremator-probe: DONE");
    g_step = Step::Over;
}

void Reset() {
    if (g_walk && g_walk->state.load() == 0) coop::director::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
    g_step = Step::Start;
    g_stepped.store(0, std::memory_order_release);
    g_stepTicks = 0;
    g_joinedAt = {};
    g_cremator.Reset();
    g_fireboxAt = {};
    g_routeCm = 0.f;
    g_lastToFireboxCm = -1.f;
    g_startMs = 0;
    g_nextSampleMs = 0;
    g_prevAnim = -1;
    g_pulledBack = false;
    g_died = false;
    g_linesLeftAfterDeath = 0;
    HostReset();
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    const Mode mode = ModeOf();
    // The client arms that enter the cremator by their own walk and the host arm (which judges the landing) watch.
    if (mode != Mode::Local && mode != Mode::Host) return;
    // Install is the dev lanes' retry pump, called every tick: a refused registration (the gate not armed yet) is
    // retried, its warning said once; the first accepted one is latched and said, and ends the retries.
    static bool sWatching = false;
    static bool sWarned = false;
    if (sWatching) return;
    if (coop::cremator_occupancy::AddListener(mode == Mode::Host ? &HostOnOverlap : &OnLocalOverlap)) {
        sWatching = true;
        UE_LOGI("cremator-probe: watching the cremator's begin-overlap");
        return;
    }
    if (sWarned) return;
    sWarned = true;
    UE_LOGW("cremator-probe: could not watch the cremator's begin-overlap yet (retried every tick)");
}

void Tick() {
    const Mode mode = ModeOf();
    if (mode == Mode::Off) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (mode == Mode::Host) {
        if (s->role() == coop::net::Role::Host) HostTick(*s);
        return;
    }
    if (s->role() != coop::net::Role::Client || g_step == Step::Over) return;
    // After the death the pawn may be swapped or absent: the sample then prints its sentinels and the line
    // count still runs out.
    void* local = coop::players::Registry::Get().Local();
    if (g_step == Step::Run) { RunTick(local); return; }
    if (g_step == Step::PlacedOnly) { PlacedOnlyTick(local); return; }
    if (g_step == Step::WaitMove) { WaitMoveTick(local); return; }
    if (g_step == Step::Walk) { WalkTick(); return; }
    if (g_step == Step::StepIn) { StepInTick(local); return; }
    if (!local) return;
    if (g_step == Step::Start) StartTick(local);
    else LeverTick(local);
}

void OnDisconnect() {
    if (g_step == Step::Walk || g_step == Step::StepIn || g_step == Step::Lever || g_step == Step::Run || g_step == Step::WaitMove ||
        g_step == Step::PlacedOnly)
        UE_LOGW("cremator-probe: FAIL the session ended mid-run");
    g_session.store(nullptr, std::memory_order_release);
    Reset();
}

}  // namespace coop::dev::cremator_probe
