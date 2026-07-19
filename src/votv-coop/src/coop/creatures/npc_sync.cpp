// coop/npc_sync.cpp -- Phase 5N1 NPC sync foundation (extracted 2026-05-25).
//
// See coop/npc_sync.h for the public interface.
// Architecture findings: research/findings/npc-creatures/votv-npc-sync-prereqs-RE-2026-05-24.md.
//
// SPLIT (M-1 2026-05-29): the client-side receivers (OnEntitySpawn /
// OnEntityDestroy / DrainClientMirrors) moved to coop/npc_mirror.cpp.
// This TU now owns host-side PRE/POST + the suppress interceptor +
// resolution + lifecycle bookkeeping; the receivers consume resolved
// UFunction refs through npc_mirror::SetClientRefs() (called once from
// Install) and access the shared allowlist + session pointer + bypass
// slot through the public accessors GetSession / IsAllowlistedClass /
// MarkIncomingNpcSpawn / ClearIncomingNpcSpawn.

#include "coop/creatures/npc_sync.h"
#include "npc_sync_internal.h"  // install/state seam (impl-private, src-local): shared globals + callback decls

#include "coop/dev/rng_roll_census.h"       // channel (a): BeginDeferred pass-through census
#include "coop/world/spawn_authority.h"     // T1 Inc-1 shipping tripwire (park-class spawner spawn on client)
#include "coop/element/element_deleter.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"  // PropMirrors/NpcMirrors/WaMirrors
#include "coop/element/identity_destroy.h"   // RetireMirror (the single destroy funnel, Inc B)
#include "coop/element/npc.h"
#include "coop/element/registry.h"
#include "coop/creatures/kerfur_entity.h"   // K-4b: reserve the stable KerfurId when a fresh kerfur NPC binds
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/creatures/npc_mirror.h"
#include "coop/creatures/npc_world_enum.h"  // ClearPendingExSpawns (OnDisconnect; InstallExSpawnCatch moved to the install TU)
#include "coop/props/prop_echo_suppress.h"  // InMirrorSpawnScope (census mirror exclusion, F-6)
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
// NOTE (K-0 2026-06-16): engine.h / kerfur.h / puppet.h were dropped when
// RegisterExistingWorldNpcs moved to coop/npc_world_enum.cpp -- they were the
// last users of GetActorLocation/Rotation, HasSaveKey, and ReadCharacterIsFalling
// in this TU (the pose stream that used puppet had already moved to npc_pose_host).

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

// Cached session pointer set by Install(). The interceptor reads role()/
// connected()/SendEntitySpawn() through this. nullptr until first Install.
//
// Audit C2 (2026-05-27): atomic Session* (was plain pointer). The interceptor
// fires from parallel-anim worker threads; plain-pointer deref races with
// harness SetSession(nullptr) on shutdown. Mirrors item_activate.cpp +
// prop_lifecycle.cpp atomic pattern.
std::atomic<coop::net::Session*> g_session_ptr{nullptr};

inline coop::net::Session* LoadSession() {
    return g_session_ptr.load(std::memory_order_acquire);
}

// Bypass slot for Inc2/Inc3's wire-received NPC spawns. Set immediately
// before the client-side BeginDeferred call; consumed by the next
// interceptor fire that sees the matching class.
//
// Atomic because:
//   * SET happens on the game thread (OnEntitySpawn) and on OnDisconnect.
//   * READ + CLEAR happens in NpcSuppress_Interceptor, which fires on
//     ProcessEvent's dispatching thread -- "usually game thread; sometimes
//     a task-graph worker for parallel anim" per game_thread.h:118-120.
// The plain-pointer version raced: a worker-thread interceptor reading the
// slot mid-store could observe a torn / stale value, fail to consume, and
// leave the bypass active for the next local spawn (= a duplicated
// non-suppressed NPC actor).
//
// `exchange(nullptr, acquire)` on the consume side gives single-instruction
// read-and-clear semantics so we don't have the "read-match-clear" three-
// step window that the plain-pointer version had.
std::atomic<void*> g_incomingNpcSpawnClass{nullptr};

