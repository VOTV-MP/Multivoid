// coop/creatures/npc_sync.cpp -- the host side of NPC sync: the BeginDeferredSpawnFromClass
// interceptor that enrols an allowlisted NPC as an Npc Element and broadcasts EntitySpawn (and, on
// a connected client, suppresses the local spawn), the POST observer that binds the spawned actor,
// the K2_DestroyActor PRE that broadcasts EntityDestroy, and the silent register and release the
// kerfur conversion uses. The receivers live in npc_mirror.cpp, the pose egress in
// npc_pose_host.cpp, the install in npc_sync_install.cpp, the world walk in npc_world_enum.cpp.

#include "coop/creatures/npc_sync.h"
#include "npc_sync_internal.h"  // install/state seam (impl-private, src-local): shared globals + callback decls

#include "coop/dev/rng_roll_census.h"       // channel (a): BeginDeferred pass-through census
#include "coop/world/spawn_authority.h"     // the tripwire for a park-class spawner spawning on a client
#include "coop/element/element_deleter.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"  // PropMirrors/NpcMirrors/WaMirrors
#include "coop/element/identity_destroy.h"   // RetireMirror, the single destroy funnel
#include "coop/element/npc.h"
#include "coop/element/registry.h"
#include "coop/creatures/kerfur_entity.h"   // AllocKerfurId and ReleaseKerfurForEid, the id's two ends
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/creatures/npc_mirror.h"
#include "coop/creatures/npc_world_enum.h"  // ClearPendingExSpawns at disconnect
#include "coop/props/prop_echo_suppress.h"  // InMirrorSpawnScope, the census's mirror exclusion
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::npc_sync {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

// The session pointer the interceptor reads role() and Send*() through; an atomic, since the
// interceptor fires from parallel-anim workers while the harness may SetSession(nullptr) at
// shutdown.
std::atomic<coop::net::Session*> g_session_ptr{nullptr};

inline coop::net::Session* LoadSession() {
    return g_session_ptr.load(std::memory_order_acquire);
}

// The bypass slot for a wire-received NPC spawn: set on the game thread just before the client's
// BeginDeferred call, consumed by the next interceptor fire that sees the matching class. Atomic
// with a single-instruction read-and-clear, since the interceptor may fire on a parallel-anim
// worker; a torn read once left the bypass armed for the next local spawn, a duplicate NPC.
std::atomic<void*> g_incomingNpcSpawnClass{nullptr};

// Every Npc Element, host-allocated or client-mirrored, is owned by the one MirrorManager<Npc>
// (NpcMirrors()). A process is host or client, so the manager holds either kind, never both, and
// IsMirror() is the discriminator the drain uses.
using coop::element::NpcMirrors;   // canonical accessor (coop/element/mirror_managers.h)

// The reverse map, live actor to ElementId, filled by the POST observer; the K2_DestroyActor
// PRE's O(1) lookup is the gate for "an NPC we own". Host bookkeeping, not identity, so not the
// manager's. Guarded: the POST and the PRE run on parallel-anim workers. A leaf lock: the destroy
// paths release it before NpcMirrors().Take or an Npc destructor, and AllocAndInstall never
// touches it.
std::mutex g_actorToNpcIdMutex;
std::unordered_map<void*, coop::element::ElementId> g_actorToNpcId;

// The thread-local pending-spawn slot: the PRE writes the allocated ElementId and the
// params-frame pointer, and the POST consumes only when its own params pointer matches. PRE and
// POST run on one thread for one call, and the engine allocates a fresh frame per call, so the
// pointer is the correlation token; a class match short-circuited on a failed non-NPC spawn
// nested inside an NPC's constructor and let the inner POST drain the outer Element.
struct PendingNpcSpawn {
    coop::element::ElementId eid;
    const void* paramsPtr;
};
thread_local PendingNpcSpawn t_pendingNpc{coop::element::kInvalidId, nullptr};

// A subclass-aware allowlist match (kerfurOmega alone has 20 subclasses), through
// ue_wrap::reflection::IsDescendantOfAny so the SuperStruct offset stays in the wrapper; one
// chain walk checking every base per hop.
bool IsClassOrDerivedFromAnyAllowlisted(void* cls) {
    return R::IsDescendantOfAny(cls, g_npcAllowlist, P::name::kNpcAllowlistSize);
}

}  // namespace; the callbacks below are named, registered by npc_sync_install.cpp

