// coop/props/remote_prop_destroy.cpp -- the prop-DESTROY receiver path.
//
// EXTRACTED from remote_prop.cpp 2026-06-30 (anti-smear: remote_prop.cpp was over the 800 soft cap; the
// destroy path is a distinct concept). ONE concept: apply a host PropDestroy to this peer -- resolve the
// doomed local actor (by key or eid), drain its mirror, then the terminal teardown (clear any kinematic
// drive, release a local grab, echo-suppress, K2_DestroyActor). Includes the destroy-before-load deferred
// re-apply (TryApplyDestroy, driven by the quiescence_drain order owner) and the local-consume helpers.
//
// The cached K2_DestroyActor UFunction lives here (it is the destroy concept's own state); remote_prop.cpp's
// OnConvert echo-destroy routes through DestroyEchoSuppressed (declared in remote_prop_internal.h) so that TU
// never touches the destroy-fn global. ResolveLiveActorByEid stays in remote_prop.cpp (used by the drive /
// release / convert paths too) and is shared via remote_prop_internal.h.
//
// Game-thread ONLY (the event_feed drain + the quiescence sweep). No mutex.

#include "coop/props/remote_prop.h"
#include "remote_prop_internal.h"  // impl-private (src-local), NOT under include/

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
#include "ue_wrap/engine.h"                   // ReleaseMainPlayerGrabIfHolding
#include "ue_wrap/hot_path_guard.h"           // UE_ASSERT_GAME_THREAD
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"                     // IsChipPile / IsGarbageClump
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"              // P::name::ActorClassName / DestroyActorFn

#include <string>

namespace coop::remote_prop {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

namespace {

// Cached K2_DestroyActor UFunction for receiver-side destroys.
void* g_destroyActorFn = nullptr;
void* g_actorCls       = nullptr;

bool ResolveDestroyFn() {
    if (g_destroyActorFn && R::IsLive(g_actorCls)) return true;
    g_actorCls = R::FindClass(P::name::ActorClassName);
    if (!g_actorCls) return false;
    g_destroyActorFn = R::FindFunction(g_actorCls, P::name::DestroyActorFn);
    return g_destroyActorFn != nullptr;
}

// Drop a Prop mirror by sender's eid. Drain pattern lives inside MirrorManager::Take (extract under lock,
// drained-destructs outside). Idempotent: silent no-op if eid is not in the map. Use Take (not Drop) so the
// "drained" log line only fires when a mirror actually was present -- avoids a misleading confirmation on
// PropDestroy bounces for eids that were never registered locally (audit fix 2026-05-29).
void UnregisterPropMirror(coop::element::ElementId eid) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    auto drained = coop::element::PropMirrors().Take(eid);
    if (!drained) return;
    // K-5: evict the client held-pose map symmetrically (it is populated at RegisterPropMirror). This is the
    // general mirror-teardown choke-point -- covers a kerfur prop mirror dropped by a plain PropDestroy (not
    // just the conversion path's explicit evict). No-op for a non-kerfur mirror / on the host (the map is
    // empty there). GetKerfurMirrorEidForActor also self-heals, but evicting here keeps the map tight + the
    // lifecycle symmetric (RegisterPropMirror populates <-> this evicts).
    coop::kerfur_entity::ForgetKerfurPropMirror(drained->GetActor());
    UE_LOGI("remote_prop::UnregisterPropMirror: eid=%u drained", eid);
    // drained falls out of scope here; ~Prop -> ~Element -> Registry::UnregisterMirror (via m_mirror=true).
}

// The terminal local teardown of a RESOLVED doomed actor -- shared by the in-time OnDestroy and the deferred
// re-apply (quiescence_drain::ApplyPendingDestroys -> TryApplyDestroy). Clears any drive, releases a local
// grab, echo-suppresses, then K2_DestroyActor. Game thread.
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
    // If any slot was kinematically driving this prop, clear that slot's cache so we don't try to drive a
    // destroyed actor next tick. (ClearAnyDriveFor stays in remote_prop.cpp -- it owns g_drives.)
    ClearAnyDriveFor(actor);
    // 2026-05-25 cross-peer destroy: if this peer's local mainPlayer is currently grabbing the doomed actor,
    // tear down the PHC grab cleanly first (no-op when not grabbed). Routed through ue_wrap::engine (Principle 7).
    ue_wrap::engine::ReleaseMainPlayerGrabIfHolding(localPlayer, actor);
    // Mark BEFORE the engine call so our K2_DestroyActor PRE observer sees it and skips the broadcast (echo).
    coop::prop_echo_suppress::MarkIncomingDestroy(actor);
    // Un-root FIRST: a nativized runtime pile mirror (native_pile_mirror) is AddToRoot'd (no save/world ref
    // to stop GC), so a rooted PendingKill actor would leak its GUObjectArray slot forever. Harmless no-op on
    // a save-loaded native / keyed prop (never rooted by us). Mirrors the proxy's RetireProxy un-root.
    R::RemoveFromRoot(actor);
    R::CallFunction(actor, g_destroyActorFn, nullptr);
}

