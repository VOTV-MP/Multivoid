// coop/props/remote_prop_destroy.cpp -- the prop-destroy receiver path, one concept: apply a
// host PropDestroy to this peer. Resolve the doomed local actor (by eid, then key), drain its
// mirror, then the terminal teardown (clear any kinematic drive, release a local grab,
// echo-suppress, destroy). Includes the destroy-before-load deferred re-apply (TryApplyDestroy,
// driven by the quiescence-drain order owner) and the local-consume helpers. The cached
// destroy UFunction lives here, the destroy concept's own state; the convert path's
// echo-destroy routes through DestroyEchoSuppressed (remote_prop_internal.h), so that TU
// never touches it. ResolveLiveActorByEid stays in remote_prop.cpp (the drive, release and
// convert paths share it). Game thread only (the event drain and the quiescence sweep); no
// mutex.

#include "coop/props/native_pile_mirror.h"
#include "coop/props/remote_prop.h"
#include "remote_prop_internal.h"  // impl-private (src-local), NOT under include/

#include "coop/dev/spawn_match_probe.h"        // NoteDestroy (the fuzzy-adoption watch)
#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"     // PropMirrors() (UnregisterPropMirror)
#include "coop/element/quiescence_drain.h"    // ArmPendingDestroy (destroy-before-load capture)
#include "coop/props/join_membership_sweep.h" // HasLoadTailQuiesced (steady-state vs load-tail gate for the defer)
#include "coop/creatures/kerfur_entity.h"     // ForgetKerfurPropMirror (mirror-teardown choke-point)
#include "coop/player/players_registry.h"     // players::Registry::Get().Local() (TryApplyDestroy)
#include "coop/props/prop_echo_suppress.h"    // MarkIncomingDestroy
#include "coop/props/prop_element_tracker.h"  // ResolveLiveActorByKey
#include "coop/props/trash_channel.h"         // ClearClientCarry (destroyed carried proxy)
#include "coop/props/trash_proxy.h"           // IsProxy / RetireProxy (proxy teardown path)
#include "ue_wrap/engine/engine.h"                   // ReleaseMainPlayerGrabIfHolding
#include "ue_wrap/core/hot_path_guard.h"           // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"                     // IsChipPile / IsGarbageClump
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"              // P::name::ActorClassName / DestroyActorFn

#include <string>

