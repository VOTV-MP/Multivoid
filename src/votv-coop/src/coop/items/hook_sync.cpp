// coop/items/hook_sync.cpp -- Grappling hook and rope visual synchronization.

#include "coop/items/hook_sync.h"

#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/actors/hook.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace coop::hook_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

coop::net::Session* g_session = nullptr;
std::atomic<bool>   g_seamsInstalled{false};
void*               g_finishSpawnFn = nullptr;
void*               g_destroyFn     = nullptr;

thread_local bool   t_spawningMirror = false;
// DestroyActor synchronously runs the K2_DestroyActor post seam.  Calls made
// by this subsystem must not let that seam erase a map entry an outer caller
// is about to erase itself.
thread_local bool   t_destroyingMirror = false;
uint16_t            g_nextNetId      = 0;

inline float DistSq(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    float dx = a.X - b.X;
    float dy = a.Y - b.Y;
    float dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

inline double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct LocalHook {
    void* actor = nullptr;
    int32_t internalIdx = -1;
    uint16_t netId = 0;
    ue_wrap::hook::HookSnapshot lastSnap{};
    double lastSendTime = 0.0;
};

struct MirrorHook {
    uint8_t ownerSlot = 0;
    uint16_t hookNetId = 0;
    uint8_t classType = 0;
    void* mirrorActor = nullptr;
    int32_t internalIdx = -1;
    ue_wrap::hook::HookSnapshot currentSnap{};
    ue_wrap::hook::HookSnapshot targetSnap{};
};

std::unordered_map<void*, LocalHook> g_localHooks;
std::unordered_map<uint32_t, MirrorHook> g_mirrorHooks; // key: (ownerSlot << 16) | netId

inline uint32_t MakeMirrorKey(uint8_t ownerSlot, uint16_t netId) {
    return (static_cast<uint32_t>(ownerSlot) << 16) | static_cast<uint32_t>(netId);
}

void DestroyMirrorActor(void* actor) {
    if (!actor) return;
    t_destroyingMirror = true;
    ue_wrap::hook::DestroyHookMirror(actor);
    t_destroyingMirror = false;
}

void PackPayload(coop::net::HookSyncPayload& p, coop::net::HookSyncOp op, uint8_t ownerSlot,
                 uint16_t netId, const ue_wrap::hook::HookSnapshot& s) {
    p.op          = static_cast<uint8_t>(op);
    p.ownerSlot   = ownerSlot;
    p.hookNetId   = netId;
    p.classType   = s.classType;
    p.flags       = 0;
    if (s.attachedA)    p.flags |= coop::net::HookFlag::AttachedA;
    if (s.attachedB)    p.flags |= coop::net::HookFlag::AttachedB;
    if (s.playerHooked) p.flags |= coop::net::HookFlag::PlayerHooked;
    if (s.isThrown)     p.flags |= coop::net::HookFlag::IsThrown;
    p.cableLength = s.cableLength;
    p.posAx       = s.posA.X;
    p.posAy       = s.posA.Y;
    p.posAz       = s.posA.Z;
    p.rotAPitch   = s.rotA.Pitch;
    p.rotAYaw     = s.rotA.Yaw;
    p.rotARoll    = s.rotA.Roll;
    p.posBx       = s.posB.X;
    p.posBy       = s.posB.Y;
    p.posBz       = s.posB.Z;
    p.rotBPitch   = s.rotB.Pitch;
    p.rotBYaw     = s.rotB.Yaw;
    p.rotBRoll    = s.rotB.Roll;
}

void UnpackPayload(const coop::net::HookSyncPayload& p, ue_wrap::hook::HookSnapshot& s) {
    s.valid        = true;
    s.classType    = p.classType;
    s.attachedA    = (p.flags & coop::net::HookFlag::AttachedA) != 0;
    s.attachedB    = (p.flags & coop::net::HookFlag::AttachedB) != 0;
    s.playerHooked = (p.flags & coop::net::HookFlag::PlayerHooked) != 0;
    s.isThrown     = (p.flags & coop::net::HookFlag::IsThrown) != 0;
    s.cableLength  = p.cableLength;
    s.posA         = ue_wrap::FVector{p.posAx, p.posAy, p.posAz};
    s.rotA         = ue_wrap::FRotator{p.rotAPitch, p.rotAYaw, p.rotARoll};
    s.posB         = ue_wrap::FVector{p.posBx, p.posBy, p.posBz};
    s.rotB         = ue_wrap::FRotator{p.rotBPitch, p.rotBYaw, p.rotBRoll};
}

void BroadcastHook(coop::net::HookSyncOp op, uint16_t netId, const ue_wrap::hook::HookSnapshot& s) {
    if (!g_session || !g_session->connected()) return;
    uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
    if (localSlot == coop::players::kPeerIdUnknown) localSlot = 0;

    coop::net::HookSyncPayload payload{};
    PackPayload(payload, op, localSlot, netId, s);
    g_session->SendReliable(coop::net::ReliableKind::HookSync, &payload, sizeof(payload));
}

void OnFinishSpawnPost(void* /*context*/, void* /*sourceObject*/, void* spawnedResult) {
    if (!spawnedResult || t_spawningMirror) return;
    if (!ue_wrap::hook::EnsureResolved() || !ue_wrap::hook::IsHook(spawnedResult)) return;

    if (g_localHooks.find(spawnedResult) != g_localHooks.end()) return;

    LocalHook lh{};
    lh.actor = spawnedResult;
    lh.internalIdx = R::InternalIndexOf(spawnedResult);
    lh.netId = ++g_nextNetId;
    if (!ue_wrap::hook::ReadHookSnapshot(spawnedResult, lh.lastSnap)) return;
    lh.lastSendTime = NowSeconds();

    g_localHooks[spawnedResult] = lh;
    UE_LOGI("hook_sync: spawned local hook %p (netId=%u, classType=%u)",
            spawnedResult, lh.netId, lh.lastSnap.classType);

    BroadcastHook(coop::net::HookSyncOp::Spawn, lh.netId, lh.lastSnap);
}

void OnK2DestroyPost(void* context, void* /*sourceObject*/, void* /*result*/) {
    if (!context) return;

    auto it = g_localHooks.find(context);
    if (it != g_localHooks.end()) {
        UE_LOGI("hook_sync: destroyed local hook %p (netId=%u)", context, it->second.netId);
        BroadcastHook(coop::net::HookSyncOp::Destroy, it->second.netId, it->second.lastSnap);
        g_localHooks.erase(it);
        return;
    }

    // An explicit mirror cleanup owns its map erase.  K2_DestroyActor is
    // synchronous, so erasing here as well invalidates the outer iterator.
    if (t_destroyingMirror) return;

    // Also check if the engine independently reaped one of our mirrors.
    for (auto mit = g_mirrorHooks.begin(); mit != g_mirrorHooks.end(); ++mit) {
        if (mit->second.mirrorActor == context) {
            g_mirrorHooks.erase(mit);
            break;
        }
    }
}

void EnsureHooksInstalled() {
    if (g_seamsInstalled.load(std::memory_order_acquire)) return;

    if (!g_finishSpawnFn) {
        if (void* gsCls = R::FindClass(P::name::GameplayStaticsClass)) {
            g_finishSpawnFn = R::FindFunction(gsCls, P::name::FinishSpawningActorFn);
        }
    }
    if (!g_destroyFn) {
        if (void* actorCls = R::FindClass(P::name::ActorClassName)) {
            g_destroyFn = R::FindFunction(actorCls, P::name::DestroyActorFn);
        }
    }

    if (!g_finishSpawnFn || !g_destroyFn) return;

    ue_wrap::ufunction_hook::InstallPostHook(g_finishSpawnFn, &OnFinishSpawnPost);
    ue_wrap::ufunction_hook::InstallPostHook(g_destroyFn, &OnK2DestroyPost);
    g_seamsInstalled.store(true, std::memory_order_release);
    UE_LOGI("hook_sync: installed FinishSpawningActor and K2_DestroyActor seams");
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session = session;
    ue_wrap::hook::EnsureResolved();
    EnsureHooksInstalled();
}

void Tick() {
    EnsureHooksInstalled();

    const double now = NowSeconds();
    uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
    if (localSlot == coop::players::kPeerIdUnknown) localSlot = 0;

    // 1. Poll and clean up dead local hooks
    std::vector<void*> deadLocal;
    for (auto& [actor, lh] : g_localHooks) {
        if (!actor || !R::IsLiveByIndex(actor, lh.internalIdx)) {
            deadLocal.push_back(actor);
            continue;
        }

        ue_wrap::hook::HookSnapshot cur{};
        if (!ue_wrap::hook::ReadHookSnapshot(actor, cur)) continue;

        // Check if movement or state change warrants an update packet
        const bool stateChanged = (cur.attachedA != lh.lastSnap.attachedA ||
                                   cur.attachedB != lh.lastSnap.attachedB ||
                                   cur.playerHooked != lh.lastSnap.playerHooked ||
                                   cur.isThrown != lh.lastSnap.isThrown);

        const float distA = DistSq(cur.posA, lh.lastSnap.posA);
        const float distB = DistSq(cur.posB, lh.lastSnap.posB);
        const bool moved = (distA > 4.f || distB > 4.f);
        const bool lengthChanged = (std::fabs(cur.cableLength - lh.lastSnap.cableLength) > 4.f);

        // Throttle: 30 Hz for flying/moving hooks (dt >= 0.033s), 1 Hz heartbeat for static
        const double minInterval = (cur.isThrown || cur.playerHooked || moved) ? 0.033 : 1.0;

        if (stateChanged || ((moved || lengthChanged) && (now - lh.lastSendTime >= minInterval))) {
            lh.lastSnap = cur;
            lh.lastSendTime = now;
            BroadcastHook(coop::net::HookSyncOp::Update, lh.netId, cur);
        }
    }

    for (void* actor : deadLocal) {
        auto it = g_localHooks.find(actor);
        if (it != g_localHooks.end()) {
            BroadcastHook(coop::net::HookSyncOp::Destroy, it->second.netId, it->second.lastSnap);
            g_localHooks.erase(it);
        }
    }

    // 2. Interpolate and apply mirror hooks
    std::vector<uint32_t> deadMirrors;
    for (auto& [key, mh] : g_mirrorHooks) {
        if (!mh.mirrorActor || !R::IsLiveByIndex(mh.mirrorActor, mh.internalIdx)) {
            deadMirrors.push_back(key);
            continue;
        }

        // Interpolate Head A
        float dAx = mh.targetSnap.posA.X - mh.currentSnap.posA.X;
        float dAy = mh.targetSnap.posA.Y - mh.currentSnap.posA.Y;
        float dAz = mh.targetSnap.posA.Z - mh.currentSnap.posA.Z;
        if (dAx * dAx + dAy * dAy + dAz * dAz > 10000.f) {
            mh.currentSnap.posA = mh.targetSnap.posA;
        } else {
            mh.currentSnap.posA.X += dAx * 0.4f;
            mh.currentSnap.posA.Y += dAy * 0.4f;
            mh.currentSnap.posA.Z += dAz * 0.4f;
        }
        mh.currentSnap.rotA = mh.targetSnap.rotA;

        // Interpolate Head B
        float dBx = mh.targetSnap.posB.X - mh.currentSnap.posB.X;
        float dBy = mh.targetSnap.posB.Y - mh.currentSnap.posB.Y;
        float dBz = mh.targetSnap.posB.Z - mh.currentSnap.posB.Z;
        if (dBx * dBx + dBy * dBy + dBz * dBz > 10000.f) {
            mh.currentSnap.posB = mh.targetSnap.posB;
        } else {
            mh.currentSnap.posB.X += dBx * 0.4f;
            mh.currentSnap.posB.Y += dBy * 0.4f;
            mh.currentSnap.posB.Z += dBz * 0.4f;
        }
        mh.currentSnap.rotB = mh.targetSnap.rotB;

        mh.currentSnap.cableLength  = mh.targetSnap.cableLength;
        mh.currentSnap.attachedA    = mh.targetSnap.attachedA;
        mh.currentSnap.attachedB    = mh.targetSnap.attachedB;
        mh.currentSnap.playerHooked = mh.targetSnap.playerHooked;
        mh.currentSnap.isThrown     = mh.targetSnap.isThrown;

        // Find the remote player's puppet actor to anchor Head B if playerHooked is true
        void* puppetActor = nullptr;
        if (coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(mh.ownerSlot)) {
            puppetActor = rp->actor();
        }

        ue_wrap::hook::ApplyMirrorSnapshot(mh.mirrorActor, mh.currentSnap, puppetActor);
    }

    for (uint32_t k : deadMirrors) {
        g_mirrorHooks.erase(k);
    }
}

void OnHookSync(const coop::net::HookSyncPayload& payload, uint8_t senderSlot) {
    uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
    const bool hasLocalSlot = localSlot != coop::players::kPeerIdUnknown;

    // A client receives its own packets back from the host relay, whose transport
    // sender is the host rather than the original owner.  Owner identity is what
    // matters here; checking senderSlot as well created a duplicate local mirror.
    if (hasLocalSlot && payload.ownerSlot == localSlot) return;

    if (payload.ownerSlot >= coop::net::kMaxPeers ||
        payload.classType > static_cast<uint8_t>(coop::net::HookClassType::HookFlesh) ||
        payload.flags & ~static_cast<uint8_t>(coop::net::HookFlag::AttachedA |
                                              coop::net::HookFlag::AttachedB |
                                              coop::net::HookFlag::PlayerHooked |
                                              coop::net::HookFlag::IsThrown)) {
        UE_LOGW("hook_sync: dropped malformed packet (sender=%u owner=%u op=%u class=%u flags=0x%02X)",
                senderSlot, payload.ownerSlot, payload.op, payload.classType, payload.flags);
        return;
    }

    const uint32_t key = MakeMirrorKey(payload.ownerSlot, payload.hookNetId);
    const auto op = static_cast<coop::net::HookSyncOp>(payload.op);
    if (op != coop::net::HookSyncOp::Spawn &&
        op != coop::net::HookSyncOp::Update &&
        op != coop::net::HookSyncOp::Destroy) {
        UE_LOGW("hook_sync: dropped packet with invalid operation %u from slot %u",
                payload.op, senderSlot);
        return;
    }

    ue_wrap::hook::HookSnapshot snap{};
    UnpackPayload(payload, snap);

    if (op == coop::net::HookSyncOp::Spawn) {
        auto it = g_mirrorHooks.find(key);
        if (it != g_mirrorHooks.end()) {
            DestroyMirrorActor(it->second.mirrorActor);
            g_mirrorHooks.erase(it);
        }

        t_spawningMirror = true;
        void* mirror = ue_wrap::hook::SpawnHookMirror(payload.classType, snap);
        t_spawningMirror = false;

        if (mirror) {
            MirrorHook mh{};
            mh.ownerSlot = payload.ownerSlot;
            mh.hookNetId = payload.hookNetId;
            mh.classType = payload.classType;
            mh.mirrorActor = mirror;
            mh.internalIdx = R::InternalIndexOf(mirror);
            mh.currentSnap = snap;
            mh.targetSnap = snap;
            g_mirrorHooks[key] = mh;
            UE_LOGI("hook_sync: spawned remote hook mirror key=0x%08X (owner=%u, netId=%u, classType=%u)",
                    key, payload.ownerSlot, payload.hookNetId, payload.classType);
        }
    } else if (op == coop::net::HookSyncOp::Update) {
        auto it = g_mirrorHooks.find(key);
        if (it != g_mirrorHooks.end()) {
            it->second.targetSnap = snap;
        } else {
            // Missed spawn packet -- spawn mirror on first update
            t_spawningMirror = true;
            void* mirror = ue_wrap::hook::SpawnHookMirror(payload.classType, snap);
            t_spawningMirror = false;

            if (mirror) {
                MirrorHook mh{};
                mh.ownerSlot = payload.ownerSlot;
                mh.hookNetId = payload.hookNetId;
                mh.classType = payload.classType;
                mh.mirrorActor = mirror;
                mh.internalIdx = R::InternalIndexOf(mirror);
                mh.currentSnap = snap;
                mh.targetSnap = snap;
                g_mirrorHooks[key] = mh;
            }
        }
    } else if (op == coop::net::HookSyncOp::Destroy) {
        auto it = g_mirrorHooks.find(key);
        if (it != g_mirrorHooks.end()) {
            UE_LOGI("hook_sync: destroyed remote hook mirror key=0x%08X", key);
            DestroyMirrorActor(it->second.mirrorActor);
            g_mirrorHooks.erase(it);
        }
    }
}

void QueueConnectReplayForSlot(int slot) {
    if (!g_session || !g_session->connected()) return;

    uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
    if (localSlot == coop::players::kPeerIdUnknown) localSlot = 0;

    // Replay local hooks
    for (auto& [actor, lh] : g_localHooks) {
        ue_wrap::hook::HookSnapshot snap{};
        if (ue_wrap::hook::ReadHookSnapshot(actor, snap)) {
            coop::net::HookSyncPayload payload{};
            PackPayload(payload, coop::net::HookSyncOp::Spawn, localSlot, lh.netId, snap);
            g_session->SendReliableToSlot(slot, coop::net::ReliableKind::HookSync, &payload, sizeof(payload));
        }
    }

    // Replay remote mirror hooks known to this peer
    for (auto& [key, mh] : g_mirrorHooks) {
        coop::net::HookSyncPayload payload{};
        PackPayload(payload, coop::net::HookSyncOp::Spawn, mh.ownerSlot, mh.hookNetId, mh.targetSnap);
        g_session->SendReliableToSlot(slot, coop::net::ReliableKind::HookSync, &payload, sizeof(payload));
    }
}

void OnDisconnect() {
    for (auto& [key, mh] : g_mirrorHooks) {
        DestroyMirrorActor(mh.mirrorActor);
    }
    g_mirrorHooks.clear();
    g_localHooks.clear();
}

}  // namespace coop::hook_sync