// Npc Element ownership (PR-FOUNDATION-3 Inc2, 2026-05-30): the SOLE canonical
// owner of every Npc Element -- host-allocated AND client-mirrored -- is now the
// shared singleton coop::element::MirrorManager<Npc>::Instance() (NpcMirrors()),
// which coop::npc_mirror already used for client mirrors. The host side here
// used to keep a bespoke g_npcElements unordered_map; that duplicate owner is
// retired (RULE 2). The host now AllocAndInstall's into the SAME manager.
//
// A process is host XOR client, so the manager holds either host-AllocAndInstall'd
// elements (m_mirror=false -> dtor FreeId) OR client-Install'd mirrors
// (m_mirror=true -> dtor UnregisterMirror), never both. That IsMirror() flag is
// the discriminator a unified drain uses (npc_mirror::DrainClientMirrors).
using coop::element::NpcMirrors;   // canonical accessor (coop/element/mirror_managers.h)

// Reverse lookup: live AActor* -> ElementId. Populated by the POST-spawn
// observer when it captures the BeginDeferredSpawn ReturnValue; the
// K2_DestroyActor PRE observer's O(1) hash lookup IS the gate for "is this an
// NPC we own?". This is host-side bookkeeping (not Element identity), so it
// stays a dedicated map -- it is NOT something MirrorManager owns. The POST
// observer + K2_DestroyActor PRE run on parallel-anim worker threads per
// game_thread.h:118-120, so the map is guarded.
//
// Lock order vs Registry::m_mutex: g_actorToNpcIdMutex is now a LEAF -- the
// destruction paths release it BEFORE calling NpcMirrors().Take (type mutex)
// or destructing an Npc (Registry mutex via FreeId / the ElementDeleter Flush),
// so it never nests with either. The interceptor's AllocAndInstall takes the
// Registry mutex then the type mutex (documented in mirror_manager.h); it does
// NOT touch g_actorToNpcIdMutex (the POST observer populates the reverse map).
std::mutex g_actorToNpcIdMutex;
std::unordered_map<void*, coop::element::ElementId> g_actorToNpcId;

// Thread-local pending-spawn slot: PRE interceptor writes the just-allocated
// ElementId + the params-frame pointer; POST observer reads + clears only
// when its own params pointer MATCHES. PRE and POST run on the same thread
// for the same UFunction call -- params pointer is the unique correlation
// token (engine allocates a fresh frame per call).
//
// Why params-correlation instead of a class-match in POST (audited 2026-05-28):
// the class-match `if (spawnedActor && !IsClassOr...)` short-circuits to
// false when spawnedActor is null (failed inner non-NPC spawn nested inside
// an NPC's constructor), letting the inner POST consume the outer NPC's
// pending eid and prematurely drain the outer Element. Params-pointer
// correlation eliminates the ambiguity: each call's params is unique;
// inner calls have different params than outer; mismatched POST returns
// early without touching the slot.
struct PendingNpcSpawn {
    coop::element::ElementId eid;
    const void* paramsPtr;
};
thread_local PendingNpcSpawn t_pendingNpc{coop::element::kInvalidId, nullptr};

// Subclass-aware allowlist match: an exact-pointer match leaks all 30+ NPC
// subclasses (e.g. kerfurOmega has 20 subclasses incl. kerfurOmega_
// mannequinSpawner -- and npc_zombie has 14 subclasses, etc). Routed
// through ue_wrap::reflection::IsDescendantOfAny so the UStruct_SuperStruct
// offset stays in the wrapper layer (Principle 7); the helper walks the
// chain ONCE checking all allowlisted bases per hop.
bool IsClassOrDerivedFromAnyAllowlisted(void* cls) {
    return R::IsDescendantOfAny(cls, g_npcAllowlist, P::name::kNpcAllowlistSize);
}

}  // namespace [state; the callbacks below are named -- registered cross-TU by npc_sync_install.cpp]

// GetWorldContext + RotatorToQuat moved to ue_wrap/engine (M-3 2026-05-29);
// call via E::GetWorldContext() / E::RotatorToQuat(...).