// The POST observer on BeginDeferredSpawnFromClass: when the PRE enrolled an Npc Element and
// stashed (eid, params), this reads the returned AActor* off the params frame, binds it into the
// Element and publishes the reverse-map entry. It fires for every caller of the function, so the
// params pointer gates it: an inner non-NPC call has different params and leaves the slot alone.
void NpcSpawn_POST(void* /*self*/, void* /*function*/, void* params) {
    if (!params || g_npcSpawnReturnParamOff < 0) return;
    // Only consume t_pendingNpc when the params pointer matches the PRE's.
    if (t_pendingNpc.paramsPtr != params) {
        // A pending eid with a non-matching params pointer means an inner NPC POST stole the slot
        // from an outer PRE, and the outer Element is orphaned; believed unreachable in VOTV's
        // spawn paths, logged so it would show.
        if (t_pendingNpc.eid != coop::element::kInvalidId &&
            t_pendingNpc.paramsPtr != nullptr) {
            UE_LOGW("npc-sync[host POST]: params mismatch with pending eid=%u "
                    "(pending params=%p, this POST params=%p) -- outer NPC's "
                    "Element will be ORPHANED if its own POST never fires",
                    t_pendingNpc.eid, t_pendingNpc.paramsPtr, params);
        }
        return;
    }
    const coop::element::ElementId eid = t_pendingNpc.eid;
    t_pendingNpc = {coop::element::kInvalidId, nullptr};  // consume
    if (eid == coop::element::kInvalidId) return;  // defensive; shouldn't happen with matching params
    void* spawnedActor = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(params) + g_npcSpawnReturnParamOff);
    if (!spawnedActor) {
        // The spawn failed: drain the Element and defer its destruction to the game-thread
        // ElementDeleter flush (this POST fires on a worker). Take returns null if already drained;
        // Enqueue(null) is a no-op.
        coop::element::RetireMirror(eid);
        UE_LOGW("npc-sync[host POST]: BeginDeferredSpawn returned null for eid=%u; "
                "released Element back to Registry (deferred to ElementDeleter)", eid);
        return;
    }
    auto* el = coop::element::Registry::Get().Get(eid);
    if (!el) {
        // OnDisconnect drained the manager between this thread's PRE and POST: the actor is an
        // orphan (live, no Element, never in the reverse map, so no EntityDestroy on death).
        // Acceptable at disconnect, since the client's own handler tears its mirrors down; logged
        // loudly.
        UE_LOGW("npc-sync[host POST]: eid=%u not in Registry (disconnect-race?) -- "
                "actor %p is ORPHANED (will not broadcast EntityDestroy on death)",
                eid, spawnedActor);
        return;
    }
    // Registry::Get can return an element a K2_DestroyActor PRE already Took and Enqueued (out of
    // the manager, still in the Registry until the deferred FreeId); binding an actor to it would
    // resurrect a half-dead Element, so the deleter's IsBeingDeleted flag is honoured.
    if (el->IsBeingDeleted()) {
        UE_LOGW("npc-sync[host POST]: eid=%u is being-deleted (Take+Enqueued by a "
                "destroy race) -- skipping bind; actor %p left to its own lifecycle",
                eid, spawnedActor);
        return;
    }
    el->SetActor(spawnedActor, R::InternalIndexOf(spawnedActor));
    {
        std::lock_guard<std::mutex> lk(g_actorToNpcIdMutex);
        g_actorToNpcId[spawnedActor] = eid;
    }
    UE_LOGI("npc-sync[host POST]: bound actor=%p to Npc eid=%u typeName='%s'",
            spawnedActor, eid, el->GetTypeName().c_str());
    // A fresh kerfur NPC (a dev spawn or a purchase) gets its stable KerfurId reserved at first
    // sighting, idempotent per actor. A conversion's turn-on NPC spawns via EX_CallMath and never
    // fires this POST; RegisterHostNpcSilent and BindFormActor own those. The leaf lock is released
    // above, so AllocKerfurId's lock order never nests with it.
    if (void* spawnedCls = R::ClassOf(spawnedActor)) {
        if (coop::kerfur_entity::IsKerfurClass(spawnedCls)) {
            coop::kerfur_entity::AllocKerfurId(spawnedActor, eid, coop::kerfur_entity::Form::Npc,
                                               R::ToString(R::NameOf(spawnedCls)));
        }
    }
}

