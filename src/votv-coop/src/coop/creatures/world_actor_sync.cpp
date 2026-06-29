// coop/world_actor_sync.cpp -- see coop/world_actor_sync.h.
//
// HOST-AUTHORITATIVE mirror of the ~14 NON-Character event actors (gray saucers, Rozital mothership,
// ariral ships, sky UFO, jellyfish, firetank). A self-contained sibling of npc_sync (host PRE/POST +
// suppress interceptor + lifecycle) FUSED with the client receiver (the npc_mirror half) in ONE TU --
// it is much simpler than npc_sync (no kerfur, no CMC, no save-persist, no adoption), so it does not
// need the SetClientRefs split; the host + client paths share the module-resolved UFunction pointers.
//
// Identity is the class NAME (R::NameEquals against kWorldActorAllowlist), NOT a resolved UClass*
// pointer set. This is DELIBERATE and the key divergence from npc_sync: NPC BP classes load at level
// entry (so npc_sync can gate its interceptor install on all-resolved), but WA EVENT classes load
// LAZILY when their event approaches -- gating on resolution would never arm the interceptor, and a
// per-tick FindClass re-resolve would race the spawn. NameEquals is alloc-free + GUObjectArray-walk-free
// + race-free (a class's FName is readable the instant it exists, and it must exist to spawn). v1 matches
// the leaf event-actor names exactly (no super-walk -- the design lists leaf classes; a subclass that
// surfaces in smoke gets curated into kWorldActorAllowlist).

#include "coop/creatures/world_actor_sync.h"