// POST observer on BeginDeferredSpawnFromClass: when the host PRE interceptor
// allocated an Npc Element + stashed (eid, params) in t_pendingNpc, this POST
// reads the returned AActor* off the params frame and binds it into the
// Element. Also publishes the actor -> id reverse lookup so the K2_DestroyActor
// PRE can find the Element when the engine destroys the NPC.
//
// PARAMS-CORRELATION GATE (audited 2026-05-28): POST fires for EVERY caller
// of BeginDeferredSpawnFromClass (prop spawns, UI widgets, anything nested
// inside an NPC's constructor). The earlier class-match guard was buggy: it
// short-circuited on null spawnedActor, letting a failed non-NPC nested
// spawn consume the outer NPC's pending eid and drain its Element. The
// params pointer is the unique correlation token (engine allocates a fresh
// frame per call), so we check params equality and skip everything if this
// POST is not the matching PRE's POST. Inner non-NPC calls (failed or not)
// have different params, so they leave t_pendingNpc alone for the enclosing
// NPC POST to find.
void NpcSpawn_POST(void* /*self*/, void* /*function*/, void* params) {
    if (!params || g_npcSpawnReturnParamOff < 0) return;
    // Params correlation: only consume t_pendingNpc when the params pointer
    // matches the PRE's. Non-NPC POSTs (different params) return early.
    if (t_pendingNpc.paramsPtr != params) {
        // Diagnostic for the nested-NPC leak case: if t_pendingNpc has a
        // pending eid AND paramsPtr is non-null but doesn't match, an inner
        // NPC POST stole the slot from an outer NPC PRE. Outer Element is
        // now orphaned in NpcMirrors() (no actor, no g_actorToNpcId
        // entry). Currently believed
        // unreachable in VOTV's spawn paths (NPCs come from purchase or
        // events, not from other NPCs' constructors), but log so we'd see
        // it if it ever fires.
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
        // Spawn failed (engine returned null). Drain the Element from the
        // canonical owner (NpcMirrors) and DEFER its destruction to the
        // game-thread ElementDeleter Flush: this POST fires on a parallel-anim
        // worker per game_thread.h:118-120, so Enqueue moves the actual
        // ~Npc/FreeId to net_pump::Tick's controlled drain point instead of
        // running it on the worker. Take returns null if the element was
        // already drained (OnDisconnect race); Enqueue(null) is a no-op.
        coop::element::RetireMirror(eid);
        UE_LOGW("npc-sync[host POST]: BeginDeferredSpawn returned null for eid=%u; "
                "released Element back to Registry (deferred to ElementDeleter)", eid);
        return;
    }
    auto* el = coop::element::Registry::Get().Get(eid);
    if (!el) {
        // Race: OnDisconnect drained NpcMirrors() between this thread's
        // PRE and POST. The Element was destroyed + its id freed. The
        // engine's NPC actor (spawnedActor) is now an ORPHAN: live in
        // the world, no Element, never in g_actorToNpcId, so K2_DestroyActor
        // PRE won't broadcast EntityDestroy. Acceptable for the disconnect
        // case (client mirrors are torn down by client's own disconnect
        // handler), but log loudly so this is visible. Future: explicit
        // orphan-cleanup hook on disconnect that walks live actors of
        // allowlisted classes and either re-binds or destroys them.
        UE_LOGW("npc-sync[host POST]: eid=%u not in Registry (disconnect-race?) -- "
                "actor %p is ORPHANED (will not broadcast EntityDestroy on death)",
                eid, spawnedActor);
        return;
    }
    // Deferred-delete gate (review-fix 2026-05-30, finding 9): Registry::Get can
    // return a NON-null element that a K2_DestroyActor PRE already Take+Enqueued
    // into the ElementDeleter (out of NpcMirrors, but still in Registry until the
    // deferred FreeId at Flush). Binding an actor to such a doomed element would
    // resurrect a half-dead Element the deleter is about to destroy. Honor the
    // IsBeingDeleted() flag the ElementDeleter sets on Enqueue -- this is the
    // resolve-site gate the deferred-delete design (element_deleter.h) relies on.
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
    // K-4b (kerfur redesign): a fresh kerfur NPC (dev-spawn / purchase via the interceptor) gets its
    // stable host-range KerfurId reserved at first sighting -- idempotent per actor, host-only inside
    // AllocKerfurId. (Conversion turn-on NPCs spawn via EX_CallMath and never fire this POST;
    // RegisterHostNpcSilent + BindFormActor own those + reuse the dying form's K.) The g_actorToNpcId
    // leaf lock is released above, so AllocKerfurId's g_mutex->Registry order never nests with it.
    if (void* spawnedCls = R::ClassOf(spawnedActor)) {
        if (coop::kerfur_entity::IsKerfurClass(spawnedCls)) {
            coop::kerfur_entity::AllocKerfurId(spawnedActor, eid, coop::kerfur_entity::Form::Npc,
                                               R::ToString(R::NameOf(spawnedCls)));
        }
    }
}

