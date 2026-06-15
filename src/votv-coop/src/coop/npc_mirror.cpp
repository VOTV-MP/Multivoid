// coop/npc_mirror.cpp -- Inc3 client-side NPC mirror materialization
// (extracted from npc_sync.cpp M-1 2026-05-29).
//
// See coop/npc_mirror.h for the public interface + dependency notes.
//
// Behavior preserved byte-for-byte from the prior in-line implementation
// in npc_sync.cpp: same validation gates, same allowlist routing through
// npc_sync::IsAllowlistedClass, same MirrorManager<Npc> Install/Take/
// DrainAll pattern, same ABBA-safe destruction ordering (drain-then-
// destruct outside the manager mutex).

#include "coop/npc_mirror.h"

#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/npc_adoption.h"  // v75: deferred class-match adoption for save-persisted NPCs
#include "coop/remote_prop_spawn.h"  // TickClientReconcile (deferred prop divergence sweep)
#include "coop/npc_sync.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/kerfur.h"  // thorough park (NeutralizeAiTimers) for fresh-spawn mirrors
#include "ue_wrap/log.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace coop::npc_mirror {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// Client-side UFunction cache. Populated by SetClientRefs() once
// npc_sync::Install resolves all of these together. Plain non-atomic
// storage is safe because SetClientRefs and the receivers all run on
// the game thread (event_feed dispatches via game_thread::Post).
void*   g_spawnFn             = nullptr;
void*   g_finishSpawnFn       = nullptr;
void*   g_gsCdo               = nullptr;
int32_t g_spawnReturnParamOff = -1;
void*   g_k2DestroyFn         = nullptr;

inline coop::element::MirrorManager<coop::element::Npc>& NpcMirrors() {
    return coop::element::MirrorManager<coop::element::Npc>::Instance();
}

// v75 (supersedes v74's key-adoption -- the kerfur int_save key is RANDOM per peer, bytecode-
// proven, so key-equality could never match): a save-persisted NPC the joining client loaded
// from the transferred save (savePersisted=1) is ADOPTED by class-match via a DEFERRED POLL in
// coop/npc_adoption (the local twin spawns via un-hookable EX_CallMath at an unpredictable time,
// so we poll for it). OnEntitySpawn hands save-persisted spawns to npc_adoption::ArmAdoption;
// only host-spawned transients (savePersisted=0) fresh-spawn here via SpawnFreshNpcMirror.
// The post-snapshot ghost sweep (DestroyUntrackedClientNpcs) is triggered by npc_adoption once the
// connect snapshot is delivered (SnapshotComplete) AND every pending adoption has converged.

}  // namespace

void SetClientRefs(const ClientRefs& refs) {
    g_spawnFn             = refs.spawnFn;
    g_finishSpawnFn       = refs.finishSpawnFn;
    g_gsCdo               = refs.gsCdo;
    g_spawnReturnParamOff = refs.spawnReturnParamOff;
    g_k2DestroyFn         = refs.k2DestroyFn;
    // v75: the adoption module needs NO UFunction refs of its own -- its bind path only parks
    // (DisableCharacterTicks + NeutralizeAiTimers), its timeout fallback re-enters
    // npc_mirror::SpawnFreshNpcMirror, and its ghost sweep re-enters DestroyUntrackedClientNpcs;
    // all three read npc_mirror's own globals above.
}

