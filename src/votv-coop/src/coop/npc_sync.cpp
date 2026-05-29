// coop/npc_sync.cpp -- Phase 5N1 NPC sync foundation (extracted 2026-05-25).
//
// See coop/npc_sync.h for the public interface.
// Architecture findings: research/findings/votv-npc-sync-prereqs-RE-2026-05-24.md.
//
// SPLIT (M-1 2026-05-29): the client-side receivers (OnEntitySpawn /
// OnEntityDestroy / DrainClientMirrors) moved to coop/npc_mirror.cpp.
// This TU now owns host-side PRE/POST + the suppress interceptor +
// resolution + lifecycle bookkeeping; the receivers consume resolved
// UFunction refs through npc_mirror::SetClientRefs() (called once from
// Install) and access the shared allowlist + session pointer + bypass
// slot through the public accessors GetSession / IsAllowlistedClass /
// MarkIncomingNpcSpawn / ClearIncomingNpcSpawn.

#include "coop/npc_sync.h"

#include "coop/element/npc.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/npc_mirror.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

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

// Idempotency: once installed (or permanently failed) we short-circuit.
// std::atomic for memory-model correctness -- Install runs on the game
// thread, the interceptor/observer paths can read this indirectly via
// other latches that depend on Install having completed.
std::atomic<bool> g_installed{false};

// 12 NPC UClass* pointers (zombie/kerfur/krampus/funguy/goreSlither/insomniacs/
// fossilhounds/antibreathers/orborbs + 3 ariral variants), resolved at install
// from the kNpcAllowlist names in sdk_profile.h. Sized by kNpcAllowlistSize
// so a future addition to the allowlist constant binds the array length too.
void* g_npcAllowlist[ue_wrap::profile::name::kNpcAllowlistSize] = {};

void* g_npcSpawnFn = nullptr;
int32_t g_npcSpawnActorClassParamOff = -1;
int32_t g_npcSpawnReturnParamOff = -1;
int32_t g_npcSpawnXformParamOff = -1;  // SpawnTransform; cached at Install time

// Install bookkeeping for the receiver-side cache (M-1 2026-05-29 split):
// the client-side UFunction refs LIVE in coop::npc_mirror via
// SetClientRefs(). These two locals are kept here only because the
// resolution happens in two stages (GameplayStatics primitives first,
// then AActor::K2_DestroyActor once the AActor class binds), and we
// need to push the FULL set together when k2DestroyFn lands. Receivers
// in npc_mirror DO NOT read these; the canonical copy is on the
// receiver side.
void* g_installCacheFinishSpawnFn = nullptr;
void* g_installCacheGsCdo = nullptr;

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

// CLIENT-side mirror manager (MirrorManager<Npc>) ownership migrated to
// coop::npc_mirror (M-1 2026-05-29 split). Lifetime owners on the
// client side: npc_mirror::OnEntitySpawn (populate), OnEntityDestroy
// (drain one), DrainClientMirrors (drain all on OnDisconnect).

// Host-side Npc Element ownership + lookup.
//
//   g_npcElements    -- owns the unique_ptr by ElementId. Created by the
//                       host-side PRE interceptor; destroyed by the
//                       K2_DestroyActor PRE observer when its actor dies,
//                       OR by OnDisconnect on session end. O(1) erase by id.
//   g_actorToNpcId   -- reverse lookup: live AActor* -> ElementId. Populated
//                       by the POST-spawn observer when it captures the
//                       BeginDeferredSpawn ReturnValue. K2_DestroyActor PRE
//                       fires for every actor destroy in the world; the
//                       O(1) hash lookup IS the gate for "is this an NPC
//                       we own?".
//
// Both maps share `g_npcElementsMutex`. The interceptor + POST observer +
// K2_DestroyActor PRE all run on parallel-anim worker threads per
// game_thread.h:118-120 (interceptor) and our own PRE observer dispatch.
//
// ABBA hazard with `coop::element::Registry::m_mutex`: the Npc destructor
// calls Registry::FreeId which acquires m_mutex. The interceptor path
// acquires m_mutex FIRST (via Registry::AllocHostId) then g_npcElementsMutex
// SECOND (via the insert). Any code that needs to destroy an Npc must
// (a) drain the unique_ptr out under g_npcElementsMutex, (b) release the
// lock, (c) let the destructor run -- so FreeId never holds both locks.
std::mutex g_npcElementsMutex;
std::unordered_map<coop::element::ElementId, std::unique_ptr<coop::element::Npc>> g_npcElements;
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