#include "coop/element/element_deleter.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/registry.h"
#include "coop/element/world_actor.h"
#include "coop/sync/sync_create.h"   // the single WorldActor mirror create funnel (Inc A)
#include "coop/sync/sync_destroy.h"  // RetireMirror (the single destroy funnel, Inc B)
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::world_actor_sync {
namespace {

namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

std::atomic<coop::net::Session*> g_session{nullptr};
inline coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

std::atomic<bool> g_installed{false};
// True if Install permanently disabled the WA lifecycle (no K2_DestroyActor / observer-table full):
// the interceptor host-broadcast gates on this so a partial-lifecycle install never leaks Elements.
std::atomic<bool> g_disabledThisProcess{false};

// Spawn path (resolved once at Install; shared by the host interceptor + the client materialize).
void*   g_spawnFn = nullptr;
int32_t g_spawnActorClassParamOff = -1;
int32_t g_spawnReturnParamOff = -1;
int32_t g_spawnXformParamOff = -1;
void*   g_finishSpawnFn = nullptr;
void*   g_gsCdo = nullptr;
void*   g_k2DestroyFn = nullptr;

// Bypass slot for wire-received WA spawns (client materialize): SET on the game thread immediately
// before BeginDeferred (OnWorldActorSpawn), READ+CLEAR in the interceptor (parallel-anim worker
// possible). Same atomic read-and-clear shape as npc_sync's g_incomingNpcSpawnClass.
std::atomic<void*> g_incomingWorldActorClass{nullptr};

// Observer-install latches (idempotent re-Install; read on a worker thread).
std::atomic<bool> g_postObserverInstalled{false};
std::atomic<bool> g_destroyObserverInstalled{false};

// The canonical owner of every WorldActor element (host AllocAndInstall'd m_mirror=false XOR client
// Install'd m_mirror=true -- a process is host XOR client for WorldActors, like Npc).
inline coop::element::MirrorManager<coop::element::WorldActor>& WaMirrors() {
    return coop::element::MirrorManager<coop::element::WorldActor>::Instance();
}

// Host-side reverse lookup: live AActor* -> ElementId (the K2_DestroyActor PRE gate). Guarded (POST +
// destroy PRE run on parallel-anim workers). LEAF lock -- never nested under the Registry/type mutex
// (the destroy path releases it BEFORE Take/destruct).
std::mutex g_actorToWaIdMutex;
std::unordered_map<void*, coop::element::ElementId> g_actorToWaId;

// Thread-local pending-spawn slot (params-pointer correlation -- the same token npc_sync uses to
// disambiguate nested non-WA BeginDeferred calls; the engine allocates a fresh frame per call).
struct PendingWaSpawn {
    coop::element::ElementId eid;
    const void* paramsPtr;
};
thread_local PendingWaSpawn t_pendingWa{coop::element::kInvalidId, nullptr};

// Exact class-NAME allowlist match (host interceptor): alloc-free + walk-free + race-free. NameOf(cls)
// is the class leaf name (e.g. "rozitBorg_C"), exactly the FindClass key.
bool IsAllowlistedClass(void* cls) {
    if (!cls) return false;
    const auto& nm = R::NameOf(cls);
    for (size_t i = 0; i < P::name::kWorldActorAllowlistSize; ++i)
        if (R::NameEquals(nm, P::name::kWorldActorAllowlist[i])) return true;
    return false;
}

// Wire-className trust gate (client receiver): the wstring built from the wire string.
bool IsAllowlistedClassNameW(const std::wstring& nm) {
    for (size_t i = 0; i < P::name::kWorldActorAllowlistSize; ++i)
        if (nm == P::name::kWorldActorAllowlist[i]) return true;
    return false;
}

// Read the FTransform spawn param into the payload (translation + FQuat->FRotator). Identical math to
// npc_sync's interceptor (FQuat XYZW @ +0, FVector translation @ +0x10).
void ReadSpawnXform(const void* params, coop::net::EntitySpawnPayload& p) {
    if (g_spawnXformParamOff < 0) return;
    const uint8_t* xf = reinterpret_cast<const uint8_t*>(params) + g_spawnXformParamOff;
    const float qx = *reinterpret_cast<const float*>(xf + 0);
    const float qy = *reinterpret_cast<const float*>(xf + 4);
    const float qz = *reinterpret_cast<const float*>(xf + 8);
    const float qw = *reinterpret_cast<const float*>(xf + 12);
    p.locX = *reinterpret_cast<const float*>(xf + 0x10);
    p.locY = *reinterpret_cast<const float*>(xf + 0x14);
    p.locZ = *reinterpret_cast<const float*>(xf + 0x18);
    const float sinp = 2.f * (qw * qy - qz * qx);
    const float sinp_c = sinp > 1.f ? 1.f : (sinp < -1.f ? -1.f : sinp);
    constexpr float kRadToDeg = 57.29577951308232f;
    p.rotPitch = std::asin(sinp_c) * kRadToDeg;
    p.rotYaw   = std::atan2(2.f * (qw * qz + qx * qy), 1.f - 2.f * (qy * qy + qz * qz)) * kRadToDeg;
    p.rotRoll  = std::atan2(2.f * (qw * qx + qy * qz), 1.f - 2.f * (qx * qx + qy * qy)) * kRadToDeg;
}

// POST observer on BeginDeferredSpawnFromClass: bind the returned AActor* into the WorldActor the PRE
// interceptor allocated (params-pointer correlation, same gate as npc_sync's NpcSpawn_POST).
void WorldActorSpawn_POST(void* /*self*/, void* /*function*/, void* params) {
    if (!params || g_spawnReturnParamOff < 0) return;
    if (t_pendingWa.paramsPtr != params) {
        if (t_pendingWa.eid != coop::element::kInvalidId && t_pendingWa.paramsPtr != nullptr) {
            UE_LOGW("world-actor[host POST]: params mismatch with pending eid=%u -- outer WA Element "
                    "may be ORPHANED if its own POST never fires", t_pendingWa.eid);
        }
        return;
    }
    const coop::element::ElementId eid = t_pendingWa.eid;
    t_pendingWa = {coop::element::kInvalidId, nullptr};  // consume
    if (eid == coop::element::kInvalidId) return;
    void* spawnedActor = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(params) + g_spawnReturnParamOff);
    if (!spawnedActor) {
        coop::sync::RetireMirror(eid);
        UE_LOGW("world-actor[host POST]: BeginDeferredSpawn returned null for eid=%u -- released", eid);
        return;
    }
    auto* el = coop::element::Registry::Get().Get(eid);
    if (!el) {
        UE_LOGW("world-actor[host POST]: eid=%u not in Registry (disconnect-race?) -- actor %p ORPHANED",
                eid, spawnedActor);
        return;
    }
    if (el->IsBeingDeleted()) {
        UE_LOGW("world-actor[host POST]: eid=%u being-deleted (destroy race) -- skipping bind", eid);
        return;
    }
    el->SetActor(spawnedActor, R::InternalIndexOf(spawnedActor));
    {
        std::lock_guard<std::mutex> lk(g_actorToWaIdMutex);
        g_actorToWaId[spawnedActor] = eid;
    }
    UE_LOGI("world-actor[host POST]: bound actor=%p to WorldActor eid=%u typeName='%s'",
            spawnedActor, eid, el->GetTypeName().c_str());
}

// K2_DestroyActor PRE observer: O(1) hash gate on g_actorToWaId; a hit releases the Element + broadcasts
// WorldActorDestroy.
void WorldActorDestroy_PRE(void* self, void* /*function*/, void* /*params*/) {
    if (!self) return;
    coop::element::ElementId eid = coop::element::kInvalidId;
    {
        std::lock_guard<std::mutex> lk(g_actorToWaIdMutex);
        auto it = g_actorToWaId.find(self);
        if (it == g_actorToWaId.end()) return;  // not a WA we track
        eid = it->second;
        g_actorToWaId.erase(it);
    }
    coop::sync::RetireMirror(eid);
    UE_LOGI("world-actor[host destroy PRE]: actor=%p WorldActor eid=%u released (deferred)", self, eid);
    auto* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    coop::net::EntityDestroyPayload p{};
    p.elementId = static_cast<uint32_t>(eid);
    if (!s->SendReliable(coop::net::ReliableKind::WorldActorDestroy, &p, sizeof(p)))
        UE_LOGW("world-actor[host destroy PRE]: SendReliable(WorldActorDestroy) failed for eid=%u", eid);
}

bool WorldActorSuppress_Interceptor(void* self, void* params) {
    (void)self;  // self = the UGameplayStatics CDO
    if (!params || g_spawnActorClassParamOff < 0) return false;
    auto* s = LoadSession();
    if (!s || !s->connected()) return false;  // pre-connect: don't filter

    if (s->role() == coop::net::Role::Host) {
        if (g_disabledThisProcess.load(std::memory_order_acquire)) return false;
        void* actorClass = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(params) + g_spawnActorClassParamOff);
        if (!actorClass || !IsAllowlistedClass(actorClass)) return false;  // not a WA; let it run

        coop::net::EntitySpawnPayload p{};
        p.savePersisted = 0;  // event WAs are never save objects -> client fresh-spawns a mirror
        ReadSpawnXform(params, p);
        const std::wstring cls = R::ToString(R::NameOf(actorClass));
        p.className.len = 0;
        for (size_t i = 0; i < cls.size() && i < 63; ++i)
            p.className.data[p.className.len++] = static_cast<char>(cls[i]);

        auto wa = std::make_unique<coop::element::WorldActor>();
        std::string typeName8;
        for (size_t i = 0; i < cls.size() && i < 63; ++i) typeName8.push_back(static_cast<char>(cls[i]));
        wa->SetTypeName(std::move(typeName8));
        const coop::element::ElementId eid =
            WaMirrors().AllocAndInstall(std::move(wa), /*isHost=*/true);
        if (eid == coop::element::kInvalidId) {
            UE_LOGW("world-actor[host]: AllocAndInstall kInvalidId for '%ls' -- skipping broadcast",
                    cls.c_str());
            return false;
        }
        p.elementId = static_cast<uint32_t>(eid);
        t_pendingWa = {eid, params};  // for the matching POST (same thread, same call)
        if (s->SendReliable(coop::net::ReliableKind::WorldActorSpawn, &p, sizeof(p))) {
            UE_LOGI("world-actor[host]: broadcast WorldActorSpawn class='%ls' eid=%u loc=(%.0f,%.0f,%.0f) "
                    "rot=(p=%.1f y=%.1f r=%.1f)", cls.c_str(), p.elementId, p.locX, p.locY, p.locZ,
                    p.rotPitch, p.rotYaw, p.rotRoll);
        } else {
            UE_LOGW("world-actor[host]: SendReliable(WorldActorSpawn) failed -- eid=%u not broadcast",
                    p.elementId);
        }
        return false;  // host spawns normally (pass-through)
    }

    if (s->role() != coop::net::Role::Client) return false;
    void* actorClass = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(params) + g_spawnActorClassParamOff);
    if (!actorClass) return false;

    // Bypass slot: a wire-received materialization set g_incomingWorldActorClass right before its own
    // BeginDeferred. Consume it (atomic read-and-clear) + allow the spawn through.
    void* expected = actorClass;
    if (g_incomingWorldActorClass.compare_exchange_strong(
            expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire)) {
        UE_LOGI("world-actor-suppress[client]: allow-through wire spawn for class=%p (bypass consumed)",
                actorClass);
        return false;
    }
    // Defense-in-depth: a CLIENT should never fire an event locally (its scheduler is dormant), but if
    // an allowlisted WA does spawn locally, suppress it (the host streams the authoritative one).
    if (IsAllowlistedClass(actorClass)) {
        UE_LOGI("world-actor-suppress[client]: skipping BeginDeferred for allowlisted WA class=%p",
                actorClass);
        if (g_spawnReturnParamOff >= 0)
            *reinterpret_cast<void**>(
                reinterpret_cast<uint8_t*>(params) + g_spawnReturnParamOff) = nullptr;
        return true;  // SKIP the original
    }
    return false;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_installed.load(std::memory_order_acquire)) return;
    // Throttle the unresolved-window retries (each Find* walks GUObjectArray). Unlike npc_sync we do NOT
    // wait for the actor BP classes -- only the engine-core spawn path -- so this resolves promptly.
    static int s_retry = 0;
    if (s_retry > 0) { --s_retry; return; }

    void* gsCls = R::FindClass(P::name::GameplayStaticsClass);
    if (!gsCls) { s_retry = 60; return; }
    void* fn = R::FindFunction(gsCls, P::name::BeginDeferredSpawnFn);
    if (!fn) {
        UE_LOGW("world-actor: %ls.%ls UFunction not found -- disabled permanently",
                P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn);
        g_installed.store(true, std::memory_order_release);
        return;
    }
    const int32_t classOff = R::FindParamOffset(fn, L"ActorClass");
    const int32_t retOff   = R::FindParamOffset(fn, L"ReturnValue");
    if (classOff < 0 || retOff < 0) {
        UE_LOGW("world-actor: BeginDeferred params not found (ActorClass=%d ReturnValue=%d) -- disabled",
                classOff, retOff);
        g_installed.store(true, std::memory_order_release);
        return;
    }
    const int32_t xformOff = R::FindParamOffset(fn, L"SpawnTransform");  // may be -1 (position-less spawns still work)
    void* finishFn = R::FindFunction(gsCls, P::name::FinishSpawningActorFn);
    void* gsCdo    = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    void* actorCls = R::FindClass(P::name::ActorClassName);
    void* destroyFn = actorCls ? R::FindFunction(actorCls, P::name::DestroyActorFn) : nullptr;
    if (!finishFn || !gsCdo || !destroyFn) {
        // Lifecycle cannot close without K2_DestroyActor; client materialize needs finish+CDO. Without
        // a guaranteed destroy observer we'd leak Elements -- disable for the process (the WA still
        // spawns locally on the host; it just doesn't sync this session).
        g_disabledThisProcess.store(true, std::memory_order_release);
        g_installed.store(true, std::memory_order_release);
        UE_LOGE("world-actor: cannot resolve finish/CDO/K2_DestroyActor (finish=%p cdo=%p destroy=%p) "
                "-- WorldActor sync DISABLED for process lifetime", finishFn, gsCdo, destroyFn);
        return;
    }
    g_spawnFn = fn;
    g_spawnActorClassParamOff = classOff;
    g_spawnReturnParamOff = retOff;
    g_spawnXformParamOff = xformOff;
    g_finishSpawnFn = finishFn;
    g_gsCdo = gsCdo;
    g_k2DestroyFn = destroyFn;

    // Atomic two-observer registration (POST binds the actor; destroy PRE closes the lifecycle). If
    // EITHER fails, roll the other back so a half-installed state doesn't burn an observer-table slot.
    if (!g_postObserverInstalled.load(std::memory_order_acquire) ||
        !g_destroyObserverInstalled.load(std::memory_order_acquire)) {
        const bool postOk = GT::RegisterPostObserver(fn, &WorldActorSpawn_POST);
        if (!postOk) {
            g_disabledThisProcess.store(true, std::memory_order_release);
            UE_LOGE("world-actor: RegisterPostObserver FAILED (table full) -- WA sync DISABLED");
        } else if (!GT::RegisterPreObserver(destroyFn, &WorldActorDestroy_PRE)) {
            GT::UnregisterObservers(fn, &WorldActorSpawn_POST);
            g_disabledThisProcess.store(true, std::memory_order_release);
            UE_LOGE("world-actor: RegisterPreObserver FAILED for K2_DestroyActor -- WA sync DISABLED "
                    "+ rolled back POST");
        } else {
            g_postObserverInstalled.store(true, std::memory_order_release);
            g_destroyObserverInstalled.store(true, std::memory_order_release);
            UE_LOGI("world-actor: registered POST (actor bind) + K2_DestroyActor PRE (lifecycle close)");
        }
    }

    g_installed.store(true, std::memory_order_release);
    if (g_disabledThisProcess.load(std::memory_order_acquire)) {
        UE_LOGW("world-actor: lifecycle observer install FAILED -- skipping interceptor registration");
        return;
    }
    // A SECOND interceptor on BeginDeferred -- conflict-free with npc_sync's (disjoint allowlists;
    // game_thread.h multi-interceptor support: each fires, the matching one acts, both return false on
    // the host so the spawn proceeds).
    GT::RegisterInterceptor(fn, &WorldActorSuppress_Interceptor);
    UE_LOGI("world-actor: installed interceptor + observers on %ls.%ls (ActorClass@%d Return@%d Xform@%d; "
            "%zu allowlisted names; NAME-matched, lazy-load-safe)",
            P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn,
            classOff, retOff, xformOff, P::name::kWorldActorAllowlistSize);
}

