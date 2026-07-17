// coop/creatures/owner_entity_sync.cpp -- see the header for the tier design.
// Shapes reused: host_spawn_watcher (BeginDeferred POST + param-transform
// read), roach_sync (lazy resolve + keepalive), npc_mirror (mirror park via
// puppet::DisableCharacterTicks), prop_echo_suppress::ScopedMirrorSpawn (the
// receiver's own spawn call must not re-trigger the owner observer).

#include "coop/creatures/owner_entity_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"    // kMaxPeers
#include "coop/props/prop_echo_suppress.h"   // ScopedMirrorSpawn / InMirrorSpawnScope

#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/puppet.h"                  // DisableCharacterTicks (the npc-mirror park)
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace coop::owner_entity_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace P  = ue_wrap::profile;

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- the member-class table (classId = index). Extend here for the next
// stalker-entity member; every row is per-peer-owned + cross-peer mirrored.
constexpr const wchar_t* kOwnerEntityClasses[] = {
    L"eyer_C",   // classId 0: the night "Eyes" stalker (ticker_eyers)
};
constexpr size_t kOwnerEntityClassCount =
    sizeof(kOwnerEntityClasses) / sizeof(kOwnerEntityClasses[0]);

// Resolved UClass* per row. Written on the game thread (Install retry), read
// by the POST observer (game-thread too -- actor spawns are GT-only).
void* g_classes[kOwnerEntityClassCount] = {};

int ClassIdOf(void* cls) {
    if (!cls) return -1;
    for (size_t i = 0; i < kOwnerEntityClassCount; ++i)
        if (g_classes[i] == cls) return static_cast<int>(i);
    return -1;
}

// ---- BeginDeferred seam (own resolve; host_spawn_watcher pattern) ----
void*   g_beginDeferredFn = nullptr;
int32_t g_classParamOff   = -1;
int32_t g_returnParamOff  = -1;
int32_t g_xformParamOff   = -1;
bool    g_observerArmed   = false;   // also the permanent give-up latch
void*   g_k2DestroyFn     = nullptr; // AActor::K2_DestroyActor (mirror teardown)

constexpr long long kTickPeriodMs      = 250;    // 4 Hz driver
constexpr long long kKeepaliveMs       = 10000;  // Spawn re-announce (late-join delivery)
constexpr float     kPoseSendDist      = 5.f;    // movement threshold
constexpr float     kPoseSendYawDeg    = 5.f;

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---- OWNER side: entities this peer spawned natively ----
struct Owned {
    uint16_t seq;
    uint8_t  classId;
    void*    actor;
    int32_t  idx;
    float    x, y, z, yaw;   // last sent
    long long lastAnnounceMs;
};
std::vector<Owned> g_owned;
uint16_t g_nextSeq = 1;
constexpr size_t kMaxOwned = 8;   // eyers are rare singletons; runaway backstop

// ---- RECEIVER side: mirrors of other peers' entities ----
struct Mirror {
    void*   actor;
    int32_t idx;
};
std::unordered_map<uint32_t, Mirror> g_mirrors;  // key = (slot << 16) | seq

uint32_t Key(int slot, uint16_t seq) {
    return (static_cast<uint32_t>(slot) << 16) | seq;
}

float YawOfQuat(float qx, float qy, float qz, float qw) {
    constexpr float kRadToDeg = 57.29577951308232f;
    return std::atan2(2.f * (qw * qz + qx * qy), 1.f - 2.f * (qy * qy + qz * qz)) * kRadToDeg;
}

void DestroyMirrorActor(void* actor, int32_t idx) {
    if (!actor || !R::IsLiveByIndex(actor, idx)) return;
    if (!g_k2DestroyFn) return;
    R::CallFunction(actor, g_k2DestroyFn, nullptr);
}