void OnEntitySpawn(const coop::net::EntitySpawnPayload& payload) {
    using ue_wrap::ParamFrame;
    using ue_wrap::Call;
    auto* s = coop::npc_sync::GetSession();
    if (!s) return;
    // Defensive host-side echo guard: host never receives its own broadcasts
    // (lane fan-out is to peer slots only), but if it somehow did the
    // materialization would create a duplicate actor adjacent to the
    // original. Drop.
    if (s->role() == coop::net::Role::Host) {
        UE_LOGI("npc-sync[client OnSpawn]: received on host -- dropping (loopback bounce)");
        return;
    }
    // PR-FOUNDATION-1 (2026-05-29): EntitySpawn is host-authoritative
    // (the senderPeerSlot==0 gate at event_feed::EntitySpawn already
    // rejected non-host senders); the eid must be in the host range.
    // Canonical helper replaces the inline check that this site had
    // first (the audit's reference implementation -- D2-1 generalises
    // it across all receivers).
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(payload.elementId)) {
        UE_LOGW("npc-sync[client OnSpawn]: elementId=%u out of allowed host range "
                "[1, %u) -- dropping",
                payload.elementId, coop::element::kHostRangeSize);
        return;
    }
    // Wire-string class name -> wstring.
    if (payload.className.len == 0 || payload.className.len > 63) {
        UE_LOGW("npc-sync[client OnSpawn]: bad className.len=%u -- dropping",
                payload.className.len);
        return;
    }
    std::wstring classW;
    classW.reserve(payload.className.len);
    for (uint8_t i = 0; i < payload.className.len; ++i) {
        classW.push_back(static_cast<wchar_t>(static_cast<unsigned char>(payload.className.data[i])));
    }
    // Trust-boundary: validate floats finite + within bounds (same magnitude
    // rule as PropSpawn receiver / coop::net::kMaxCoord).
    const float vals[6] = {payload.locX, payload.locY, payload.locZ,
                           payload.rotPitch, payload.rotYaw, payload.rotRoll};
    for (float v : vals) {
        if (!std::isfinite(v)) {
            UE_LOGW("npc-sync[client OnSpawn]: non-finite float in payload -- dropping (eid=%u)",
                    payload.elementId);
            return;
        }
    }
    if (std::fabs(payload.locX) > coop::net::kMaxCoord ||
        std::fabs(payload.locY) > coop::net::kMaxCoord ||
        std::fabs(payload.locZ) > coop::net::kMaxCoord) {
        UE_LOGW("npc-sync[client OnSpawn]: loc out of bounds (%.0f, %.0f, %.0f) -- dropping (eid=%u)",
                payload.locX, payload.locY, payload.locZ, payload.elementId);
        return;
    }
    // Duplicate-eid guard: already mirroring this id? Should not happen
    // (host allocates each id exactly once across its lifetime, and the
    // reliable channel doesn't redeliver), but if it does we'd duplicate
    // the actor -- drop the late one. Note: this is a non-locked early
    // exit; a concurrent duplicate that races past this check is caught
    // by the Install false-return path below (the orphan actor gets
    // K2_DestroyActor'd before this function returns).
    if (NpcMirrors().Get(payload.elementId) != nullptr) {
        UE_LOGW("npc-sync[client OnSpawn]: eid=%u already mirrored locally -- dropping duplicate",
                payload.elementId);
        return;
    }
    // Resolve actor class. NPC BP classes load with the level; if not yet
    // loaded the packet is dropped (next host broadcast can rebind on a
    // future spawn, but THIS host spawn is gone). Log + skip.
    void* actorClass = R::FindClass(classW.c_str());
    if (!actorClass) {
        UE_LOGW("npc-sync[client OnSpawn]: class '%ls' not found in GUObjectArray -- dropping "
                "(BP class not loaded? eid=%u)",
                classW.c_str(), payload.elementId);
        return;
    }
    // Allowlist gate (trust-boundary defense): a misbehaving / malicious
    // peer could send EntitySpawn with an arbitrary className. Limit
    // materialization to subclasses of the 12 NPC bases.
    if (!coop::npc_sync::IsAllowlistedClass(actorClass)) {
        UE_LOGW("npc-sync[client OnSpawn]: class '%ls' is NOT on NPC allowlist -- "
                "rejecting (eid=%u; peer is broadcasting non-NPC EntitySpawn?)",
                classW.c_str(), payload.elementId);
        return;
    }
    // v75: route by whether the joining client has a LOCAL TWIN of this NPC (savePersisted).
    // savePersisted=1 -> a save object (the kerfur) the client ALSO loaded from the transferred
    // save; its local twin spawned via un-hookable EX_CallMath at an unpredictable time. ADOPT it
    // by class-match via a DEFERRED POLL (coop/npc_adoption) -- the kerfur's int_save Key is minted
    // RANDOM per peer (bytecode-proven: kerfurOmega::loadData drops the base key restore), so
    // key-equality could never match; class + untracked-local is the only portable identity.
    // savePersisted=0 -> a host-spawned transient (enemy) the client has NO local copy of ->
    // fresh-spawn a mirror now.
    if (payload.savePersisted) {
        coop::npc_adoption::ArmAdoption(payload.elementId, classW, actorClass,
                                        payload.locX, payload.locY, payload.locZ,
                                        payload.rotPitch, payload.rotYaw, payload.rotRoll);
        return;
    }
    SpawnFreshNpcMirror(classW, actorClass, payload.elementId,
                        payload.locX, payload.locY, payload.locZ,
                        payload.rotPitch, payload.rotYaw, payload.rotRoll);
}

