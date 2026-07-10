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

#include "coop/world/world_actor_sync.h"

#include "world_actor_detail.h"  // co-located private header (src tree, not include/)

#include "coop/creatures/piramid_sync.h"  // v100 auxYaw: the piramid heading producer/consumer

#include "coop/element/element_deleter.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"  // PropMirrors/NpcMirrors/WaMirrors
#include "coop/element/registry.h"
#include "coop/element/world_actor.h"
#include "coop/element/identity_destroy.h"  // RetireMirror (the single destroy funnel, Inc B)
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

#include <atomic>
#include <chrono>
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
using coop::element::WaMirrors;   // canonical accessor (coop/element/mirror_managers.h)

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

// Read the FTransform spawn param into the payload (translation + FQuat->FRotator + v99 Scale3D).
// Identical math to npc_sync's interceptor (FQuat XYZW @ +0, FVector translation @ +0x10, Scale3D
// @ +0x20 -- the piramid spawner passes 2.0 here; losing it half-sizes the mirror).
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
    p.scaleX = *reinterpret_cast<const float*>(xf + 0x20);
    p.scaleY = *reinterpret_cast<const float*>(xf + 0x24);
    p.scaleZ = *reinterpret_cast<const float*>(xf + 0x28);
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
        coop::element::RetireMirror(eid);
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
    coop::element::RetireMirror(eid);
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
    // HOSTING-gated tracking (RULE 1 root fix 2026-07-05): the host branch runs with or
    // without peers -- a WA spawned while the host is alone must be tracked so the join
    // connect-snapshot can deliver it (the 0s pyramid failure). Only the broadcast inside
    // is connected-gated. The client suppress branch still requires a live connection.
    if (!s) return false;

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
        UE_LOGI("world-actor[host]: tracked WorldActorSpawn class='%ls' eid=%u loc=(%.0f,%.0f,%.0f) "
                "rot=(p=%.1f y=%.1f r=%.1f)", cls.c_str(), p.elementId, p.locX, p.locY, p.locZ,
                p.rotPitch, p.rotYaw, p.rotRoll);
        // Alone-host spawns are delivered by the join connect-snapshot instead.
        if (s->connected() &&
            !s->SendReliable(coop::net::ReliableKind::WorldActorSpawn, &p, sizeof(p))) {
            UE_LOGW("world-actor[host]: SendReliable(WorldActorSpawn) failed -- eid=%u not broadcast",
                    p.elementId);
        }
        return false;  // host spawns normally (pass-through)
    }

    if (s->role() != coop::net::Role::Client || !s->connected()) return false;
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
    // LIFECYCLE runs while HOSTING (alone included); only the batch PUBLISH is peer-dependent
    // (RULE 1 root fix 2026-07-05): a WA tracked while alone that self-destroys while alone
    // must dead-retire then, or the join connect-snapshot would iterate a corpse element.
    auto* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) return;
    const bool connected = s->connected();

    static std::vector<coop::element::WorldActor*> elems;
    WaMirrors().Snapshot(elems);
    static std::vector<coop::net::WorldActorPoseSnapshot> batch;
    batch.clear();

    // Pose-walk dead-retire (2026-07-04, the piramid lifecycle gap): a WA's event-end destroy
    // is often a SELF K2_DestroyActor (EX/native self-call the PRE observer never sees -- the
    // npc_sync.cpp:841 class; piramid2_C's checkIfReached END branch is one). A BOUND actor
    // that reads dead here is terminal (GC'd/recycled) -- close the lifecycle exactly as the
    // PRE would have: reverse-map erase + RetireMirror + WorldActorDestroy broadcast. Unbound
    // (actor==null, pre-POST window) elements are NOT dead -- skip them as before.
    struct DeadWa { void* actor; coop::element::ElementId eid; };
    std::vector<DeadWa> dead;

    int truncated = 0;
    for (coop::element::WorldActor* el : elems) {
        if (!el) continue;
        void* actor = el->GetActor();
        if (!actor) continue;                                    // unbound (pre-POST) -- not dead
        if (!R::IsLiveByIndex(actor, el->GetInternalIdx())) {    // bound + gone = self-destroyed
            dead.push_back({actor, el->GetId()});
            continue;
        }
        if (!connected) continue;  // no peers: lifecycle-only pass, no batch to build
        if (static_cast<int>(batch.size()) >= coop::net::kMaxWorldActorBatchEntries) { ++truncated; continue; }
        coop::net::WorldActorPoseSnapshot snap{};
        snap.elementId = static_cast<uint32_t>(el->GetId());
        const auto loc = E::GetActorLocation(actor);
        const auto rot = E::GetActorRotation(actor);
        snap.x = loc.X; snap.y = loc.Y; snap.z = loc.Z;
        snap.pitch = ue_wrap::NormalizeAxis(rot.Pitch);
        snap.yaw   = ue_wrap::NormalizeAxis(rot.Yaw);
        snap.roll  = ue_wrap::NormalizeAxis(rot.Roll);
        // v100 auxYaw: the class-specific VISIBLE heading. piramid2_C keeps its root at yaw 0 and
        // turns its movementVector ArrowComponent instead (the AnimBP orients the body off it) --
        // stream THAT so the client mirror faces where the host does, including the up-to-10 s
        // post-stop easing no position delta can see. Other classes: facing == actor yaw.
        snap.auxYaw = snap.yaw;
        if (el->GetTypeName() == "piramid2_C") {
            float hy = 0.f;
            if (coop::piramid_sync::ReadHostHeadingYaw(actor, hy))
                snap.auxYaw = ue_wrap::NormalizeAxis(hy);
            // v102 head axis: the head/searchlight idle look target (relLook). Zeros when
            // unresolved -- the client apply treats all-zero as "no value, keep current".
            coop::piramid_sync::ReadHostRelLook(actor, snap.auxX, snap.auxY, snap.auxZ);
            // v104 chase axis: the live wispTarget IDENTITY (0 = idle) -- the mirror selects
            // the native chase look-at branch with it (the 0y walk-phase head residue).
            snap.auxTargetEid = coop::piramid_sync::ReadHostWispTargetEid(actor);
        }
        batch.push_back(snap);
    }
    for (const DeadWa& d : dead) {
        {
            std::lock_guard<std::mutex> lk(g_actorToWaIdMutex);
            g_actorToWaId.erase(d.actor);
        }
        coop::element::RetireMirror(d.eid);
        UE_LOGI("world-actor[host dead-retire]: eid=%u actor no longer live (PE-invisible "
                "self-destroy) -- released%s", static_cast<uint32_t>(d.eid),
                connected ? " + broadcasting destroy" : " (alone: nothing to broadcast)");
        if (!connected) continue;  // a joiner never learned this eid -- no destroy to send
        coop::net::EntityDestroyPayload dp{};
        dp.elementId = static_cast<uint32_t>(d.eid);
        if (!s->SendReliable(coop::net::ReliableKind::WorldActorDestroy, &dp, sizeof(dp)))
            UE_LOGW("world-actor[host dead-retire]: SendReliable(WorldActorDestroy) failed for eid=%u",
                    static_cast<uint32_t>(d.eid));
    }
    // [WA-TRACE host-read] 1 Hz while anything streams: the EXACT coords this tick read off each
    // live WA actor (= what the net thread will serialize). The freeze-hunt discriminator: if these
    // coords move while the client's applied pose doesn't, the break is downstream (wire/apply/drive);
    // if they are frozen at spawn, the host read itself is the break (2026-07-05 0s-frozen-pyramid).
    if (!batch.empty()) {
        static long long s_lastReadTraceMs = 0;
        const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (nowMs - s_lastReadTraceMs >= 1000) {
            s_lastReadTraceMs = nowMs;
            for (const auto& snap : batch)
                UE_LOGI("[WA-TRACE host-read] eid=%u loc=(%.0f,%.0f,%.0f) yaw=%.1f aux=%.1f (n=%zu connected=%d)",
                        snap.elementId, snap.x, snap.y, snap.z, snap.yaw, snap.auxYaw, batch.size(), connected ? 1 : 0);
        }
    }
    if (!connected) return;  // publish is peer-dependent
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
        const auto scl = E::GetActorScale3D(actor);  // v99: the piramid is scale 2 -- the mirror must match
        p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
        p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
        p.scaleX = scl.X; p.scaleY = scl.Y; p.scaleZ = scl.Z;
        if (s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::WorldActorSpawn, &p, sizeof(p)))
            ++sent;
    }
    if (sent > 0 || unbound > 0)
        UE_LOGI("world-actor: connect-snapshot -- sent %d existing WA(s) to slot %d (%zu element(s), "
                "%d unbound-skipped)", sent, peerSlot, elems.size(), unbound);
}