// The K2_DestroyActor PRE, fired for every actor destroy: the reverse-map lookup is the gate (a
// non-NPC touches the lock and returns); a hit Takes the Element from the manager, defers its
// destruction to the game-thread flush and broadcasts EntityDestroy.
void NpcDestroy_PRE(void* self, void* /*function*/, void* /*params*/) {
    if (!self) return;
    coop::element::ElementId eid = coop::element::kInvalidId;
    {
        std::lock_guard<std::mutex> lk(g_actorToNpcIdMutex);
        auto it = g_actorToNpcId.find(self);
        if (it == g_actorToNpcId.end()) return;  // not an NPC we track
        eid = it->second;
        g_actorToNpcId.erase(it);
    }
    // Drain the Element and defer its destruction to the game-thread flush (this PRE may run on a
    // worker); the leaf lock is released above, so Take's mutex and the deferred FreeId's never
    // nest with it. Take returns null on an already drained eid (a double destroy, or a disconnect
    // race).
    coop::element::RetireMirror(eid);
    // A kerfur reaching this seam died for good: a conversion destroys its old form from inside
    // the blueprint, where the PRE cannot see it, and the converge releases that form itself. The
    // kerfur's own PE-invisible death edge belongs to the conversion poll, so this is the belt for
    // a VISIBLE destroy, not the lane's main path.
    coop::kerfur_entity::ReleaseKerfurForEid(eid);
    UE_LOGI("npc-sync[host destroy PRE]: actor=%p Npc eid=%u released (deferred)", self, eid);
    // Broadcast EntityDestroy so the client mirrors tear their copy down.
    auto* s = LoadSession();
    if (!s || !s->connected()) return;
    if (s->role() != coop::net::Role::Host) return;
    if (!s->SendEntityDestroy(static_cast<uint32_t>(eid))) {
        UE_LOGW("npc-sync[host destroy PRE]: SendEntityDestroy failed for eid=%u", eid);
    }
}