// K2_DestroyActor PRE observer handle bookkeeping (so a second Install()
// doesn't double-register). std::atomic so the host PRE interceptor can
// read these from a parallel-anim worker without UB while the game thread
// writes them in Install().
std::atomic<bool> g_destroyObserverInstalled{false};
std::atomic<bool> g_spawnPostObserverInstalled{false};
// True after Install permanently gave up registering one of the lifecycle
// observers (table full). PRE gates host-side EntitySpawn broadcasts on
// `!g_npcSyncDisabledThisProcess` so a partial-lifecycle install never
// leaks Npc Elements (allocated by PRE, never bound or destroyed).
std::atomic<bool> g_npcSyncDisabledThisProcess{false};

// Subclass-aware allowlist match: an exact-pointer match leaks all 30+ NPC
// subclasses (e.g. kerfurOmega has 20 subclasses incl. kerfurOmega_
// mannequinSpawner -- and npc_zombie has 14 subclasses, etc). Routed
// through ue_wrap::reflection::IsDescendantOfAny so the UStruct_SuperStruct
// offset stays in the wrapper layer (Principle 7); the helper walks the
// chain ONCE checking all allowlisted bases per hop.
bool IsClassOrDerivedFromAnyAllowlisted(void* cls) {
    return R::IsDescendantOfAny(cls, g_npcAllowlist, P::name::kNpcAllowlistSize);
}

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
        // now orphaned in g_npcElements (no actor, no g_actorToNpcId
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
        // Spawn failed (engine returned null). Drain the Element back to
        // the Registry -- mirrors the PRE allocation path with destructors
        // running OUTSIDE g_npcElementsMutex (ABBA fix).
        std::unique_ptr<coop::element::Npc> drained;
        {
            std::lock_guard<std::mutex> lk(g_npcElementsMutex);
            auto it = g_npcElements.find(eid);
            if (it != g_npcElements.end()) {
                drained = std::move(it->second);
                g_npcElements.erase(it);
            }
        }
        // drained destructor fires here, outside the lock -- safe FreeId.
        UE_LOGW("npc-sync[host POST]: BeginDeferredSpawn returned null for eid=%u; "
                "released Element back to Registry", eid);
        return;
    }
    auto* el = coop::element::Registry::Get().Get(eid);
    if (!el) {
        // Race: OnDisconnect drained g_npcElements between this thread's
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
    el->SetActor(spawnedActor, R::InternalIndexOf(spawnedActor));
    {
        std::lock_guard<std::mutex> lk(g_npcElementsMutex);
        g_actorToNpcId[spawnedActor] = eid;
    }
    UE_LOGI("npc-sync[host POST]: bound actor=%p to Npc eid=%u typeName='%s'",
            spawnedActor, eid, el->GetTypeName().c_str());
}