unsigned int HostEnrollExSpawn(void* actor) {
    // HOSTING-gated, not connected-gated (RULE 1 root fix 2026-07-05): a WA spawned while
    // the host is alone (the pre-first-join pyramid) must still be tracked -- the join
    // connect-snapshot is what delivers it to a later joiner. Only the broadcast below is
    // peer-dependent.
    auto* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) return 0;
    if (!g_installed.load(std::memory_order_acquire) ||
        g_disabledThisProcess.load(std::memory_order_acquire)) return 0;
    if (!actor) return 0;
    void* cls = R::ClassOf(actor);
    if (!cls || !IsAllowlistedClass(cls)) return 0;
    {
        // Dedup vs the interceptor+POST path: a PE-dispatched spawn was already bound there.
        std::lock_guard<std::mutex> lk(g_actorToWaIdMutex);
        auto it = g_actorToWaId.find(actor);
        if (it != g_actorToWaId.end()) return static_cast<unsigned int>(it->second);
    }
    const std::wstring clsW = R::ToString(R::NameOf(cls));
    auto wa = std::make_unique<coop::element::WorldActor>();
    std::string typeName8;
    for (size_t i = 0; i < clsW.size() && i < 63; ++i) typeName8.push_back(static_cast<char>(clsW[i]));
    wa->SetTypeName(std::move(typeName8));
    const coop::element::ElementId eid = WaMirrors().AllocAndInstall(std::move(wa), /*isHost=*/true);
    if (eid == coop::element::kInvalidId) {
        UE_LOGW("world-actor[host ex-enroll]: AllocAndInstall kInvalidId for '%ls' -- skipped",
                clsW.c_str());
        return 0;
    }
    coop::element::WorldActor* el = WaMirrors().Get(eid);
    if (!el) {
        UE_LOGW("world-actor[host ex-enroll]: eid=%u vanished post-alloc (disconnect race?)", eid);
        return 0;
    }
    el->SetActor(actor, R::InternalIndexOf(actor));
    {
        std::lock_guard<std::mutex> lk(g_actorToWaIdMutex);
        g_actorToWaId[actor] = eid;
    }
    coop::net::EntitySpawnPayload p{};
    p.elementId = static_cast<uint32_t>(eid);
    p.savePersisted = 0;  // event WAs are never save objects
    p.className.len = 0;
    for (size_t i = 0; i < clsW.size() && i < 63; ++i)
        p.className.data[p.className.len++] = static_cast<char>(clsW[i]);
    const auto loc = E::GetActorLocation(actor);   // post-Finish (the drain runs next pump tick)
    const auto rot = E::GetActorRotation(actor);
    const auto scl = E::GetActorScale3D(actor);    // v99: the deferred actor carries the spawner's scale
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
    p.scaleX = scl.X; p.scaleY = scl.Y; p.scaleZ = scl.Z;
    UE_LOGI("world-actor[host ex-enroll]: '%ls' eid=%u at (%.0f,%.0f,%.0f) scale=(%.2f,%.2f,%.2f) "
            "(EX_CallMath BeginDeferred, source-gated catch)", clsW.c_str(), eid, loc.X, loc.Y, loc.Z,
            scl.X, scl.Y, scl.Z);
    // Broadcast only with peers present -- an alone-host enroll is delivered by the join
    // connect-snapshot instead (a peer-less SendReliable would just WARN-spam).
    if (s->connected() &&
        !s->SendReliable(coop::net::ReliableKind::WorldActorSpawn, &p, sizeof(p))) {
        UE_LOGW("world-actor[host ex-enroll]: SendReliable(WorldActorSpawn) failed for eid=%u "
                "(element kept; the connect snapshot can still deliver it)", eid);
    }
    return static_cast<unsigned int>(eid);
}

// The CLIENT half (OnWorldActorSpawn / OnWorldActorDestroy / TickClientWorldActors)
// lives in world_actor_mirror.cpp (extracted 2026-07-05, modular file-size rule).
// Shared internals: world_actor_detail.h (implemented below -- this TU owns the
// session pointer, the Install-resolved spawn path and the bypass slot).

namespace detail {

coop::net::Session* Session() { return LoadSession(); }

SpawnPath GetSpawnPath() {
    return SpawnPath{g_spawnFn, g_finishSpawnFn, g_gsCdo, g_spawnReturnParamOff, g_k2DestroyFn};
}

void SetIncomingClass(void* cls) {
    g_incomingWorldActorClass.store(cls, std::memory_order_release);
}

void ClearIncomingClass() {
    g_incomingWorldActorClass.store(nullptr, std::memory_order_release);
}

bool IsAllowlistedClassNameW(const std::wstring& nm) {
    return coop::world_actor_sync::IsAllowlistedClassNameW(nm);
}

}  // namespace detail

}  // namespace coop::world_actor_sync