bool NpcSuppress_Interceptor(void* self, void* params) {
    (void)self;  // self = the UGameplayStatics CDO; we don't use it
    // Cheapest checks first.
    if (!params || g_npcSpawnActorClassParamOff < 0) return false;
    auto* s = LoadSession();
    // The host branch tracks with or without peers: an event creature spawned while the host is
    // alone must be enrolled so the join snapshot can deliver it. Only the broadcast is
    // connected-gated; the client suppress branch needs a live connection.
    if (!s) return false;

    // The host path: an allowlisted class gets an Npc Element and an EntitySpawn broadcast, and the
    // spawn proceeds (the interceptor runs before the original, reading SpawnTransform from the
    // params; false lets the original run). The POST binds the actor, the K2_DestroyActor PRE sends
    // EntityDestroy, and the client receiver materialises a mirror through MarkIncomingNpcSpawn.
    if (s->role() == coop::net::Role::Host) {
        // If either lifecycle observer failed to register, the whole host path is skipped: an
        // Element with no POST to bind it or no PRE to release it would leak and leave clients with
        // orphan mirrors. The NPC still spawns locally.
        if (g_npcSyncDisabledThisProcess.load(std::memory_order_acquire)) return false;

        void* actorClass = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(params) + g_npcSpawnActorClassParamOff);
        if (!actorClass || !IsClassOrDerivedFromAnyAllowlisted(actorClass)) {
            // The roll census, host half: a BP deferred spawn we do not track.
            coop::dev::rng_roll_census::NotePassThrough(actorClass, /*isHostRole=*/true);
            return false;  // not an NPC; let it run, no broadcast
        }
        // The SpawnTransform param; the offset is resolved once at Install (a function-local static
        // here was a data race on worker threads).
        coop::net::EntitySpawnPayload p{};
        // A runtime spawn fires after the client's save load, so the client has no local twin:
        // savePersisted 0 makes it fresh-spawn a mirror (save-persisted NPCs reach the client via
        // the world walk and the snapshot, which set 1).
        p.savePersisted = 0;
        if (g_npcSpawnXformParamOff >= 0) {
            // FTransform: FQuat Rotation (16 bytes), FVector Translation, FVector Scale3D,
            // 16-aligned.
            const uint8_t* xform = reinterpret_cast<const uint8_t*>(params) + g_npcSpawnXformParamOff;
            // FQuat (XYZW).
            const float qx = *reinterpret_cast<const float*>(xform + 0);
            const float qy = *reinterpret_cast<const float*>(xform + 4);
            const float qz = *reinterpret_cast<const float*>(xform + 8);
            const float qw = *reinterpret_cast<const float*>(xform + 12);
            // Translation at +0x10.
            p.locX = *reinterpret_cast<const float*>(xform + 0x10);
            p.locY = *reinterpret_cast<const float*>(xform + 0x14);
            p.locZ = *reinterpret_cast<const float*>(xform + 0x18);
            // Scale3D at +0x20, carried so a scaled spawn mirrors at true size; NPC spawners pass
            // 1.0 today.
            p.scaleX = *reinterpret_cast<const float*>(xform + 0x20);
            p.scaleY = *reinterpret_cast<const float*>(xform + 0x24);
            p.scaleZ = *reinterpret_cast<const float*>(xform + 0x28);
            // FQuat to FRotator in degrees for the FRotator-based pose pipeline (the standard UE4
            // conversion; the asin input is clamped).
            const float sinp = 2.f * (qw * qy - qz * qx);
            const float sinp_clamped = sinp >  1.f ?  1.f
                                     : sinp < -1.f ? -1.f : sinp;
            const float pitchRad = std::asin(sinp_clamped);
            const float yawRad   = std::atan2(2.f * (qw * qz + qx * qy),
                                              1.f - 2.f * (qy * qy + qz * qz));
            const float rollRad  = std::atan2(2.f * (qw * qx + qy * qz),
                                              1.f - 2.f * (qx * qx + qy * qy));
            constexpr float kRadToDeg = 57.29577951308232f;
            p.rotPitch = pitchRad * kRadToDeg;
            p.rotYaw   = yawRad   * kRadToDeg;
            p.rotRoll  = rollRad  * kRadToDeg;
        }
        // The class name for the wire: actorClass is a UClass*, so NameOf gives "npc_zombie_C"
        // (ClassNameOf would give "Class").
        const std::wstring cls = R::ToString(R::NameOf(actorClass));
        p.className.len = 0;
        for (size_t i = 0; i < cls.size() && i < 63; ++i) {
            p.className.data[p.className.len++] = static_cast<char>(cls[i]);
        }
        // Allocate an Npc Element from the unified Registry (the host range).
        auto npc = std::make_unique<coop::element::Npc>();
        std::string typeName8;
        for (size_t i = 0; i < cls.size() && i < 63; ++i) {
            typeName8.push_back(static_cast<char>(cls[i]));
        }
        npc->SetTypeName(std::move(typeName8));
        // AllocAndInstall into the one manager (host range, not a mirror); it takes the Registry
        // mutex, then its own. On failure the Npc is dropped.
        const coop::element::ElementId eid =
            NpcMirrors().AllocAndInstall(std::move(npc), /*isHost=*/true);
        if (eid == coop::element::kInvalidId) {
            // The Registry is exhausted (32768 active elements) or an id collided: no broadcast.
            UE_LOGW("npc-sync[host]: NpcMirrors().AllocAndInstall returned kInvalidId for '%ls' "
                    "-- skipping EntitySpawn broadcast (Registry exhausted / lifecycle bug?)",
                    cls.c_str());
            return false;
        }
        p.elementId = static_cast<uint32_t>(eid);
        // Stash (eid, params) for the matching POST on this thread; the params pointer
        // disambiguates nested calls.
        t_pendingNpc = {eid, params};
        UE_LOGI("npc-sync[host]: tracked EntitySpawn class='%ls' elementId=%u loc=(%.0f, %.0f, %.0f) rot=(p=%.1f y=%.1f r=%.1f)",
                cls.c_str(), p.elementId, p.locX, p.locY, p.locZ,
                p.rotPitch, p.rotYaw, p.rotRoll);
        // An alone host's spawns are delivered by the join snapshot instead.
        if (s->connected() && !s->SendEntitySpawn(p)) {
            UE_LOGW("npc-sync[host]: SendEntitySpawn failed (reliable channel busy?) -- NPC elementId=%u not broadcast",
                    p.elementId);
        }
        // The original spawn always proceeds on the host (false = pass-through).
        return false;
    }

    // Only a connected client suppresses; before the connection nothing is filtered.
    if (s->role() != coop::net::Role::Client || !s->connected()) return false;

    // Read the ActorClass UClass* param from the FFrame buffer.
    void* actorClass = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(params) + g_npcSpawnActorClassParamOff);
    if (!actorClass) return false;  // BP bug or default; let UE4 handle

    // The bypass slot: the client receiver sets it to the class just before
    // BeginDeferredActorSpawnFromClass for a host-streamed NPC, and it is consumed here so the
    // original runs; single-shot, cleared on consume. compare_exchange clears it only when it
    // equals actorClass, so an unrelated local spawn between the set and this read leaves it for
    // the wire spawn that follows.
    void* expected = actorClass;
    if (g_incomingNpcSpawnClass.compare_exchange_strong(
            expected, nullptr,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        UE_LOGI("npc-suppress[client]: allow-through wire-received spawn for class=%p (bypass slot consumed)",
                actorClass);
        return false;
    }

    // The subclass-aware allowlist match.
    if (IsClassOrDerivedFromAnyAllowlisted(actorClass)) {
        UE_LOGI("npc-suppress[client]: skipping BeginDeferredActorSpawnFromClass for class=%p (matches NPC allowlist by hierarchy walk)",
                actorClass);
        // Zero the AActor* return so the BP graph receives nullptr and bails;
        // K2Node_SpawnActorFromClass emits a null check before FinishSpawningActor by convention
        // (not confirmed on every VOTV spawner; one without it would crash on
        // FinishSpawningActor(nullptr)).
        if (g_npcSpawnReturnParamOff >= 0) {
            *reinterpret_cast<void**>(
                reinterpret_cast<uint8_t*>(params) + g_npcSpawnReturnParamOff) = nullptr;
        }
        return true;  // SKIP the original
    }
    // The spawn-authority tripwire: a park-class spawner spawning on a connected client, a log-only
    // alarm and the late-instance signal (the reconcile walk parks it). Pre-resolved pointer
    // compares only, since this may be a worker thread.
    coop::spawn_authority::NoteClientSpawnPassThrough(actorClass);
    // The roll census, client half: a BP deferred spawn the suppression does not cover; a no-op
    // unless the dev ini flag is on. Our own mirror spawns re-enter this interceptor on the game
    // thread and are excluded, or they would inflate the very count the census exists for; the
    // scope is game-thread state, so it is consulted only there.
    if (!(ue_wrap::game_thread::IsGameThread() &&
          coop::prop_echo_suppress::InMirrorSpawnScope()))
        coop::dev::rng_roll_census::NotePassThrough(actorClass, /*isHostRole=*/false);
    return false;  // not an NPC; let it run
}

