// coop/props/prop_destroy_seam.cpp -- the actor-destroy half of the prop lifecycle: the
// K2_DestroyActor patch seam, the explicit converge destroy for a destroy the seam cannot see,
// and the echo-suppressed local destroy. The shared session cache rides
// prop_lifecycle_detail.h.

#include "coop/props/prop_lifecycle.h"

#include "prop_lifecycle_detail.h"  // co-located private header (src tree, not include/)

#include "coop/creatures/kerfur_convert.h"  // TryCaptureKerfurPropDestroy, the destroy-edge first refusal
#include "coop/creatures/kerfur_entity.h"   // ReleaseKerfurForEid, past the refusal
#include "coop/element/mirror_manager.h"
#include "coop/element/prop.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/prop_drop_intent.h"
#include "coop/items/coingun_sync.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/session/world_load_episode.h"
#include "ue_wrap/engine/engine.h"  // IsChildActor
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <string>

namespace coop::prop_lifecycle {

namespace {
namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace PT = coop::prop_element_tracker;
}  // namespace

void DestroySeamBody(void* self) {
    auto* s = LoadSession();
    if (!self || !s) return;
    // The destroy seam fires for every actor destroy in the world, so the keyed-interactable test
    // cannot be promoted to a fast-path gate: it resolves the extra pile classes with object-array
    // walks until all three resolve, and during that window (early boot, UI teardown) every
    // non-prop destroy would burn walks with string allocations. The session-null and
    // not-connected gates stay first. The Element id is captured before the keyed-prop unmark
    // drains the shadow, or every destroy broadcast would carry an invalid id.
    const coop::element::ElementId destroyEid = PT::GetPropElementIdForActor(self);
    PT::UnmarkProcessedInit(self);
    PT::UnmarkKnownKeyedProp(self);
    if (!s->connected()) return;
    if (coop::prop_echo_suppress::ConsumeIncomingDestroy(self)) {
        UE_LOGI("grab_hook[destroy-seam]: actor %p was wire-received destroy -- skip rebroadcast",
                self);
        return;
    }
    if (!ue_wrap::prop::IsKeyedInteractable(self)) return;
    // A dying parent-owned sub-actor (a kerfur's eye camera on every toggle) is destroyed by its
    // parent's engine cascade on every peer, so broadcasting its keyed destroy is at best wire
    // noise (per-peer random keys never match) and at worst a same-key hazard. A cheap read; only
    // keyed actors reach it.
    if (ue_wrap::engine::IsChildActor(self)) return;
    const std::wstring keyStr = ue_wrap::prop::GetInteractableKeyString(self);
    // An empty name stringifies to None. A keyed prop broadcasts by key; the non-keyable trash
    // clump (its key never sticks and always reads None) rides our eid instead, key None plus eid,
    // so the receiver's eid-routable destroy despawns its mirror, the spawn-by-eid symmetry.
    // Without it the clump's morph destroy (the pile-to-clump and clump-to-pile calls destroy the
    // actor) was dropped here, the mirror leaked, and the grab-and-throw duplicated forever. Only
    // an actor with neither a key nor an eid is dropped.
    const bool keyless = (keyStr.empty() || keyStr == L"None");
    const bool hasEid  = (destroyEid != coop::element::kInvalidId);
    if (keyless && !hasEid) return;
    // The world-load episode gate. While a joining client is inside its own world load, the game
    // destroys and recreates every keyed prop as local net-zero rebuild churn, which the client
    // re-binds by key. The destroy patch catches those destroys and would broadcast them, and the
    // host would then destroy its authoritative copies by key: most of its keyed props on a bare
    // join, never recovered. So a client's outbound destroy broadcast is suppressed for the
    // episode; the local destroy already ran, so this peer's world is unaffected. Client-scoped;
    // the role guard is defence in depth on a shared seam. The suppression covers keyless
    // destroys too: inside its own load a client generates no events, it is being torn down and
    // rebuilt, and a joining client otherwise broadcasts one eid-only clump destroy per level
    // pile, each with a client-band eid the host never saw, which the host parks and expires in
    // the same minute its send buffer is full. The window is the load episode or the reconcile
    // window, of any kind: a junk broadcast costs more than a suppressed destroy, which the
    // bracket re-expresses. A keyed suppression in the reconcile window alone warns with a rate
    // latch, since it is the trace if a player's genuine destroy vanished.
    const bool inLoadEpisode = coop::world_load_episode::InEpisode();  // read once
    if (s->role() == coop::net::Role::Client &&
        (inLoadEpisode || coop::world_load_episode::InReconcileWindow())) {
        const bool newSegment = !inLoadEpisode;
        if (newSegment && !keyless) {
            static uint32_t sWarned = 0;
            ++sWarned;
            if (sWarned <= 5 || (sWarned <= 100 && sWarned % 10 == 0) || sWarned % 100 == 0) {
                UE_LOGW("grab_hook[destroy-seam]: CLIENT suppressed KEYED DESTROY #%u actor=%p "
                        "key='%ls' eid=%u -- inside the RECONCILE window (bracket apply churn; "
                        "if a player's genuine destroy vanished, this line is the trace)",
                        sWarned, self, keyStr.c_str(),
                        (destroyEid == coop::element::kInvalidId) ? 0u
                                                                  : static_cast<unsigned>(destroyEid));
            }
            return;
        }
        UE_LOGI("grab_hook[destroy-seam]: CLIENT suppressed %s DESTROY actor=%p key='%ls' eid=%u "
                "-- inside %s (loadObjects/rebuild churn; host-wipe fix, not broadcast)",
                keyless ? "eid-only" : "KEYED", self, keyless ? L"None" : keyStr.c_str(),
                (destroyEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(destroyEid),
                newSegment ? "the reconcile window" : "world-load episode");
        return;
    }
    // The kerfur first refusal at the destroy chokepoint, the destroy-edge twin of the express-side
    // adoption: the turn-on verb destroys its prop after spawning the NPC, so this seam fires
    // mid-conversion, and the kerfur layer must get first refusal before the generic relay (on a
    // client the relay killed the host's authoritative prop before the request landed; on the
    // host the generic broadcast and drain left its own turn-on with no converge). Consulted after
    // the echo and episode gates, since wire teardowns and load churn are not conversions; the
    // capture owns the wire when it returns true. A cheap class-pointer gate inside.
    if (coop::kerfur_convert::TryCaptureKerfurPropDestroy(self, destroyEid)) return;
    // Past the refusal a kerfur prop is dying for real, not converting: drop its record so the id
    // is freed and its currentEid stops answering for a kerfur once the registry recycles it. A
    // no-op for every prop that is not a tracked kerfur's current form, which is nearly all of them.
    coop::kerfur_entity::ReleaseKerfurForEid(destroyEid);
    coop::net::WireKey wk{};
    wk.len = 0;
    if (!keyless) {
        for (size_t i = 0; i < keyStr.size() && i < 31; ++i) {
            wk.data[wk.len++] = static_cast<char>(keyStr[i]);
        }
    }
    const char* roleStr =
        s->role() == coop::net::Role::Host ? "HOST" : "CLIENT";
    // The payload carries both the wire key (the receiver's lookup) and the element id (the
    // routing by id). The id lookup is best-effort: the actor may already be unmarked by the time
    // we get here (a parallel-anim race), in which case it is invalid.
    coop::net::PropDestroyPayload dp{};
    dp.key = wk;
    // The invalid id becomes 0 on the wire, the protocol's sender-had-no-Element sentinel.
    dp.elementId = (destroyEid == coop::element::kInvalidId) ? 0u : destroyEid;
    UE_LOGI("grab_hook[destroy-seam]: %s broadcasting DESTROY actor=%p key='%ls' eid=%u%s",
            roleStr, self, keyless ? L"None" : keyStr.c_str(), dp.elementId,
            keyless ? " (eid-only: trash clump)" : "");
    // If this prop is dying inside the coin gun's verb bracket, the sale is authored first, on
    // this same lane, so FIFO delivers it while the host's copy is still alive, which the mint
    // requires: the sale positions its coins from the sold prop's component. After every gate
    // above, so the load episode and the reconcile window are inherited and a joining client's
    // churn can never author a sale. The capture of the client's own coins is unconditional (it
    // keys on the bracket) while the authorization is decided later and elsewhere, so a refusal
    // takes the local coins too; the destroy below is unchanged, so a refusal costs the item, the
    // same economic outcome as before the sale lane, and the host answers the refusal with a
    // result, so the seller is told. The sale carries the same identity pair as this destroy, the
    // key first and the eid as the keyless fallback: a client mints no Element row for its own
    // save-loaded keyed prop, so the eid alone was 0 for exactly the props a player shoots.
    if (coop::coingun_sync::IsInCoinGunVerb())
        coop::coingun_sync::SendSaleForDyingProp(keyless ? std::wstring() : keyStr, dp.elementId);

    s->SendPropDestroy(dp);  // channel queues internally; always accepted
    // A client that just broadcast a keyed destroy may be about to re-place the same prop (a
    // pickup, then a place). The key is parked so the place authors a host-authoritative drop
    // intent, and only for a key whose destroy we just propagated, so the host re-spawn makes
    // exactly one prop. Never reached inside the world-load episode, so join churn never parks.
    if (!keyless && s->role() == coop::net::Role::Client) {
        coop::prop_drop_intent::NoteClientKeyedDestroy(keyStr);
    }
}

// The patch callback for the actor's destroy: the dying actor is the dispatch context (a member
// call runs on the actor), and the frame's object is merely the caller. Game thread only, the
// same contract the observer had.
void OnK2DestroyFunc(void* context, void* /*srcObj*/, void* /*result*/) {
    DestroySeamBody(context);
}



void DestroyLocalProp(void* actor, bool deferred) {
    if (!actor) return;
    // The slot reference is captured now, while the actor is live (every caller just resolved
    // it): the deferred task probes it a tick later, and a raw pointer with a bare liveness check
    // was the cross-task violator. Alive reads array slots only.
    ue_wrap::CachedObjRef ref;
    ref.Set(actor);
    auto doDestroy = [ref]() {
        static ue_wrap::CachedObjRef sActorCls;
        static void* sDestroyFn = nullptr;
        if (!sActorCls.Alive()) {
            sActorCls.Set(R::FindClass(P::name::ActorClassName));
            sDestroyFn = nullptr;
        }
        if (sActorCls.Raw() && !sDestroyFn) {
            sDestroyFn = R::FindFunction(sActorCls.Raw(), P::name::DestroyActorFn);
        }
        if (!sDestroyFn) {
            UE_LOGW("spawner-suppress: K2_DestroyActor UFunction unresolved -- cannot destroy local %p", ref.Raw());
            return;
        }
        void* target = ref.Get();
        if (!target) {
            UE_LOGI("spawner-suppress: deferred destroy target %p no longer live (already destroyed elsewhere) -- skip",
                    ref.Raw());
            return;
        }
        // Marked before the destroy, so our own observer skips the broadcast.
        coop::prop_echo_suppress::MarkIncomingDestroy(target);
        R::CallFunction(target, sDestroyFn, nullptr);
    };
    if (deferred) {
        GT::Post(doDestroy);
    } else {
        doDestroy();
    }
}


}  // namespace coop::prop_lifecycle