bool SpawnFreshNpcMirror(const std::wstring& classW, void* actorClass, uint32_t elementId,
                         float locX, float locY, float locZ,
                         float rotPitch, float rotYaw, float rotRoll) {
    using ue_wrap::ParamFrame;
    using ue_wrap::Call;
    // UFunction + CDO must be resolved (npc_sync::Install pushes them via SetClientRefs; a
    // receiver can fire before Install completes on a fast-handshake peer). The FTransform we
    // build stays at identity/origin when the wire pose is zeroed (degraded host case), which
    // mirrors the host's interceptor behavior. Visibly wrong but not a crash.
    if (!g_spawnFn || !g_finishSpawnFn || !g_gsCdo ||
        g_spawnReturnParamOff < 0) {
        UE_LOGW("npc-sync[client OnSpawn]: receiver UFunctions not yet resolved "
                "(spawnFn=%p finishFn=%p gsCdo=%p retOff=%d) -- dropping eid=%u",
                g_spawnFn, g_finishSpawnFn, g_gsCdo,
                g_spawnReturnParamOff, elementId);
        return false;
    }
    void* worldCtx = E::GetWorldContext();
    if (!worldCtx) {
        UE_LOGW("npc-sync[client OnSpawn]: no world context -- dropping (eid=%u)",
                elementId);
        return false;
    }
    // Build FTransform from wire pose. NPCs have no scale in EntitySpawn (defaults to unit).
    ue_wrap::FTransform xform{};
    E::RotatorToQuat(rotPitch, rotYaw, rotRoll,
                     xform.RotX, xform.RotY, xform.RotZ, xform.RotW);
    xform.TX = locX;
    xform.TY = locY;
    xform.TZ = locZ;
    // xform scale stays at unit (constructor default).

    // Mark the bypass slot BEFORE BeginDeferredActorSpawnFromClass so our own interceptor (client
    // role) allows the spawn through instead of suppressing it. Single-shot: consumed by the next
    // interceptor fire matching this class.
    coop::npc_sync::MarkIncomingNpcSpawn(actorClass);

    constexpr uint8_t kAlwaysSpawn = 1;
    void* spawned = nullptr;
    {
        ParamFrame begin(g_spawnFn);
        if (!begin.valid()) {
            UE_LOGE("npc-sync[client OnSpawn]: ParamFrame(BeginDeferred) invalid -- dropping eid=%u",
                    elementId);
            // Clear bypass slot defensively -- the spawn we marked won't happen.
            coop::npc_sync::ClearIncomingNpcSpawn();
            return false;
        }
        begin.Set<void*>(L"WorldContextObject", worldCtx);
        begin.Set<void*>(L"ActorClass", actorClass);
        begin.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        begin.Set<uint8_t>(L"CollisionHandlingOverride", kAlwaysSpawn);
        begin.Set<void*>(L"Owner", nullptr);
        if (!Call(g_gsCdo, begin)) {
            UE_LOGE("npc-sync[client OnSpawn]: BeginDeferredActorSpawnFromClass call failed for "
                    "'%ls' eid=%u", classW.c_str(), elementId);
            coop::npc_sync::ClearIncomingNpcSpawn();
            return false;
        }
        spawned = begin.Get<void*>(L"ReturnValue");
    }
    if (!spawned) {
        UE_LOGE("npc-sync[client OnSpawn]: BeginDeferred returned null for '%ls' eid=%u "
                "(suppressor swallowed it? bypass slot not consumed?)",
                classW.c_str(), elementId);
        // Clear defensively so a subsequent local NPC spawn of the same class doesn't pass through.
        coop::npc_sync::ClearIncomingNpcSpawn();
        return false;
    }
    {
        ParamFrame finish(g_finishSpawnFn);
        if (!finish.valid()) {
            UE_LOGE("npc-sync[client OnSpawn]: ParamFrame(FinishSpawning) invalid -- "
                    "the actor %p is in a half-spawned state. Forcing K2_DestroyActor to clean up.",
                    spawned);
            if (g_k2DestroyFn && R::IsLive(spawned)) {
                R::CallFunction(spawned, g_k2DestroyFn, nullptr);
            } else if (!g_k2DestroyFn) {
                UE_LOGE("npc-sync[client OnSpawn]: g_k2DestroyFn=nullptr at half-spawn "
                        "cleanup -- actor %p LEAKS (Install gating invariant violated)",
                        spawned);
            }
            return false;
        }
        finish.Set<void*>(L"Actor", spawned);
        finish.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        if (!Call(g_gsCdo, finish)) {
            UE_LOGE("npc-sync[client OnSpawn]: FinishSpawningActor call failed for "
                    "%p '%ls' eid=%u -- forcing K2_DestroyActor",
                    spawned, classW.c_str(), elementId);
            if (g_k2DestroyFn && R::IsLive(spawned)) {
                R::CallFunction(spawned, g_k2DestroyFn, nullptr);
            } else if (!g_k2DestroyFn) {
                UE_LOGE("npc-sync[client OnSpawn]: g_k2DestroyFn=nullptr at FinishSpawn "
                        "failure cleanup -- actor %p LEAKS (Install gating invariant violated)",
                        spawned);
            }
            return false;
        }
    }

    // Build mirror Element + hand off to MirrorManager::Install (alloc-under-lock +
    // RegisterMirror + rollback-on-fail). Install returns false on duplicate-eid race or
    // Registry::RegisterMirror failure -- we own the orphan actor and must K2_DestroyActor it.
    auto mirror = std::make_unique<coop::element::Npc>();
    std::string typeName8;
    typeName8.reserve(classW.size());
    for (wchar_t c : classW) typeName8.push_back(static_cast<char>(c));
    mirror->SetTypeName(std::move(typeName8));
    mirror->SetActor(spawned, R::InternalIndexOf(spawned));

    const coop::element::ElementId eid =
        static_cast<coop::element::ElementId>(elementId);

    if (!NpcMirrors().Install(eid, std::move(mirror))) {
        UE_LOGW("npc-sync[client OnSpawn]: MirrorManager::Install(eid=%u) failed "
                "(duplicate eid race / Registry collision) -- destroying orphan actor %p",
                eid, spawned);
        if (g_k2DestroyFn && R::IsLive(spawned)) {
            R::CallFunction(spawned, g_k2DestroyFn, nullptr);
        } else if (!g_k2DestroyFn) {
            UE_LOGE("npc-sync[client OnSpawn]: g_k2DestroyFn=nullptr at MirrorManager "
                    "rollback cleanup -- actor %p LEAKS (Install gating invariant violated)",
                    spawned);
        }
        return false;
    }
    // Park the mirror so the streamed pose drive is authoritative: CMC tick OFF + actor tick OFF
    // (the AnimBP still ticks on the mesh and reads the CMC.Velocity our pose drive writes).
    ue_wrap::puppet::DisableCharacterTicks(spawned);
    // THOROUGH PARK: DisableCharacterTicks does not stop FTimerManager timers. A kerfur arms three
    // looping timers (timer_face/timer_kerf/checkDoor) that keep running local AI on the parked
    // mirror (the 200s timer_kerf can flip it into a local spooky-kill state). No-op on non-kerfur.
    ue_wrap::kerfur::NeutralizeAiTimers(spawned);

    UE_LOGI("npc-sync[client OnSpawn]: materialized mirror eid=%u class='%ls' actor=%p loc=(%.0f, %.0f, %.0f)",
            elementId, classW.c_str(), spawned, locX, locY, locZ);
    return true;
}

