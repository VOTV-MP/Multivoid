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
#include "ue_wrap/core/script_gate.h"
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
namespace SG = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};

UE_FACT_FN(kRowBeginDeferred, "explosion", L"GameplayStatics", L"BeginDeferredActorSpawnFromClass", Declared);
UE_FACT_FN(kRowCaptureBeginPlay, "explosion", L"explosion_C", L"ReceiveBeginPlay", Declared);
constexpr int kTagCapture = 0x45584350;  // 'EXCP'

// The install runs off the pump every tick, so a resolve that fails is retried on a one-second throttle, at
// most 30 tries, then said once; never a per-tick retry.
constexpr uint64_t kResolveRetryMs  = 1000;
constexpr int      kMaxResolveTries = 30;
bool     g_hookInstalled = false;
bool     g_gaveUp        = false;
bool     g_captureInstalled = false;
bool     g_captureGaveUp    = false;
int      g_captureTries     = 0;
bool     g_captureReadLogged = false;
bool     g_sendFailLogged    = false;
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

// The host's capture: at an explosion's begin-play, where the Blueprint's own writes are done, its class, transform
// and parameters are built into an ExplosionSpawn. Always Run: the host's explosion is the game's own.
SG::Verdict OnCapturePre(const SG::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!c.object || !s || !s->connected() || s->role() != coop::net::Role::Host || s->connectedPeerCount() <= 0)
        return SG::Verdict::Run;
    bool byName = false;
    const int classId = X::ClassIdOf(R::ClassOf(c.object), byName);
    if (classId < 0) return SG::Verdict::Run;
    coop::net::ExplosionSpawnPayload p{};
    X::Params params{};
    if (!X::ReadParams(c.object, params) || !X::ReadTransform(c.object, p.loc, p.rot, p.scale)) {
        if (!g_captureReadLogged) {
            g_captureReadLogged = true;
            UE_LOGE("[EXPLOSION] host capture could not read an explosion's parameters or transform; it is not sent");
        }
        return SG::Verdict::Run;
    }
    p.classId = static_cast<uint8_t>(classId);
    p.flags = static_cast<uint8_t>((params.ignoreWalls ? 1u : 0u) | (params.skipSetting ? 2u : 0u));
    p.objType = static_cast<uint8_t>(params.objType < 0 ? 0 : (params.objType > 255 ? 255 : params.objType));
    p.damage = params.damage;
    p.radius = params.radius;
    p.force = params.force;
    p.fireStrength = params.fireStrength;
    p.fireFuel = params.fireFuel;
    p.innerShake = params.innerShake;
    p.outerShake = params.outerShake;
    // The actor the blast must not hurt: an element by its id, else a server box by its list index.
    if (void* ignore = X::FirstIgnore(c.object)) {
        const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(ignore);
        if (eid != coop::element::kInvalidId) {
            p.ignoreKind = 1;
            p.ignoreId = eid;
        } else if (const int32_t box = ue_wrap::serverbox::IndexOf(ignore); box >= 0) {
            p.ignoreKind = 2;
            p.ignoreId = static_cast<uint32_t>(box);
        }
    }
    ++g_counters.captured;
    // Declared divergence from MTA, which relays an explosion only to the players within MAX_EXPLOSION_SYNC_DISTANCE
    // (CGame.cpp:2932-2943): here every client gets it, as explosions broadcast unconditionally in this mod; the
    // session leaves out a peer whose world is not ready. A send that fails is a one-shot cue lost, never retried.
    if (!s->SendReliable(coop::net::ReliableKind::ExplosionSpawn, &p, sizeof(p))) {
        ++g_counters.sendFailed;
        if (!g_sendFailLogged) {
            g_sendFailLogged = true;
            UE_LOGW("[EXPLOSION] host capture send failed class=%d (channel busy); not retried", classId);
        }
    }
    UE_LOGI("[EXPLOSION] host capture class=%d", classId);
    return SG::Verdict::Run;
}

}  // namespace

void Install(coop::net::Session* s) {
    g_session.store(s, std::memory_order_release);
    const bool birthSettled = g_hookInstalled || g_gaveUp;
    const bool captureSettled = g_captureInstalled || g_captureGaveUp;
    if (birthSettled && captureSettled) return;
    const uint64_t now = NowMs();
    if (now < g_nextTryMs) return;
    g_nextTryMs = now + kResolveRetryMs;
    if (!birthSettled) {
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
    if (!captureSettled) {
        if (SG::WatchName(kRowCaptureBeginPlay, kTagCapture, &OnCapturePre, nullptr)) {
            g_captureInstalled = true;
            UE_LOGI("[EXPLOSION] capture watch installed on explosion_C::ReceiveBeginPlay");
        } else if (++g_captureTries >= kMaxResolveTries) {
            g_captureGaveUp = true;
            UE_LOGE("[EXPLOSION] the capture watch was refused %d times; the host's explosions are not sent",
                    kMaxResolveTries);
        }
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
    if (c.dissolved || c.captured || c.applied[0] || c.applied[1] || c.applied[2])
        UE_LOGI("[EXPLOSION] session end: dissolved=%u byName=%u destroyedByTask=%u captured=%u sendFailed=%u "
                "applied=%u/%u/%u", c.dissolved, c.dissolvedByName, c.destroyedByTask, c.captured, c.sendFailed,
                c.applied[0], c.applied[1], c.applied[2]);
    X::Reset();
    g_lastWorld = nullptr;
    g_counters  = Counters{};
    std::memset(g_dropLogged, 0, sizeof(g_dropLogged));
    g_captureReadLogged = false;
    g_sendFailLogged = false;
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
    const X::Params params{p.damage, p.radius, p.force, p.fireStrength, p.fireFuel, p.innerShake, p.outerShake,
                           static_cast<int32_t>(p.objType), (p.flags & 1u) != 0, (p.flags & 2u) != 0};
    void* actor = nullptr;
    {
        LaneSpawnScope scope;  // the birth seam leaves the lane's own spawn alone
        actor = X::SpawnDeferred(cls, p.loc, p.rot, p.scale, params, IgnoreActorOf(p));
    }
    if (!actor) {
        UE_LOGW("[EXPLOSION] apply class=%u spawned nothing", static_cast<unsigned>(p.classId));
        return;
    }
    ++g_counters.applied[p.classId];
    UE_LOGI("[EXPLOSION] apply class=%u", static_cast<unsigned>(p.classId));
}

}  // namespace coop::explosion_sync
