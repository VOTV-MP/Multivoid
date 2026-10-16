// coop/dev/explosion_drill.cpp -- see coop/dev/explosion_drill.h.

#include "coop/dev/explosion_drill.h"
#include "coop/dev/explosion_drill_internal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/world/explosion_sync.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace coop::dev::explosion_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace SG = ue_wrap::script_gate;
namespace JP = coop::join_progress;
namespace ES = coop::explosion_sync;
using Clock = std::chrono::steady_clock;
using detail::Arm;

constexpr auto kWatchRetry = std::chrono::seconds(1);
constexpr int  kWatchTries = 30;
constexpr int  kTag = 0x45584452;  // 'EXDR'
constexpr float kLoadHeight = 50000.f;
constexpr float kWorldAhead = 1500.f;

// The drill's own instrument: the explosion's begin-play, which a dissolved explosion never reaches.
UE_FACT_FN(kRowDrillBeginPlay, "dev.explosion_drill", L"explosion_C", L"ReceiveBeginPlay", Declared);

enum class Step : uint8_t { Idle, Spawned, Broken, Done };

// One half's run: the barrel, the counters at its spawn, and its clock.
struct Run {
    Step                 step = Step::Idle;
    ue_wrap::CachedObjRef barrel;
    Clock::time_point    at{};
    ES::Counters         base{};
    uint32_t             beginPlay0 = 0;
};

Run g_load;
Run g_world;

uint32_t g_beginPlay = 0;
bool     g_watched = false;
bool     g_watchDead = false;
int      g_watchTries = 0;
Clock::time_point g_nextWatch{};

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::explosion_drill);
        if (v == "dissolve") return Arm::Dissolve;
        if (v == "apply") return Arm::Apply;
        if (v == "coast") return Arm::Coast;
        if (v == "spawn_malformed") return Arm::SpawnMalformed;
        return Arm::Off;
    }();
    return a;
}

SG::Verdict OnBeginPlayPre(const SG::Call&) {
    ++g_beginPlay;
    return SG::Verdict::Run;
}

bool WatchLive() { return g_watched && SG::NameWatchLive(kRowDrillBeginPlay, kTag); }

// Registered under the install policy (a refused registration is retried each second, 30 times, then the drill is
// abandoned) and its name resolved on the game thread. False once the watch is dead for good.
bool EnsureWatch() {
    if (g_watchDead) return false;
    if (!g_watched && Clock::now() >= g_nextWatch) {
        g_nextWatch = Clock::now() + kWatchRetry;
        g_watched = SG::WatchName(kRowDrillBeginPlay, kTag, &OnBeginPlayPre, nullptr);
        if (!g_watched && ++g_watchTries >= kWatchTries) {
            g_watchDead = true;
            UE_LOGW("[EXPLOSION-DRILL] ABANDONED: the begin-play watch was refused %d times", kWatchTries);
            return false;
        }
    }
    if (!g_watched) return true;
    SG::ResolvePendingNames();
    if (SG::NameWatchSettled(kRowDrillBeginPlay, kTag) && !WatchLive()) {
        g_watchDead = true;
        UE_LOGW("[EXPLOSION-DRILL] ABANDONED: the begin-play watch settled dead, so a leak cannot be seen");
        return false;
    }
    return true;
}

bool Expired(const Run& r) { return r.at != Clock::time_point{} && Clock::now() - r.at >= detail::kBound; }

void Finish(Run& r) {
    if (void* b = r.barrel.Get()) E::DestroyActor(b);
    r.barrel.Reset();
    r.step = Step::Done;
}

void Fail(Run& r, const char* half, const char* why) {
    UE_LOGW("[EXPLOSION-DRILL] FAIL %s %s", half, why);
    Finish(r);
}

void Abandon(Run& r, const char* why) {
    UE_LOGW("[EXPLOSION-DRILL] ABANDONED: %s", why);
    Finish(r);
}

// The barrel's class, from the index or else the load by path; null when neither has it.
void* BarrelClass() {
    if (void* c = ue_wrap::object_index::ClassByName(L"prop_bombBarrel_C")) return c;
    return ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/prop_bombBarrel.prop_bombBarrel_C");
}

// The barrel's own broken(): its Blueprint spawns the explosion.
bool CallBroken(void* barrel) {
    void* fn = R::FindFunction(R::ClassOf(barrel), L"broken");
    return fn && R::CallFunction(barrel, fn, nullptr);
}

// Spawn the barrel at `at`. 1 when it stands, 0 to try again next call, -1 when the drill cannot go on.
int Spawn(Run& r, const ue_wrap::FVector& at) {
    void* cls = BarrelClass();
    if (!cls) {
        Abandon(r, "prop_bombBarrel_C is neither loaded nor loadable by path");
        return -1;
    }
    void* barrel = E::SpawnActor(cls, at);
    if (!barrel) return 0;  // the world refuses spawns for the moment
    r.barrel.Set(barrel);
    r.base = ES::GetCounters();
    r.beginPlay0 = g_beginPlay;
    r.step = Step::Spawned;
    return 1;
}

// Break the barrel once it stands.
bool Break(Run& r, const char* half) {
    void* barrel = r.barrel.Get();
    if (!barrel) {
        Fail(r, half, "the barrel is gone before its broken()");
        return false;
    }
    if (!CallBroken(barrel)) {
        Abandon(r, "the barrel's broken() did not run");
        return false;
    }
    r.step = Step::Broken;
    return true;
}

bool ClientRuns(coop::net::Session* s) {
    return s && s->running() && s->role() == coop::net::Role::Client;
}

