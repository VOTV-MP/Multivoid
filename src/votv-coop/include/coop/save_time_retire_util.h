// coop/save_time_retire_util.h -- shared primitives for the save-time exact-key
// reconcile sweeps (the mirror-identity JOIN-WINDOW race class).
//
// Three verified instances of the mirror-identity window-race pattern
// (docs/COOP_MIRROR_IDENTITY_WINDOW_RACE.md) post-quiescence-retire a STALE
// save-loaded local form by matching the host's save-time EXACT position key:
//   - pile_reconcile  (object<->object: stale native chipPile@old vs proxy@new)
//   - kerfur_reconcile (class-change: stale off-prop kerfur vs the active NPC)
// Both use the SAME inner kernel: a 1cm^2 exact match with consumed[] claim-
// tracking (no two keys claim one actor) + ambiguous(>1)->skip (never destroy
// the wrong one) + an UnmarkKnownKeyedProp+DestroyActor retire. This header
// centralizes ONLY that kernel + the destroy sequence + the 1cm constant, so
// each instance keeps its own .cpp and its own load-bearing seams.
//
// DELIBERATELY NOT here (the per-class seams -- folding them in is how the
// 17:06 regression happened):
//   - the pending map (value type differs: chipType-tagged vs bare position),
//   - the class predicate (IsChipPile vs IsKerfurPropClass),
//   - the mirror-exclusion (kerfur excludes host Prop mirrors; pile does not),
//   - the >50% RATIO VALVE. The valve is DENOMINATOR-DEPENDENT: pile's
//     denominator is ALL live natives (>50% genuinely flags a racing bracket,
//     KEEP it); kerfur's denominator is ONLY the stale set (a valve there
//     false-aborts the lone correct retire -- the 17:06 bug -- so it has NONE).
//     A shared valve would re-introduce that mis-port, so there is NO valve in
//     this header: each sweep applies (or omits) its valve in its OWN .cpp,
//     with the denominator in plain sight.
//
// One-feature header (RULE 2026-05-25); ~70 LOC, header-only (no .cpp).

#pragma once

#include "coop/prop_element_tracker.h"  // UnmarkKnownKeyedProp
#include "ue_wrap/engine.h"             // DestroyActor
#include "ue_wrap/reflection.h"         // IsLiveByIndex
#include "ue_wrap/types.h"             // FVector

#include <vector>

namespace coop::save_time_retire_util {

// 1 cm^2 -- exact. The save round-trip is bit-for-bit: both peers loaded the
// SAME transferred save, so a save-loaded actor sits at the host's captured
// save-time position to <1cm. Position uniqueness (no two forms share a save
// position to <1cm) makes the match unambiguous in practice; the ambiguous->
// skip guard below keeps it FAIL-SAFE if two ever sat <1cm apart.
constexpr float kExactMatchR2Cm = 1.0f;

// Silence the K2_DestroyActor PRE observer (drop the actor's client-minted eid
// FIRST so no stray PropDestroy fires on the superseded client eid -- the same
// fresh-mirror invariant the adopt-bind path enforces), then destroy it.
// Game-thread only (no mutex). This is the byte-identical retire both sweeps ran.
inline void UnmarkAndDestroy(void* actor) {
    coop::prop_element_tracker::UnmarkKnownKeyedProp(actor);
    ue_wrap::engine::DestroyActor(actor);
}

// Find the SINGLE unconsumed candidate within kExactMatchR2Cm of `key`.
//
// `Cand` must expose float `.x .y .z`, `void* .actor`, int32_t `.idx`
// (the InternalIndex captured at collect time, for the GC-robust
// IsLiveByIndex liveness check -- the candidate raw ptr lives across the
// multi-second bracket and must not be deref'd before the index check).
// `secondaryOk(const Cand&)` applies any per-class tie-break (e.g. chipType
// equality for pile; always-true for kerfur). `consumed` is parallel to
// `cands` (same size); the CALLER marks the returned index consumed.
//
// Returns the matched index, or -1 if the match is ambiguous (>1) or absent
// (0) -- in BOTH cases the caller retires nothing (never the wrong one). This
// function is PURE: it mutates nothing and destroys nothing -- the caller owns
// the consume + the retire.
template <typename Cand, typename SecondaryMatch>
int FindExactMatch(const std::vector<Cand>& cands,
                   const std::vector<bool>& consumed,
                   const ue_wrap::FVector& key,
                   SecondaryMatch secondaryOk) {
    int matchCount = 0, matchIdx = -1;
    for (int i = 0; i < static_cast<int>(cands.size()); ++i) {
        if (consumed[i]) continue;
        if (!secondaryOk(cands[i])) continue;
        const float dx = cands[i].x - key.X;
        const float dy = cands[i].y - key.Y;
        const float dz = cands[i].z - key.Z;
        if (dx * dx + dy * dy + dz * dz > kExactMatchR2Cm) continue;
        if (!ue_wrap::reflection::IsLiveByIndex(cands[i].actor, cands[i].idx)) continue;
        ++matchCount;
        matchIdx = i;
    }
    return (matchCount == 1) ? matchIdx : -1;
}

}  // namespace coop::save_time_retire_util
