// coop/props/prop_destroy_seam.cpp -- the actor-destroy half of the prop
// lifecycle: the K2_DestroyActor Func-patch seam (DestroySeamBody /
// OnK2DestroyFunc), the explicit v67 converge destroy
// (SyncDestroyedTrackedProp), and the echo-suppressed local destroy helper
// (DestroyLocalProp).
//
// EXTRACTED from prop_lifecycle.cpp 2026-07-10 (966 LOC, past the 800 soft
// cap; this was the flagged extraction). Behavior preserved byte-for-byte;
// the shared session cache rides prop_lifecycle_detail.h.

#include "coop/props/prop_lifecycle.h"

#include "prop_lifecycle_detail.h"  // co-located private header (src tree, not include/)

#include "coop/creatures/kerfur_convert.h"  // TryCaptureKerfurPropDestroy (destroy-edge first refusal, take-9)
#include "coop/element/mirror_manager.h"
#include "coop/element/prop.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/prop_drop_intent.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/session/world_load_episode.h"
#include "ue_wrap/engine/engine.h"  // IsChildActor (child-actor exclusion, take-7 floating-CCTV RCA)
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
    // The destroy seam fires for EVERY actor destroy in the world.
    // We CANNOT promote IsKeyedInteractable to a fast-path gate here:
    // ue_wrap::prop::IsKeyedInteractable internally calls ResolveExtraBases
    // which does R::FindClass walks for trashBitsPile_C / prop_garbageClump_C /
    // actorChipPile_C until all three resolve. During the pre-resolution
    // window (early boot, widget/UI teardown phase), every non-prop_C
    // actor destroy would burn multiple GUObjectArray walks with wstring
    // allocations -- the documented install-loop bomb pattern (see
    // [[feedback-install-idempotent-o1-steady-state]]). The session-null
    // and not-connected gates here are what historically prevented the
    // bomb from firing during the unresolved-classes window. Keep them
    // first. (Audited + smoke-FAILED + reverted 2026-05-28.)
    // Capture the Prop Element id BEFORE UnmarkKnownKeyedProp drains the
    // shadow (audit fix 2026-05-28 -- the prior order returned kInvalidId
    // on every destroy broadcast).
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
    // CHILD-ACTOR EXCLUSION (2026-07-12, take-7 floating-CCTV RCA; predicate + full rationale:
    // ue_wrap::engine::IsChildActor + prop_element_tracker::MarkPropElement). A dying parent-owned
    // sub-actor (kerfur eye cam on every toggle) is destroyed by its parent's engine cascade on
    // every peer -- broadcasting its keyed destroy is at best wire noise (per-peer random keys
    // never match) and at worst a same-key hazard. Cheap 8-byte read, only keyed actors reach it.
    if (ue_wrap::engine::IsChildActor(self)) return;
    const std::wstring keyStr = ue_wrap::prop::GetInteractableKeyString(self);
    // FName(NAME_None) stringifies to "None" -- a KEYED prop broadcasts by Key (the
    // common path). The NON-KEYABLE trash clump (prop_garbageClump_C: setKey doesn't
    // stick, key always reads None) instead rides OUR eid: broadcast key=None + eid so
    // the receiver's eid-routable OnDestroy despawns its mirror (v26 spawn-by-eid
    // symmetry). WITHOUT this the clump's morph-destroy (toClump/turnToPile call
    // K2_DestroyActor -- IDA-confirmed, votv-chippile-clump-morph-RE-2026-05-27.md) was
    // dropped here -> the mirror leaked -> the infinite grab/throw dupe. Only drop when
    // there is NEITHER a Key NOR an eid (a genuinely unsyncable actor).
    // [[project-bug-trash-chippile-uaf-crash]]
    const bool keyless = (keyStr.empty() || keyStr == L"None");
    const bool hasEid  = (destroyEid != coop::element::kInvalidId);
    if (keyless && !hasEid) return;
    // v107 (2026-07-08) HOST-WIPE ROOT FIX -- world-load episode gate. While a joining CLIENT is
    // inside its own world-load (the game's mainGamemode.loadObjects pre-delete + respawn), the
    // game destroys+recreates every keyed prop as LOCAL, net-zero world-rebuild churn (the client
    // re-binds each key via join_membership_sweep). Pre-v106 those destroys dispatched via EX_*
    // (ProcessEvent-invisible) and never crossed the wire; the v106 K2_DestroyActor Func-patch
    // catches them and would broadcast the destroy half -> the HOST destroys its AUTHORITATIVE
    // copies by key (measured 2026-07-08 bare join: host 3345->1255 keyed props, never recovered).
    // Suppress the OUTBOUND broadcast of KEYED destroys for the duration of the episode; the local
    // K2_DestroyActor already ran, so this peer's world is unaffected. Scoped to KEYED (the wipe is
    // 100%% keyed props) so eid-only (pile) destroys pass through untouched -- piles are already
    // fixed and the host DEFERS them anyway. Client-scoped (the host never arms the episode); the
    // role guard is defense-in-depth on the shared bidirectional seam. Vetted /qf rounds 0-13.
    // See coop/session/world_load_episode.h + research/findings/props-lifecycle/votv-destroy-seam-hostwipe-and-rock-rdrop-RE-2026-07-08.md
    if (!keyless && s->role() == coop::net::Role::Client &&
        coop::world_load_episode::InEpisode()) {
        UE_LOGI("grab_hook[destroy-seam]: CLIENT suppressed KEYED DESTROY actor=%p key='%ls' eid=%u "
                "-- inside world-load episode (loadObjects churn; host-wipe fix, not broadcast)",
                self, keyStr.c_str(),
                (destroyEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(destroyEid));
        return;
    }
    // KERFUR FIRST-REFUSAL at the DESTROY chokepoint (take-9 2026-07-13, the destroy-edge twin of
    // prop_lifecycle's express-side TryAdoptFreshKerfurProp). The turn-on verb spawnKerfuro destroys
    // its prop AFTER spawning the NPC, so this seam fires mid-conversion: the kerfur layer must get
    // first refusal before the generic relay (CLIENT: the relay killed the host's authoritative prop
    // before the turn-on request landed -> the kerfur deleted on every peer; HOST: the generic
    // broadcast + the element drain above left the host's own turn-on with NO converge at all).
    // Consults AFTER the echo/episode gates (wire teardowns + load churn are not conversions); the
    // capture converges/owns the wire itself when it returns true. Cheap class-pointer gate inside.
    if (coop::kerfur_convert::TryCaptureKerfurPropDestroy(self, destroyEid)) return;
    coop::net::WireKey wk{};
    wk.len = 0;
    if (!keyless) {
        for (size_t i = 0; i < keyStr.size() && i < 31; ++i) {
            wk.data[wk.len++] = static_cast<char>(keyStr[i]);
        }
    }
    const char* roleStr =
        s->role() == coop::net::Role::Host ? "HOST" : "CLIENT";
    // v12 (2026-05-28): construct PropDestroyPayload with both wire key
    // (existing receiver lookup path) and elementId (forward-compat for
    // event_feed routing-by-elementId). Lookup is best-effort: actor may
    // have been Unmark'd already by the time we get here (parallel-anim
    // race), in which case elementId is kInvalidId (0xFFFFFFFF on the
    // wire -- distinct from 0 = no Element ever assigned).
    coop::net::PropDestroyPayload dp{};
    dp.key = wk;
    // Translate kInvalidId (C++ sentinel) → 0 (wire sentinel) per the
    // protocol.h contract that "elementId == 0 → sender had no Element".
    dp.elementId = (destroyEid == coop::element::kInvalidId) ? 0u : destroyEid;
    // (v15 stamped a senderContext byte here; v16 PR-FOUNDATION-1b
    // moved stale-gen defense to the header senderEpoch.)
    UE_LOGI("grab_hook[destroy-seam]: %s broadcasting DESTROY actor=%p key='%ls' eid=%u%s",
            roleStr, self, keyless ? L"None" : keyStr.c_str(), dp.elementId,
            keyless ? " (eid-only: trash clump)" : "");
    s->SendPropDestroy(dp);  // channel queues internally; always accepted
    // F2 Inc-1 (2026-07-09): a CLIENT that just broadcast a KEYED destroy may be about to RE-PLACE the
    // same prop (hold-R pickup -> hold-R place). Park the key so the place authors a host-authoritative
    // PropDropIntent -- and ONLY for a key whose destroy we just propagated (the host destroyed its
    // copy via THIS broadcast) -> the host re-spawn makes exactly one prop, no dup. Never reached inside
    // the world-load episode (that path returns above), so join churn never parks. See
    // coop/props/prop_drop_intent.h + [[lesson-client-keyed-prop-move-two-wire-halves]].
    if (!keyless && s->role() == coop::net::Role::Client) {
        coop::prop_drop_intent::NoteClientKeyedDestroy(keyStr);
    }
}

