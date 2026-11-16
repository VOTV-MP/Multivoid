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
#include "coop/element/class_load_wait.h"
#include "ue_wrap/core/class_resolve.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/actors/puppet.h"                  // DisableCharacterTicks (the npc-mirror park)
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace coop::owner_entity_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace P  = ue_wrap::profile;

UE_FACT_FN(kRowBeginDeferredSpawn, "owner_entity_sync", L"GameplayStatics", L"BeginDeferredActorSpawnFromClass",
           Declared);

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- the member-class table (classId = index). Extend here for the next
// stalker-entity member; every row is per-peer-owned + cross-peer mirrored.
constexpr const wchar_t* kOwnerEntityClasses[] = {
    L"eyer_C",   // classId 0: the night "Eyes" stalker (ticker_eyers)
    // classId 1: the Shadow Figura (mainGamemode.CustomEvent_3). Bound to ONE player: it spawns on a 200 m ring about that
    // peer's pawn, its `target` is that peer's camera, and it walks to the camera and ends when within 1.75 m of it (its 0.1 s
    // `CustomEvent_0` timer), so it stays per peer and the others are shown a parked copy.
    L"figura_C",
};
constexpr size_t kOwnerEntityClassCount =
    sizeof(kOwnerEntityClasses) / sizeof(kOwnerEntityClasses[0]);

// Each row's class is judged by its name, which lives for the process, so the observer can arm before
// the class loads and no class pointer is held. Resolved by Install before the observer arms, read by
// the POST observer, both on the game thread. The receiver looks the class up where it spawns.
R::FName g_classNames[kOwnerEntityClassCount] = {};

int ClassIdOf(void* cls) {
    if (!cls) return -1;
    const R::FName& name = R::NameOf(cls);
    for (size_t i = 0; i < kOwnerEntityClassCount; ++i)
        if (name.ComparisonIndex == g_classNames[i].ComparisonIndex && name.Number == g_classNames[i].Number)
            return static_cast<int>(i);
    return -1;
}

// ---- BeginDeferred seam (own resolve; host_spawn_watcher pattern) ----
ue_wrap::facts::Resolved g_beginDeferredFn;
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
    long long bornMs = 0;
    bool    lateCleared = false;  // the timer its begin play arms a second in was cleared too
    uint8_t classId = 0;
};
std::unordered_map<uint32_t, Mirror> g_mirrors;  // key = (slot << 16) | seq

uint32_t Key(int slot, uint16_t seq) {
    return (static_cast<uint32_t>(slot) << 16) | seq;
}

// Inverse of Key's slot half -- used by the W5 receive-side per-sender mirror cap.
int SlotOfKey(uint32_t key) { return static_cast<int>(key >> 16); }

float YawOfQuat(float qx, float qy, float qz, float qw) {
    constexpr float kRadToDeg = 57.29577951308232f;
    return std::atan2(2.f * (qw * qz + qx * qy), 1.f - 2.f * (qy * qy + qz * qz)) * kRadToDeg;
}