bool IsInstalled() { return g_installed.load(std::memory_order_acquire); }

void OnDisconnect() {
    // Drain the unified MirrorManager<WorldActor> (a process holds host XOR client WA elements, like
    // Npc). K2_DestroyActor ONLY the client mirror actors (IsMirror()==true); the host's real WA
    // Elements are release-only (m_mirror=false -> dtor FreeId; the real actors stay in the host world).
    std::vector<coop::element::WorldActor*> snap;
    WaMirrors().Snapshot(snap);
    size_t nMirrors = 0, nHost = 0, nK2 = 0;
    for (coop::element::WorldActor* el : snap) {
        if (!el) continue;
        if (!el->IsMirror()) { ++nHost; continue; }
        ++nMirrors;
        void* actor = el->GetActor();
        if (actor && g_k2DestroyFn && R::IsLive(actor)) {
            R::CallFunction(actor, g_k2DestroyFn, nullptr);  // K2_DestroyActor (game thread)
            ++nK2;
        }
    }
    const size_t total = WaMirrors().DrainAll();
    if (total > 0)
        UE_LOGI("world-actor: drained %zu WorldActor element(s) (%zu host release-only, %zu client "
                "mirror(s); K2 on %zu live mirror actor(s))", total, nHost, nMirrors, nK2);
    {
        std::lock_guard<std::mutex> lk(g_actorToWaIdMutex);
        g_actorToWaId.clear();
    }
    g_incomingWorldActorClass.store(nullptr, std::memory_order_release);
}