// K2_DestroyActor PRE observer: fires for EVERY actor destroy in the world.
// The O(1) hash lookup on g_actorToNpcId IS the gate -- non-NPC destroys hit
// the lock briefly then return. Hits look up the owning Npc Element, Take it
// from NpcMirrors() and DEFER its destruction via the ElementDeleter (the
// actual ~Npc/FreeId runs at the game-thread Flush, off this worker thread),
// and broadcast an EntityDestroy reliable packet.
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
    // Drain the Element from the canonical owner (NpcMirrors) and DEFER its
    // destruction to the game-thread ElementDeleter Flush. This PRE fires on
    // the ProcessEvent-dispatching thread (often a parallel-anim worker per
    // game_thread.h:118-120); Enqueue moves the actual ~Npc/FreeId off the
    // worker to net_pump::Tick's one controlled drain point. g_actorToNpcIdMutex
    // is released above, so Take's type mutex and the deferred FreeId's Registry
    // mutex never nest with it. Take returns null on an already-drained eid
    // (double K2 / OnDisconnect race); Enqueue(null) is a no-op.
    coop::element::RetireMirror(eid);
    UE_LOGI("npc-sync[host destroy PRE]: actor=%p Npc eid=%u released (deferred)", self, eid);
    // Broadcast EntityDestroy so client mirrors tear down their copy.
    // Client-side receiver materialization lands in a future PR; for now
    // the destroy packet is just logged on the wire side.
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
    // HOSTING-gated tracking (RULE 1 root fix 2026-07-05, the 0s pyramid failure): the host
    // branch tracks with or without peers -- an event creature spawned while the host is
    // alone must be enrolled so the join connect-snapshot can deliver it. Only the
    // EntitySpawn broadcast below is connected-gated; the client suppress branch still
    // requires a live connection.
    if (!s) return false;

    // Phase 5N1 Inc2: HOST-side broadcast path. When env var is set and
    // ActorClass is allowlisted, the host emits an EntitySpawn reliable
    // packet and lets the spawn proceed normally. The interceptor runs
    // BEFORE the original UFunction; we read the SpawnTransform from
    // params before the spawn actually happens. Returns FALSE so the
    // original runs (host wants the NPC to actually spawn locally).
    //
    // Lifecycle shape (all implemented):
    //   - POST observer tracks the returned AActor* in g_actorToNpcId
    //   - K2_DestroyActor PRE sends EntityDestroy
    //   - client-side receiver materializes a mirror via
    //     MarkIncomingNpcSpawn + BeginDeferred + FinishSpawning
    if (s->role() == coop::net::Role::Host) {
        // Lifecycle gate: if either lifecycle observer permanently failed
        // to register, skip the whole host-side
        // sync path. Allocating an Element without a guaranteed POST
        // (which binds the actor pointer) or K2_DestroyActor PRE (which
        // releases the Element) would just leak Elements + leave the
        // client with orphan mirrors. Better to log once and degrade
        // gracefully -- the NPC still spawns locally, it just doesn't
        // sync to peers this session.
        if (g_npcSyncDisabledThisProcess.load(std::memory_order_acquire)) return false;

        void* actorClass = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(params) + g_npcSpawnActorClassParamOff);
        if (!actorClass || !IsClassOrDerivedFromAnyAllowlisted(actorClass)) {
            // rng_roll_census channel (a), HOST half: a BP deferred spawn we do NOT track.
            coop::dev::rng_roll_census::NotePassThrough(actorClass, /*isHostRole=*/true);
            return false;  // not an NPC; let it run, no broadcast
        }
        // Read the SpawnTransform param. Offset resolved ONCE at Install()
        // time -- the prior function-local-static check-then-write was a
        // data race on parallel-anim worker threads (audited 2026-05-28).
        coop::net::EntitySpawnPayload p{};
        // v75: a RUNTIME interceptor spawn fires AFTER the client's save-load is done, so the
        // client has no local twin for it -> savePersisted stays 0 -> the client fresh-spawns a
        // mirror. (Save-persisted NPCs reach the client via the world-enum/connect-snapshot path,
        // which sets savePersisted=1.) Explicit for intent; p{} already zero-inits it.
        p.savePersisted = 0;
        if (g_npcSpawnXformParamOff >= 0) {
            // FTransform layout: FQuat Rotation (16B) + FVector Translation (12B)
            // + FVector Scale3D (12B) -- 48 bytes total, but UE4 also aligns to
            // 16 so it's typically 48 with internal padding.
            const uint8_t* xform = reinterpret_cast<const uint8_t*>(params) + g_npcSpawnXformParamOff;
            // FQuat (XYZW)
            const float qx = *reinterpret_cast<const float*>(xform + 0);
            const float qy = *reinterpret_cast<const float*>(xform + 4);
            const float qz = *reinterpret_cast<const float*>(xform + 8);
            const float qw = *reinterpret_cast<const float*>(xform + 12);
            // FVector translation @ +0x10 (16; after the 16-byte FQuat).
            p.locX = *reinterpret_cast<const float*>(xform + 0x10);
            p.locY = *reinterpret_cast<const float*>(xform + 0x14);
            p.locZ = *reinterpret_cast<const float*>(xform + 0x18);
            // v99: FVector Scale3D @ +0x20 -- carried so a scaled spawn mirrors at true size (the
            // piramid WA case proved the class; NPC spawners pass 1.0 today but the wire is honest).
            p.scaleX = *reinterpret_cast<const float*>(xform + 0x20);
            p.scaleY = *reinterpret_cast<const float*>(xform + 0x24);
            p.scaleZ = *reinterpret_cast<const float*>(xform + 0x28);
            // Convert FQuat -> FRotator (in degrees) for wire compatibility
            // with our existing FRotator-based pose pipeline. Standard UE4
            // conversion: pitch = asin(2(wy - zx)); yaw = atan2(2(wz + xy),
            // 1 - 2(yy + zz)); roll = atan2(2(wx + yz), 1 - 2(xx + yy)).
            // We use a single normalize for sin clamp.
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
        // Class name -> wire string. actorClass IS a UClass*; NameOf returns
        // its own FName (e.g., "npc_zombie_C"). (ClassNameOf would return the
        // class-of-the-class, i.e., "Class" -- wrong.)
        const std::wstring cls = R::ToString(R::NameOf(actorClass));
        p.className.len = 0;
        for (size_t i = 0; i < cls.size() && i < 63; ++i) {
            p.className.data[p.className.len++] = static_cast<char>(cls[i]);
        }
        // Tier 3 PoC (2026-05-28, per
        // research/findings/mta/votv-mta-cclientelement-audit-2026-05-28.md):
        // allocate an Npc Element via the unified Registry instead of the
        // retired g_nextNpcSessionId atomic. The wire field `sessionId` keeps
        // its name through this PoC (rename to `elementId` lands at the v12
        // protocol bump per audit section 4.5); the value semantics shift to
        // "Registry-allocated id from the host range [0, 32768)".
        auto npc = std::make_unique<coop::element::Npc>();
        std::string typeName8;
        for (size_t i = 0; i < cls.size() && i < 63; ++i) {
            typeName8.push_back(static_cast<char>(cls[i]));
        }
        npc->SetTypeName(std::move(typeName8));
        // PR-FOUNDATION-3 Inc2: AllocAndInstall into the canonical NpcMirrors()
        // (host range, m_mirror=false). Replaces the prior AllocHostId + bespoke
        // g_npcElements emplace -- ONE owner now. The manager takes the Registry
        // mutex (AllocHostId) then its own type mutex (the host-authoritative
        // order documented in mirror_manager.h). On failure `npc` is dropped.
        const coop::element::ElementId eid =
            NpcMirrors().AllocAndInstall(std::move(npc), /*isHost=*/true);
        if (eid == coop::element::kInvalidId) {
            // Registry exhausted (32768 active elements) or id collision. Log +
            // skip the wire broadcast.
            UE_LOGW("npc-sync[host]: NpcMirrors().AllocAndInstall returned kInvalidId for '%ls' "
                    "-- skipping EntitySpawn broadcast (Registry exhausted / lifecycle bug?)",
                    cls.c_str());
            return false;
        }
        p.elementId = static_cast<uint32_t>(eid);
        // Stash (eid, params) for the matching POST observer (same thread,
        // same UFunction call). Params pointer is the unique correlation
        // token -- POST checks equality to disambiguate nested non-NPC calls.
        t_pendingNpc = {eid, params};
        UE_LOGI("npc-sync[host]: tracked EntitySpawn class='%ls' elementId=%u loc=(%.0f, %.0f, %.0f) rot=(p=%.1f y=%.1f r=%.1f)",
                cls.c_str(), p.elementId, p.locX, p.locY, p.locZ,
                p.rotPitch, p.rotYaw, p.rotRoll);
        // Alone-host spawns are delivered by the join connect-snapshot instead.
        if (s->connected() && !s->SendEntitySpawn(p)) {
            UE_LOGW("npc-sync[host]: SendEntitySpawn failed (reliable channel busy?) -- NPC elementId=%u not broadcast",
                    p.elementId);
        }
        // ALWAYS let the original spawn proceed on the host. Returning
        // false from the interceptor = pass-through to the real UFunction.
        return false;
    }

    // Host runs spawners normally; only a CONNECTED client suppresses (pre-connect: don't filter).
    if (s->role() != coop::net::Role::Client || !s->connected()) return false;

    // Read the ActorClass UClass* param from the FFrame buffer.
    void* actorClass = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(params) + g_npcSpawnActorClassParamOff);
    if (!actorClass) return false;  // BP bug or default; let UE4 handle

    // Audit-fix C2 (2026-05-25): bypass slot for wire-received NPC spawns.
    // Inc2's client-side dispatcher will set g_incomingNpcSpawnClass = cls
    // immediately before calling BeginDeferredActorSpawnFromClass to
    // materialize a host-streamed NPC. We consume the slot here and allow
    // the original through. Single-shot: cleared on consume so a stray
    // local spawn of the same class doesn't accidentally pass through.
    //
    // Atomic read-and-clear pattern: load + compare_exchange so we get
    // exactly one consumer even when
    // the interceptor fires on a parallel-anim worker thread concurrently
    // with the game-thread OnEntitySpawn setter. compare_exchange clears
    // the slot only when its value equals actorClass -- so an unrelated
    // local spawn fired between SET and our read leaves the slot intact
    // for the matching wire-spawn that follows.
    void* expected = actorClass;
    if (g_incomingNpcSpawnClass.compare_exchange_strong(
            expected, nullptr,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        UE_LOGI("npc-suppress[client]: allow-through wire-received spawn for class=%p (bypass slot consumed)",
                actorClass);
        return false;
    }

    // Subclass-aware allowlist match. See IsClassOrDerivedFromAnyAllowlisted.
    if (IsClassOrDerivedFromAnyAllowlisted(actorClass)) {
        UE_LOGI("npc-suppress[client]: skipping BeginDeferredActorSpawnFromClass for class=%p (matches NPC allowlist by hierarchy walk)",
                actorClass);
        // Zero the AActor* return so the BP graph receives nullptr
        // and bails. UE4's K2Node_SpawnActorFromClass emits a null-check
        // before the FinishSpawningActor call per UE4 engine convention --
        // NOT YET IDA-confirmed on VOTV spawners; the RE TODO in
        // research/findings/npc-creatures/votv-npc-sync-prereqs-RE-2026-05-24.md
        // section 4 (lines 559-563) tracks this. Per UE4 standard
        // emission patterns the risk is low, but if a specific VOTV
        // spawner BP emits non-null-checking code, we'd see a
        // FinishSpawningActor(nullptr, ...) crash here.
        if (g_npcSpawnReturnParamOff >= 0) {
            *reinterpret_cast<void**>(
                reinterpret_cast<uint8_t*>(params) + g_npcSpawnReturnParamOff) = nullptr;
        }
        return true;  // SKIP the original
    }
    // spawn_authority TRIPWIRE (shipping, always-on): a t1 park-class SPAWNER spawning on a
    // connected client -- log-only regression alarm + the late-instance signal (the reconcile
    // walk parks it). Pre-resolved pointer compares only (this thread may be a parallel-anim
    // worker).
    coop::spawn_authority::NoteClientSpawnPassThrough(actorClass);
    // rng_roll_census channel (a), CLIENT half: the silent pass-through -- a connected client is
    // about to run a BP deferred spawn our suppression does not cover (the exact set the T1
    // structural-vs-allowlist fork adjudicates on). Name-agnostic; no-op unless the [dev] ini
    // flag is on. MIRROR-SPAWN EXCLUSION (audit 2026-07-10 F-6): our own wire-mirror spawns
    // (owner-entity / ambient) re-enter this interceptor on the game thread -- censusing them
    // as client ROLLS would inflate exactly the data the T1 fork adjudicates on. The scope is
    // GT-only state, so consult it only from the GT (worker dispatches are never our mirrors).
    if (!(ue_wrap::game_thread::IsGameThread() &&
          coop::prop_echo_suppress::InMirrorSpawnScope()))
        coop::dev::rng_roll_census::NotePassThrough(actorClass, /*isHostRole=*/false);
    return false;  // not an NPC; let it run
}

