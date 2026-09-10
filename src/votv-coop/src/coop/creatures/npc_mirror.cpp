// coop/creatures/npc_mirror.cpp -- the client-side NPC mirror: materialising a host EntitySpawn (a
// fresh spawn, or the adoption of a local twin), the wire destroy, the disconnect and world-swap
// drains, the ghost sweep and the per-tick pose drive. See coop/creatures/npc_mirror.h.

#include "coop/creatures/npc_mirror.h"

#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"  // PropMirrors/NpcMirrors/WaMirrors
#include "coop/element/npc.h"
#include "coop/element/registry.h"
#include "coop/element/identity_create.h"   // the NPC mirror create funnel
#include "coop/creatures/kerfur_convert_client.h"  // TakeParkedGhostByEid
#include "coop/element/mirror_defer.h"  // a fresh mirror stays hidden until the reveal
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/creatures/npc_adoption.h"
#include "coop/creatures/kerfur_prop_adoption.h"  // the deferred prop-form kerfur adoption
#include "coop/creatures/kerfur_reconcile.h"  // ArmPendingRetireByEid
#include "coop/props/join_membership_sweep.h"  // TickClientReconcile
#include "coop/dev/kerfur_census.h"  // the one-shot kerfur census
#include "coop/creatures/npc_sync.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/actors/kerfur.h"  // NeutralizeAiTimers
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"  // NpcClass_Wisp (the wisp mirror keeps its actor tick)
#include "ue_wrap/core/types.h"

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

// The client-side UFunction cache, filled by SetClientRefs once npc_sync::Install resolves them.
// Plain storage: SetClientRefs and the receivers all run on the game thread.
void*   g_spawnFn             = nullptr;
void*   g_finishSpawnFn       = nullptr;
void*   g_gsCdo               = nullptr;
int32_t g_spawnReturnParamOff = -1;
void*   g_k2DestroyFn         = nullptr;

using coop::element::NpcMirrors;   // canonical accessor (coop/element/mirror_managers.h)

// A save-persisted NPC the joining client also loaded from the transferred save is adopted by
// class match through npc_adoption's deferred poll: the local twin spawns on an invisible path
// at an unpredictable time, and its save key is minted at random per peer, so key equality
// cannot match. Only host-spawned transients fresh-spawn here. The post-snapshot ghost sweep
// runs from npc_adoption once the snapshot is delivered and every pending adoption has
// converged.

}  // namespace

void SetClientRefs(const ClientRefs& refs) {
    g_spawnFn             = refs.spawnFn;
    g_finishSpawnFn       = refs.finishSpawnFn;
    g_gsCdo               = refs.gsCdo;
    g_spawnReturnParamOff = refs.spawnReturnParamOff;
    g_k2DestroyFn         = refs.k2DestroyFn;
    // The adoption module needs no UFunction refs of its own: its bind path only parks, and its
    // timeout fallback and ghost sweep re-enter this file's functions.
}