void TickPoseStream() {
    // HOST-only: read each live WA's transform each tick + publish ONE WorldActorPose batch for the net
    // thread to fan out (so client mirrors MOVE). An EMPTY batch clears it (stop sending when WAs vanish).
    // Game thread (net-pump asserts GT) -> the scratch statics are single-threaded.
    auto* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host || !s->connected()) return;

    static std::vector<coop::element::WorldActor*> elems;
    WaMirrors().Snapshot(elems);
    static std::vector<coop::net::WorldActorPoseSnapshot> batch;
    batch.clear();

    int truncated = 0;
    for (coop::element::WorldActor* el : elems) {
        if (!el) continue;
        void* actor = el->GetActor();
        if (!actor || !R::IsLiveByIndex(actor, el->GetInternalIdx())) continue;  // unbound / GC'd
        if (static_cast<int>(batch.size()) >= coop::net::kMaxWorldActorBatchEntries) { ++truncated; continue; }
        coop::net::WorldActorPoseSnapshot snap{};
        snap.elementId = static_cast<uint32_t>(el->GetId());
        const auto loc = E::GetActorLocation(actor);
        const auto rot = E::GetActorRotation(actor);
        snap.x = loc.X; snap.y = loc.Y; snap.z = loc.Z;
        snap.pitch = ue_wrap::NormalizeAxis(rot.Pitch);
        snap.yaw   = ue_wrap::NormalizeAxis(rot.Yaw);
        snap.roll  = ue_wrap::NormalizeAxis(rot.Roll);
        batch.push_back(snap);
    }
    if (truncated > 0) {
        static bool s_warned = false;
        if (!s_warned) { s_warned = true;
            UE_LOGW("world-actor-pose: >%d WAs this tick -- %d truncated (raise kMaxWorldActorBatchEntries)",
                    coop::net::kMaxWorldActorBatchEntries, truncated); }
    }
    static bool s_loggedFirst = false;
    if (!batch.empty() && !s_loggedFirst) { s_loggedFirst = true;
        UE_LOGI("world-actor-pose: streaming %zu WorldActor(s) -> peers (first batch)", batch.size()); }
    s->SetLocalWorldActorPoseBatch(batch);  // by const-ref copy; our static `batch` keeps its buffer
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    // HOST-only: re-send WorldActorSpawn (class + CURRENT transform) for every already-spawned WA to the
    // freshly-connected client, so a joiner mirrors actors that spawned BEFORE it joined. The client's
    // OnWorldActorSpawn materializes each; MirrorManager::Install is idempotent so a re-send is a no-op.
    auto* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) return;
    if (peerSlot < 1) return;  // SendReliableToSlot range-validates the upper bound

    std::vector<coop::element::WorldActor*> elems;
    WaMirrors().Snapshot(elems);
    int sent = 0, unbound = 0;
    for (coop::element::WorldActor* el : elems) {
        if (!el) continue;
        void* actor = el->GetActor();
        if (!actor || !R::IsLiveByIndex(actor, el->GetInternalIdx())) { ++unbound; continue; }
        coop::net::EntitySpawnPayload p{};
        const std::string& tn = el->GetTypeName();
        p.className.len = 0;
        for (size_t i = 0; i < tn.size() && i < 63; ++i)
            p.className.data[p.className.len++] = tn[i];
        p.elementId = static_cast<uint32_t>(el->GetId());
        p.savePersisted = 0;
        const auto loc = E::GetActorLocation(actor);
        const auto rot = E::GetActorRotation(actor);
        p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
        p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
        if (s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::WorldActorSpawn, &p, sizeof(p)))
            ++sent;
    }
    if (sent > 0 || unbound > 0)
        UE_LOGI("world-actor: connect-snapshot -- sent %d existing WA(s) to slot %d (%zu element(s), "
                "%d unbound-skipped)", sent, peerSlot, elems.size(), unbound);
}