void OnEntityDestroy(const coop::net::EntityDestroyPayload& payload) {
    auto* s = coop::npc_sync::GetSession();
    if (!s) return;
    if (s->role() == coop::net::Role::Host) {
        UE_LOGI("npc-sync[client OnDestroy]: received on host -- dropping (loopback bounce)");
        return;
    }
    // PR-FOUNDATION-1 (2026-05-29): canonical host-range helper.
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(payload.elementId)) {
        UE_LOGW("npc-sync[client OnDestroy]: elementId=%u out of allowed host range "
                "[1, %u) -- dropping",
                payload.elementId, coop::element::kHostRangeSize);
        return;
    }
    const coop::element::ElementId eid =
        static_cast<coop::element::ElementId>(payload.elementId);
    // Drain the mirror from the client table under MirrorManager's own
    // mutex (Take releases that mutex before returning); THEN call
    // K2_DestroyActor (game-thread UFunction call cannot legally race
    // with another thread holding the manager's mutex) and let the
    // unique_ptr dtor run (which calls Registry::UnregisterMirror with
    // Registry::m_mutex held -- ABBA-safe because the MirrorManager
    // mutex is not held at that point).
    std::unique_ptr<coop::element::Npc> drained = NpcMirrors().Take(eid);
    if (!drained) {
        UE_LOGI("npc-sync[client OnDestroy]: eid=%u not in client mirror table -- "
                "ignoring (destroy without prior spawn, or already-drained)",
                payload.elementId);
        return;
    }
    void* actor = drained->GetActor();
    // v74 "floating camera" fix is STRUCTURAL, not a teardown crutch: an ADOPTED mirror IS the
    // client's own real save-kerfur (Inc-1), so K2_DestroyActor below runs its real ReceiveDestroyed
    // + engine child-actor cascade EXACTLY as on the host (which does not orphan the cam/camera
    // child -- RE sec 4). The old non-standard deferred-spawned mirror -- whose cascade was the
    // suspected orphan source -- is no longer the path for save-persisted kerfurs.
    if (actor && g_k2DestroyFn && R::IsLive(actor)) {
        R::CallFunction(actor, g_k2DestroyFn, nullptr);
        UE_LOGI("npc-sync[client OnDestroy]: K2_DestroyActor on mirror eid=%u actor=%p",
                payload.elementId, actor);
    } else if (actor && !R::IsLive(actor)) {
        UE_LOGI("npc-sync[client OnDestroy]: mirror eid=%u actor=%p already not-live -- "
                "skipping K2_DestroyActor (engine destroyed it elsewhere)",
                payload.elementId, actor);
    } else if (!actor) {
        UE_LOGI("npc-sync[client OnDestroy]: mirror eid=%u has null actor -- nothing to destroy "
                "(mirror was registered without ever binding an actor)",
                payload.elementId);
    } else {
        UE_LOGW("npc-sync[client OnDestroy]: K2_DestroyActor UFunction unresolved (g_k2DestroyFn=%p) -- "
                "cannot tear down actor %p; mirror Element will drop but engine actor leaks",
                g_k2DestroyFn, actor);
    }
    // drained's destructor fires here -> Registry::UnregisterMirror(eid).
}