namespace coop::remote_prop {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

namespace {

// The cached destroy UFunction for receiver-side destroys.
void* g_destroyActorFn = nullptr;
ue_wrap::CachedObjRef g_actorCls;  // a slot-validated, self-healing cache

bool ResolveDestroyFn() {
    if (g_destroyActorFn && g_actorCls.Alive()) return true;
    g_actorCls.Set(R::FindClass(P::name::ActorClassName));
    if (!g_actorCls.Raw()) return false;
    g_destroyActorFn = R::FindFunction(g_actorCls.Raw(), P::name::DestroyActorFn);
    return g_destroyActorFn != nullptr;
}

// Drop a Prop mirror by the sender's eid. The drain pattern lives inside the manager's Take
// (extract under lock, destruct outside). Idempotent; Take rather than Drop, so the drained
// log line fires only when a mirror was present.
void UnregisterPropMirror(coop::element::ElementId eid) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    auto drained = coop::element::PropMirrors().Take(eid);
    if (!drained) return;
    // Evict the client held-pose map symmetrically (populated at RegisterPropMirror): this is the
    // general mirror-teardown chokepoint, so it also covers a kerfur prop mirror dropped by a
    // plain destroy. A no-op for a non-kerfur mirror and on the host.
    coop::kerfur_entity::ForgetKerfurPropMirror(drained->GetActor());
    UE_LOGI("remote_prop::UnregisterPropMirror: eid=%u drained", eid);
    // drained falls out of scope here; the element destructor unregisters the mirror.
}

// The terminal local teardown of a resolved doomed actor, shared by the in-time destroy and
// the deferred re-apply: clear any drive, release a local grab, echo-suppress, then destroy.
// Game thread.
void DestroyResolvedLocalActor_(void* actor, const std::wstring& keyW,
                                const coop::net::PropDestroyPayload& payload, void* localPlayer) {
    if (!ResolveDestroyFn()) {
        UE_LOGW("remote_prop::OnDestroy: K2_DestroyActor UFunction unresolved -- dropping");
        return;
    }
    UE_LOGI("remote_prop::OnDestroy: key '%ls' eid=%u -> destroying local actor %p",
            keyW.c_str(), payload.elementId, actor);
    if (ue_wrap::prop::IsChipPile(actor) || ue_wrap::prop::IsGarbageClump(actor)) {
        UE_LOGI("[PILE] CLIENT destroy eid=%u -> mirror %p removed (the pile/clump vanished here too, "
                "matching the host)", payload.elementId, actor);
    }
    // Clear any slot's drive on this prop, so nothing drives a destroyed actor next tick (the
    // drive table lives in remote_prop.cpp).
    ClearAnyDriveFor(actor);
    // If this peer's local player is grabbing the doomed actor, tear the grab down cleanly first
    // (a no-op when not grabbed).
    ue_wrap::engine::ReleaseMainPlayerGrabIfHolding(localPlayer, actor);
    // Mark before the engine call, so our destroy observer sees it and skips the broadcast (an
    // echo).
    coop::prop_echo_suppress::MarkIncomingDestroy(actor);
    // Unpin first: a nativized runtime pile mirror is GC-pinned (no save or world reference stops
    // GC), and a rooted pending-kill actor would leak its object-array slot forever. A harmless
    // no-op on a save-loaded native or a keyed prop.
    coop::native_pile_mirror::Unpin(actor);
    R::CallFunction(actor, g_destroyActorFn, nullptr);
}

// The core handler. With allowDefer, the in-time event path: if the doomed save-loaded prop
// has not materialised on this peer yet (it loads on its own timeline and the host's destroy
// raced ahead), the destroy is queued on the drain-edge order owner and re-applied after the
// bind at the quiescence sweep, never dropped, since a destroy dropped before the load left
// the prop to load unopposed, a duplicate. Without it, the deferred re-apply: a still-missing
// actor returns false to stay queued. True iff a local actor was destroyed or a proxy
// retired. Game thread.
bool OnDestroyImpl_(const coop::net::PropDestroyPayload& payload, void* localPlayer, bool allowDefer) {
    // Dispatched from the event drain on the game thread.
    UE_ASSERT_GAME_THREAD("g_drives (remote_prop::OnDestroy)");
    // A trash proxy mirror is retired through its own teardown, which erases the registry entry
    // and so releases the entry's GC pin; destroying the actor by any other route leaves it
    // pinned, and a rooted pending-kill actor anchors its whole world. Trash rides an empty key
    // and an eid; return before the keyed path.
    if (payload.elementId != 0 && payload.elementId != coop::element::kInvalidId &&
        coop::trash_proxy::IsProxy(payload.elementId)) {
        // A destroyed carried proxy clears the local carry-state toggle too (the host aborted the
        // carry), else the next use press throws a dead eid forever. The retire clears the drive.
        coop::trash_channel::ClearClientCarry(payload.elementId);
        coop::trash_proxy::RetireProxy(payload.elementId);
        return true;
    }
    const std::wstring keyW = KeyToWString(payload.key);
    // Resolve the doomed actor before draining the mirror, and eid first: the eid rides the wire
    // exactly, so identity survives key divergence, and a mirror bound under a synthetic row key
    // (the ghost-adopt path) carries a local random actor key the sender's key can never match;
    // key first leaked that actor, the drain dropping the row that held the pointer while the
    // key lookup found nothing. The MTA entity-remove shape resolves by element id only; the key
    // stays as the join-bootstrap fallback for eids this peer has no row for. The drain must
    // come after this lookup, since draining first makes the eid resolve null and the mirror is
    // never destroyed.
    void* actor = nullptr;
    if (payload.elementId != 0 && payload.elementId != coop::element::kInvalidId) {
        actor = ResolveLiveActorByEid(payload.elementId);
    }
    if (!actor && !keyW.empty()) {
        // The arbiter echo short-circuit: if we destroyed this key ourselves as the authority's
        // half of a transaction a client asked for, the client's own destroy behind it is an echo
        // with nothing left to find; our destroy evicted the key from the index, so the resolve
        // below would miss and pay a full object-array walk to rediscover what we know. One-shot,
        // so only the echo is short-circuited and a later destroy of a re-placed prop under the
        // same key resolves normally.
        if (coop::prop_echo_suppress::ConsumeArbiterConsumedKey(keyW)) {
            UnregisterPropMirror(payload.elementId);
            UE_LOGI("remote_prop::OnDestroy: key '%ls' eid=%u -- ECHO of a destroy this host already "
                    "performed as the arbiter; nothing to resolve, no scan paid",
                    keyW.c_str(), payload.elementId);
            return false;
        }
        actor = coop::prop_element_tracker::ResolveLiveActorByKey(keyW);
    }
    // Now drain the wire-received mirror element. It must vacate the registry whether or not the
    // local actor still exists (an echo bounce where we initiated the destroy and the actor is
    // already gone); after the eid resolution above, so that lookup could still see it. A silent
    // no-op for unknown eids.
    UnregisterPropMirror(payload.elementId);
    if (!actor) {
        if (keyW.empty() &&
            (payload.elementId == 0 || payload.elementId == coop::element::kInvalidId)) {
            UE_LOGW("remote_prop::OnDestroy: empty key AND no eid -- dropping");
            return false;
        }
        if (allowDefer) {
            // Destroy-before-load is a load-tail bridge: defer only while the world is still
            // async-loading, where the doomed save prop may yet materialise. In steady state there
            // is no before-load: an absent eid is a host-only or already-gone prop this peer never
            // mirrored, and the destroy is an idempotent no-op. Arming it would pin the order
            // owner's pending-work flag forever and run the full-array reconcile several times a
            // second in perpetuity.
            if (coop::join_membership_sweep::HasLoadTailQuiesced()) {
                UE_LOGI("remote_prop::OnDestroy: key '%ls' eid=%u no local actor in STEADY state (load tail "
                        "quiesced) -- no-op drop (nothing to destroy; not a before-load race; no perpetual defer)",
                        keyW.c_str(), payload.elementId);
                return false;
            }
            // Pre-quiescence, a genuine before-load race: hand the destroy to the drain-edge order
            // owner instead of dropping it; it re-applies at the quiescence sweep, after the bind,
            // so delivery order cannot leak a duplicate, and the order owner drops it if the target
            // never loads.
            UE_LOGI("remote_prop::OnDestroy: key '%ls' eid=%u has no local actor YET -- DEFERRING to the "
                    "quiescence drain-edge (destroy-before-load; the order owner applies it post-bind)",
                    keyW.c_str(), payload.elementId);
            coop::element::quiescence_drain::ArmPendingDestroy(payload);
        } else {
            UE_LOGI("remote_prop::OnDestroy: key '%ls' eid=%u still has no local actor -- keep pending",
                    keyW.c_str(), payload.elementId);
        }
        return false;
    }
    coop::dev::spawn_match_probe::NoteDestroy(actor, payload.elementId);
    DestroyResolvedLocalActor_(actor, keyW, payload, localPlayer);
    return true;
}

}  // namespace