void OnWorldActorSpawn(const coop::net::EntitySpawnPayload& payload) {
    using ue_wrap::ParamFrame;
    using ue_wrap::Call;
    auto* s = LoadSession();
    if (!s) return;
    if (s->role() == coop::net::Role::Host) {
        UE_LOGI("world-actor[client OnSpawn]: received on host -- dropping (loopback bounce)");
        return;
    }
    // Host-authoritative: the eid must be in the host range (the event_feed senderPeerSlot==0 gate
    // already rejected non-host senders).
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(payload.elementId)) {
        UE_LOGW("world-actor[client OnSpawn]: eid=%u out of host range [1, %u) -- dropping",
                payload.elementId, coop::element::kHostRangeSize);
        return;
    }
    if (payload.className.len == 0 || payload.className.len > 63) {
        UE_LOGW("world-actor[client OnSpawn]: bad className.len=%u -- dropping", payload.className.len);
        return;
    }
    std::wstring classW;
    classW.reserve(payload.className.len);
    for (uint8_t i = 0; i < payload.className.len; ++i)
        classW.push_back(static_cast<wchar_t>(static_cast<unsigned char>(payload.className.data[i])));
    // Trust-boundary: finite + bounded floats (a NaN must not reach BeginDeferred's SpawnTransform).
    const float vals[6] = {payload.locX, payload.locY, payload.locZ,
                           payload.rotPitch, payload.rotYaw, payload.rotRoll};
    for (float v : vals) {
        if (!std::isfinite(v)) {
            UE_LOGW("world-actor[client OnSpawn]: non-finite float -- dropping (eid=%u)", payload.elementId);
            return;
        }
    }
    if (std::fabs(payload.locX) > coop::net::kMaxCoord ||
        std::fabs(payload.locY) > coop::net::kMaxCoord ||
        std::fabs(payload.locZ) > coop::net::kMaxCoord) {
        UE_LOGW("world-actor[client OnSpawn]: loc out of bounds -- dropping (eid=%u)", payload.elementId);
        return;
    }
    // Trust-boundary allowlist gate: only materialize allowlisted WA class names (a peer could send an
    // arbitrary className; this forces R::FindClass GUObjectArray walks otherwise).
    if (!IsAllowlistedClassNameW(classW)) {
        UE_LOGW("world-actor[client OnSpawn]: class '%ls' not on WA allowlist -- rejecting (eid=%u)",
                classW.c_str(), payload.elementId);
        return;
    }
    // Duplicate-eid guard.
    if (WaMirrors().Get(payload.elementId) != nullptr) {
        UE_LOGW("world-actor[client OnSpawn]: eid=%u already mirrored -- dropping duplicate", payload.elementId);
        return;
    }
    if (!g_spawnFn || !g_finishSpawnFn || !g_gsCdo || g_spawnReturnParamOff < 0) {
        UE_LOGW("world-actor[client OnSpawn]: receiver UFunctions unresolved -- dropping (eid=%u)",
                payload.elementId);
        return;
    }
    void* actorClass = R::FindClass(classW.c_str());
    if (!actorClass) {
        UE_LOGW("world-actor[client OnSpawn]: class '%ls' not loaded -- dropping (eid=%u)",
                classW.c_str(), payload.elementId);
        return;
    }
    void* worldCtx = E::GetWorldContext();
    if (!worldCtx) {
        UE_LOGW("world-actor[client OnSpawn]: no world context -- dropping (eid=%u)", payload.elementId);
        return;
    }
    ue_wrap::FTransform xform{};
    E::RotatorToQuat(payload.rotPitch, payload.rotYaw, payload.rotRoll,
                     xform.RotX, xform.RotY, xform.RotZ, xform.RotW);
    xform.TX = payload.locX; xform.TY = payload.locY; xform.TZ = payload.locZ;

    // Bypass our own client interceptor for THIS spawn, then BeginDeferred + FinishSpawning the mirror.
    g_incomingWorldActorClass.store(actorClass, std::memory_order_release);
    constexpr uint8_t kAlwaysSpawn = 1;
    void* spawned = nullptr;
    {
        ParamFrame begin(g_spawnFn);
        if (!begin.valid()) {
            UE_LOGE("world-actor[client OnSpawn]: ParamFrame(BeginDeferred) invalid -- dropping eid=%u",
                    payload.elementId);
            g_incomingWorldActorClass.store(nullptr, std::memory_order_release);
            return;
        }
        begin.Set<void*>(L"WorldContextObject", worldCtx);
        begin.Set<void*>(L"ActorClass", actorClass);
        begin.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        begin.Set<uint8_t>(L"CollisionHandlingOverride", kAlwaysSpawn);
        begin.Set<void*>(L"Owner", nullptr);
        if (!Call(g_gsCdo, begin)) {
            UE_LOGE("world-actor[client OnSpawn]: BeginDeferred call failed for '%ls' eid=%u",
                    classW.c_str(), payload.elementId);
            g_incomingWorldActorClass.store(nullptr, std::memory_order_release);
            return;
        }
        spawned = begin.Get<void*>(L"ReturnValue");
    }
    if (!spawned) {
        UE_LOGE("world-actor[client OnSpawn]: BeginDeferred returned null for '%ls' eid=%u (suppressor "
                "swallowed it?)", classW.c_str(), payload.elementId);
        g_incomingWorldActorClass.store(nullptr, std::memory_order_release);
        return;
    }
    {
        ParamFrame finish(g_finishSpawnFn);
        if (!finish.valid()) {
            UE_LOGE("world-actor[client OnSpawn]: ParamFrame(FinishSpawning) invalid -- forcing K2 on %p",
                    spawned);
            if (g_k2DestroyFn && R::IsLive(spawned)) R::CallFunction(spawned, g_k2DestroyFn, nullptr);
            return;
        }
        finish.Set<void*>(L"Actor", spawned);
        finish.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        if (!Call(g_gsCdo, finish)) {
            UE_LOGE("world-actor[client OnSpawn]: FinishSpawning failed for %p '%ls' eid=%u -- forcing K2",
                    spawned, classW.c_str(), payload.elementId);
            if (g_k2DestroyFn && R::IsLive(spawned)) R::CallFunction(spawned, g_k2DestroyFn, nullptr);
            return;
        }
    }

    const coop::element::ElementId eid = static_cast<coop::element::ElementId>(payload.elementId);
    if (!coop::sync::CreateOrAdoptWorldActorMirror(eid, spawned, classW, /*senderSlot=*/-1)) {
        UE_LOGW("world-actor[client OnSpawn]: CreateOrAdoptWorldActorMirror(eid=%u) failed -- destroying orphan actor %p",
                payload.elementId, spawned);
        if (g_k2DestroyFn && R::IsLive(spawned)) R::CallFunction(spawned, g_k2DestroyFn, nullptr);
        return;
    }
    // Park the mirror so the streamed pose drive is authoritative: GENERIC actor-tick OFF (no CMC read --
    // a WorldActor is a plain AActor). Any residual component tick is overwritten each frame by the
    // pose drive's SetActorLocation/SetActorRotation (the MTA dead-reckoning model).
    E::SetActorTickEnabled(spawned, false);
    UE_LOGI("world-actor[client OnSpawn]: materialized mirror eid=%u class='%ls' actor=%p loc=(%.0f,%.0f,%.0f)",
            payload.elementId, classW.c_str(), spawned, payload.locX, payload.locY, payload.locZ);
}