// Func-patch callback for Actor.K2_DestroyActor: the dying actor is the dispatch CONTEXT
// (a member call runs ON the actor); FFrame::Object is merely the caller. K2_DestroyActor
// is game-thread-only in UE4 (actor destruction), same thread contract the PE observer had.
void OnK2DestroyFunc(void* context, void* /*srcObj*/, void* /*result*/) {
    DestroySeamBody(context);
}


void SyncDestroyedTrackedProp(void* actorKey, coop::element::ElementId eid) {
    // See prop_lifecycle.h. Contract: NEVER dereference `actorKey` (the caller
    // may hold a PendingKill or GC-purged pointer; v67 kerfur_convert calls
    // this a tick after the BP-internal destroy). The wire key comes from the
    // ELEMENT; the pointer is only the tracker maps' key.
    if (!actorKey || eid == coop::element::kInvalidId) return;
    std::string key8;
    {
        auto* el =
            coop::element::MirrorManager<coop::element::Prop>::Instance().Get(eid);
        if (!el) return;  // already drained -- double call / raced the real PRE
        key8 = el->GetName();
    }
    auto* s = LoadSession();
    if (s && s->connected()) {
        coop::net::PropDestroyPayload dp{};
        dp.key.len = 0;
        for (size_t i = 0; i < key8.size() && i < 31; ++i) {
            dp.key.data[dp.key.len++] = key8[i];
        }
        dp.elementId = eid;  // eid rides along (keyless elements route by it)
        UE_LOGI("prop_lifecycle[explicit destroy]: broadcasting DESTROY key='%s' eid=%u (BP-internal K2_DestroyActor -- v67 converge)",
                key8.c_str(), static_cast<uint32_t>(eid));
        s->SendPropDestroy(dp);
    }
    // Same teardown the organic destroy seam runs: processed-Init latch
    // out, then UnmarkKnownKeyedProp (key index + reverse map + Element drain
    // via ElementDeleter). Both are pointer-as-map-key only.
    PT::UnmarkProcessedInit(actorKey);
    PT::UnmarkKnownKeyedProp(actorKey);
}


