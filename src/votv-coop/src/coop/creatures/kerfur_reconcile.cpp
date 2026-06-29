// coop/kerfur_reconcile.cpp -- see coop/kerfur_reconcile.h. The kerfur off->active dup RETIRE (scope A).

#include "coop/creatures/kerfur_reconcile.h"

#include "coop/element/mirror_manager.h"
#include "coop/element/element_deleter.h"  // ElementDeleter (mirror-aware teardown of a bound off-prop)
#include "coop/element/prop.h"
#include "coop/element/npc.h"          // MirrorManager<Npc> (ROOT-2 probe: is the replacement NPC live?)
#include "coop/creatures/kerfur_entity.h"        // IsKerfurPropClass
#include "coop/props/remote_prop.h"          // ResolveMirrorEidByActor (bound-mirror eid for the teardown)
#include "coop/net/session.h"
#include "coop/creatures/npc_sync.h"             // GetSession (the shared client gate)
#include "coop/props/save_time_retire_util.h" // FindExactMatch + UnmarkAndDestroy (the shared kernel; pulls in
                                         // prop_element_tracker for the Unmark+Destroy retire)
#include "ue_wrap/engine.h"            // GetActorLocation (the local off-prop world pos)
#include "ue_wrap/hot_path_guard.h"    // UE_ASSERT_GAME_THREAD
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::kerfur_reconcile {
namespace {

namespace R = ue_wrap::reflection;

// Save-time retires that MISSED at KerfurConvert-apply (their local off-prop had not async-loaded yet).
// Keyed by the broadcast K; retried at the post-quiescence sweep. Game-thread only (no mutex).
std::unordered_map<uint32_t, ue_wrap::FVector> g_pendingRetire;

// The 1cm exact match + ambiguous->skip + retire is the shared mirror-identity kernel
// (coop/save_time_retire_util.h, kExactMatchR2Cm); the comment there carries the rationale.

bool IsClient() {
    auto* s = coop::npc_sync::GetSession();
    return s && s->role() == coop::net::Role::Client;
}

// A live LOCAL (non-mirror) off-form kerfur candidate found in one GUObjectArray walk.
struct LocalOffProp { void* actor; int32_t idx; float x, y, z; };

// Walk the GUObjectArray once for live LOCAL off-form kerfurs -- prop_kerfurOmega_C (+ skins) that are
// NOT host-driven wire mirrors. A host kerfur off-prop mirror (IsMirror()) is legit host state and must
// never be retired; only the client's own STALE save-loaded local off-prop (the dup) is a candidate.
void CollectLocalOffPropKerfurs(std::vector<LocalOffProp>& out) {
    // The authoritative "host owns this actor" set: prop MIRROR Elements (the same signal kerfur_convert
    // uses -- the per-actor reverse maps go stale on a fuzzy-adopt rekey).
    std::unordered_set<void*> mirrors;
    {
        std::vector<coop::element::Prop*> v;
        coop::element::MirrorManager<coop::element::Prop>::Instance().Snapshot(v);
        for (auto* el : v)
            if (el && el->IsMirror() && el->GetActor()) mirrors.insert(el->GetActor());
    }
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls || !coop::kerfur_entity::IsKerfurPropClass(cls)) continue;  // off-form kerfurs only
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;       // CDO
        // b (#1, 2026-06-26): a LIVE-broadcast off-prop mirror (a kerfur the host turned OFF in-window) is
        // legit current host state -> keep (exclude). But a SAVE-IDENTITY BOUND mirror (IsBoundMirrorNative)
        // can be STALE: the join-window save-identity bind binds the save-loaded off-prop as a host-range
        // mirror at its OLD off eid, and if the host then turned that kerfur ON in the window the off-prop is
        // dead (its NPC form is the truth) -- but the failed mid-join KerfurConvert never destroyed it. Such a
        // stale bound mirror MUST be a retire candidate (the pending-retire's save-time key, armed only on a
        // host turn-ON, is the discriminator: a still-off bound kerfur has no pending retire at its position).
        // So exclude only NON-bound mirrors; include bound mirrors so FindExactMatch can pair the stale one.
        if (mirrors.count(obj) && !coop::prop_element_tracker::IsBoundMirrorNative(obj)) continue;
        // RESIDUAL (audit M2, safe in scope): a save-off-prop mid-adoption (bound by kerfur_prop_adoption
        // but not yet a Mirror Element) is NOT excluded here. It cannot be wrongly retired in THIS path: a
        // retire only fires for a kerfur the host turned ON -- so the host is NOT expressing it as an
        // off-prop, so nothing is adopting THAT kerfur's off-prop concurrently, and a DIFFERENT kerfur's
        // in-flight adoption sits at a different save position -> outside the 1cm exact-key match.
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(obj);
        out.push_back(LocalOffProp{obj, R::InternalIndexOf(obj), loc.X, loc.Y, loc.Z});
    }
}