void OnEntitySpawn(const coop::net::EntitySpawnPayload& payload) {
    using ue_wrap::ParamFrame;
    using ue_wrap::Call;
    auto* s = coop::npc_sync::GetSession();
    if (!s) return;
    // The host never receives its own broadcasts (the fan-out is to peer slots only); should it,
    // materialising would duplicate the actor.
    if (s->role() == coop::net::Role::Host) {
        UE_LOGI("npc-sync[client OnSpawn]: received on host -- dropping (loopback bounce)");
        return;
    }
    // EntitySpawn is host-authoritative (the router already rejected non-host senders); the eid
    // must be in the host range.
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(payload.elementId)) {
        UE_LOGW("npc-sync[client OnSpawn]: elementId=%u out of allowed host range "
                "[1, %u) -- dropping",
                payload.elementId, coop::element::kHostRangeSize);
        return;
    }
    // The wire class name to a wstring.
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
    // Floats finite and within kMaxCoord, as for every world-position payload.
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
    // The duplicate-eid guard: the host allocates each id once and the reliable channel does not
    // redeliver, but a late duplicate would duplicate the actor. An unlocked early exit; a race
    // past it is caught by the install failure path below, which destroys the orphan.
    if (NpcMirrors().Get(payload.elementId) != nullptr) {
        UE_LOGW("npc-sync[client OnSpawn]: eid=%u already mirrored locally -- dropping duplicate",
                payload.elementId);
        return;
    }
    // The actor class. NPC classes load with the level; unloaded, the packet is dropped and this
    // spawn is gone.
    void* actorClass = R::FindClass(classW.c_str());
    if (!actorClass) {
        UE_LOGW("npc-sync[client OnSpawn]: class '%ls' not found in GUObjectArray -- dropping "
                "(BP class not loaded? eid=%u)",
                classW.c_str(), payload.elementId);
        return;
    }
    // The allowlist gate: a peer could send any className, so only subclasses of the NPC bases
    // materialise.
    if (!coop::npc_sync::IsAllowlistedClass(actorClass)) {
        UE_LOGW("npc-sync[client OnSpawn]: class '%ls' is NOT on NPC allowlist -- "
                "rejecting (eid=%u; peer is broadcasting non-NPC EntitySpawn?)",
                classW.c_str(), payload.elementId);
        return;
    }
    // A kerfur that was off in the transferred save and the host turned on in the join window
    // carries the off-prop's host eid; the KerfurConvert that would have linked them is gated
    // pre-world mid-join, so a retire of the client's stale off-prop mirror at that exact eid is
    // armed here, and the quiescence-driven sweep destroys it once bound. Independent of how the
    // NPC mirror itself materialises below.
    if (payload.retireOffEid != 0)
        coop::kerfur_reconcile::ArmPendingRetireByEid(
            static_cast<coop::element::ElementId>(payload.retireOffEid));
    // Routed by whether the client has a local twin. savePersisted: a save object (the kerfur) the
    // client also loaded, adopted by class match through the deferred poll, since its save key is
    // random per peer and class plus untracked-local is the only portable identity. Otherwise a
    // host-spawned transient the client has no copy of: a fresh mirror now.
    if (payload.savePersisted) {
        coop::npc_adoption::ArmAdoption(payload.elementId, classW, actorClass,
                                        payload.locX, payload.locY, payload.locZ,
                                        payload.rotPitch, payload.rotYaw, payload.rotRoll);
        return;
    }
    // A kerfur this client just turned on arrives as a mid-session spawn. The client already
    // spawned its own kerfur on the invisible conversion path, and kerfur_convert's poll parked it
    // tagged with the converting eid; that exact actor is bound as the mirror instead of a
    // duplicate spawned beside it. Only the initiating client has a ghost at this eid; other peers
    // fall through to the fresh spawn.
    if (payload.convertFromEid != 0) {
        if (void* ghost = coop::kerfur_convert_client::TakeParkedGhostByEid(payload.convertFromEid, /*wantNpc=*/true)) {
            if (AdoptExistingNpcAsMirror(ghost, payload.elementId, classW)) return;
            // Adopt failed (an eid collision): a fresh mirror instead; kerfur_convert's cleanup
            // reaps the ghost.
        }
    }
    SpawnFreshNpcMirror(classW, actorClass, payload.elementId,
                        payload.locX, payload.locY, payload.locZ,
                        payload.rotPitch, payload.rotYaw, payload.rotRoll,
                        coop::net::SanitizeWireScaleAxis(payload.scaleX),
                        coop::net::SanitizeWireScaleAxis(payload.scaleY),
                        coop::net::SanitizeWireScaleAxis(payload.scaleZ));
}

bool AdoptExistingNpcAsMirror(void* actor, uint32_t elementId, const std::wstring& classW) {
    if (!actor || !R::IsLive(actor)) return false;
    // The same Element build, install and park as SpawnFreshNpcMirror's tail, binding an existing
    // actor: the client's own game-spawned kerfur, fully initialised.
    const coop::element::ElementId eid = static_cast<coop::element::ElementId>(elementId);
    if (!coop::element::CreateOrAdoptNpcMirror(eid, actor, classW, /*senderSlot=*/-1)) {
        UE_LOGW("npc-mirror[adopt]: CreateOrAdoptNpcMirror(eid=%u) failed for existing actor %p -- leaving it for "
                "kerfur_convert ghost cleanup", elementId, actor);
        return false;
    }
    // Parked so the streamed pose is authoritative: movement and actor ticks off, kerfur AI timers
    // off.
    ue_wrap::puppet::DisableCharacterTicks(actor);
    ue_wrap::kerfur::NeutralizeAiTimers(actor);
    UE_LOGI("npc-mirror[adopt]: bound EXISTING local actor %p as host mirror eid=%u class='%ls' "
            "(kerfur turn-on -- no respawn, no ghost)", actor, elementId, classW.c_str());
    return true;
}