void SetSession(coop::net::Session* session) {
    g_session_ptr.store(session, std::memory_order_release);
}

void MarkIncomingNpcSpawn(void* npcClass) {
    // Release ordering pairs with the interceptor's compare_exchange acquire,
    // so the actor-class pointer + everything written before this store is
    // visible to the consuming thread.
    g_incomingNpcSpawnClass.store(npcClass, std::memory_order_release);
}

void OnDisconnect() {
    // PR-FOUNDATION-3 Inc2 (2026-05-30): the host's Npc Elements now live in the
    // canonical NpcMirrors() (the bespoke g_npcElements is retired, RULE 2), so
    // releasing them is UNIFIED with the client-mirror drain. One call --
    // npc_mirror::DrainClientMirrors() -- drains the single MirrorManager<Npc>,
    // K2_DestroyActor'ing ONLY the IsMirror() elements (client puppet mirrors)
    // and release-only for the host's real NPC Elements (m_mirror=false -> dtor
    // FreeId; the real NPC actors stay in the host's world). Role-correct via
    // the IsMirror() flag, since a process holds host XOR client Npc elements.
    //
    // ORDER MATTERS (review-fix 2026-05-30, npc-migration-review finding 1):
    // DRAIN FIRST, then clear the reverse map. DrainClientMirrors destroys the
    // host Elements -> Registry::Get(eid) becomes null. A NpcSpawn_POST that
    // races on a parallel-anim worker concurrently with this can resolve+bind an
    // in-flight eid and RE-INSERT into g_actorToNpcId; clearing the reverse map
    // AFTER the drain removes any such stale entry, and a POST landing after the
    // drain hits its existing null-Registry "disconnect-race" guard. (The old
    // clear-then-drain order left a window where the re-inserted entry survived,
    // pointing at a freed eid into the next session.) Any Npc already Taken by a
    // K2_DestroyActor PRE before this sits in the ElementDeleter queue (drained
    // separately at net_pump::Tick) and is NOT in NpcMirrors -- no double-drain.
    //
    // K2_DestroyActor (inside DrainClientMirrors, for client mirrors only) calls
    // ProcessEvent which is GAME-THREAD ONLY. OnDisconnect's caller is on the
    // game thread (Session::Stop fires from the net-pump path dispatched from
    // the game thread; the smoke path likewise). If a future path calls
    // OnDisconnect off-thread, that K2_DestroyActor would assert.
    coop::npc_mirror::DrainClientMirrors();
    {
        std::lock_guard<std::mutex> lk(g_actorToNpcIdMutex);
        g_actorToNpcId.clear();
    }

    g_incomingNpcSpawnClass.store(nullptr, std::memory_order_release);
    // 2026-07-03: drop queued-but-undrained EX-spawn catches -- a stale address must never
    // drain into the next session (perf audit W1).
    coop::npc_world_enum::ClearPendingExSpawns();
}