// b (#1): retire a matched stale off-prop. A save-identity BOUND mirror needs a MIRROR-aware teardown:
// UnmarkAndDestroy alone would leave the Prop mirror Element dangling (UnmarkKnownKeyedProp resolves the
// LOCAL reverse-map only -> early-returns for a mirror -> never Takes the mirror Element, so the host-range
// eid would point at a destroyed actor). Mirror the trash_proxy::RetireProxy teardown: resolve the host eid,
// clear the bound-mirror mark + local maps, destroy the actor, unbind the Prop mirror Element via the deferred
// deleter. A non-mirror LOCAL off-prop keeps the plain UnmarkAndDestroy.
void RetireMatchedOffProp_(void* actor) {
    if (!actor) return;
    if (!coop::prop_element_tracker::IsBoundMirrorNative(actor)) {
        coop::save_time_retire_util::UnmarkAndDestroy(actor);
        return;
    }
    const coop::element::ElementId eid = coop::remote_prop::ResolveMirrorEidByActor(actor);
    coop::prop_element_tracker::UnmarkKnownKeyedProp(actor);   // clear the bound-mirror mark + any local maps
    if (R::IsLive(actor)) ue_wrap::engine::DestroyActor(actor);
    if (eid != coop::element::kInvalidId)
        coop::element::ElementDeleter::Get().Enqueue(
            coop::element::MirrorManager<coop::element::Prop>::Instance().Take(eid));
    UE_LOGI("kerfur_reconcile: retired bound-mirror stale off-prop actor=%p eid=%u (mirror-aware teardown -- "
            "host turned this kerfur ON in-window; the failed mid-join KerfurConvert left the off-prop alive)",
            actor, static_cast<unsigned>(eid));
}

}  // namespace

void Reset() {
    g_pendingRetire.clear();
}

void ArmPendingRetire(uint32_t hostEid, const ue_wrap::FVector& saveTimePos) {
    UE_ASSERT_GAME_THREAD("kerfur_reconcile::ArmPendingRetire");
    const bool isNew = (g_pendingRetire.find(hostEid) == g_pendingRetire.end());
    g_pendingRetire[hostEid] = saveTimePos;  // idempotent per host eid (a re-announce refreshes the key)
    if (isNew)  // log once per kerfur (not on every connect-snapshot re-announce)
        UE_LOGI("kerfur_reconcile: ARMED off->active retire eid=%u at save-time (%.1f,%.1f,%.1f) "
                "(from npc EntitySpawn -- the channel that reaches the joiner; quiescence sweep will retire "
                "the stale local off-prop)", hostEid, saveTimePos.X, saveTimePos.Y, saveTimePos.Z);
}

