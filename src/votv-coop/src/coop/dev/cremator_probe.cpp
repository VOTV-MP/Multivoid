// coop/dev/cremator_probe.cpp -- see coop/dev/cremator_probe.h.

#include "coop/dev/cremator_probe.h"

#include "cremator_probe_detail.h"  // co-located private header (src tree, not include/)

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
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

namespace E = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace R = ue_wrap::reflection;

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

void* ObjectProperty(void* object, const wchar_t* name) {
    if (!object || !R::IsLive(object)) return nullptr;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(object), name);
    if (off < 0) return nullptr;
    void* value = nullptr;
    std::memcpy(&value, static_cast<const char*>(object) + off, sizeof(value));
    return value && R::IsLive(value) ? value : nullptr;
}

bool ReadBool(void* object, const wchar_t* name, bool& value) {
    if (!object || !R::IsLive(object)) return false;
    int32_t offset = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(object), name, offset, mask) || offset < 0 || !mask) return false;
    value = (*reinterpret_cast<const uint8_t*>(static_cast<const char*>(object) + offset) & mask) != 0;
    return true;
}

bool ReadFloat(void* object, const wchar_t* name, float& value) {
    if (!object || !R::IsLive(object)) return false;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(object), name);
    if (off < 0) return false;
    std::memcpy(&value, static_cast<const char*>(object) + off, sizeof(value));
    return true;
}

int Bit(void* object, const wchar_t* name) {
    bool v = false;
    return ReadBool(object, name, v) ? (v ? 1 : 0) : -1;
}

bool CallNoParams(void* object, const wchar_t* fnName) {
    if (!object || !R::IsLive(object)) return false;
    void* fn = R::FindFunction(R::ClassOf(object), fnName);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return ue_wrap::Call(object, f);
}

void ReadMachine(void* cremator, int& actors, float& health0) {
    actors = -1;
    health0 = kNoHealth;
    if (!cremator || !R::IsLive(cremator)) return;
    void* cls = R::ClassOf(cremator);
    // A TArray is { void* Data; int32 Num; int32 Max }.
    const int32_t actorsOff = R::FindPropertyOffset(cls, L"actors");
    if (actorsOff >= 0) std::memcpy(&actors, static_cast<const char*>(cremator) + actorsOff + sizeof(void*), sizeof(int32_t));
    const int32_t healthOff = R::FindPropertyOffset(cls, L"health");
    if (healthOff < 0) return;
    void* data = nullptr;
    int32_t num = 0;
    std::memcpy(&data, static_cast<const char*>(cremator) + healthOff, sizeof(data));
    std::memcpy(&num, static_cast<const char*>(cremator) + healthOff + sizeof(void*), sizeof(num));
    if (num > 0 && data) std::memcpy(&health0, data, sizeof(health0));
}

Nearest FindNearestFirebox(const ue_wrap::FVector& from) {
    Nearest n{from, INFINITY, nullptr, {}};
    void* cls = OI::ClassByName(L"cremator_C");
    if (!cls) return n;
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        auto& s = *static_cast<Nearest*>(ctx);
        void* firebox = ObjectProperty(obj, L"firebox");
        ue_wrap::FVector at{};
        if (!firebox || !E::TryGetComponentLocation(firebox, at)) return;
        const float dx = at.X - s.from.X, dy = at.Y - s.from.Y, dz = at.Z - s.from.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < s.best) { s.best = d2; s.actor = obj; s.at = at; }
    }, &n);
    return n;
}

bool ShutDoor(void* cremator, bool& opened) {
    opened = false;
    void* door = ObjectProperty(cremator, L"swingerDoor");
    return !(door && ReadBool(door, L"opened", opened) && opened) || CallNoParams(door, L"close");
}

bool OpenDoor(void* cremator, bool& opened) {
    opened = false;
    void* door = ObjectProperty(cremator, L"swingerDoor");
    if (!door) return false;
    // open(damage): the frame is zeroed, so `damage` is false, the door's own player path.
    const bool ran = (ReadBool(door, L"opened", opened) && opened) || CallNoParams(door, L"open");
    ReadBool(door, L"opened", opened);
    return ran;
}

}  // namespace detail

namespace {

namespace E = ue_wrap::engine;
namespace V = ue_wrap::vitals;
using namespace detail;

enum class Step : uint8_t { Start, Walk, Lever, Run, PlacedOnly, Over };

constexpr float kPullbackAtS = 40.f;
constexpr float kSurvivedAtS = 90.f;
constexpr float kPlaceOnlyEndS = 120.f;
constexpr int kLinesAfterDeath = 10;
constexpr float kFireboxReachCm = 50.f;
// The walk's budget is the harness's meet walk's (harness/autotest/meet.cpp): twice a walk's pace over the
// longer of the route and the line, plus a margin for the director's stuck episodes and the doors on the way.
constexpr float kPaceCmPerSec = 300.f;
constexpr int kRouteMarginSec = 60;

std::atomic<coop::net::Session*> g_session{nullptr};

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
    if (!OI::ClassByName(L"cremator_C")) { Fail("no cremator_C class"); return; }

    const Nearest n = FindNearestFirebox(pawnAt);
    if (!n.actor) { Fail("no cremator with a firebox is loaded"); return; }

