// coop/dev/cremator_probe.cpp -- see coop/dev/cremator_probe.h.

#include "coop/dev/cremator_probe.h"

#include "coop/config/config.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"
#include "coop/session/teleport_client.h"

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

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace coop::dev::cremator_probe {
namespace {

namespace E = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace R = ue_wrap::reflection;
namespace V = ue_wrap::vitals;

enum class Step : uint8_t { Place, Lever, Run, Over };

constexpr uint64_t kSampleMs = 1000;
constexpr float kPullbackAtS = 40.f;
constexpr float kSurvivedAtS = 90.f;
constexpr int kLinesAfterDeath = 10;

std::atomic<coop::net::Session*> g_session{nullptr};

Step g_step = Step::Place;
ue_wrap::CachedObjRef g_cremator;
uint64_t g_leverMs = 0;
uint64_t g_nextSampleMs = 0;
bool g_pulledBack = false;
bool g_died = false;
int g_linesLeftAfterDeath = 0;

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::cremator_probe);
    return s;
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

// A boolean as the log prints it: -1 when it could not be read.
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

struct Nearest {
    ue_wrap::FVector from;
    float best;
    void* actor;
    ue_wrap::FVector at;
};

void Fail(const char* why) {
    UE_LOGW("cremator-probe: FAIL %s", why);
    UE_LOGI("cremator-probe: DONE");
    g_step = Step::Over;
}

void PlaceTick(void* local) {
    if (!coop::net_pump::IsWorldSettled()) return;
    ue_wrap::FVector pawnAt{};
    if (!E::TryGetActorLocation(local, pawnAt)) return;
    void* cls = OI::ClassByName(L"cremator_C");
    if (!cls) { Fail("no cremator_C class"); return; }

    Nearest n{pawnAt, INFINITY, nullptr, {}};
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        auto& s = *static_cast<Nearest*>(ctx);
        void* firebox = ObjectProperty(obj, L"firebox");
        ue_wrap::FVector at{};
        if (!firebox || !E::TryGetComponentLocation(firebox, at)) return;
        const float dx = at.X - s.from.X, dy = at.Y - s.from.Y, dz = at.Z - s.from.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < s.best) { s.best = d2; s.actor = obj; s.at = at; }
    }, &n);
    if (!n.actor) { Fail("no cremator with a firebox is loaded"); return; }

    ue_wrap::FRotator rot{};
    if (!E::TryGetActorRotation(n.actor, rot)) { Fail("the cremator's rotation is unreadable"); return; }
    const std::wstring key = ue_wrap::prop::GetActorSaveKeyString(n.actor);
    UE_LOGI("cremator-probe: cremator key='%ls' firebox=(%.0f,%.0f,%.0f) yaw=%.1f pawn=(%.0f,%.0f,%.0f)",
            key.c_str(), n.at.X, n.at.Y, n.at.Z, rot.Yaw, pawnAt.X, pawnAt.Y, pawnAt.Z);

    // The placement a host teleport applies on a client, so the probe measures the feature's own landing.
    const bool moved = coop::teleport_client::ApplyLocally({n.at.X, n.at.Y, n.at.Z, rot.Pitch, rot.Yaw, rot.Roll});
    bool invinc = false;
    const bool haveInvinc = E::ReadMainPlayerStartInvinc(local, invinc);
    UE_LOGI("cremator-probe: PLACED moved=%d startInvinc=%d", moved ? 1 : 0, haveInvinc ? (invinc ? 1 : 0) : -1);
    if (!moved) { Fail("the placement reported no move"); return; }
    g_cremator.Set(n.actor);
    g_step = Step::Lever;
}

void LeverTick() {
    void* cremator = g_cremator.Get();
    if (!cremator) { Fail("the cremator died before the lever"); return; }
    void* door = ObjectProperty(cremator, L"swingerDoor");
    bool opened = false;
    if (door && ReadBool(door, L"opened", opened) && opened && !CallNoParams(door, L"close")) {
        Fail("the open door's close() did not run");
        return;
    }
    if (!CallNoParams(cremator, L"useLever")) { Fail("useLever did not run"); return; }
    g_leverMs = coop::net::NowMs();
    g_nextSampleMs = g_leverMs + kSampleMs;
    UE_LOGI("cremator-probe: LEVER doorWasOpened=%d", opened ? 1 : 0);
    g_step = Step::Run;
}

void LogSample(void* local, float t) {
    void* cremator = g_cremator.Get();
    void* door = ObjectProperty(cremator, L"swingerDoor");
    float hp = -1.f, burn = -1.f, windup = -1.f;
    V::Read(V::Field::Health, &hp);
    V::Read(V::Field::BurningTime, &burn);
    ReadFloat(cremator, L"windup", windup);
    bool ragdoll = false, dead = false;
    const int deadBit = local && E::ReadMainPlayerRagdollState(local, ragdoll, dead) ? (dead ? 1 : 0) : -1;
    ue_wrap::FVector pos{};
    if (!local || !E::TryGetActorLocation(local, pos)) pos = {};
    UE_LOGI("cremator-probe: T=%.0f hp=%.1f burn=%.1f active=%d anim=%d windup=%.3f closed=%d doorOpened=%d dead=%d "
            "pos=(%.0f,%.0f,%.0f)",
            t, hp, burn, Bit(cremator, L"isActive"), Bit(cremator, L"anim"), windup, Bit(cremator, L"isClosed"),
            Bit(door, L"opened"), deadBit, pos.X, pos.Y, pos.Z);
    if (deadBit == 1 && !g_died) {
        g_died = true;
        g_linesLeftAfterDeath = kLinesAfterDeath;
        UE_LOGI("cremator-probe: DIED T=%.0f", t);
    }
}

void RunTick(void* local) {
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextSampleMs) return;
    g_nextSampleMs = now + kSampleMs;
    const float t = static_cast<float>(now - g_leverMs) / 1000.f;

    // The pull-back goes before the sample so the line of the second it ran in already shows its effect's start.
    if (t >= kPullbackAtS && !g_pulledBack && !g_died) {
        g_pulledBack = true;
        UE_LOGI("cremator-probe: PULLBACK ran=%d", CallNoParams(g_cremator.Get(), L"useLever") ? 1 : 0);
    }
    const bool wasDead = g_died;
    LogSample(local, t);
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

void Reset() {
    g_step = Step::Place;
    g_cremator.Reset();
    g_leverMs = 0;
    g_nextSampleMs = 0;
    g_pulledBack = false;
    g_died = false;
    g_linesLeftAfterDeath = 0;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!Enabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Client || g_step == Step::Over) return;
    // After the death the pawn may be swapped or absent: the sample then prints its sentinels and the line
    // count still runs out.
    void* local = coop::players::Registry::Get().Local();
    if (g_step == Step::Run) { RunTick(local); return; }
    if (!local) return;
    if (g_step == Step::Place) PlaceTick(local);
    else LeverTick();
}

void OnDisconnect() {
    if (g_step == Step::Lever || g_step == Step::Run) UE_LOGW("cremator-probe: FAIL the session ended mid-run");
    g_session.store(nullptr, std::memory_order_release);
    Reset();
}

}  // namespace coop::dev::cremator_probe