int SweepReconcileSaveTimeKerfurs() {
    UE_ASSERT_GAME_THREAD("kerfur_reconcile::SweepReconcileSaveTimeKerfurs");
    if (g_pendingRetire.empty()) return 0;
    if (!IsClient()) { g_pendingRetire.clear(); return 0; }
    const size_t pendingN = g_pendingRetire.size();

    // FRESH walk of live LOCAL off-form kerfurs (the late natives are present post-quiescence). Match each
    // pending save-time key to a now-loaded local off-prop within 1 cm; claim-track so two keys never
    // claim one actor; an ambiguous (>1) or absent (0) key is skipped (never retire the wrong one).
    std::vector<LocalOffProp> cands;
    CollectLocalOffPropKerfurs(cands);
    std::vector<bool> consumed(cands.size(), false);
    std::vector<std::pair<void*, uint32_t>> toRetire;  // (off-prop actor, the NPC hostEid that replaces it)
    toRetire.reserve(pendingN);
    for (const auto& [hostEid, key] : g_pendingRetire) {
        // No secondary tie-break for kerfur (position alone is the key; correctly-adopted off-props are
        // host mirrors, already excluded from `cands`). Ambiguous(>1)/absent(0) -> -1 -> retire nothing.
        const int matchIdx = coop::save_time_retire_util::FindExactMatch(
            cands, consumed, key, [](const LocalOffProp&) { return true; });
        if (matchIdx >= 0) { consumed[matchIdx] = true; toRetire.push_back({cands[matchIdx].actor, hostEid}); }
    }

    // NO >50% ratio-valve here (REMOVED 2026-06-24, hands-on 17:06 root). A ratio valve was mis-ported
    // from pile_reconcile, where the denominator is ALL live native piles (claimed + unclaimed) so >50%
    // genuinely flags a racing bracket. Here `cands` is ONLY non-mirror LOCAL off-prop kerfurs -- every
    // correctly-adopted off-prop is a host MIRROR, excluded from `cands` -- so the denominator IS the
    // stale set, and the CORRECT action (retire the lone stale off-prop) is always 100% -> the valve
    // false-aborted the exact case it should allow (17:06: "sweep-retire ABORTED -- 1 of 1 (>50%)" while
    // the off-prop sat on its exact save-time key). The kerfur racing-bracket mode is "off-prop not loaded
    // -> 0 matches" (handled below by no-match), never over-match: each retire is one ARMED pending (a
    // deliberate host turn-on) matched by the EXACT 1cm save-time key under position uniqueness +
    // ambiguous(>1)->skip + the non-mirror IsKerfurPropClass gate -- so the retire set is intrinsically
    // bounded + precise, and no ratio guard fits or is needed.
    for (const auto& [actor, hostEid] : toRetire) {
        // ROOT-2 PROBE (2026-06-29, the "5-of-6 on join"): the off->active retire is DETERMINISTIC (armed
        // by the NPC EntitySpawn), but the replacement NPC materialize is a FUZZY npc_adoption class+pose
        // match that CAN fail. If we retire the off-prop while the replacement NPC (hostEid) is NOT a live
        // bound mirror, this kerfur VANISHES (off-prop gone, NPC never appeared) = the missing-kerfur hole.
        // The 10:30 log did NOT trip this (the fuzzy adopt succeeded there), so this WARN pins whether a
        // real 5-of-6 is a retire-without-replacement. Diagnostic only (RULE-2-exempt); no behaviour change.
        auto* npc = coop::element::MirrorManager<coop::element::Npc>::Instance().Get(
            static_cast<coop::element::ElementId>(hostEid));
        const bool npcLive = npc && npc->GetActor() && R::IsLive(npc->GetActor());
        if (!npcLive)
            UE_LOGW("[KERFUR-R2] retire-WITHOUT-replacement: off-prop actor=%p retired but its replacement "
                    "NPC eid=%u is NOT a live bound mirror -> this kerfur will be MISSING (the 5-of-6 hole). "
                    "npc_adoption fuzzy match has not bound it; a re-toggle is the only recovery.",
                    actor, static_cast<unsigned>(hostEid));
        RetireMatchedOffProp_(actor);
    }
    UE_LOGI("kerfur_reconcile: sweep-retire -- %zu of %zu pending save-time kerfur retire(s) at "
            "post-quiescence (the late-loaded stale local off-prop destroyed; the active NPC is the sole "
            "form -- join-window off->active dup fixed)",
            toRetire.size(), pendingN);
    g_pendingRetire.clear();
    return static_cast<int>(toRetire.size());
}

}  // namespace coop::kerfur_reconcile