void SetSession(coop::net::Session* session) {
    g_session_ptr.store(session, std::memory_order_release);
}

void MarkIncomingNpcSpawn(void* npcClass) {
    // The release pairs with the interceptor's acquire, so the class pointer and everything written
    // before it are visible to the consumer.
    g_incomingNpcSpawnClass.store(npcClass, std::memory_order_release);
}

void OnDisconnect() {
    // One drain for both roles: npc_mirror::DrainClientMirrors empties the manager, destroying only
    // the IsMirror() elements (client mirrors) and releasing the host's real Elements (their actors
    // stay in the world). Drain first, then clear the reverse map: a POST racing on a worker can
    // bind an in-flight eid and re-insert, and clearing after the drain removes it, while a POST
    // landing after the drain hits its null-Registry guard. K2_DestroyActor inside the drain is
    // game thread only; the caller is on it.
    coop::npc_mirror::DrainClientMirrors();
    {
        std::lock_guard<std::mutex> lk(g_actorToNpcIdMutex);
        g_actorToNpcId.clear();
    }

    g_incomingNpcSpawnClass.store(nullptr, std::memory_order_release);
    // Drop queued, undrained EX-spawn catches: a stale address must never drain into the next
    // session.
    coop::npc_world_enum::ClearPendingExSpawns();
}

