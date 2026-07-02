// coop/kerfur_reconcile.cpp -- see coop/kerfur_reconcile.h. The kerfur off->active dup RETIRE (scope A),
// v91 DETERMINISTIC: retire the off-prop MIRROR bound at the host eid carried on the npc EntitySpawn. No
// fuzzy 1cm position match (the prior position-keyed sweep is gone -- RULE 2).

#include "coop/creatures/kerfur_reconcile.h"

#include "coop/element/mirror_manager.h"
#include "coop/element/identity_destroy.h"  // RetireMirror (the single destroy funnel, Inc B)
#include "coop/element/prop.h"               // MirrorManager<Prop> (the off-prop mirror at retireOffEid)
#include "coop/creatures/kerfur_entity.h"    // IsKerfurPropClass (class sanity gate before teardown)
#include "coop/props/prop_element_tracker.h" // UnmarkKnownKeyedProp (clear the bound-mirror mark + local maps)
#include "coop/net/session.h"
#include "coop/creatures/npc_sync.h"         // GetSession (the shared client gate)
#include "ue_wrap/engine.h"                  // DestroyActor
#include "ue_wrap/hot_path_guard.h"          // UE_ASSERT_GAME_THREAD
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <unordered_map>

namespace coop::kerfur_reconcile {
namespace {

namespace R = ue_wrap::reflection;

// Off-prop host EIDs to retire (the kerfur turned ON in the join window; its save-loaded off-prop on the
// joiner is the stale dup). Armed from the npc EntitySpawn's retireOffEid; retired at the quiescence sweep
// once the off-prop has bound to its host eid. Game-thread only (no mutex).
// BOUNDED (2026-07-03, docs/piles/12): value = post-quiescence passes the eid stayed unbound. An off-prop
// that never binds (host-only kerfur / bind stolen) must not retry forever -- HasPendingRetire feeds
// quiescence_drain::HasPendingWork, and one unbindable entry pins the 4 Hz full-array drain in perpetuity
// (the same uncapped-leg class as the pos-corrections). ~10 s at the 250 ms debounce, then a LOUD drop.
std::unordered_map<coop::element::ElementId, int> g_pendingRetire;
constexpr int kMaxRetirePasses = 40;

bool IsClient() {
    auto* s = coop::npc_sync::GetSession();
    return s && s->role() == coop::net::Role::Client;
}

}  // namespace

void Reset() {
    g_pendingRetire.clear();
}

bool HasPendingRetire() { return !g_pendingRetire.empty(); }

void ArmPendingRetireByEid(coop::element::ElementId offEid) {
    UE_ASSERT_GAME_THREAD("kerfur_reconcile::ArmPendingRetireByEid");
    if (offEid == coop::element::kInvalidId) return;
    if (g_pendingRetire.emplace(offEid, 0).second)  // log once per eid (not on every re-announce)
        UE_LOGI("kerfur_reconcile: ARMED off->active retire of off-prop eid=%u (deterministic -- the host "
                "turned this kerfur ON in the join window; its npc EntitySpawn carried the off-prop's host "
                "eid; the quiescence sweep retires the mirror bound there once it binds)",
                static_cast<unsigned>(offEid));
}

int SweepReconcileSaveTimeKerfurs() {
    UE_ASSERT_GAME_THREAD("kerfur_reconcile::SweepReconcileSaveTimeKerfurs");
    if (g_pendingRetire.empty()) return 0;
    if (!IsClient()) { g_pendingRetire.clear(); return 0; }

    auto& mgr = coop::element::MirrorManager<coop::element::Prop>::Instance();
    int retired = 0;
    // Retire every pending whose off-prop has now bound; KEEP the not-yet-bound ones for the next sweep
    // (an early drop could let a late async bind leave a surviving duplicate). Deterministic by eid -- no
    // GUObjectArray walk, no position match.
    for (auto it = g_pendingRetire.begin(); it != g_pendingRetire.end();) {
        const coop::element::ElementId offEid = it->first;
        coop::element::Prop* el = mgr.Get(offEid);
        void* actor = el ? el->GetActor() : nullptr;
        // IsLiveByIndex, never raw IsLive: a purge frees the row-held actor while the row lingers; raw IsLive
        // on the freed pointer misreads. Not bound yet -> retry next sweep, BOUNDED (see g_pendingRetire).
        if (!actor || !el || !R::IsLiveByIndex(actor, el->GetInternalIdx())) {
            if (++it->second >= kMaxRetirePasses) {
                UE_LOGW("kerfur_reconcile: dropping pending off-prop retire eid=%u after %d post-quiescence "
                        "passes -- the off-prop never bound here (host-only kerfur / no local twin); a retire "
                        "with no target cannot pin the 4 Hz drain (FPS-pin guard)",
                        static_cast<unsigned>(offEid), it->second);
                it = g_pendingRetire.erase(it);
                continue;
            }
            ++it;
            continue;
        }

        // Class sanity: the mirror at this eid MUST be a kerfur off-prop (defense against an eid mix-up).
        void* cls = R::ClassOf(actor);
        if (!cls || !coop::kerfur_entity::IsKerfurPropClass(cls)) {
            UE_LOGW("kerfur_reconcile: pending retire eid=%u resolves to a NON-kerfur-off-prop mirror "
                    "actor=%p -- NOT retiring (eid mix-up?); dropping the pending entry",
                    static_cast<unsigned>(offEid), actor);
            it = g_pendingRetire.erase(it);
            continue;
        }

        // Mirror-aware teardown (mirror the trash_proxy::RetireProxy shape): clear the bound-mirror mark +
        // local maps, destroy the actor, unbind the Prop mirror Element via the single destroy funnel.
        coop::prop_element_tracker::UnmarkKnownKeyedProp(actor);
        ue_wrap::engine::DestroyActor(actor);
        coop::element::RetireMirror(offEid);  // type-dispatched Take + Enqueue (Inc B)
        UE_LOGI("kerfur_reconcile: retired off-prop eid=%u actor=%p (DETERMINISTIC eid-keyed -- host turned "
                "this kerfur ON in-window; the failed mid-join KerfurConvert left the off-prop alive; the "
                "active NPC is now the sole form)", static_cast<unsigned>(offEid), actor);
        ++retired;
        it = g_pendingRetire.erase(it);
    }
    return retired;
}

}  // namespace coop::kerfur_reconcile