// QueueConnectBroadcastForSlot + TickPoseStream (host-side NPC transform egress)
// -> coop/npc_pose_host.cpp (extracted 2026-06-07 per the 800-LOC soft cap). They
// share this TU's NpcMirrors() singleton + the GetSession() accessor; the receiver
// drive lives in npc_mirror.cpp.

// Install (the staged reflection resolve + observer/interceptor registration), IsInstalled,
// GetDevSpawnRefs, and IsHostNpcSyncDisabled were EXTRACTED to npc_sync_install.cpp 2026-07-19
// (s28 modular cut). The install-owned state (g_installed latch, resolve caches, observer
// latches, g_npcSpawnFn) moved with them; the five shared globals the hot paths here read
// (3 param offsets + g_npcAllowlist + g_npcSyncDisabledThisProcess) are DEFINED there and
// extern-declared in npc_sync_internal.h.

// Public accessors used by coop::npc_mirror to share state without a
// second source of truth (M-1 2026-05-29 split). Each one wraps a
// single internal global; npc_mirror reads through these instead of
// keeping its own copy. See coop/npc_mirror.cpp for callers.

coop::net::Session* GetSession() {
    return g_session_ptr.load(std::memory_order_acquire);
}

bool IsAllowlistedClass(void* cls) {
    // Same subclass-aware walk used by the host interceptor. Returns
    // false when cls is null OR not derived from any of the 12 NPC
    // bases. The allowlist may not be fully resolved yet (Install
    // gates the interceptor install until all 12 bind, but receivers
    // can fire from a fast-handshake peer while the host is still
    // resolving) -- in that case the unresolved slots are null,
    // IsDescendantOfAny skips them, and the check effectively becomes
    // "match any of the resolved subset". That's strictly tighter
    // (false rejects) until the allowlist completes, which is the
    // safer failure mode.
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
    // The K2_DestroyActor PRE body, callable for destroys that PRE cannot see
    // (BP-internal by-name K2_DestroyActor -- kerfurOmega.dropKerfurProp). The
    // body uses `actor` as a MAP KEY only (find/erase + Take(eid) +
    // SendEntityDestroy) -- never dereferences it -- so it is safe on a
    // PendingKill or even GC-purged pointer, and a double call (or a call
    // racing the real PRE) no-ops on the map miss.
    NpcDestroy_PRE(actor, nullptr, nullptr);
}