void OnDestroy(const coop::net::PropDestroyPayload& payload, void* localPlayer) {
    OnDestroyImpl_(payload, localPlayer, /*allowDefer=*/true);
}

// The deferred re-apply, called by the drain-edge order owner at the quiescence sweep.
// Resolves the local player itself (the sweep has no caller-passed pawn). True iff the doomed
// actor has now loaded and was destroyed, so the owner erases it from the pending queue;
// false keeps it queued. Never re-arms.
bool TryApplyDestroy(const coop::net::PropDestroyPayload& payload) {
    void* localPlayer = coop::players::Registry::Get().Local();
    return OnDestroyImpl_(payload, localPlayer, /*allowDefer=*/false);
}

void ConsumeLocalActor(void* actor) {
    // An echo-suppressed local destroy, used by the spawn receiver to consume this peer's own copy
    // of a shared world chipPile when the other peer grabbed theirs (the pile is a separate local
    // object with no cross-peer id, destroyed directly, by position). The mark makes our own
    // destroy observer skip re-broadcasting it: a local consume, not a new authoritative destroy.
    UE_ASSERT_GAME_THREAD("ConsumeLocalActor (K2_DestroyActor)");
    if (!actor || !R::IsLive(actor)) return;
    if (!ResolveDestroyFn()) {
        UE_LOGW("remote_prop::ConsumeLocalActor: K2_DestroyActor unresolved -- cannot consume %p", actor);
        return;
    }
    coop::prop_echo_suppress::MarkIncomingDestroy(actor);
    // A pinned materialised native reaching here (a redundant convert-landed pile the save load
    // consumes) must be unpinned first: destroying a rooted actor only sets pending kill while
    // the root keeps it alive, a live orphan. The same release the authoritative destroy path
    // does; a no-op on an unpinned game native.
    coop::native_pile_mirror::Unpin(actor);
    R::CallFunction(actor, g_destroyActorFn, nullptr);
}

// The echo-suppressed retire of the old rendering after a convert rebind (declared in
// remote_prop_internal.h, so the convert TU calls it without touching the destroy function):
// clear any drive, then the echo-suppressed destroy.
void DestroyEchoSuppressed(void* actor) {
    if (!actor) return;
    ClearAnyDriveFor(actor);
    if (ResolveDestroyFn()) {
        coop::prop_echo_suppress::MarkIncomingDestroy(actor);
        R::CallFunction(actor, g_destroyActorFn, nullptr);
    } else {
        // Drop with a warning, as the other destroy paths do, not a silent no-op.
        UE_LOGW("remote_prop::DestroyEchoSuppressed: K2_DestroyActor unresolved -- actor %p not destroyed", actor);
    }
}

}  // namespace coop::remote_prop