// K2_DestroyActor PRE observer: fires for EVERY actor destroy in the world.
// The O(1) hash lookup on g_actorToNpcId IS the gate -- non-NPC destroys hit
// the lock briefly then return. Hits look up the owning Npc Element, drain
// it from g_npcElements (so its destructor fires outside the lock per ABBA
// fix), and broadcast an EntityDestroy reliable packet.
void NpcDestroy_PRE(void* self, void* /*function*/, void* /*params*/) {
    if (!self) return;
    coop::element::ElementId eid = coop::element::kInvalidId;
    std::unique_ptr<coop::element::Npc> drained;
    {
        std::lock_guard<std::mutex> lk(g_npcElementsMutex);
        auto it = g_actorToNpcId.find(self);
        if (it == g_actorToNpcId.end()) return;  // not an NPC we track
        eid = it->second;
        g_actorToNpcId.erase(it);
        auto eit = g_npcElements.find(eid);
        if (eit != g_npcElements.end()) {
            drained = std::move(eit->second);
            g_npcElements.erase(eit);
        }
    }
    // drained destructor runs OUTSIDE g_npcElementsMutex -- the Element
    // destructor calls Registry::FreeId, which acquires Registry::m_mutex.
    // Releasing g_npcElementsMutex before this avoids the ABBA hazard.
    UE_LOGI("npc-sync[host destroy PRE]: actor=%p Npc eid=%u released", self, eid);
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
    if (!s || !s->connected()) return false;  // pre-connect: don't filter

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
            return false;  // not an NPC; let it run, no broadcast
        }
        // Read the SpawnTransform param. Offset resolved ONCE at Install()
        // time -- the prior function-local-static check-then-write was a
        // data race on parallel-anim worker threads (audited 2026-05-28).
        coop::net::EntitySpawnPayload p{};
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
        // research/findings/votv-mta-cclientelement-audit-2026-05-28.md):
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
        const coop::element::ElementId eid =
            coop::element::Registry::Get().AllocHostId(npc.get());
        if (eid == coop::element::kInvalidId) {
            // Registry exhausted (32768 active elements). Log + skip the
            // wire broadcast.
            UE_LOGW("npc-sync[host]: Registry::AllocHostId returned kInvalidId for '%ls' "
                    "-- skipping EntitySpawn broadcast (element lifecycle bug?)",
                    cls.c_str());
            return false;
        }
        p.elementId = static_cast<uint32_t>(eid);
        // Move ownership into g_npcElements keyed by ElementId; stash the
        // id in the thread-local pending-slot for the POST observer to
        // pick up and bind the returned AActor*.
        {
            std::lock_guard<std::mutex> lk(g_npcElementsMutex);
            g_npcElements.emplace(eid, std::move(npc));
        }
        // Stash (eid, params) for the matching POST observer (same thread,
        // same UFunction call). Params pointer is the unique correlation
        // token -- POST checks equality to disambiguate nested non-NPC calls.
        t_pendingNpc = {eid, params};
        if (s->SendEntitySpawn(p)) {
            UE_LOGI("npc-sync[host]: broadcast EntitySpawn class='%ls' elementId=%u loc=(%.0f, %.0f, %.0f) rot=(p=%.1f y=%.1f r=%.1f)",
                    cls.c_str(), p.elementId, p.locX, p.locY, p.locZ,
                    p.rotPitch, p.rotYaw, p.rotRoll);
        } else {
            UE_LOGW("npc-sync[host]: SendEntitySpawn failed (reliable channel busy?) -- NPC elementId=%u not broadcast",
                    p.elementId);
        }
        // ALWAYS let the original spawn proceed on the host. Returning
        // false from the interceptor = pass-through to the real UFunction.
        return false;
    }

    // Host runs spawners normally; only CLIENT suppresses.
    if (s->role() != coop::net::Role::Client) return false;

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
        // research/findings/votv-npc-sync-prereqs-RE-2026-05-24.md
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
    return false;  // not an NPC; let it run
}

}  // namespace

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
    // Tier 3 PoC 2026-05-28: release every host-allocated Npc Element back
    // to the Registry. The atomic g_nextNpcSessionId counter is retired
    // (RULE 2); the unified Registry's free-list replaces it.
    //
    // ABBA deadlock fix (audited 2026-05-28): the Npc destructors call
    // Registry::FreeId which acquires Registry::m_mutex. The interceptor
    // path acquires Registry::m_mutex FIRST (via AllocHostId) then
    // g_npcElementsMutex SECOND (via the push_back). Inverting that order
    // here would deadlock with a parallel-anim worker mid-spawn. Drain
    // into a local vector under g_npcElementsMutex, release the lock,
    // THEN let the destructors run -- Registry::m_mutex is acquired
    // without g_npcElementsMutex held.
    std::unordered_map<coop::element::ElementId, std::unique_ptr<coop::element::Npc>> drained;
    {
        std::lock_guard<std::mutex> lk(g_npcElementsMutex);
        drained.swap(g_npcElements);
        g_actorToNpcId.clear();  // pure POD; safe to clear under lock
    }
    const size_t nReleased = drained.size();
    drained.clear();  // destructors run here, OUTSIDE g_npcElementsMutex
    if (nReleased > 0) {
        UE_LOGI("npc-sync: OnDisconnect released %zu Npc element(s) back to Registry",
                nReleased);
    }

    // Inc3 receiver (client-side) drain: clean up every mirror Element +
    // K2_DestroyActor the local mirror actor. Implementation lives in
    // coop::npc_mirror; this TU only orchestrates the call ordering
    // (host elements drained above, then client mirrors, then bypass
    // slot clear).
    //
    // K2_DestroyActor calls ProcessEvent which is GAME-THREAD ONLY. The
    // caller of OnDisconnect is expected to be on the game thread already
    // (Session::Stop fires from harness's net-pump thread, which is
    // dispatched from the game thread; the smoke test path likewise calls
    // from GT). If a future path calls OnDisconnect off-thread, the
    // K2_DestroyActor call inside DrainClientMirrors would assert.
    coop::npc_mirror::DrainClientMirrors();

    g_incomingNpcSpawnClass.store(nullptr, std::memory_order_release);
}