// A mirror ends when its owner's eyer does. The class arms its own ends on timers: `despawn` (not on screen for a
// moment), `del` (the viewer came near, or day broke; armed a second after the begin play) and `addAnger` (the stare).
// K2_ClearTimer(Object, FunctionName) is the inverse of the graph's K2_SetTimerDelegate; the blink is left running.
// The Figura arms one: the 0.1 s `CustomEvent_0`, which reads the LOCAL camera and destroys the actor within 1.75 m of it.
void ClearMirrorTimers(void* actor, uint8_t classId) {
    static void* sCdo = nullptr;
    static void* sFn = nullptr;
    if (!sCdo) sCdo = R::FindClassDefaultObject(L"KismetSystemLibrary");
    if (sCdo && !sFn) sFn = R::FindFunction(R::ClassOf(sCdo), L"K2_ClearTimer");
    if (!sCdo || !sFn) return;
    static const wchar_t* const kEyerTimers[] = {L"despawn", L"del", L"addAnger"};
    static const wchar_t* const kFiguraTimers[] = {L"CustomEvent_0"};
    const wchar_t* const* names = classId == 1 ? kFiguraTimers : kEyerTimers;
    const size_t nameCount = classId == 1 ? sizeof(kFiguraTimers) / sizeof(kFiguraTimers[0]) : sizeof(kEyerTimers) / sizeof(kEyerTimers[0]);
    for (size_t ni = 0; ni < nameCount; ++ni) {
        const wchar_t* name = names[ni];
        ue_wrap::ParamFrame f(sFn);
        if (!f.valid()) continue;
        f.Set<void*>(L"Object", actor);
        const int32_t num = static_cast<int32_t>(std::wcslen(name)) + 1;
        struct { const wchar_t* data; int32_t n; int32_t m; } fs{name, num, num};
        if (f.SetRaw(L"FunctionName", &fs, sizeof(fs))) ue_wrap::Call(sCdo, f);
    }
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
    const int classId = ClassIdOf(actorClass);   // two integer compares a row
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
    // param -- ue_wrap::FTransform IS the engine's layout, so the cast is exact.
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

// A member spawned from inside a Blueprint graph (the black fog's ghosts, the night ticker's eyer: an EX_CallMath
// BeginDeferred, which no ProcessEvent observer sees). Caught at the native seam, where the actor is not placed yet,
// so it waits here for the next tick to be read and announced.
struct PendingOwn {
    void*   actor;
    int32_t idx;
};
std::vector<PendingOwn> g_pendingOwn;

void OnSpawnNative(void* /*context*/, void* /*source*/, void* spawned) {
    if (!spawned || !GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (coop::prop_echo_suppress::InMirrorSpawnScope()) return;  // our own mirror spawn
    if (ClassIdOf(R::ClassOf(spawned)) < 0) return;
    if (g_pendingOwn.size() >= kMaxOwned) return;
    g_pendingOwn.push_back({spawned, R::InternalIndexOf(spawned)});
}

// The tick's first step: each waiting member that is still alive and not owned yet (the ProcessEvent observer owns a
// spawn that came its way) is read where its spawn finished and announced.
void AdoptPendingOwn(coop::net::Session* s, long long now) {
    if (g_pendingOwn.empty()) return;
    std::vector<PendingOwn> pending;
    pending.swap(g_pendingOwn);
    for (const PendingOwn& pe : pending) {
        if (!R::IsLiveByIndex(pe.actor, pe.idx)) continue;
        bool known = false;
        for (const Owned& o : g_owned) known = known || o.actor == pe.actor;
        const int classId = known ? -1 : ClassIdOf(R::ClassOf(pe.actor));
        if (classId < 0) continue;
        if (g_owned.size() >= kMaxOwned) {
            UE_LOGW("owner_entity: owned cap %zu hit -- NOT broadcasting this '%ls' spawn", kMaxOwned,
                    kOwnerEntityClasses[classId]);
            continue;
        }
        ue_wrap::FVector loc{};
        ue_wrap::FRotator rot{};
        if (!E::TryGetActorLocation(pe.actor, loc)) continue;
        E::TryGetActorRotation(pe.actor, rot);
        Owned o{};
        o.seq = g_nextSeq++;
        if (g_nextSeq == 0) g_nextSeq = 1;
        o.classId = static_cast<uint8_t>(classId);
        o.actor = pe.actor;
        o.idx = pe.idx;
        o.x = loc.X; o.y = loc.Y; o.z = loc.Z;
        o.yaw = rot.Yaw;
        o.lastAnnounceMs = now;
        g_owned.push_back(o);
        coop::net::OwnerEntitySpawnPayload p{};
        p.seq = o.seq; p.classId = o.classId;
        p.x = o.x; p.y = o.y; p.z = o.z; p.yaw = o.yaw;
        const bool sent = s->SendReliable(coop::net::ReliableKind::OwnerEntitySpawn, &p, sizeof(p));
        UE_LOGI("owner_entity: OWN '%ls' spawned from a graph seq=%u at (%.0f,%.0f,%.0f) -- %s",
                kOwnerEntityClasses[classId], o.seq, o.x, o.y, o.z,
                sent ? "announced" : "announce send FAILED (the keepalive re-announces)");
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

    // Native classes, in the index from boot: the observer arms on the first call.
    if (!g_beginDeferredFn) {
        void* gsCls = ue_wrap::object_index::ClassByName(P::name::GameplayStaticsClass);
        if (!gsCls) return;
        g_beginDeferredFn = ue_wrap::facts::ResolveOn(kRowBeginDeferredSpawn, gsCls);
        if (!g_beginDeferredFn) {
            UE_LOGW("owner_entity: BeginDeferred not found -- lane disabled for process lifetime");
            g_observerArmed = true;  // give up
            return;
        }
        g_classParamOff  = R::FindParamOffset(g_beginDeferredFn.Fn(), L"ActorClass");
        g_returnParamOff = R::FindParamOffset(g_beginDeferredFn.Fn(), L"ReturnValue");
        g_xformParamOff  = R::FindParamOffset(g_beginDeferredFn.Fn(), L"SpawnTransform");
        if (g_classParamOff < 0 || g_returnParamOff < 0 || g_xformParamOff < 0) {
            UE_LOGW("owner_entity: BeginDeferred param offsets missing -- lane disabled");
            g_observerArmed = true;
            return;
        }
    }
    if (!g_k2DestroyFn) {
        void* actorCls = ue_wrap::object_index::ClassByName(P::name::ActorClassName);
        if (!actorCls) return;
        g_k2DestroyFn = R::FindFunction(actorCls, L"K2_DestroyActor");
        if (!g_k2DestroyFn) {
            UE_LOGW("owner_entity: Actor has no K2_DestroyActor -- lane disabled");
            g_observerArmed = true;
            return;
        }
    }
    for (size_t i = 0; i < kOwnerEntityClassCount; ++i) {
        g_classNames[i] = ue_wrap::fname_utils::StringToFName(kOwnerEntityClasses[i]);
        if (g_classNames[i].ComparisonIndex == 0) {
            UE_LOGW("owner_entity: the name '%ls' did not convert -- lane disabled", kOwnerEntityClasses[i]);
            g_observerArmed = true;
            return;
        }
    }

    if (!GT::RegisterPostObserver(g_beginDeferredFn, &OnSpawnPost)) {
        UE_LOGE("owner_entity: RegisterPostObserver FAILED (table full?) -- lane disabled");
        g_observerArmed = true;
        return;
    }
    g_observerArmed = true;
    const bool native = ue_wrap::ufunction_hook::InstallPostHook(g_beginDeferredFn, &OnSpawnNative);
    if (!native)
        UE_LOGW("owner_entity: the native spawn seam did not install -- a member a Blueprint graph spawns (the black "
                "fog's eyes, the night's) stays on the peer that rolled it");
    UE_LOGI("owner_entity: armed (%zu member class(es); eyer_C classId=0, figura_C classId=1) -- per-peer owned, "
            "cross-peer mirrored; graph spawns %s", kOwnerEntityClassCount, native ? "seen" : "NOT seen");
}

void Tick() {
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    static long long sLastMs = 0;
    const long long now = NowMs();
    if (now - sLastMs < kTickPeriodMs) return;
    sLastMs = now;

    AdoptPendingOwn(s, now);
    // OWNER: death-watch + keepalive re-announce + pose stream.
    for (size_t i = 0; i < g_owned.size();) {
        Owned& o = g_owned[i];
        if (!R::IsLiveByIndex(o.actor, o.idx)) {
            coop::net::OwnerEntityDestroyPayload d{};
            d.seq = o.seq;
            if (!s->SendReliable(coop::net::ReliableKind::OwnerEntityDestroy, &d, sizeof(d))) {
                ++i;  // send failed (channel exhaustion): keep the entry, retry
                continue;  // next tick -- a lost death is a permanent ghost
            }
            UE_LOGI("owner_entity: OWN seq=%u died locally -- destroy announced", o.seq);
            g_owned.erase(g_owned.begin() + i);
            continue;
        }
        ue_wrap::FVector loc{};
        const bool locRead = E::TryGetActorLocation(o.actor, loc);
        ue_wrap::FRotator rot{};   // unread: the stored yaw, the last real one
        const float yaw = locRead && E::TryGetActorRotation(o.actor, rot) ? rot.Yaw : o.yaw;
        // Keepalive FIRST: a continuously-moving entity must not
        // starve the Spawn re-announce -- it IS the late-joiner delivery, and
        // it carries the pose anyway.
        // An unread tick announces the stored pose, the last real one.
        if (now - o.lastAnnounceMs >= kKeepaliveMs) {
            if (locRead) { o.x = loc.X; o.y = loc.Y; o.z = loc.Z; o.yaw = yaw; }
            AnnounceOwned(s, o, now);
            ++i;
            continue;
        }
        if (!locRead) { ++i; continue; }   // no pose update this tick
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
        if (!R::IsLiveByIndex(it->second.actor, it->second.idx)) { it = g_mirrors.erase(it); continue; }
        if (!it->second.lateCleared && now - it->second.bornMs >= 1500) {
            it->second.lateCleared = true;
            ClearMirrorTimers(it->second.actor, it->second.classId);
        }
        ++it;
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
    if (!FiniteVec(p.x, p.y, p.z, p.yaw)) return;  // wire NaN/Inf guard
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
    // SECURITY: kMaxOwned bounds the SEND side (g_owned above) --
    // it constrains OUR sender, not a hostile peer's. The receive path spawned a REAL actor for
    // every unseen (slot, seq), so one peer could walk seq and force 65 536 spawns; and this kind is
    // on the relay whitelist, so it froze every client too. Enforce the same intent where the actor
    // is actually created. Counted from the map rather than a side counter: g_mirrors is erased on
    // several paths, and a parallel counter would drift out of step with it.
    {
        size_t fromSender = 0;
        for (const auto& kv : g_mirrors)
            if (SlotOfKey(kv.first) == senderPeerSlot) ++fromSender;
        if (fromSender >= kMaxOwned) {
            UE_LOGW("owner_entity: slot %d already has %zu mirrors (cap %zu) -- refusing spawn "
                    "seq=%u", senderPeerSlot, fromSender, kMaxOwned, p.seq);
            return;
        }
    }
    // The class comes from this peer's own id table, not from a wire name: the table is the bound. A class this
    // peer has not loaded loads inside the per-tick budget; past it the spawn is parked and runs next tick.
    const auto resolved = ue_wrap::class_resolve::ResolveClass(kOwnerEntityClasses[p.classId],
                                                              ue_wrap::class_resolve::AnyFamily(),
                                                              ue_wrap::class_resolve::Load::Budgeted);
    if (resolved.state == ue_wrap::class_resolve::ClassState::Pending) {
        // No element id here, so no bind_wait mark: the wait is cancelled by the (slot, seq) the destroy carries.
        coop::element::class_load_wait::Park("OwnerEntitySpawn", kOwnerEntityClasses[p.classId], key,
                                             [p, senderPeerSlot] { OnSpawnMsg(p, senderPeerSlot); });
        return;
    }
    void* cls = resolved.cls;
    if (!cls) return;  // absent here: said once by the resolver, and the sender's keepalive re-delivery is refused alike
    // Resolve-once statics (GT-only path): the GameplayStatics CDO + Finish fn.
    static void* s_gsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    static void* s_finishFn = [] {
        void* gsCls = ue_wrap::object_index::ClassByName(P::name::GameplayStaticsClass);
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
        ue_wrap::ParamFrame begin(g_beginDeferredFn.Fn());
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
        // Collision OFF in the DEFERRED window: BeginPlay and the
        // initial overlap update run INSIDE FinishSpawningActor -- a viewer
        // standing at the wire transform must not be overlap-killed by the
        // killsphere during that one frame.
        E::SetActorEnableCollision(spawned, false);
        // A mirror lives as long as its owner's eyer does: the class's own daylight end (its `del` timer) is the
        // owner's to run, and the black fog's eyes are born by day.
        {
            int32_t off = -1;
            uint8_t mask = 0;
            if (R::FindBoolProperty(cls, L"ignoreDaySpawn", off, mask) && off >= 0)
                *(static_cast<uint8_t*>(spawned) + off) |= mask;
        }
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
    ClearMirrorTimers(spawned, p.classId);
    g_mirrors[key] = Mirror{spawned, R::InternalIndexOf(spawned), NowMs(), false, p.classId};
    UE_LOGI("owner_entity: mirror '%ls' materialized for slot=%d seq=%u at (%.0f,%.0f,%.0f) "
            "(brain-parked, collision-off)",
            kOwnerEntityClasses[p.classId], senderPeerSlot, p.seq, p.x, p.y, p.z);
}

void OnPoseMsg(const coop::net::OwnerEntityPosePayload& p, int senderPeerSlot) {
    if (!GT::IsGameThread()) return;
    if (!FiniteVec(p.x, p.y, p.z, p.yaw)) return;  // wire NaN/Inf guard
    auto it = g_mirrors.find(Key(senderPeerSlot, p.seq));
    if (it == g_mirrors.end()) return;  // unknown: the keepalive Spawn re-delivers
    if (!R::IsLiveByIndex(it->second.actor, it->second.idx)) { g_mirrors.erase(it); return; }
    E::SetActorLocation(it->second.actor, {p.x, p.y, p.z});
    E::SetActorRotation(it->second.actor, {0.f, p.yaw, 0.f});
}

void OnDestroyMsg(const coop::net::OwnerEntityDestroyPayload& p, int senderPeerSlot) {
    if (!GT::IsGameThread()) return;
    // HOST-fanned leaver teardown: a client has no transport edge
    // for ANOTHER client's slot, so the host re-keys the teardown via
    // originSlot. Only the HOST (transport slot 0) may speak for another slot.
    int slot = senderPeerSlot;
    if (p.originSlot != 0 && senderPeerSlot == 0) slot = p.originSlot;
    if (p.seq == 0) {  // wildcard: every entity of that slot (the leaver teardown)
        OnPeerLeftSlot(slot);
        return;
    }
    // A spawn still waiting on its class's load under this (slot, seq) ends here: it is never spawned. This lane
    // has no element id, so no BIND-WAIT mark: a pose for the entity finds no mirror and drops, as before.
    if (coop::element::class_load_wait::Cancel("OwnerEntitySpawn", Key(slot, p.seq))) {
        UE_LOGI("owner_entity: slot=%d seq=%u -- its spawn was waiting on a class load and is cancelled; it will "
                "never spawn", slot, p.seq);
        return;
    }
    auto it = g_mirrors.find(Key(slot, p.seq));
    if (it == g_mirrors.end()) return;
    DestroyMirrorActor(it->second.actor, it->second.idx);
    g_mirrors.erase(it);
    UE_LOGI("owner_entity: mirror slot=%d seq=%u destroyed (owner announced death)",
            slot, p.seq);
}

size_t MirrorCount(uint8_t classId) {
    size_t n = 0;
    for (const auto& kv : g_mirrors)
        if (kv.second.classId == classId && R::IsLiveByIndex(kv.second.actor, kv.second.idx)) ++n;
    return n;
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
    // The departed slot's spawns still waiting on a class load are never spawned.
    const size_t cancelled = coop::element::class_load_wait::CancelIf(
        "OwnerEntitySpawn", [slot](uint32_t k) { return SlotOfKey(k) == slot; });
    if (cancelled > 0)
        UE_LOGI("owner_entity: %zu spawn(s) of departed slot %d were waiting on a class load and are cancelled",
                cancelled, slot);
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
    g_pendingOwn.clear();
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::owner_entity_sync