    ue_wrap::FRotator rot{};
    if (!E::TryGetActorRotation(n.actor, rot)) { Fail("the cremator's rotation is unreadable"); return; }
    const std::wstring key = ue_wrap::prop::GetActorSaveKeyString(n.actor);
    UE_LOGI("cremator-probe: cremator key='%ls' firebox=(%.0f,%.0f,%.0f) yaw=%.1f pawn=(%.0f,%.0f,%.0f)",
            key.c_str(), n.at.X, n.at.Y, n.at.Z, rot.Yaw, pawnAt.X, pawnAt.Y, pawnAt.Z);

    // The door is shut at rest, and the firebox is behind it.
    bool opened = false;
    if (!OpenDoor(n.actor, opened)) { Fail("the door's open() did not run"); return; }
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
        Fail(why);
        return;
    }
    void* local = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    const float dist = local && E::TryGetActorLocation(local, at) ? Between(at, g_fireboxAt) : -1.f;
    UE_LOGI("cremator-probe: WALKED in route=%.0f distToFirebox=%.0f", g_routeCm, dist);
    g_walk.reset();
    if (ModeOf() == Mode::PlaceOnly) {
        bool opened = false;
        if (!ShutDoor(g_cremator.Get(), opened)) { Fail("the open door's close() did not run"); return; }
        g_startMs = coop::net::NowMs();
        g_nextSampleMs = g_startMs + kSampleMs;
        g_step = Step::PlacedOnly;
        return;
    }
    g_step = Step::Lever;
}

void LogSample(void* local, float t, const char* edge) {
    void* cremator = g_cremator.Get();
    void* door = ObjectProperty(cremator, L"swingerDoor");
    float hp = -1.f, burn = -1.f, windup = -1.f, health0 = kNoHealth;
    int actors = -1;
    V::Read(V::Field::Health, &hp);
    V::Read(V::Field::BurningTime, &burn);
    ReadFloat(cremator, L"windup", windup);
    ReadMachine(cremator, actors, health0);
    bool ragdoll = false, dead = false;
    const int deadBit = local && E::ReadMainPlayerRagdollState(local, ragdoll, dead) ? (dead ? 1 : 0) : -1;
    ue_wrap::FVector pos{};
    if (!local || !E::TryGetActorLocation(local, pos)) pos = {};
    UE_LOGI("cremator-probe: T=%.0f hp=%.1f burn=%.1f active=%d anim=%d windup=%.3f closed=%d doorOpened=%d "
            "actors=%d health0=%.1f dead=%d pos=(%.0f,%.0f,%.0f)%s%s",
            t, hp, burn, Bit(cremator, L"isActive"), Bit(cremator, L"anim"), windup, Bit(cremator, L"isClosed"),
            Bit(door, L"opened"), actors, health0, deadBit, pos.X, pos.Y, pos.Z, edge ? " edge=" : "",
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
    if (!ShutDoor(cremator, opened)) { Fail("the open door's close() did not run"); return; }
    if (!CallNoParams(cremator, L"useLever")) { Fail("useLever did not run"); return; }
    g_startMs = coop::net::NowMs();
    g_nextSampleMs = g_startMs + kSampleMs;
    UE_LOGI("cremator-probe: LEVER doorWasOpened=%d", opened ? 1 : 0);
    g_prevAnim = Bit(cremator, L"anim");
    LogSample(local, 0.f, "lever");
    g_step = Step::Run;
}

void RunTick(void* local) {
    const uint64_t now = coop::net::NowMs();
    const float t = static_cast<float>(now - g_startMs) / 1000.f;

    // The handle's finish is the edge of `anim` going false; a 1 s sample can straddle it, so it is read every tick.
    const int anim = Bit(g_cremator.Get(), L"anim");
    if (g_prevAnim == 1 && anim == 0) LogSample(local, t, "handle-finish");
    g_prevAnim = anim;

    if (now < g_nextSampleMs) return;
    g_nextSampleMs = now + kSampleMs;

    // The pull-back goes before the sample so the line of the second it ran in already shows its effect's start.
    if (t >= kPullbackAtS && !g_pulledBack && !g_died) {
        g_pulledBack = true;
        UE_LOGI("cremator-probe: PULLBACK ran=%d", CallNoParams(g_cremator.Get(), L"useLever") ? 1 : 0);
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

// The place-only arm: the client stands in the firebox and says what happens to it, touching nothing.
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
    UE_LOGI("cremator-probe: placeonly T=%.0f hp=%.1f burn=%.1f dead=%d", t, hp, burn,
            haveDead ? (dead ? 1 : 0) : -1);
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
    if (g_step == Step::Walk) { WalkTick(); return; }
    if (!local) return;
    if (g_step == Step::Start) StartTick(local);
    else LeverTick(local);
}

void OnDisconnect() {
    if (g_step == Step::Walk || g_step == Step::Lever || g_step == Step::Run || g_step == Step::PlacedOnly)
        UE_LOGW("cremator-probe: FAIL the session ended mid-run");
    g_session.store(nullptr, std::memory_order_release);
    Reset();
}

}  // namespace coop::dev::cremator_probe
