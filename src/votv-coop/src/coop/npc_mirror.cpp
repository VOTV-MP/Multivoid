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
#include "coop/npc_sync.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
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

}  // namespace

void SetClientRefs(const ClientRefs& refs) {
    g_spawnFn             = refs.spawnFn;
    g_finishSpawnFn       = refs.finishSpawnFn;
    g_gsCdo               = refs.gsCdo;
    g_spawnReturnParamOff = refs.spawnReturnParamOff;
    g_k2DestroyFn         = refs.k2DestroyFn;
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
    // UFunction + CDO must be resolved (npc_sync::Install pushes them via
    // SetClientRefs; receiver can fire before Install completes on a
    // fast-handshake peer).
    //
    // Note: g_spawnXformParamOff is owned by npc_sync's interceptor side
    // and isn't read here. Install treats missing SpawnTransform as
    // non-fatal ("position-less spawns still work"), so the receiver must
    // too. The FTransform we build below stays at identity / origin when
    // the wire pose is zeroed (degraded host case), which mirrors the
    // host's interceptor behavior in that case. Visibly wrong but not a
    // crash; aligns sender + receiver behavior.
    if (!g_spawnFn || !g_finishSpawnFn || !g_gsCdo ||
        g_spawnReturnParamOff < 0) {
        UE_LOGW("npc-sync[client OnSpawn]: receiver UFunctions not yet resolved "
                "(spawnFn=%p finishFn=%p gsCdo=%p retOff=%d) -- dropping eid=%u",
                g_spawnFn, g_finishSpawnFn, g_gsCdo,
                g_spawnReturnParamOff, payload.elementId);
        return;
    }
    void* worldCtx = E::GetWorldContext();
    if (!worldCtx) {
        UE_LOGW("npc-sync[client OnSpawn]: no world context -- dropping (eid=%u)",
                payload.elementId);
        return;
    }
    // Build FTransform from wire pose. NPCs have no scale in EntitySpawn
    // (scale is a runtime variant signal we don't currently sync; defaults
    // to unit -- matches host's spawn behavior since the BP rarely sets
    // non-unit scale on NPCs).
    ue_wrap::FTransform xform{};
    E::RotatorToQuat(payload.rotPitch, payload.rotYaw, payload.rotRoll,
                     xform.RotX, xform.RotY, xform.RotZ, xform.RotW);
    xform.TX = payload.locX;
    xform.TY = payload.locY;
    xform.TZ = payload.locZ;
    // xform scale stays at unit (constructor default).

    // Mark the bypass slot BEFORE BeginDeferredActorSpawnFromClass so our
    // own interceptor (client role) allows the spawn through instead of
    // suppressing it. Single-shot: consumed by the next interceptor fire
    // matching this class.
    coop::npc_sync::MarkIncomingNpcSpawn(actorClass);

    constexpr uint8_t kAlwaysSpawn = 1;
    void* spawned = nullptr;
    {
        ParamFrame begin(g_spawnFn);
        if (!begin.valid()) {
            UE_LOGE("npc-sync[client OnSpawn]: ParamFrame(BeginDeferred) invalid -- dropping eid=%u",
                    payload.elementId);
            // Clear bypass slot defensively -- the spawn we marked won't
            // happen, so a stray local spawn of the same class shouldn't
            // accidentally pass through.
            coop::npc_sync::ClearIncomingNpcSpawn();
            return;
        }
        begin.Set<void*>(L"WorldContextObject", worldCtx);
        begin.Set<void*>(L"ActorClass", actorClass);
        begin.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        begin.Set<uint8_t>(L"CollisionHandlingOverride", kAlwaysSpawn);
        begin.Set<void*>(L"Owner", nullptr);
        if (!Call(g_gsCdo, begin)) {
            UE_LOGE("npc-sync[client OnSpawn]: BeginDeferredActorSpawnFromClass call failed for "
                    "'%ls' eid=%u", classW.c_str(), payload.elementId);
            coop::npc_sync::ClearIncomingNpcSpawn();
            return;
        }
        spawned = begin.Get<void*>(L"ReturnValue");
    }
    if (!spawned) {
        UE_LOGE("npc-sync[client OnSpawn]: BeginDeferred returned null for '%ls' eid=%u "
                "(suppressor swallowed it? bypass slot not consumed?)",
                classW.c_str(), payload.elementId);
        // Audit critical #2 / 2026-05-28: MarkIncomingNpcSpawn set the slot
        // unconditionally before BeginDeferred; if Call succeeded but the
        // engine returned null (e.g. the suppressor consumed an aliased
        // class via concurrent fire OR a different interceptor swallowed
        // the spawn), the slot might still be populated. Clear defensively
        // so a subsequent local NPC spawn of the same class doesn't
        // accidentally pass through and produce a rogue non-suppressed
        // duplicate.
        coop::npc_sync::ClearIncomingNpcSpawn();
        return;
    }
    {
        ParamFrame finish(g_finishSpawnFn);
        if (!finish.valid()) {
            UE_LOGE("npc-sync[client OnSpawn]: ParamFrame(FinishSpawning) invalid -- "
                    "the actor %p is in a half-spawned state (BeginDeferred returned, "
                    "FinishSpawning never ran). Forcing K2_DestroyActor to clean up.",
                    spawned);
            if (g_k2DestroyFn && R::IsLive(spawned)) {
                R::CallFunction(spawned, g_k2DestroyFn, nullptr);
            } else if (!g_k2DestroyFn) {
                // Latent invariant: this branch is only reachable when the
                // interceptor is live (otherwise MarkIncomingNpcSpawn was
                // never called, the suppressor swallowed BeginDeferred,
                // and `spawned` would be null). Interceptor registration
                // gates on k2DestroyFn binding (npc_sync.cpp Install), so
                // g_k2DestroyFn=nullptr here would mean the gating broke.
                UE_LOGE("npc-sync[client OnSpawn]: g_k2DestroyFn=nullptr at half-spawn "
                        "cleanup -- actor %p LEAKS (Install gating invariant violated; "
                        "see npc_mirror.cpp two-push pattern)",
                        spawned);
            }
            return;
        }
        finish.Set<void*>(L"Actor", spawned);
        finish.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
        if (!Call(g_gsCdo, finish)) {
            UE_LOGE("npc-sync[client OnSpawn]: FinishSpawningActor call failed for "
                    "%p '%ls' eid=%u -- forcing K2_DestroyActor",
                    spawned, classW.c_str(), payload.elementId);
            if (g_k2DestroyFn && R::IsLive(spawned)) {
                R::CallFunction(spawned, g_k2DestroyFn, nullptr);
            } else if (!g_k2DestroyFn) {
                UE_LOGE("npc-sync[client OnSpawn]: g_k2DestroyFn=nullptr at FinishSpawn "
                        "failure cleanup -- actor %p LEAKS (Install gating invariant "
                        "violated; see npc_mirror.cpp two-push pattern)",
                        spawned);
            }
            return;
        }
    }

    // Build mirror Element + hand off to MirrorManager::Install. The
    // template encapsulates the 5-step pattern (alloc-under-lock +
    // RegisterMirror + rollback-on-fail). Install returns false on
    // duplicate-eid race or Registry::RegisterMirror failure -- in
    // both cases we own the orphan actor and must K2_DestroyActor it.
    auto mirror = std::make_unique<coop::element::Npc>();
    std::string typeName8;
    typeName8.reserve(payload.className.len);
    for (uint8_t i = 0; i < payload.className.len; ++i) {
        typeName8.push_back(static_cast<char>(payload.className.data[i]));
    }
    mirror->SetTypeName(std::move(typeName8));
    mirror->SetActor(spawned, R::InternalIndexOf(spawned));

    const coop::element::ElementId eid =
        static_cast<coop::element::ElementId>(payload.elementId);

    if (!NpcMirrors().Install(eid, std::move(mirror))) {
        UE_LOGW("npc-sync[client OnSpawn]: MirrorManager::Install(eid=%u) failed "
                "(duplicate eid race or Registry::RegisterMirror collision) -- "
                "destroying orphan actor %p",
                eid, spawned);
        if (g_k2DestroyFn && R::IsLive(spawned)) {
            R::CallFunction(spawned, g_k2DestroyFn, nullptr);
        } else if (!g_k2DestroyFn) {
            UE_LOGE("npc-sync[client OnSpawn]: g_k2DestroyFn=nullptr at MirrorManager "
                    "rollback cleanup -- actor %p LEAKS (Install gating invariant "
                    "violated; see npc_mirror.cpp two-push pattern)",
                    spawned);
        }
        return;
    }
    UE_LOGI("npc-sync[client OnSpawn]: materialized mirror eid=%u class='%ls' actor=%p loc=(%.0f, %.0f, %.0f)",
            payload.elementId, classW.c_str(), spawned,
            payload.locX, payload.locY, payload.locZ);
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
    for (coop::element::Npc* mirror : mirrorsSnap) {
        if (!mirror) continue;
        void* actor = mirror->GetActor();
        if (actor && g_k2DestroyFn && R::IsLive(actor)) {
            R::CallFunction(actor, g_k2DestroyFn, nullptr);
            ++nMirrorsDestroyed;
        }
    }
    const size_t nMirrorsTotal = NpcMirrors().DrainAll();
    if (nMirrorsTotal > 0) {
        UE_LOGI("npc-mirror: drained %zu client mirror(s) "
                "(K2_DestroyActor on %zu live actor(s); UnregisterMirror on all elements)",
                nMirrorsTotal, nMirrorsDestroyed);
    }
}

}  // namespace coop::npc_mirror