void DestroyLocalProp(void* actor, bool deferred) {
    if (!actor) return;
    auto doDestroy = [actor]() {
        static void* sActorCls = nullptr;
        static void* sDestroyFn = nullptr;
        if (!sActorCls || !R::IsLive(sActorCls)) {
            sActorCls = R::FindClass(P::name::ActorClassName);
            sDestroyFn = nullptr;
        }
        if (sActorCls && !sDestroyFn) {
            sDestroyFn = R::FindFunction(sActorCls, P::name::DestroyActorFn);
        }
        if (!sDestroyFn) {
            UE_LOGW("spawner-suppress: K2_DestroyActor UFunction unresolved -- cannot destroy local %p", actor);
            return;
        }
        if (!R::IsLive(actor)) {
            UE_LOGI("spawner-suppress: deferred destroy target %p no longer live (already destroyed elsewhere) -- skip",
                    actor);
            return;
        }
        // Mark BEFORE calling destroy so OUR PRE-observer skips broadcast.
        coop::prop_echo_suppress::MarkIncomingDestroy(actor);
        R::CallFunction(actor, sDestroyFn, nullptr);
    };
    if (deferred) {
        GT::Post(doDestroy);
    } else {
        doDestroy();
    }
}


}  // namespace coop::prop_lifecycle