// The OWNER observer: a member-class actor spawned LOCALLY (native ticker
// roll or the F1 dev test). Mirror spawns are excluded by the mirror scope.
// GT-only (actor spawning is GT); engine calls are safe here.
void OnSpawnPost(void* /*self*/, void* /*function*/, void* params) {
    if (!GT::IsGameThread()) return;
    if (!params || g_classParamOff < 0 || g_returnParamOff < 0 || g_xformParamOff < 0) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (coop::prop_echo_suppress::InMirrorSpawnScope()) return;  // our own mirror spawn

    void* actorClass = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(params) + g_classParamOff);
    const int classId = ClassIdOf(actorClass);   // exact-ptr fast reject
    if (classId < 0) return;
    void* actor = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(params) + g_returnParamOff);
    if (!actor || !R::IsLive(actor)) return;
    if (g_owned.size() >= kMaxOwned) {
        UE_LOGW("owner_entity: owned cap %zu hit -- NOT broadcasting this '%ls' spawn",
                kMaxOwned, kOwnerEntityClasses[classId]);
        return;
    }
    // The actor is NOT positioned at Begin POST -- read the SpawnTransform
    // param (the host_spawn_watcher lesson; ue_wrap::FTransform IS the layout).
    const auto* xt = reinterpret_cast<const ue_wrap::FTransform*>(
        reinterpret_cast<const uint8_t*>(params) + g_xformParamOff);
    Owned o{};
    o.seq = g_nextSeq++;
    if (g_nextSeq == 0) g_nextSeq = 1;
    o.classId = static_cast<uint8_t>(classId);
    o.actor = actor;
    o.idx = R::InternalIndexOf(actor);
    o.x = xt->TX; o.y = xt->TY; o.z = xt->TZ;
    o.yaw = YawOfQuat(xt->RotX, xt->RotY, xt->RotZ, xt->RotW);
    o.lastAnnounceMs = NowMs();
    g_owned.push_back(o);

    coop::net::OwnerEntitySpawnPayload p{};
    p.seq = o.seq; p.classId = o.classId;
    p.x = o.x; p.y = o.y; p.z = o.z; p.yaw = o.yaw;
    if (s->SendReliable(coop::net::ReliableKind::OwnerEntitySpawn, &p, sizeof(p))) {
        UE_LOGI("owner_entity: OWN '%ls' spawned locally seq=%u at (%.0f,%.0f,%.0f) -- announced",
                kOwnerEntityClasses[classId], o.seq, o.x, o.y, o.z);
    } else {
        UE_LOGW("owner_entity: announce send FAILED for seq=%u (keepalive re-announces)", o.seq);
    }
}

void AnnounceOwned(coop::net::Session* s, Owned& o, long long now) {
    coop::net::OwnerEntitySpawnPayload p{};
    p.seq = o.seq; p.classId = o.classId;
    p.x = o.x; p.y = o.y; p.z = o.z; p.yaw = o.yaw;
    if (s->SendReliable(coop::net::ReliableKind::OwnerEntitySpawn, &p, sizeof(p)))
        o.lastAnnounceMs = now;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_observerArmed) return;
    static uint32_t sN = 0;
    if ((sN++ % 125) != 0) return;  // ~1 Hz of the pump while unresolved

    if (!g_beginDeferredFn) {
        void* gsCls = R::FindClass(P::name::GameplayStaticsClass);
        if (!gsCls) return;
        g_beginDeferredFn = R::FindFunction(gsCls, P::name::BeginDeferredSpawnFn);
        if (!g_beginDeferredFn) {
            UE_LOGW("owner_entity: BeginDeferred not found -- lane disabled for process lifetime");
            g_observerArmed = true;  // give up
            return;
        }
        g_classParamOff  = R::FindParamOffset(g_beginDeferredFn, L"ActorClass");
        g_returnParamOff = R::FindParamOffset(g_beginDeferredFn, L"ReturnValue");
        g_xformParamOff  = R::FindParamOffset(g_beginDeferredFn, L"SpawnTransform");
        if (g_classParamOff < 0 || g_returnParamOff < 0 || g_xformParamOff < 0) {
            UE_LOGW("owner_entity: BeginDeferred param offsets missing -- lane disabled");
            g_observerArmed = true;
            return;
        }
    }
    if (!g_k2DestroyFn) {
        if (void* actorCls = R::FindClass(P::name::ActorClassName))
            g_k2DestroyFn = R::FindFunction(actorCls, L"K2_DestroyActor");
        if (!g_k2DestroyFn) return;  // retry (engine class -- resolves at boot)
    }
    // Member classes load with the world -- retry until all resolve.
    size_t resolved = 0;
    for (size_t i = 0; i < kOwnerEntityClassCount; ++i) {
        if (!g_classes[i]) g_classes[i] = R::FindClass(kOwnerEntityClasses[i]);
        if (g_classes[i]) ++resolved;
    }
    if (resolved < kOwnerEntityClassCount) return;

    if (!GT::RegisterPostObserver(g_beginDeferredFn, &OnSpawnPost)) {
        UE_LOGE("owner_entity: RegisterPostObserver FAILED (table full?) -- lane disabled");
        g_observerArmed = true;
        return;
    }
    g_observerArmed = true;
    UE_LOGI("owner_entity: armed (%zu member class(es); eyer_C classId=0) -- per-peer owned, "
            "cross-peer mirrored", kOwnerEntityClassCount);
}