// The install-owned state (the param offsets, the allowlist, the disabled flag) is defined in
// npc_sync_install.cpp and declared in npc_sync_internal.h.

// The accessors npc_mirror reads through, each wrapping one internal global, so there is no
// second copy.

coop::net::Session* GetSession() {
    return g_session_ptr.load(std::memory_order_acquire);
}

bool IsAllowlistedClass(void* cls) {
    // The interceptor's walk. The allowlist may not be fully resolved yet (a fast-handshake peer's
    // receiver can fire while the host is still resolving); unresolved slots are null and skipped,
    // so the check is "any resolved base", strictly tighter until the list completes.
    return IsClassOrDerivedFromAnyAllowlisted(cls);
}

void ClearIncomingNpcSpawn() {
    g_incomingNpcSpawnClass.store(nullptr, std::memory_order_release);
}

coop::element::ElementId GetNpcIdForActor(void* actor) {
    if (!actor) return coop::element::kInvalidId;
    std::lock_guard<std::mutex> lk(g_actorToNpcIdMutex);
    auto it = g_actorToNpcId.find(actor);
    return it == g_actorToNpcId.end() ? coop::element::kInvalidId : it->second;
}

void SyncDestroyedNpcActor(void* actor) {
    // The K2_DestroyActor PRE body, for destroys the PRE cannot see (a BP-internal by-name destroy,
    // kerfurOmega.dropKerfurProp). `actor` is a map key only, never dereferenced, so a PendingKill
    // or purged pointer is safe, and a double call no-ops on the map miss.
    NpcDestroy_PRE(actor, nullptr, nullptr);
}

void SyncDestroyedNpcByEid(coop::element::ElementId eid, void* actorKey) {
    // The pose walk's dead-actor retire (a wisp's self-despawn is an EX_VirtualFunction self-call
    // the PRE never sees). Keyed by eid, unlike the by-actor variant: a same-address rebind may
    // have overwritten the reverse map, and keying by eid can never retire a different NPC; the map
    // entry goes only if it still maps to this eid. `actorKey` is never dereferenced.
    if (eid == coop::element::kInvalidId) return;
    if (actorKey) {
        std::lock_guard<std::mutex> lk(g_actorToNpcIdMutex);
        auto it = g_actorToNpcId.find(actorKey);
        if (it != g_actorToNpcId.end() && it->second == eid) g_actorToNpcId.erase(it);
    }
    coop::element::RetireMirror(eid);  // Take + deferred ~Npc/FreeId; no-op if already drained
    UE_LOGI("npc-sync[pose dead-retire]: Npc eid=%u actor=%p released (PE-invisible destroy)",
            static_cast<uint32_t>(eid), actorKey);
    auto* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (!s->SendEntityDestroy(static_cast<uint32_t>(eid))) {
        UE_LOGW("npc-sync[pose dead-retire]: SendEntityDestroy failed for eid=%u", eid);
    }
}