void SyncDestroyedNpcByEid(coop::element::ElementId eid, void* actorKey) {
    // The pose-walk dead-actor retire (2026-07-03, wisp self-despawn: the wisp's
    // K2_DestroyActor is an EX_VirtualFunction SELF-call the PRE observer never sees).
    // EID-keyed, unlike SyncDestroyedNpcActor: the by-actor variant resolves through the
    // reverse map, which a same-address actor-slot REBIND may have overwritten -- keying by
    // eid can never retire a different NPC. The map entry is erased only if it still maps
    // to THIS eid. `actorKey` is a map key only, never dereferenced.
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
    // See npc_sync.h. Same end state as NpcSpawn_POST's bind (Npc Element alloc + bound actor +
    // reverse-map entry so K2_DestroyActor PRE closes its lifecycle) but for an EX_CallMath-spawned
    // turn-on kerfur that never passed the interceptor -- and WITHOUT the wire EntitySpawn (the kerfur
    // conversion's sole signal is KerfurConvert). Game thread (the kerfur converge runs on it).
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
    // See npc_sync.h. NpcDestroy_PRE's teardown (Take from NpcMirrors + erase the actor reverse-map
    // entry + defer-destroy) MINUS the SendEntityDestroy. The kerfur converge releases the dying NPC
    // form this way; KerfurConvert carries oldEid so the clients tear down their mirror. Game thread.
    if (eid == coop::element::kInvalidId) return;
    std::unique_ptr<coop::element::Npc> drained = NpcMirrors().Take(eid);
    if (drained) {
        if (void* actor = drained->GetActor()) {
            std::lock_guard<std::mutex> lk(g_actorToNpcIdMutex);
            g_actorToNpcId.erase(actor);
        }
    }
    // Defer the ~Npc/FreeId to the game-thread ElementDeleter Flush (matches NpcDestroy_PRE). A null
    // drained (already-released eid) makes Enqueue a no-op.
    coop::element::ElementDeleter::Get().Enqueue(std::move(drained));
    // INVARIANT (2026-07-14): a silent release is safe ONLY if a KerfurConvert (carrying this eid as
    // oldEid) follows to tear down the client mirror. A release with NO following converge orphans the
    // client's NPC mirror -- and does so INVISIBLY on the client (no wire signal). That was the pre-2a
    // orphan-NPC half of the host-own turn_off race (six months, undiagnosed because this path never
    // reached the client). The sole-express converge now always fires, so the release is always paired.
    UE_LOGI("npc-sync[silent release]: Npc eid=%u released (no EntityDestroy broadcast -- MUST be paired "
            "with a KerfurConvert or the client mirror orphans)", static_cast<uint32_t>(eid));
}