void Tick() {
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    static long long sLastMs = 0;
    const long long now = NowMs();
    if (now - sLastMs < kTickPeriodMs) return;
    sLastMs = now;

    // OWNER: death-watch + keepalive re-announce + pose stream.
    for (size_t i = 0; i < g_owned.size();) {
        Owned& o = g_owned[i];
        if (!R::IsLiveByIndex(o.actor, o.idx)) {
            coop::net::OwnerEntityDestroyPayload d{};
            d.seq = o.seq;
            if (!s->SendReliable(coop::net::ReliableKind::OwnerEntityDestroy, &d, sizeof(d))) {
                ++i;  // send failed (channel exhaustion): keep the entry, retry
                continue;  // next tick (audit F-5: a lost death = permanent ghost)
            }
            UE_LOGI("owner_entity: OWN seq=%u died locally -- destroy announced", o.seq);
            g_owned.erase(g_owned.begin() + i);
            continue;
        }
        const ue_wrap::FVector loc = E::GetActorLocation(o.actor);
        const float yaw = E::GetActorRotation(o.actor).Yaw;
        // Keepalive FIRST (audit F-3): a continuously-moving entity must not
        // starve the Spawn re-announce -- it IS the late-joiner delivery, and
        // it carries the pose anyway.
        if (now - o.lastAnnounceMs >= kKeepaliveMs) {
            o.x = loc.X; o.y = loc.Y; o.z = loc.Z; o.yaw = yaw;
            AnnounceOwned(s, o, now);
            ++i;
            continue;
        }
        const float dx = loc.X - o.x, dy = loc.Y - o.y, dz = loc.Z - o.z;
        float dyaw = yaw - o.yaw;
        while (dyaw > 180.f) dyaw -= 360.f;
        while (dyaw < -180.f) dyaw += 360.f;
        if (dx * dx + dy * dy + dz * dz > kPoseSendDist * kPoseSendDist ||
            std::fabs(dyaw) > kPoseSendYawDeg) {
            o.x = loc.X; o.y = loc.Y; o.z = loc.Z; o.yaw = yaw;
            coop::net::OwnerEntityPosePayload p{};
            p.seq = o.seq; p.x = o.x; p.y = o.y; p.z = o.z; p.yaw = o.yaw;
            s->SendReliable(coop::net::ReliableKind::OwnerEntityPose, &p, sizeof(p));
        }
        ++i;
    }

    // RECEIVER: prune mirrors whose actor the engine reclaimed (world change).
    for (auto it = g_mirrors.begin(); it != g_mirrors.end();) {
        if (!R::IsLiveByIndex(it->second.actor, it->second.idx)) it = g_mirrors.erase(it);
        else ++it;
    }
}