void OnWorldActorDestroy(const coop::net::EntityDestroyPayload& payload) {
    auto* s = LoadSession();
    if (!s) return;
    if (s->role() == coop::net::Role::Host) {
        UE_LOGI("world-actor[client OnDestroy]: received on host -- dropping (loopback bounce)");
        return;
    }
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(payload.elementId)) {
        UE_LOGW("world-actor[client OnDestroy]: eid=%u out of host range -- dropping", payload.elementId);
        return;
    }
    const coop::element::ElementId eid = static_cast<coop::element::ElementId>(payload.elementId);
    std::unique_ptr<coop::element::WorldActor> drained = WaMirrors().Take(eid);
    if (!drained) {
        UE_LOGI("world-actor[client OnDestroy]: eid=%u not in mirror table -- ignoring", payload.elementId);
        return;
    }
    void* actor = drained->GetActor();
    if (actor && g_k2DestroyFn && R::IsLive(actor)) {
        R::CallFunction(actor, g_k2DestroyFn, nullptr);
        UE_LOGI("world-actor[client OnDestroy]: K2_DestroyActor on mirror eid=%u actor=%p",
                payload.elementId, actor);
    } else if (actor && !R::IsLive(actor)) {
        UE_LOGI("world-actor[client OnDestroy]: mirror eid=%u actor already not-live -- skipping K2",
                payload.elementId);
    }
    // drained's dtor fires here -> Registry::UnregisterMirror(eid).
}

