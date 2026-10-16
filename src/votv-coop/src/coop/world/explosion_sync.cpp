// coop/world/explosion_sync.cpp -- see coop/world/explosion_sync.h.

#include "coop/world/explosion_sync.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/world/explosion_parking.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/devices/serverbox.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/explosion.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace coop::explosion_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace UH = ue_wrap::ufunction_hook;
namespace X  = ue_wrap::explosion;
namespace GT = ue_wrap::game_thread;
namespace E  = ue_wrap::engine;

std::atomic<coop::net::Session*> g_session{nullptr};

UE_FACT_FN(kRowBeginDeferred, "explosion", L"GameplayStatics", L"BeginDeferredActorSpawnFromClass", Declared);

// The install runs off the pump every tick, so a resolve that fails is retried on a one-second throttle, at
// most 30 tries, then said once; never a per-tick retry.
constexpr uint64_t kResolveRetryMs  = 1000;
constexpr int      kMaxResolveTries = 30;
bool     g_hookInstalled = false;
bool     g_gaveUp        = false;
uint64_t g_nextTryMs     = 0;
int      g_tries         = 0;

Counters g_counters{};
void*    g_lastWorld = nullptr;

thread_local int t_laneScopeDepth = 0;

constexpr const char* kDropNames[static_cast<int>(DropReason::Count)] = {
    "length", "class", "ignoreKind", "nonfinite", "coord", "scale", "parking"};
bool g_dropLogged[static_cast<int>(DropReason::Count)] = {};

// Count a drop under its reason and say it once per reason and session. `at` is where it was found.
void Drop(DropReason reason, const char* at) {
    const int i = static_cast<int>(reason);
    ++g_counters.dropped[i];
    if (g_dropLogged[i]) return;
    g_dropLogged[i] = true;
    UE_LOGW("[EXPLOSION] spawn dropped reason=%s at=%s", kDropNames[i], at);
}

bool AllFinite(const float* v, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

// The actor the explosion must not hurt, as its kind names it: an element's live actor, or a server box by its
// index in the gamemode's list. Null for none or for one that is not there.
void* IgnoreActorOf(const coop::net::ExplosionSpawnPayload& p) {
    if (p.ignoreKind == 1) {
        const coop::element::Element* e = coop::element::Registry::Get().Get(p.ignoreId);
        return e ? e->LiveActor() : nullptr;
    }
    if (p.ignoreKind == 2) {
        std::vector<void*> servers;
        ue_wrap::serverbox::ReadServers(servers);
        return p.ignoreId < servers.size() ? servers[p.ignoreId] : nullptr;
    }
    return nullptr;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Every Blueprint deferred spawn passes here: a null test, up to three pointer or name tests and a two-step
// super walk on a miss.
//
// A match is dissolved: the Blueprint's result is nulled, so what it does with the spawn reads nothing, and the
// unfinished actor is destroyed by a posted task, never inside the callback (the native call is still on the
// stack). The task validates by the slot, since an unfinished deferred actor can fail a serial test.
void OnBirth(void* /*context*/, void* sourceObject, void* result) {
    if (!result) return;
    bool byName = false;
    if (X::ClassIdOf(R::ClassOf(result), byName) < 0) return;
    if (!coop::spawn_authority::IsActiveClientSession()) return;
    if (InLaneSpawnScope()) return;
    const int32_t index = R::InternalIndexOf(result);
    if (void** r = static_cast<void**>(UH::CurrentResult())) *r = nullptr;
    GT::Post([result, index] {
        if (!R::IsLiveByIndex(result, index)) return;
        if (E::DestroyActor(result)) ++g_counters.destroyedByTask;
    });
    ++g_counters.dissolved;
    if (byName) ++g_counters.dissolvedByName;
    g_counters.lastDissolved      = result;
    g_counters.lastDissolvedIndex = index;
    UE_LOGI("[EXPLOSION] dissolved role=client src=%ls by=%s",
            sourceObject ? R::ClassNameOf(sourceObject).c_str() : L"?", byName ? "name" : "class");
}

}  // namespace

void Install(coop::net::Session* s) {
    g_session.store(s, std::memory_order_release);
    if (g_hookInstalled || g_gaveUp) return;
    const uint64_t now = NowMs();
    if (now < g_nextTryMs) return;
    g_nextTryMs = now + kResolveRetryMs;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowBeginDeferred);
    if (fn && UH::InstallPostHook(fn, &OnBirth)) {
        g_hookInstalled = true;
        UE_LOGI("[EXPLOSION] birth seam installed on BeginDeferredActorSpawnFromClass");
    } else if (++g_tries >= kMaxResolveTries) {
        g_gaveUp = true;
        UE_LOGE("[EXPLOSION] BeginDeferredActorSpawnFromClass did not resolve or would not take a post hook in "
                "%d tries; a client's own explosions are not dissolved", kMaxResolveTries);
    }
}