void Install(coop::net::Session* session) {
    g_session_ptr.store(session, std::memory_order_release);  // cache (caller guarantees outlives us)
    if (g_installed.load(std::memory_order_acquire)) return;
    // THROTTLE GUARD (perf audit 2026-05-28, expanded): every Find* call below
    // walks GUObjectArray with wstring allocs per entry. Bound retries to
    // ~once per 0.5s during the unresolved window.
    static int s_installRetryCountdown = 0;
    if (s_installRetryCountdown > 0) {
        --s_installRetryCountdown;
        return;
    }
    // CACHE intermediate resolutions: once gsCls + fn + offsets are
    // resolved, skip them on subsequent retries
    // (partial NPC-class resolution would otherwise re-walk all five every
    // 0.5s tick until the 12 NPC classes finish loading).
    if (!g_npcSpawnFn) {
        void* gsCls = R::FindClass(P::name::GameplayStaticsClass);
        if (!gsCls) {
            s_installRetryCountdown = 60;
            return;
        }
        void* fn = R::FindFunction(gsCls, P::name::BeginDeferredSpawnFn);
        if (!fn) {
            UE_LOGW("npc-suppress: %ls.%ls UFunction not found -- disabled permanently",
                    P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn);
            g_installed.store(true, std::memory_order_release);
            return;
        }
        const int32_t classOff = R::FindParamOffset(fn, L"ActorClass");
        if (classOff < 0) {
            UE_LOGW("npc-suppress: %ls.%ls 'ActorClass' param not found (BP recook?) -- disabled",
                    P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn);
            g_installed.store(true, std::memory_order_release);
            return;
        }
        const int32_t retOff = R::FindParamOffset(fn, L"ReturnValue");
        if (retOff < 0) {
            UE_LOGW("npc-suppress: %ls.%ls 'ReturnValue' param not found -- disabled",
                    P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn);
            g_installed.store(true, std::memory_order_release);
            return;
        }
        const int32_t xformOff = R::FindParamOffset(fn, L"SpawnTransform");
        if (xformOff < 0) {
            UE_LOGW("npc-suppress: %ls.%ls 'SpawnTransform' param not found -- "
                    "EntitySpawn broadcasts will lack position/rotation",
                    P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn);
            // Don't bail -- position-less spawns still work.
        }
        // Inc3 receiver side (now in coop::npc_mirror): resolve
        // FinishSpawningActor + GameplayStatics CDO so OnEntitySpawn can
        // materialize mirrors without re-walking GUObjectArray on every
        // host broadcast. Non-fatal if missing (OnEntitySpawn null-checks
        // each field and logs).
        void* finishFn = R::FindFunction(gsCls, P::name::FinishSpawningActorFn);
        if (!finishFn) {
            UE_LOGW("npc-sync[receiver]: %ls.%ls UFunction not found -- client mirror "
                    "materialization will be disabled (host EntitySpawn packets will be "
                    "logged + dropped)",
                    P::name::GameplayStaticsClass, P::name::FinishSpawningActorFn);
        }
        void* gsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
        if (!gsCdo) {
            UE_LOGW("npc-sync[receiver]: %ls CDO not found -- client mirror materialization "
                    "will be disabled",
                    P::name::GameplayStaticsClass);
        }
        // Commit cache. Now subsequent retries (NPC-class partial-load) skip
        // the five Find* calls above and only retry the 12-class FindClass loop.
        g_npcSpawnFn = fn;
        g_npcSpawnActorClassParamOff = classOff;
        g_npcSpawnReturnParamOff = retOff;
        g_npcSpawnXformParamOff = xformOff;
        // Stash for the second push (where k2DestroyFn lands together);
        // also push the partial set NOW so the receiver can degrade
        // gracefully (drop with warning) if the install retries before
        // K2_DestroyActor resolves.
        g_installCacheFinishSpawnFn = finishFn;
        g_installCacheGsCdo         = gsCdo;
        coop::npc_mirror::ClientRefs refs{};
        refs.spawnFn             = fn;
        refs.finishSpawnFn       = finishFn;
        refs.gsCdo               = gsCdo;
        refs.spawnReturnParamOff = retOff;
        refs.k2DestroyFn         = nullptr;  // resolved + re-pushed below
        coop::npc_mirror::SetClientRefs(refs);
    }
    void* const fn = g_npcSpawnFn;
    const int32_t classOff = g_npcSpawnActorClassParamOff;
    const int32_t retOff = g_npcSpawnReturnParamOff;
    const int32_t xformOff = g_npcSpawnXformParamOff;

    // Resolve the 12 NPC classes. Partial-resolution OK: missing classes
    // just won't be suppressed. Most VOTV NPC BP classes are loaded with
    // /Game/Content/blueprints/npc/... -- present on gameplay-level entry.
    // Already-resolved entries skip FindClass via the `if (!g_npcAllowlist[i])`
    // cache; the unresolved-class FindClass walks are bounded by the outer
    // throttle gate (s_installRetryCountdown at the top of Install).
    size_t resolved = 0;
    for (size_t i = 0; i < P::name::kNpcAllowlistSize; ++i) {
        if (!g_npcAllowlist[i]) {
            g_npcAllowlist[i] = R::FindClass(P::name::kNpcAllowlist[i]);
        }
        if (g_npcAllowlist[i]) ++resolved;
    }
    if (resolved < P::name::kNpcAllowlistSize) {
        // Don't install yet -- want all 12 cached before going live (otherwise
        // some NPCs would be suppressed and others wouldn't, depending on
        // resolve timing). Throttle the next attempt.
        s_installRetryCountdown = 60;
        UE_LOGI("npc-suppress: NPC class load partial (%zu/%zu) -- throttled retry in ~0.5s",
                resolved, P::name::kNpcAllowlistSize);
        return;
    }

    // All 12 NPC classes resolved. Cache the function pointer + offsets.
    // Lifecycle observers go in FIRST -- if either RegisterX fails (observer
    // table full), we set g_npcSyncDisabledThisProcess and SKIP the
    // RegisterInterceptor call below, so we don't burn a permanent interceptor
    // slot for a system that can't function.
    g_npcSpawnFn = fn;
    g_npcSpawnActorClassParamOff = classOff;
    g_npcSpawnReturnParamOff = retOff;
    g_npcSpawnXformParamOff = xformOff;  // may be -1 if param missing; interceptor null-checks

    // ATOMIC two-observer registration: if EITHER lifecycle observer
    // registration fails, the other is rolled
    // back so we don't burn a permanent slot for a system that's disabled.
    // Pre-resolve K2_DestroyActor's reflection dependencies FIRST -- if they
    // fail, we skip POST entirely (no rollback needed).
    if (!g_spawnPostObserverInstalled.load(std::memory_order_acquire) ||
        !g_destroyObserverInstalled.load(std::memory_order_acquire)) {
        void* actorCls = R::FindClass(P::name::ActorClassName);
        void* destroyFn = actorCls ? R::FindFunction(actorCls, P::name::DestroyActorFn) : nullptr;
        if (!actorCls || !destroyFn) {
            g_npcSyncDisabledThisProcess.store(true, std::memory_order_release);
            UE_LOGE("npc-sync: cannot resolve %ls.%ls (actorCls=%p destroyFn=%p) -- NPC sync "
                    "DISABLED for process lifetime (Element lifecycle cannot close)",
                    P::name::ActorClassName, P::name::DestroyActorFn, actorCls, destroyFn);
        } else {
            // Promote K2_DestroyActor into the Inc3 receiver-side cache
            // owned by coop::npc_mirror (OnEntityDestroy reuses it on
            // every host teardown -- skipping the GUObjectArray walk per
            // packet). Re-push the full ClientRefs so the receiver sees
            // spawnFn/finishFn/gsCdo/returnOff (cached during the
            // GameplayStatics resolution block above) together with the
            // newly resolved k2DestroyFn.
            {
                coop::npc_mirror::ClientRefs refs{};
                refs.spawnFn             = g_npcSpawnFn;
                refs.finishSpawnFn       = g_installCacheFinishSpawnFn;
                refs.gsCdo               = g_installCacheGsCdo;
                refs.spawnReturnParamOff = g_npcSpawnReturnParamOff;
                refs.k2DestroyFn         = destroyFn;
                coop::npc_mirror::SetClientRefs(refs);
            }
            // POST observer first; if it succeeds, register K2_DestroyActor PRE.
            // If K2 fails, roll back the POST so a half-installed state doesn't
            // burn an observer-table slot.
            const bool postOk = ue_wrap::game_thread::RegisterPostObserver(fn, &NpcSpawn_POST);
            if (!postOk) {
                g_npcSyncDisabledThisProcess.store(true, std::memory_order_release);
                UE_LOGE("npc-sync: RegisterPostObserver FAILED (observer table full) -- "
                        "NPC sync DISABLED for process lifetime");
            } else if (!ue_wrap::game_thread::RegisterPreObserver(destroyFn, &NpcDestroy_PRE)) {
                // K2 failed after POST succeeded -- roll back POST so we don't
                // leave it firing forever for a disabled system.
                ue_wrap::game_thread::UnregisterObservers(fn);
                g_npcSyncDisabledThisProcess.store(true, std::memory_order_release);
                UE_LOGE("npc-sync: RegisterPreObserver FAILED for K2_DestroyActor "
                        "(observer table full) -- NPC sync DISABLED + rolled back POST "
                        "registration to free its slot");
            } else {
                g_spawnPostObserverInstalled.store(true, std::memory_order_release);
                g_destroyObserverInstalled.store(true, std::memory_order_release);
                UE_LOGI("npc-sync: registered POST observer for %ls.%ls (binds AActor* into Npc Element)",
                        P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn);
                UE_LOGI("npc-sync: registered K2_DestroyActor PRE observer (Npc Element lifecycle close)");
            }
        }
    }

    // Register the PRE interceptor LAST and only if the lifecycle observers
    // both succeeded. RegisterInterceptor consumes a slot in the 16-slot
    // interceptor table -- if NPC sync is disabled for the session, we leave
    // the slot free for other subsystems.
    // The client-side suppression that the interceptor implements is also
    // useless without the host-side broadcast pipeline being functional.
    g_installed.store(true, std::memory_order_release);
    if (g_npcSyncDisabledThisProcess.load(std::memory_order_acquire)) {
        UE_LOGW("npc-suppress: lifecycle observer install FAILED -- skipping interceptor "
                "registration entirely (NPC sync disabled for process lifetime; see prior "
                "[Error] lines)");
        return;
    }
    ue_wrap::game_thread::RegisterInterceptor(fn, &NpcSuppress_Interceptor);
    UE_LOGI("npc-suppress: installed interceptor on %ls.%ls @ %p (ActorClass@%d, ReturnValue@%d, SpawnTransform@%d, 12/12 NPC classes resolved + lifecycle observers live)",
            P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn,
            fn, classOff, retOff, xformOff);
    for (size_t i = 0; i < P::name::kNpcAllowlistSize; ++i) {
        UE_LOGI("npc-suppress: allowlist[%zu] '%ls' = %p",
                i, P::name::kNpcAllowlist[i], g_npcAllowlist[i]);
    }
}

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

// Receiver impls (OnEntitySpawn / OnEntityDestroy) moved to
// coop/npc_mirror.cpp (M-1 2026-05-29 split).

}  // namespace coop::npc_sync