void LoadJudge(Run& r, bool loading) {
    const ES::Counters c = ES::GetCounters();
    const uint32_t dissolved = c.dissolved - r.base.dissolved;
    const uint32_t leaked = g_beginPlay - r.beginPlay0;
    if (dissolved > 1) return Fail(r, "load", "more than one dissolve for the one barrel");
    if (leaked) return Fail(r, "load", "leak: the explosion entered begin-play");
    if (dissolved == 1) {
        if (c.dissolvedByName - r.base.dissolvedByName != 1)
            return Fail(r, "load", "the dissolve came by class, where the load window has no class table");
        if (!WatchLive()) {
            if (!loading || Expired(r)) Fail(r, "load", "the leak watch never went live");
            return;
        }
        UE_LOGI("[EXPLOSION-DRILL] client load PASS by=name");
        Finish(r);
        return;
    }
    if (!loading || Expired(r)) {
        UE_LOGW("[EXPLOSION-DRILL] FAIL load no dissolve before the world-ready edge (leak watch live=%d)",
                WatchLive() ? 1 : 0);
        Finish(r);
    }
}

void WorldJudge(Run& r) {
    const ES::Counters c = ES::GetCounters();
    const uint32_t dissolved = c.dissolved - r.base.dissolved;
    if (g_beginPlay != r.beginPlay0) return Fail(r, "world", "leak: the explosion entered begin-play");
    if (dissolved > 1) return Fail(r, "world", "more than one dissolve for the one barrel");
    if (c.dissolvedByName != r.base.dissolvedByName)
        return Fail(r, "world", "the dissolve came by name, where a loaded world has the class table");
    const bool destroyed = dissolved == 1 && c.destroyedByTask > r.base.destroyedByTask;
    if (destroyed && !R::IsLiveByIndex(c.lastDissolved, c.lastDissolvedIndex)) {
        UE_LOGI("[EXPLOSION-DRILL] client: dissolved actor gone live=0");
        UE_LOGI("[EXPLOSION-DRILL] client DONE PASS arm=dissolve");
        Finish(r);
        return;
    }
    if (Expired(r)) {
        UE_LOGW("[EXPLOSION-DRILL] FAIL world no verdict in 60 s (dissolved=%u destroyedByTask=%u)", dissolved,
                c.destroyedByTask - r.base.destroyedByTask);
        Finish(r);
    }
}

void DissolveLoad(coop::net::Session* s) {
    if (g_load.step == Step::Done || !ClientRuns(s)) return;
    const bool loading = JP::CurrentPhase() == JP::Phase::LoadingWorld &&
                         ue_wrap::world_identity::CurrentWorldKind() == ue_wrap::world_identity::WorldKind::Gameplay;
    switch (g_load.step) {
    case Step::Idle: {
        if (!loading) return;
        if (!EnsureWatch()) return Abandon(g_load, "the begin-play watch is dead");
        if (!g_watched) return;
        if (g_load.at == Clock::time_point{}) g_load.at = Clock::now();
        if (Spawn(g_load, {0.f, 0.f, kLoadHeight}) == 0 && Expired(g_load))
            Fail(g_load, "load", "the barrel could not be spawned in the load");
        return;
    }
    case Step::Spawned:
        if (!loading) return Fail(g_load, "load", "no dissolve before the world-ready edge (the barrel never broke)");
        Break(g_load, "load");
        return;
    case Step::Broken:
        EnsureWatch();
        LoadJudge(g_load, loading);
        return;
    case Step::Done:
        return;
    }
}

void DissolveWorld(coop::net::Session* s) {
    if (g_world.step == Step::Done || !ClientRuns(s)) return;
    switch (g_world.step) {
    case Step::Idle: {
        if (!coop::net_pump::HasAnnouncedWorldReady() || JP::CurrentPhase() != JP::Phase::Idle) return;
        if (!EnsureWatch()) return Abandon(g_world, "the begin-play watch is dead");
        if (g_world.at == Clock::time_point{}) g_world.at = Clock::now();
        if (!WatchLive()) {
            if (Expired(g_world)) Fail(g_world, "world", "the leak watch never went live");
            return;
        }
        void* pawn = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!pawn || !E::TryGetActorLocation(pawn, at)) return;
        const ue_wrap::FVector fwd = E::GetActorForwardVector(pawn);
        if (Spawn(g_world, {at.X + fwd.X * kWorldAhead, at.Y + fwd.Y * kWorldAhead, at.Z + fwd.Z * kWorldAhead}) == 0 &&
            Expired(g_world))
            Fail(g_world, "world", "the barrel could not be spawned");
        return;
    }
    case Step::Spawned:
        Break(g_world, "world");
        return;
    case Step::Broken:
        WorldJudge(g_world);
        return;
    case Step::Done:
        return;
    }
}

}  // namespace

void TickLoad(coop::net::Session* s) {
    if (ArmOf() == Arm::Dissolve) DissolveLoad(s);
}

void Tick(coop::net::Session* s) {
    const Arm arm = ArmOf();
    if (arm == Arm::Off || !s) return;
    if (arm == Arm::Dissolve) return DissolveWorld(s);
    if (s->role() == coop::net::Role::Host) {
        if (s->connected()) detail::HostTick(*s, arm);
    } else if (ClientRuns(s)) {
        detail::ClientTick(arm);
    }
}

void OnDisconnect() {
    g_load = Run{};
    g_world = Run{};
    detail::HostReset();
    detail::ClientReset();
}

}  // namespace coop::dev::explosion_drill
