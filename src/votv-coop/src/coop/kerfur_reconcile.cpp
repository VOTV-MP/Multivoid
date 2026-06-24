// coop/kerfur_reconcile.cpp -- see coop/kerfur_reconcile.h. The kerfur off->active dup RETIRE (scope A).

#include "coop/kerfur_reconcile.h"

#include "coop/element/mirror_manager.h"
#include "coop/element/prop.h"
#include "coop/kerfur_entity.h"        // IsKerfurPropClass
#include "coop/net/session.h"
#include "coop/npc_sync.h"             // GetSession (the shared client gate)
#include "coop/save_time_retire_util.h" // FindExactMatch + UnmarkAndDestroy (the shared kernel; pulls in
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
        if (mirrors.count(obj)) continue;                                    // host-driven mirror -> keep
        // RESIDUAL (audit M2, safe in scope): a save-off-prop mid-adoption (bound by kerfur_prop_adoption
        // but not yet a Mirror Element) is NOT excluded here. It cannot be wrongly retired in THIS path: a
        // retire only fires for a kerfur the host turned ON -- so the host is NOT expressing it as an
        // off-prop, so nothing is adopting THAT kerfur's off-prop concurrently, and a DIFFERENT kerfur's
        // in-flight adoption sits at a different save position -> outside the 1cm exact-key match.
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(obj);
        out.push_back(LocalOffProp{obj, R::InternalIndexOf(obj), loc.X, loc.Y, loc.Z});
    }
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
    std::vector<void*> toRetire;
    toRetire.reserve(pendingN);
    for (const auto& [hostEid, key] : g_pendingRetire) {
        (void)hostEid;
        // No secondary tie-break for kerfur (position alone is the key; correctly-adopted off-props are
        // host mirrors, already excluded from `cands`). Ambiguous(>1)/absent(0) -> -1 -> retire nothing.
        const int matchIdx = coop::save_time_retire_util::FindExactMatch(
            cands, consumed, key, [](const LocalOffProp&) { return true; });
        if (matchIdx >= 0) { consumed[matchIdx] = true; toRetire.push_back(cands[matchIdx].actor); }
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
    for (void* actor : toRetire) coop::save_time_retire_util::UnmarkAndDestroy(actor);
    UE_LOGI("kerfur_reconcile: sweep-retire -- %zu of %zu pending save-time kerfur retire(s) at "
            "post-quiescence (the late-loaded stale local off-prop destroyed; the active NPC is the sole "
            "form -- join-window off->active dup fixed)",
            toRetire.size(), pendingN);
    g_pendingRetire.clear();
    return static_cast<int>(toRetire.size());
}

}  // namespace coop::kerfur_reconcile