// Core PropDestroy handler. `allowDefer`=true is the in-time event path: if the doomed save-loaded prop has
// NOT materialized on this peer yet (it loads on its own timeline; the host's destroy raced ahead), the
// destroy is QUEUED on the drain-edge order owner (quiescence_drain::ArmPendingDestroy) and re-applied AFTER
// the bind at the quiescence sweep -- NEVER dropped (the 2026-06-30 destroy-before-load 5-vs-7 race: a host
// destroy arrived 9s before the client loaded its Nrby off-prop -> "no local actor" -> the prop then loaded
// unopposed -> a dup). `allowDefer`=false is the deferred re-apply (TryApplyDestroy): a still-missing actor
// just returns false to stay queued. Returns true iff a local actor was destroyed (or a proxy retired). GT.
bool OnDestroyImpl_(const coop::net::PropDestroyPayload& payload, void* localPlayer, bool allowDefer) {
    // Clears g_drives[*] if the destroyed actor was under drive (T-10, GT-only).
    // Dispatched from event_feed::Update on the game thread (PropDestroy case).
    UE_ASSERT_GAME_THREAD("g_drives (remote_prop::OnDestroy)");
    // PROXY PATH (phase 1): a trash proxy mirror is retired through its OWN teardown (Destroy ->
    // RemoveFromRoot -> unbind). The rooted AStaticMeshActor MUST be un-rooted here or it leaks its
    // GUObjectArray slot forever. Trash rides key=None + an eid; return before the legacy keyed/BP path.
    if (payload.elementId != 0 && payload.elementId != coop::element::kInvalidId &&
        coop::trash_proxy::IsProxy(payload.elementId)) {
        // v85: a destroyed carried proxy must clear the local carry-state toggle too (the host aborted the
        // carry -- audit HIGH), else the next E-press throws a dead eid forever. RetireProxy clears the drive.
        coop::trash_channel::ClearClientCarry(payload.elementId);
        coop::trash_proxy::RetireProxy(payload.elementId);
        return true;
    }
    const std::wstring keyW = KeyToWString(payload.key);
    // Resolve the doomed actor BEFORE draining the mirror. KEYED props resolve by Key (a
    // separate key->actor index). The NON-KEYABLE trash clump rides key=None + an eid and
    // resolves THROUGH the mirror Registry (ResolveLiveActorByEid -> Registry::Get(eid)),
    // so UnregisterPropMirror MUST come AFTER this lookup -- draining first makes the eid
    // resolve null and the mirror is never destroyed (the 2026-06-03 clump-dupe
    // regression: "OnDestroy: eid=N has no local actor" while the ball kept piling up).
    // Symmetric with the v26 eid-SPAWN; MTA Packet_EntityRemove resolves by element ID
    // only. [[project-bug-trash-chippile-uaf-crash]]
    void* actor = nullptr;
    if (!keyW.empty()) {
        actor = coop::prop_element_tracker::ResolveLiveActorByKey(keyW);
    } else if (payload.elementId != 0 && payload.elementId != coop::element::kInvalidId) {
        actor = ResolveLiveActorByEid(payload.elementId);
    }
    // Now drain the wire-received mirror Element. It must vacate the Registry regardless
    // of whether the local actor still exists (echo bounce where we initiated the destroy
    // and the actor is already gone). Done AFTER the eid resolution above so that lookup
    // could still see it. Silent no-op for unknown eids (legacy elementId==0 senders).
    UnregisterPropMirror(payload.elementId);
    if (!actor) {
        if (keyW.empty() &&
            (payload.elementId == 0 || payload.elementId == coop::element::kInvalidId)) {
            UE_LOGW("remote_prop::OnDestroy: empty key AND no eid -- dropping");
            return false;
        }
        if (allowDefer) {
            // DESTROY-BEFORE-LOAD is a LOAD-TAIL BRIDGE: defer ONLY while the world is still async-loading
            // (pre-quiescence), where the doomed save-prop may yet materialize. In STEADY STATE (load tail
            // quiesced) there is no "before load" -- an absent eid is a host-only / already-gone prop this peer
            // never mirrored, so the destroy is an idempotent NO-OP. Arming it would pin the order owner's
            // HasPendingWork() true FOREVER (a host steady-state death-watch vanish for an eid we never had),
            // running the full-array reconcile 4x/sec in perpetuity (the 15:34 client-FPS death). Drop it.
            if (coop::join_membership_sweep::HasLoadTailQuiesced()) {
                UE_LOGI("remote_prop::OnDestroy: key '%ls' eid=%u no local actor in STEADY state (load tail "
                        "quiesced) -- no-op drop (nothing to destroy; not a before-load race; no perpetual defer)",
                        keyW.c_str(), payload.elementId);
                return false;
            }
            // Pre-quiescence: a genuine before-load race. Hand the destroy to the drain-edge ORDER OWNER instead
            // of dropping it -- it re-applies at the quiescence sweep, AFTER the bind, so delivery order can't
            // leak a dup (and the order owner now drops it if the target never loads). [[feedback-one-owner-order-axis]]
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
    DestroyResolvedLocalActor_(actor, keyW, payload, localPlayer);
    return true;
}

}  // namespace

void OnDestroy(const coop::net::PropDestroyPayload& payload, void* localPlayer) {
    // (The take-4 wire-order netting call that lived here is RETIRED by the 2026-07-12 join
    // barrier: no spawn is ever captured in-episode anymore, so there is nothing to cancel --
    // wire events apply strictly in arrival order on a settled world.)
    OnDestroyImpl_(payload, localPlayer, /*allowDefer=*/true);
}

// Deferred re-apply, called by the drain-edge order owner (quiescence_drain::ApplyPendingDestroys) at the
// quiescence sweep. Resolves the local player itself (the sweep has no caller-passed pawn). Returns true iff
// the doomed actor has now loaded + was destroyed (so the order owner erases it from the pending queue); false
// keeps it queued for the next drain. NEVER re-arms (allowDefer=false) -- the order owner already owns it.
bool TryApplyDestroy(const coop::net::PropDestroyPayload& payload) {
    void* localPlayer = coop::players::Registry::Get().Local();
    return OnDestroyImpl_(payload, localPlayer, /*allowDefer=*/false);
}

void ConsumeLocalActor(void* actor) {
    // Echo-suppressed local destroy. Used by OnSpawn to consume THIS peer's own copy of a shared world
    // chipPile when the other peer grabbed theirs (the pile is a separate local UObject with no cross-peer
    // id; we destroy it directly, by position). The MarkIncomingDestroy makes our own K2_DestroyActor PRE
    // observer skip re-broadcasting it -- this is a local consume, not a new authoritative destroy to fan
    // out. [[project-bug-trash-chippile-uaf-crash]]
    UE_ASSERT_GAME_THREAD("ConsumeLocalActor (K2_DestroyActor)");
    if (!actor || !R::IsLive(actor)) return;
    if (!ResolveDestroyFn()) {
        UE_LOGW("remote_prop::ConsumeLocalActor: K2_DestroyActor unresolved -- cannot consume %p", actor);
        return;
    }
    coop::prop_echo_suppress::MarkIncomingDestroy(actor);
    // A rooted materialized native (native_pile_mirror) reaching here (e.g. a redundant convert-landed pile the
    // save-load consumes) MUST be un-rooted first: K2_DestroyActor on a rooted actor only sets PendingKill while
    // the root keeps it ALIVE -> it leaks as a live orphan (the 16:42 join-window mass-move dup). Same un-root
    // the authoritative OnDestroy path already does; no-op on an unrooted game-native (the common consume case).
    R::RemoveFromRoot(actor);
    R::CallFunction(actor, g_destroyActorFn, nullptr);
}

// Echo-suppressed retire of the OLD rendering after an OnConvert re-skin rebind (declared in
// remote_prop_internal.h so remote_prop.cpp's OnConvert calls it without touching the destroy-fn global).
// Same sequence OnConvert ran inline pre-extraction: clear any drive, then echo-suppressed K2_DestroyActor.
void DestroyEchoSuppressed(void* actor) {
    if (!actor) return;
    ClearAnyDriveFor(actor);
    if (ResolveDestroyFn()) {
        coop::prop_echo_suppress::MarkIncomingDestroy(actor);
        R::CallFunction(actor, g_destroyActorFn, nullptr);
    } else {
        // Parity with DestroyResolvedLocalActor_ / ConsumeLocalActor (the old inline OnConvert echo shared
        // this warning path before the extraction). Drop-with-WARN, not a silent no-op.
        UE_LOGW("remote_prop::DestroyEchoSuppressed: K2_DestroyActor unresolved -- actor %p not destroyed", actor);
    }
}

}  // namespace coop::remote_prop