void Tick() {
    void* world = ue_wrap::world_identity::CurrentWorld();
    if (world != g_lastWorld) {
        g_lastWorld = world;
        X::Reset();
    }
    X::Resolve();
}

void OnDisconnect() {
    const Counters c = GetCounters();
    if (c.dissolved)
        UE_LOGI("[EXPLOSION] session end: dissolved=%u byName=%u destroyedByTask=%u", c.dissolved,
                c.dissolvedByName, c.destroyedByTask);
    X::Reset();
    g_lastWorld = nullptr;
    g_counters  = Counters{};
    std::memset(g_dropLogged, 0, sizeof(g_dropLogged));
}

LaneSpawnScope::LaneSpawnScope() { ++t_laneScopeDepth; }
LaneSpawnScope::~LaneSpawnScope() { --t_laneScopeDepth; }

bool InLaneSpawnScope() { return t_laneScopeDepth > 0; }

Counters GetCounters() { return g_counters; }

bool OnSpawnReceived(const uint8_t* data, size_t len) {
    coop::net::ExplosionSpawnPayload p{};
    if (!data || len != sizeof(p)) {
        Drop(DropReason::Length, "receive");
        return false;
    }
    std::memcpy(&p, data, sizeof(p));
    if (p.classId >= static_cast<uint8_t>(X::Cls::Count)) {
        Drop(DropReason::Class, "receive");
        return false;
    }
    if (p.ignoreKind > 2) {
        Drop(DropReason::IgnoreKind, "receive");
        return false;
    }
    if (!AllFinite(p.loc, 3) || !AllFinite(p.rot, 3) || !AllFinite(p.scale, 3) || !std::isfinite(p.damage) ||
        !std::isfinite(p.radius) || !std::isfinite(p.force) || !std::isfinite(p.fireStrength) ||
        !std::isfinite(p.fireFuel) || !std::isfinite(p.innerShake) || !std::isfinite(p.outerShake)) {
        Drop(DropReason::NonFinite, "receive");
        return false;
    }
    for (const float c : p.loc) {
        if (std::fabs(c) > coop::net::kMaxCoord) {
            Drop(DropReason::Coord, "receive");
            return false;
        }
    }
    // The open bound of SanitizeWireScaleAxis, but a drop and not its fallback to 1.
    for (const float s : p.scale) {
        if (!(s > 0.01f && s < 100.f)) {
            Drop(DropReason::Scale, "receive");
            return false;
        }
    }
    GT::Post([p] { Apply(p); });
    return true;
}

void Apply(const coop::net::ExplosionSpawnPayload& p) {
    if (!coop::spawn_authority::IsActiveClientSession() || InLaneSpawnScope()) return;
    if (!coop::explosion_parking::Live()) return Drop(DropReason::Parking, "apply");
    const X::Cls cls = static_cast<X::Cls>(p.classId);
    if (!X::ResolveNow(cls)) return Drop(DropReason::Class, "apply");
    [[maybe_unused]] void* ignore = IgnoreActorOf(p);
    UE_LOGI("[EXPLOSION] apply class=%u (spawn pending)", static_cast<unsigned>(p.classId));
}

}  // namespace coop::explosion_sync