void DrainClientMirrors() {
    // PR-FOUNDATION-3 Inc2 (2026-05-30): this now drains the UNIFIED
    // MirrorManager<Npc> for BOTH roles (the host's bespoke g_npcElements is
    // retired). On a CLIENT it holds puppet mirrors (m_mirror=true) -> K2 +
    // UnregisterMirror; on the HOST it holds the host's real Npc Elements
    // (m_mirror=false) -> release-only (NO K2 -- the real NPCs stay) + FreeId.
    // The K2 loop's IsMirror() gate (below) is what makes the dual role correct.
    // A process holds host XOR client Npc elements, so one branch dominates.
    //
    // GAME-THREAD SERIALIZATION CONTRACT: this function relies on the
    // caller running synchronously inside the GT pump loop -- between
    // the Snapshot() below and the DrainAll() at the end, the actors
    // are engine-destroyed but still live in NpcMirrors(). A concurrent
    // OnEntityDestroy Posted to the same GT queue would observe the
    // stale entry via Take() and call K2_DestroyActor again on the
    // (already-destroyed) actor. We rely on the GT pump's
    // serialization: Posted lambdas run one-at-a-time, so no other
    // OnEntityDestroy lambda can interleave with this call.
    // npc_sync::OnDisconnect (our only caller) is itself invoked from
    // the GT pump or from a path that holds that contract.
    //
    // Snapshot actor pointers FIRST under the manager's mutex (Snapshot
    // is just a vector<T*> copy), then drain. K2_DestroyActor runs on
    // each captured actor; the drained map then destructs sequentially
    // outside the mutex (each Npc dtor -> Registry::UnregisterMirror).
    std::vector<coop::element::Npc*> mirrorsSnap;
    NpcMirrors().Snapshot(mirrorsSnap);
    size_t nMirrorsDestroyed = 0;
    size_t nMirrorElems = 0, nHostElems = 0;
    for (coop::element::Npc* mirror : mirrorsSnap) {
        if (!mirror) continue;
        // IsMirror() gate (PR-FOUNDATION-3 Inc2): K2_DestroyActor ONLY the
        // client puppet mirrors (m_mirror=true). Post-migration this single
        // MirrorManager<Npc> also holds, on the HOST, the host's OWN Npc
        // Elements (m_mirror=false) whose actors are the REAL world NPCs --
        // K2'ing those on disconnect would wrongly despawn the host's NPCs.
        // They release tracking only via DrainAll below (dtor -> FreeId).
        if (!mirror->IsMirror()) { ++nHostElems; continue; }
        ++nMirrorElems;
        void* actor = mirror->GetActor();
        if (actor && g_k2DestroyFn && R::IsLive(actor)) {
            R::CallFunction(actor, g_k2DestroyFn, nullptr);
            ++nMirrorsDestroyed;
        }
    }
    // XOR-invariant guard (review-fix 2026-05-30, finding 6): a process holds
    // host XOR client Npc elements, so the snapshot should be a PURE host-set or
    // PURE client-set. If BOTH kinds appear in one drain, the migration's core
    // invariant is broken -- the IsMirror gate would then silently K2 only the
    // mirror subset and release the host subset, masking the regression. Surface
    // it loudly rather than letting the masking hide a future bug.
    if (nHostElems > 0 && nMirrorElems > 0) {
        UE_LOGE("npc-mirror: DrainClientMirrors saw BOTH %zu host element(s) AND "
                "%zu client mirror(s) in one drain -- host-XOR-client invariant "
                "VIOLATED (MirrorManager<Npc> must be pure per role)",
                nHostElems, nMirrorElems);
    }
    const size_t nMirrorsTotal = NpcMirrors().DrainAll();
    if (nMirrorsTotal > 0) {
        UE_LOGI("npc-mirror: drained %zu Npc element(s) from MirrorManager "
                "(%zu host release-only, %zu client mirror(s); K2_DestroyActor on "
                "%zu live mirror actor(s))",
                nMirrorsTotal, nHostElems, nMirrorElems, nMirrorsDestroyed);
    }
}