void TickClientWorldActors() {
    auto* s = LoadSession();
    if (!s || s->role() == coop::net::Role::Host) return;  // client-only (host streams, doesn't drive)

    // 1) Apply the latest received batch -> open an interp window per WA. Per-entry float validation is
    //    the trust boundary (a NaN must not reach SetActorLocation/SetActorRotation).
    std::vector<coop::net::WorldActorPoseSnapshot> batch;
    if (s->TakeRemoteWorldActorBatch(batch)) {
        for (const auto& snap : batch) {
            if (!std::isfinite(snap.x) || !std::isfinite(snap.y) || !std::isfinite(snap.z) ||
                !std::isfinite(snap.pitch) || !std::isfinite(snap.yaw) || !std::isfinite(snap.roll)) continue;
            if (std::fabs(snap.x) > 1.0e6f || std::fabs(snap.y) > 1.0e6f || std::fabs(snap.z) > 1.0e6f) continue;
            coop::element::WorldActor* el = WaMirrors().Get(snap.elementId);
            if (!el || !el->IsMirror()) continue;  // not materialized yet (connect gap) / defensive
            el->SetTargetPose(snap);
        }
        static bool s_loggedFirst = false;
        if (!batch.empty() && !s_loggedFirst) { s_loggedFirst = true;
            UE_LOGI("world-actor-pose: client applying %zu WorldActor pose(s) (first batch)", batch.size()); }
    }

    // 2) Advance the interp + drive EVERY live mirror, every frame (smooth between packets). Reused
    //    scratch (no per-tick heap alloc); client game thread only.
    static std::vector<coop::element::WorldActor*> elems;
    WaMirrors().Snapshot(elems);
    for (coop::element::WorldActor* el : elems)
        if (el && el->IsMirror()) el->Tick();
}

}  // namespace coop::world_actor_sync