bool FiniteVec(float x, float y, float z, float yaw) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(yaw);
}

void OnSpawnMsg(const coop::net::OwnerEntitySpawnPayload& p, int senderPeerSlot) {
    if (!GT::IsGameThread()) { UE_LOGW("owner_entity: OnSpawnMsg off-GT -- dropping"); return; }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (senderPeerSlot < 0 || senderPeerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    if (!FiniteVec(p.x, p.y, p.z, p.yaw)) return;  // wire NaN/Inf guard (audit F-8)
    if (p.classId >= kOwnerEntityClassCount) {
        UE_LOGW("owner_entity: OnSpawnMsg unknown classId=%u -- dropping", p.classId);
        return;
    }
    const uint32_t key = Key(senderPeerSlot, p.seq);
    auto it = g_mirrors.find(key);
    if (it != g_mirrors.end()) {
        // Keepalive re-announce of a known entity: refresh the pose.
        if (R::IsLiveByIndex(it->second.actor, it->second.idx)) {
            E::SetActorLocation(it->second.actor, {p.x, p.y, p.z});
            E::SetActorRotation(it->second.actor, {0.f, p.yaw, 0.f});
            return;
        }
        g_mirrors.erase(it);   // dead mirror (engine reclaim) -- respawn below
    }
    void* cls = g_classes[p.classId];
    if (!cls) return;  // classes unresolved: the keepalive re-delivers post-resolve
    // Resolve-once statics (GT-only path): the GameplayStatics CDO + Finish fn.
    static void* s_gsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    static void* s_finishFn = [] {
        void* gsCls = R::FindClass(P::name::GameplayStaticsClass);
        return gsCls ? R::FindFunction(gsCls, P::name::FinishSpawningActorFn) : nullptr;
    }();
    void* gsCdo = s_gsCdo;
    void* finishFn = s_finishFn;
    void* worldCtx = E::GetWorldContext();
    if (!gsCdo || !worldCtx || !g_beginDeferredFn || !finishFn) return;

    ue_wrap::FTransform xform{};
    E::RotatorToQuat(0.f, p.yaw, 0.f, xform.RotX, xform.RotY, xform.RotZ, xform.RotW);
    xform.TX = p.x; xform.TY = p.y; xform.TZ = p.z;
    xform.SX = xform.SY = xform.SZ = 1.f;
    constexpr uint8_t kAlwaysSpawn = 1;
    void* spawned = nullptr;
    {
        // The scope excludes this spawn from BOTH owner observers (ours and
        // the ambient broadcaster) -- the receiver's spawn is never re-announced.
        coop::prop_echo_suppress::ScopedMirrorSpawn scope;
        ue_wrap::ParamFrame begin(g_beginDeferredFn);
        if (!begin.valid()) return;
        begin.Set<void*>(L"WorldContextObject", worldCtx);
        begin.Set<void*>(L"ActorClass", cls);
        begin.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        begin.Set<uint8_t>(L"CollisionHandlingOverride", kAlwaysSpawn);
        begin.Set<void*>(L"Owner", nullptr);
        if (!ue_wrap::Call(gsCdo, begin)) return;
        spawned = begin.Get<void*>(L"ReturnValue");
        if (!spawned) {
            UE_LOGW("owner_entity: mirror BeginDeferred null for slot=%d seq=%u", senderPeerSlot, p.seq);
            return;
        }
        // Collision OFF in the DEFERRED window (audit F-4): BeginPlay + the
        // initial overlap update run INSIDE FinishSpawningActor -- a viewer
        // standing at the wire transform must not be overlap-killed by the
        // killsphere during that one frame.
        E::SetActorEnableCollision(spawned, false);
        ue_wrap::ParamFrame finish(finishFn);
        if (!finish.valid()) return;
        finish.Set<void*>(L"Actor", spawned);
        finish.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        if (!ue_wrap::Call(gsCdo, finish)) {
            DestroyMirrorActor(spawned, R::InternalIndexOf(spawned));
            return;
        }
    }
    // PARK the mirror: no AI (actor + CMC ticks off -> no isLooking/anger/dash
    // loop). Collision was already dropped pre-Finish (F-4).
    ue_wrap::puppet::DisableCharacterTicks(spawned);
    g_mirrors[key] = Mirror{spawned, R::InternalIndexOf(spawned)};
    UE_LOGI("owner_entity: mirror '%ls' materialized for slot=%d seq=%u at (%.0f,%.0f,%.0f) "
            "(brain-parked, collision-off)",
            kOwnerEntityClasses[p.classId], senderPeerSlot, p.seq, p.x, p.y, p.z);
}

void OnPoseMsg(const coop::net::OwnerEntityPosePayload& p, int senderPeerSlot) {
    if (!GT::IsGameThread()) return;
    if (!FiniteVec(p.x, p.y, p.z, p.yaw)) return;  // wire NaN/Inf guard (audit F-8)
    auto it = g_mirrors.find(Key(senderPeerSlot, p.seq));
    if (it == g_mirrors.end()) return;  // unknown: the keepalive Spawn re-delivers
    if (!R::IsLiveByIndex(it->second.actor, it->second.idx)) { g_mirrors.erase(it); return; }
    E::SetActorLocation(it->second.actor, {p.x, p.y, p.z});
    E::SetActorRotation(it->second.actor, {0.f, p.yaw, 0.f});
}

void OnDestroyMsg(const coop::net::OwnerEntityDestroyPayload& p, int senderPeerSlot) {
    if (!GT::IsGameThread()) return;
    // HOST-fanned leaver teardown (audit F-1): a client has no transport edge
    // for ANOTHER client's slot, so the host re-keys the teardown via
    // originSlot. Only the HOST (transport slot 0) may speak for another slot.
    int slot = senderPeerSlot;
    if (p.originSlot != 0 && senderPeerSlot == 0) slot = p.originSlot;
    if (p.seq == 0) {  // wildcard: every entity of that slot (the leaver teardown)
        OnPeerLeftSlot(slot);
        return;
    }
    auto it = g_mirrors.find(Key(slot, p.seq));
    if (it == g_mirrors.end()) return;
    DestroyMirrorActor(it->second.actor, it->second.idx);
    g_mirrors.erase(it);
    UE_LOGI("owner_entity: mirror slot=%d seq=%u destroyed (owner announced death)",
            slot, p.seq);
}

void OnPeerLeftSlot(int slot) {
    if (!GT::IsGameThread()) return;
    if (slot <= 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    int destroyed = 0;
    for (auto it = g_mirrors.begin(); it != g_mirrors.end();) {
        if (static_cast<int>(it->first >> 16) == slot) {
            DestroyMirrorActor(it->second.actor, it->second.idx);
            it = g_mirrors.erase(it);
            ++destroyed;
        } else {
            ++it;
        }
    }
    if (destroyed > 0)
        UE_LOGI("owner_entity: destroyed %d mirror(s) of departed slot %d", destroyed, slot);
    // HOST: the other clients have no transport edge for this slot -- fan the
    // teardown out on the leaver's behalf (wildcard seq=0 + originSlot).
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && s->connected() && s->role() == coop::net::Role::Host) {
        coop::net::OwnerEntityDestroyPayload d{};
        d.seq = 0;
        d.originSlot = static_cast<uint8_t>(slot);
        s->SendReliable(coop::net::ReliableKind::OwnerEntityDestroy, &d, sizeof(d));
    }
}

void OnDisconnect() {
    // Mirrors are OUR spawned actors -- they must not linger into SP. The
    // fanout runs on the game thread (net_pump teardown).
    if (GT::IsGameThread()) {
        for (auto& kv : g_mirrors) DestroyMirrorActor(kv.second.actor, kv.second.idx);
    }
    g_mirrors.clear();
    g_owned.clear();
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::owner_entity_sync