void PruneDeadClientMirrors() {
    // CLIENT world-swap teardown (v75). A save-transfer join performs TWO level loads; a mirror
    // Installed/adopted in world-1 SURVIVES the swap as a dangling Element because the host sends
    // no EntityDestroy on a client world-swap (it never world-swaps during the join) and the only
    // other client drain (DrainClientMirrors) is full-disconnect only. The stale Element then
    // blocks world-2 from re-mirroring/re-adopting the SAME host eid (OnEntitySpawn + ArmAdoption
    // both early-return on NpcMirrors().Get(eid)!=nullptr). Drop every mirror whose actor was freed
    // by the level load -- RELEASE TRACKING ONLY (NO K2_DestroyActor: the actor is already gone).
    // A still-LIVE mirror (a re-announce without an actual level change) is KEPT, so the next
    // replay's duplicate-eid drop is correct. Game thread only.
    auto* s = coop::npc_sync::GetSession();
    if (!s || s->role() != coop::net::Role::Client) return;  // host mirrors are the real NPCs -- never prune
    std::vector<coop::element::Npc*> snap;
    NpcMirrors().Snapshot(snap);
    std::vector<coop::element::ElementId> deadIds;
    for (coop::element::Npc* m : snap) {
        if (!m || !m->IsMirror()) continue;
        void* actor = m->GetActor();
        if (actor && R::IsLiveByIndex(actor, m->GetInternalIdx())) continue;  // still live -> keep
        deadIds.push_back(static_cast<coop::element::ElementId>(m->GetId()));
    }
    for (coop::element::ElementId eid : deadIds) {
        // Take drains the Element under the manager mutex; the unique_ptr dtor (outside the mutex)
        // runs Registry::UnregisterMirror -- ABBA-safe, same pattern as OnEntityDestroy. No K2.
        std::unique_ptr<coop::element::Npc> drained = NpcMirrors().Take(eid);
    }
    if (!deadIds.empty())
        UE_LOGI("npc-mirror[world-swap]: pruned %zu stale dead-actor NPC mirror(s) (re-adopt/re-mirror unblocked)",
                deadIds.size());
}