coop::element::ElementId RegisterHostNpcSilent(void* actor, const std::wstring& className) {
    // NpcSpawn_POST's end state (an Npc Element, the bound actor, the reverse-map entry) for an
    // EX_CallMath-spawned turn-on kerfur that never passed the interceptor, without the
    // EntitySpawn: the conversion's sole signal is KerfurConvert. Game thread.
    if (!actor) return coop::element::kInvalidId;
    auto npc = std::make_unique<coop::element::Npc>();
    std::string typeName8;
    for (size_t i = 0; i < className.size() && i < 63; ++i)
        typeName8.push_back(static_cast<char>(className[i]));
    npc->SetTypeName(std::move(typeName8));
    const coop::element::ElementId eid =
        NpcMirrors().AllocAndInstall(std::move(npc), /*isHost=*/true);
    if (eid == coop::element::kInvalidId) {
        UE_LOGW("npc-sync[silent register]: AllocAndInstall kInvalidId for '%ls' (Registry full?)",
                className.c_str());
        return coop::element::kInvalidId;
    }
    coop::element::Npc* el = NpcMirrors().Get(eid);
    if (!el) {
        coop::element::RetireMirror(eid);
        UE_LOGW("npc-sync[silent register]: eid=%u not retrievable after AllocAndInstall -- drained",
                static_cast<uint32_t>(eid));
        return coop::element::kInvalidId;
    }
    el->SetActor(actor, R::InternalIndexOf(actor));
    MapActorToNpcId(actor, eid);
    UE_LOGI("npc-sync[silent register]: host NPC %p class '%ls' -> eid=%u (no EntitySpawn broadcast)",
            actor, className.c_str(), static_cast<uint32_t>(eid));
    return eid;
}

void ReleaseNpcElementSilent(coop::element::ElementId eid) {
    // NpcDestroy_PRE's teardown without the EntityDestroy: the kerfur converge releases the dying
    // form this way, and KerfurConvert carries oldEid so the clients tear their mirror down. Game
    // thread.
    if (eid == coop::element::kInvalidId) return;
    std::unique_ptr<coop::element::Npc> drained = NpcMirrors().Take(eid);
    if (drained) {
        if (void* actor = drained->GetActor()) {
            std::lock_guard<std::mutex> lk(g_actorToNpcIdMutex);
            g_actorToNpcId.erase(actor);
        }
    }
    // Defer the destruction to the game-thread flush; a null drained (already released) makes
    // Enqueue a no-op.
    coop::element::ElementDeleter::Get().Enqueue(std::move(drained));
    // A silent release is safe only if a KerfurConvert carrying this eid as oldEid follows; without
    // one the client's mirror orphans invisibly. The sole-express converge always fires, so the
    // release is always paired.
    UE_LOGI("npc-sync[silent release]: Npc eid=%u released (no EntityDestroy broadcast -- MUST be paired "
            "with a KerfurConvert or the client mirror orphans)", static_cast<uint32_t>(eid));
}

void MapActorToNpcId(void* actor, coop::element::ElementId eid) {
    // Insert or overwrite the reverse-map entry; the world walk uses it to register a pre-existing
    // NPC after binding its Element, the POST observer's end state. The leaf lock never nests under
    // the Registry or type mutex.
    if (!actor) return;
    std::lock_guard<std::mutex> lk(g_actorToNpcIdMutex);
    g_actorToNpcId[actor] = eid;
}

}  // namespace coop::npc_sync
