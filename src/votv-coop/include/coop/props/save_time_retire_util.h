// coop/save_time_retire_util.h -- shared primitives for the save-time exact-key
// reconcile sweeps (the mirror-identity JOIN-WINDOW race class).
//
// Three verified instances of the mirror-identity window-race pattern
// (docs/COOP_MIRROR_IDENTITY_WINDOW_RACE.md) post-quiescence-retire a STALE
// save-loaded local form by matching the host's save-time EXACT position key:
//   - pile_spawn_bind / quiescence_drain  (object<->object: stale native chipPile@old vs proxy@new)
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

#include "coop/props/prop_echo_suppress.h"    // MarkIncomingDestroy (the silencer)
#include "coop/props/prop_element_tracker.h"  // UnmarkKnownKeyedProp
#include "ue_wrap/engine/engine.h"             // DestroyActor
#include "ue_wrap/core/reflection.h"         // IsLiveByIndex
#include "ue_wrap/core/types.h"             // FVector

#include <vector>

namespace coop::save_time_retire_util {

// 1 cm^2 -- exact. The save round-trip is bit-for-bit: both peers loaded the
// SAME transferred save, so a save-loaded actor sits at the host's captured
// save-time position to <1cm. Position uniqueness (no two forms share a save
// position to <1cm) makes the match unambiguous in practice; the ambiguous->
// skip guard below keeps it FAIL-SAFE if two ever sat <1cm apart.
constexpr float kExactMatchR2Cm = 1.0f;

// Silence the K2_DestroyActor PRE observer, then destroy. Game-thread only.
//
// THE SILENCE IS EXPLICIT, NOT A SIDE EFFECT (fixed 2026-08-23, field-measured).
// This used to rely on "UnmarkKnownKeyedProp first, so the seam reads no eid and
// the keyless+no-eid early-out swallows it". That premise is FALSE BY
// CONSTRUCTION: UnmarkKnownKeyedProp DEFERS the Element destruction to
// ElementDeleter::Flush, so the registry's actor->eid reverse is still live when
// the seam runs -- synchronously, inside the DestroyActor on the next line. The
// same deferred-drain hazard is documented at registry_reaper.cpp:267-276.
//
// What it cost, from the Linux 9-fps triage logs (2026-08-23): a joining client
// broadcast 940 PropDestroys carrying its OWN client-band eids while retiring
// level-pile twins the host had never heard of -- the host parked 1,618 of them
// as destroy-before-load and expired them. All of it landed in the join minute,
// the same window in which 485 PropSpawn sends were being refused at enqueue for
// a full send buffer. Pure self-inflicted flood.
//
// The mechanism for "this destroy is local bookkeeping" already existed and is
// what prop_lifecycle::DestroyLocalProp uses: mark it, and the seam consumes the
// mark before it looks at anything else (prop_destroy_seam.cpp:64). Ordering
// requirement: the mark must precede the destroy, because the seam fires INSIDE
// it. UnmarkKnownKeyedProp stays -- it is tracker hygiene, and it was never the
// silencer it was documented to be.
inline void UnmarkAndDestroy(void* actor) {
    coop::prop_echo_suppress::MarkIncomingDestroy(actor);
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
        // Distance FIRST (3 flops rejects ~all candidates), secondaryOk only for the <=few within 1cm: the
        // predicate may be O(k) (the 2026-07-03 hostPos-phase exclusion scans the free-savePos list), and
        // running it per-candidate made the two-phase re-bind's worst case candidates x entries x list-size
        // (perf audit WARN). Both call-site predicates are pure -> order-independent.
        const float dx = cands[i].x - key.X;
        const float dy = cands[i].y - key.Y;
        const float dz = cands[i].z - key.Z;
        if (dx * dx + dy * dy + dz * dz > kExactMatchR2Cm) continue;
        if (!secondaryOk(cands[i])) continue;
        if (!ue_wrap::reflection::IsLiveByIndex(cands[i].actor, cands[i].idx)) continue;
        ++matchCount;
        matchIdx = i;
    }
    return (matchCount == 1) ? matchIdx : -1;
}

}  // namespace coop::save_time_retire_util