int DestroyUntrackedClientNpcs() {
    // CLIENT host-authoritative reconciliation -- see npc_mirror.h. Destroy every
    // live allowlisted-NPC actor that is NOT a host-streamed mirror (a save-load
    // ghost / escaped client spawn). MTA shape: reconcile the client entity set to
    // the server's on join (reference/mtasa-blue CClientGame element sync).
    auto* s = coop::npc_sync::GetSession();
    if (!s || s->role() != coop::net::Role::Client) return 0;  // host owns NPCs; never sweep
    if (!g_k2DestroyFn) {
        UE_LOGW("npc-mirror[reconcile]: K2_DestroyActor unresolved -- cannot sweep ghost NPCs");
        return 0;
    }

    // Snapshot the LEGITIMATE (host-mirrored) actor set under the manager mutex,
    // then iterate without it (Snapshot is just a vector copy -- holding the
    // mutex across the GUObjectArray walk + K2 calls would invite ABBA with the
    // registry mutex). Every mirror binds its actor (SetActor) BEFORE Install
    // into the manager (npc_mirror::OnEntitySpawn), and both that and this run on
    // the game thread, so any element in the snapshot has a coherent bound actor.
    std::vector<coop::element::Npc*> mirrors;
    NpcMirrors().Snapshot(mirrors);
    std::unordered_set<void*> tracked;
    tracked.reserve(mirrors.size() * 2 + 1);
    for (coop::element::Npc* m : mirrors) {
        // Liveness-guard the tracked set: a mirror whose actor was freed (e.g. a world-swap that
        // never sent EntityDestroy) holds a DANGLING pointer. Inserting it could falsely match a
        // recycled GUObjectArray address. IsLiveByIndex (serial-number check) rejects dead actors.
        if (m && m->GetActor() && R::IsLiveByIndex(m->GetActor(), m->GetInternalIdx()))
            tracked.insert(m->GetActor());
    }

    const int32_t n = R::NumObjects();
    int destroyed = 0, found = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        // Fast filter FIRST (matches the host world-enum walk): the subclass-aware
        // allowlist check is a few pointer compares; >99% of GUObjectArray is not
        // an NPC and never pays for the IsLive/NameOf work below.
        void* cls = R::ClassOf(obj);
        if (!cls || !coop::npc_sync::IsAllowlistedClass(cls)) continue;
        if (tracked.count(obj) > 0) continue;  // a legitimate host mirror -- KEEP
        // GC-liveness guard before any deref / NameOf alloc.
        if (!R::IsLive(obj)) continue;
        // Skip CDOs (Default__<Class>) -- the class template, not a world instance (alloc-free).
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        // v75: SOLE caller is npc_adoption::Tick, which fires this ONCE per world only after
        // g_snapshotDelivered && g_pending.empty() (every save-persisted EntitySpawn has armed AND
        // its deferred CLASS-MATCH adoption has bound the client's own save-kerfur as a host mirror
        // -> the kerfur is in `tracked` and kept). Anything still untracked here is a genuine ghost:
        // a host-spawned enemy that escaped the PE suppressor, OR a save NPC the host's world no
        // longer has (e.g. a kerfur turned OFF after the save -> host holds only the prop form, so
        // no EntitySpawn -> no adoption armed for it). Both must go. The host's authoritative mirror
        // (if any) is tracked, so it survives.
        ++found;
        R::CallFunction(obj, g_k2DestroyFn, nullptr);  // K2_DestroyActor (game thread)
        ++destroyed;
        UE_LOGI("npc-mirror[reconcile]: destroyed untracked ghost NPC actor=%p class='%ls'",
                obj, R::ToString(R::NameOf(cls)).c_str());
    }
    if (found > 0)
        UE_LOGI("npc-mirror[reconcile]: swept %d untracked ghost NPC(s) (scanned %d "
                "GUObjectArray slots, %zu tracked mirror(s))",
                destroyed, n, mirrors.size());
    return destroyed;
}