void DestroyLocalNpcActor(void* actor) {
    if (!actor || !g_k2DestroyFn || !R::IsLive(actor)) return;
    R::CallFunction(actor, g_k2DestroyFn, nullptr);  // K2_DestroyActor (game thread)
}

bool SpawnFreshNpcMirror(const std::wstring& classW, void* actorClass, uint32_t elementId,
                         float locX, float locY, float locZ,
                         float rotPitch, float rotYaw, float rotRoll,
                         float scaleX, float scaleY, float scaleZ) {
    using ue_wrap::ParamFrame;
    using ue_wrap::Call;
    // The UFunctions and the CDO must be resolved (a receiver can fire before Install completes on
    // a fast handshake). A zeroed wire pose spawns at the origin: visibly wrong, not a crash.
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
    // The FTransform from the wire pose, scale included (the caller sanitised it).
    ue_wrap::FTransform xform{};
    E::RotatorToQuat(rotPitch, rotYaw, rotRoll,
                     xform.RotX, xform.RotY, xform.RotZ, xform.RotW);
    xform.TX = locX;
    xform.TY = locY;
    xform.TZ = locZ;
    xform.SX = scaleX;
    xform.SY = scaleY;
    xform.SZ = scaleZ;

    // The bypass slot is marked before the spawn so our own client-side interceptor lets it
    // through; consumed by the next interceptor fire for this class.
    coop::npc_sync::MarkIncomingNpcSpawn(actorClass);

    constexpr uint8_t kAlwaysSpawn = 1;
    void* spawned = nullptr;
    {
        ParamFrame begin(g_spawnFn);
        if (!begin.valid()) {
            UE_LOGE("npc-sync[client OnSpawn]: ParamFrame(BeginDeferred) invalid -- dropping eid=%u",
                    elementId);
            // The slot is cleared: the spawn it was marked for will not happen.
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
        // Cleared so a later local spawn of the same class does not pass through.
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

    // The mirror Element, installed through the create funnel; a false return (a duplicate-eid
    // race, a Registry collision) leaves us owning the orphan actor, which is destroyed.
    const coop::element::ElementId eid =
        static_cast<coop::element::ElementId>(elementId);

    if (!coop::element::CreateOrAdoptNpcMirror(eid, spawned, classW, /*senderSlot=*/-1)) {
        UE_LOGW("npc-sync[client OnSpawn]: CreateOrAdoptNpcMirror(eid=%u) failed "
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
    // Parked so the streamed pose drive is authoritative: movement and actor ticks off (the AnimBP
    // still ticks and reads the velocity the drive writes). The wisp keeps its actor tick: its
    // ReceiveTick is per-viewer cosmetics (fade-in, bob, the shy despawn), not AI, so only its
    // movement parks; its landing gate reads a floor the parked movement never updates, so the pose
    // drive replays that edge (MarkWispMirror, then the landing drive on the grounded edge). A
    // local shy despawn makes the host's later EntityDestroy a no-op.
    const bool wispMirror = (classW == ue_wrap::profile::name::NpcClass_Wisp);
    if (wispMirror) {
        ue_wrap::puppet::DisableMovementTick(spawned);
        if (coop::element::Npc* el = coop::element::NpcMirrors().Get(eid)) {
            el->MarkWispMirror();
        } else {
            // Unreachable after a successful install; a silent miss would be a mirror that never
            // fades in.
            UE_LOGW("npc-sync[client OnSpawn]: wisp mirror eid=%u not retrievable post-install -- "
                    "landing drive NOT armed (mirror would stay faded-out)", eid);
        }
    } else {
        ue_wrap::puppet::DisableCharacterTicks(spawned);
    }
    // Disabling the ticks does not stop timers: a kerfur arms three looping ones that keep running
    // local AI on a parked mirror. A no-op on other classes.
    ue_wrap::kerfur::NeutralizeAiTimers(spawned);

    // Hidden until the reveal, after the install so it is enumerable. A fresh-spawn NPC has no
    // local twin, so it is confirmed and revealed at the curtain lift; collision off, so the
    // invisible capsule cannot block the player.
    coop::mirror_defer::OnMirrorSpawned(elementId, spawned, /*collisionOff=*/true, /*holdUntilQuiescence=*/false);

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
    // The host-range gate.
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(payload.elementId)) {
        UE_LOGW("npc-sync[client OnDestroy]: elementId=%u out of allowed host range "
                "[1, %u) -- dropping",
                payload.elementId, coop::element::kHostRangeSize);
        return;
    }
    const coop::element::ElementId eid =
        static_cast<coop::element::ElementId>(payload.elementId);
    // The mirror is drained from the table under the manager's mutex (released before Take
    // returns), then K2_DestroyActor runs, then the unique_ptr destructor unregisters from the
    // Registry under its own mutex: never both mutexes at once.
    std::unique_ptr<coop::element::Npc> drained = NpcMirrors().Take(eid);
    if (!drained) {
        UE_LOGI("npc-sync[client OnDestroy]: eid=%u not in client mirror table -- "
                "ignoring (destroy without prior spawn, or already-drained)",
                payload.elementId);
        return;
    }
    void* actor = drained->GetActor();          // raw: only for the null-vs-dead log split
    void* liveActor = drained->LiveActor();     // slot-validated
    // An adopted mirror is the client's own save kerfur, so K2_DestroyActor runs its real
    // ReceiveDestroyed and the engine's child-actor cascade as on the host, which does not orphan
    // the camera child.
    if (liveActor && g_k2DestroyFn) {
        R::CallFunction(liveActor, g_k2DestroyFn, nullptr);
        UE_LOGI("npc-sync[client OnDestroy]: K2_DestroyActor on mirror eid=%u actor=%p",
                payload.elementId, liveActor);
    } else if (actor && !liveActor) {
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
    // drained's destructor runs here and unregisters the mirror.
}

void DrainClientMirrors() {
    // Drains the one MirrorManager<Npc> for both roles: on a client it holds the mirrors (destroyed
    // and unregistered), on the host the host's own Npc Elements (released only; the real NPCs
    // stay). The IsMirror gate below makes that correct; a process holds one kind or the other. The
    // caller runs synchronously inside the game-thread pump: between the snapshot and the drain the
    // actors are destroyed but still in the table, and a concurrent OnEntityDestroy would Take a
    // stale entry and destroy again; posted lambdas run one at a time, so none can interleave. The
    // actor pointers are snapshotted under the mutex, destroyed, and the drained map destructs
    // outside it.
    std::vector<coop::element::Npc*> mirrorsSnap;
    NpcMirrors().Snapshot(mirrorsSnap);
    size_t nMirrorsDestroyed = 0;
    size_t nMirrorElems = 0, nHostElems = 0;
    for (coop::element::Npc* mirror : mirrorsSnap) {
        if (!mirror) continue;
        // Only client mirrors are destroyed; on the host the table holds the real world NPCs, which
        // DrainAll alone releases.
        if (!mirror->IsMirror()) { ++nHostElems; continue; }
        ++nMirrorElems;
        void* actor = mirror->GetActor();
        // IsLiveByIndex, not IsLive: this drain also runs on the quit-to-menu teardown, where the
        // world is already dying and a recycled slot passes IsLive; destroying the foreign occupant
        // is fatal.
        if (actor && g_k2DestroyFn && R::IsLiveByIndex(actor, mirror->GetInternalIdx())) {
            R::CallFunction(actor, g_k2DestroyFn, nullptr);
            ++nMirrorsDestroyed;
        }
    }
    // A process holds host or client elements, never both; both kinds in one drain means the
    // invariant broke, and the gate above would mask it, so it is logged loud.
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
    // The client world-swap teardown. A save-transfer join performs two level loads, and a mirror
    // installed in the first world survives the swap as a dangling Element: the host sends no
    // EntityDestroy for a client swap, and the disconnect drain is full-disconnect only. The stale
    // Element then blocks the second world from re-mirroring the same eid, so every mirror whose
    // actor the level load freed is released (tracking only; the actor is gone). A still-live
    // mirror is kept. Game thread only.
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
        // Take drains under the manager mutex; the destructor unregisters outside it. No destroy.
        std::unique_ptr<coop::element::Npc> drained = NpcMirrors().Take(eid);
    }
    if (!deadIds.empty())
        UE_LOGI("npc-mirror[world-swap]: pruned %zu stale dead-actor NPC mirror(s) (re-adopt/re-mirror unblocked)",
                deadIds.size());
}

int DestroyUntrackedClientNpcs() {
    // The client reconciliation: destroy every live allowlisted NPC actor that is not a host mirror
    // (a save-load ghost, an escaped client spawn). The client's entity set is conformed to the
    // host's on join.
    auto* s = coop::npc_sync::GetSession();
    if (!s || s->role() != coop::net::Role::Client) return 0;  // host owns NPCs; never sweep
    if (!g_k2DestroyFn) {
        UE_LOGW("npc-mirror[reconcile]: K2_DestroyActor unresolved -- cannot sweep ghost NPCs");
        return 0;
    }

    // The host-mirrored actor set is snapshotted under the manager mutex and iterated without it
    // (holding it across the walk and the destroys would invite a lock-order inversion with the
    // registry). Every mirror binds its actor before install, on the game thread, so the snapshot
    // is coherent.
    std::vector<coop::element::Npc*> mirrors;
    NpcMirrors().Snapshot(mirrors);
    std::unordered_set<void*> tracked;
    tracked.reserve(mirrors.size() * 2 + 1);
    for (coop::element::Npc* m : mirrors) {
        // A mirror whose actor was freed holds a dangling pointer, which could match a recycled
        // address; IsLiveByIndex rejects it.
        if (m && m->GetActor() && R::IsLiveByIndex(m->GetActor(), m->GetInternalIdx()))
            tracked.insert(m->GetActor());
    }

    const int32_t n = R::NumObjects();
    int destroyed = 0, found = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        // The class filter first: a few pointer compares, and almost nothing in GUObjectArray is an
        // NPC.
        void* cls = R::ClassOf(obj);
        if (!cls || !coop::npc_sync::IsAllowlistedClass(cls)) continue;
        if (tracked.count(obj) > 0) continue;  // a legitimate host mirror -- KEEP
        // The liveness guard before any deref.
        if (!R::IsLive(obj)) continue;
        // CDOs skipped.
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        // The one caller is npc_adoption::Tick, once per world, after the snapshot is delivered and
        // every adoption has bound the client's own save kerfur as a mirror (so it is tracked and
        // kept). Anything still untracked is a ghost: an enemy that escaped the suppressor, or a
        // save NPC the host's world no longer has (a kerfur turned off after the save, for which no
        // spawn arrives).
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

    // 1. The latest batch, if any: an interpolation window per NPC. Per-entry validation is the
    // trust boundary.
    std::vector<coop::net::EntityPoseSnapshot> batch;
    if (s->TakeRemoteNpcBatch(batch)) {
        for (const auto& snap : batch) {
            if (!std::isfinite(snap.x) || !std::isfinite(snap.y) || !std::isfinite(snap.z) ||
                !std::isfinite(snap.yaw) || !std::isfinite(snap.speed)) continue;
            if (std::fabs(snap.x) > 1.0e6f || std::fabs(snap.y) > 1.0e6f || std::fabs(snap.z) > 1.0e6f) continue;
            // The look target is validated only when its bit is set (other NPCs leave it zero with
            // the bit clear); a corrupt one with the bit set drops the snapshot.
            if (snap.stateBits & coop::net::kEntityPoseBitHasLookAt) {
                if (!std::isfinite(snap.lookAtX) || !std::isfinite(snap.lookAtY) || !std::isfinite(snap.lookAtZ)) continue;
                if (std::fabs(snap.lookAtX) > 1.0e6f || std::fabs(snap.lookAtY) > 1.0e6f || std::fabs(snap.lookAtZ) > 1.0e6f) continue;
            }
            // The body yaw: garbage must not reach K2_SetWorldRotation.
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

    // 2. Advance and drive every live mirror every frame. The scratch vector is reused, so the tick
    // does not allocate.
    static std::vector<coop::element::Npc*> elems;
    NpcMirrors().Snapshot(elems);
    for (coop::element::Npc* el : elems)
        if (el && el->IsMirror()) el->Tick();

    // 3. The deferred adoption of save-persisted NPCs and the one-shot ghost sweep, both owned by
    // npc_adoption; a single compare once converged.
    coop::npc_adoption::Tick();
    // 3b. The deferred adoption of save-loaded prop-form kerfurs, the prop analogue; a single bool
    // once empty.
    coop::kerfur_prop_adoption::Tick();

    // 4. The deferred prop divergence sweep: it waits for the save load's late key-minting tail to
    // quiesce, so a prop the host converted away is destroyed by its real key instead of skipped
    // into a ghost. A single bool read when nothing is armed.
    coop::join_membership_sweep::TickClientReconcile();

    // The one-shot kerfur census at quiescence, after both sweeps: reads and logs only.
    coop::kerfur_census::Tick();  // one-shot at quiescence; periodic when kerfur_census=1
}

}  // namespace coop::npc_mirror