void MapActorToNpcId(void* actor, coop::element::ElementId eid) {
    // Insert/overwrite the live-actor -> ElementId reverse-map entry (the same map GetNpcIdForActor
    // reads + NpcDestroy_PRE gates on). Used by the extracted world-enum (coop/npc_world_enum) to
    // register a pre-existing world NPC after it AllocAndInstalls + binds the Element -- reaching the
    // same end state the POST observer reaches for a fresh spawn. g_actorToNpcIdMutex is a LEAF
    // (never nested under the Registry/type mutex), matching the POST observer's lock discipline.
    if (!actor) return;
    std::lock_guard<std::mutex> lk(g_actorToNpcIdMutex);
    g_actorToNpcId[actor] = eid;
}

// RegisterExistingWorldNpcs (the HOST-only pre-existing world-NPC GUObjectArray walk) moved to
// coop/npc_world_enum.{h,cpp} (K-0 2026-06-16, 800-LOC soft cap). It consumes this TU's host-side
// state through the public accessors above (IsHostNpcSyncDisabled / MapActorToNpcId) + GetSession /
// IsInstalled / IsAllowlistedClass / GetNpcIdForActor + the shared NpcMirrors() singleton.

// Receiver impls (OnEntitySpawn / OnEntityDestroy) moved to
// coop/npc_mirror.cpp (M-1 2026-05-29 split).

}  // namespace coop::npc_sync