void TickClientNpcs() {
    auto* s = coop::npc_sync::GetSession();
    if (!s || s->role() == coop::net::Role::Host) return;  // client-only (the host streams, it doesn't drive mirrors)

    // 1) Apply the latest received batch (if any) -> open an interp window per NPC. Per-entry float
    //    validation is the trust boundary (a NaN must not reach SetActorLocation).
    std::vector<coop::net::EntityPoseSnapshot> batch;
    if (s->TakeRemoteNpcBatch(batch)) {
        for (const auto& snap : batch) {
            if (!std::isfinite(snap.x) || !std::isfinite(snap.y) || !std::isfinite(snap.z) ||
                !std::isfinite(snap.yaw) || !std::isfinite(snap.speed)) continue;
            if (std::fabs(snap.x) > 1.0e6f || std::fabs(snap.y) > 1.0e6f || std::fabs(snap.z) > 1.0e6f) continue;
            // v39 head-look target: validate ONLY when the bit marks it meaningful (non-kerfur NPCs
            // leave lookAt zero + the bit clear -- gating avoids dropping their otherwise-valid pose).
            // A corrupt lookAt WITH the bit set drops the whole snap (same policy as the pos fields);
            // it can't reach DriveKerfurLookAt anyway (hasLookAt_ is only set from a validated snap).
            if (snap.stateBits & coop::net::kEntityPoseBitHasLookAt) {
                if (!std::isfinite(snap.lookAtX) || !std::isfinite(snap.lookAtY) || !std::isfinite(snap.lookAtZ)) continue;
                if (std::fabs(snap.lookAtX) > 1.0e6f || std::fabs(snap.lookAtY) > 1.0e6f || std::fabs(snap.lookAtZ) > 1.0e6f) continue;
            }
            // v40 body yaw (a degree angle): a NaN/garbage value must not reach K2_SetWorldRotation.
            if ((snap.stateBits & coop::net::kEntityPoseBitHasBodyYaw) &&
                (!std::isfinite(snap.bodyYaw) || std::fabs(snap.bodyYaw) > 1.0e6f)) continue;
            coop::element::Npc* el = NpcMirrors().Get(snap.elementId);
            if (!el || !el->IsMirror()) continue;  // not materialized yet (connect gap) / defensive
            el->SetTargetNpcPose(snap);
        }
        static bool s_loggedFirst = false;
        if (!batch.empty() && !s_loggedFirst) { s_loggedFirst = true;
            UE_LOGI("npc-pose: client applying %zu NPC pose(s) (first batch)", batch.size()); }
    }

    // 2) Advance the interp + drive EVERY live mirror, every frame (smooth between packets).
    // Reused scratch (no per-tick heap alloc): Snapshot() clears+fills it; client game thread only.
    // (`batch` above stays local -- its default ctor never allocates, and the common no-new-batch
    // tick takes the early `if (!Take)` path, so it adds no per-tick alloc.)
    static std::vector<coop::element::Npc*> elems;
    NpcMirrors().Snapshot(elems);
    for (coop::element::Npc* el : elems)
        if (el && el->IsMirror()) el->Tick();

    // 3) v75: drive the deferred class-match adoption of save-persisted NPCs (the kerfur), AND the
    // post-SnapshotComplete one-shot ghost sweep (both owned by npc_adoption, which holds the
    // connect-snapshot-delivered + converged timing). NO-OP (single integer compare) once adoption
    // has converged and the sweep has run -- zero steady-state cost. Game thread only.
    coop::npc_adoption::Tick();

    // 4) Drive the deferred PROP divergence sweep (the save-transfer kerfur-ghost fix): it waits for
    // the save load's late key-minting tail to quiesce before adjudicating, so a host-converted-away
    // save prop is destroyed with its real key instead of keyless-skipped into a ghost. NO-OP (single
    // bool read) when no sweep is armed -- the same client tick already gated to non-host above.
    coop::remote_prop_spawn::TickClientReconcile();
}

}  // namespace coop::npc_mirror
