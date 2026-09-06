// coop/props/save_time_retire_util.h -- shared primitives for the save-time exact-key
// reconcile sweeps: the mirror-identity JOIN-WINDOW race class.
//
// Two sweeps post-quiescence-retire a STALE save-loaded local form by matching the host's
// save-time EXACT position key -- pile_spawn_bind with quiescence_drain, and
// kerfur_reconcile. Both run the SAME kernel: a 1 cm^2 exact match with consumed[] claim
// tracking so no two keys claim one actor, ambiguous (>1) skipped so the wrong one is never
// destroyed, and an UnmarkKnownKeyedProp plus DestroyActor retire. Centralised here: that
// kernel, the destroy sequence, the 1 cm constant.
// DELIBERATELY NOT here, since folding a per-class seam in is how this regressed once: the
// pending map, the class predicate, the mirror exclusion, and the >50% RATIO VALVE. That
// valve is DENOMINATOR-DEPENDENT -- pile's denominator is ALL live natives, where >50%
// genuinely flags a racing bracket, so pile keeps it; kerfur's is ONLY the stale set, where
// a valve false-aborts the lone correct retire, so kerfur has none. Each sweep applies or
// omits its own in its own .cpp, with the denominator in plain sight.

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
// THE SILENCE IS EXPLICIT, NOT A SIDE EFFECT. It cannot rest on "UnmarkKnownKeyedProp
// first, so the seam reads no eid and the keyless-and-no-eid early-out swallows it":
// UnmarkKnownKeyedProp DEFERS the Element destruction to ElementDeleter::Flush, so the
// registry's actor-to-eid reverse is still live when the seam runs synchronously inside the
// DestroyActor on the next line. The same hazard is documented in registry_reaper.cpp.
//
// Unmarked, a joining client broadcasts a PropDestroy carrying its OWN client-band eid for
// every level-pile twin it retires -- identities the host never heard of, which it parks as
// destroy-before-load and expires -- in the join minute, when the send buffer is already
// refusing spawns. The mark is what prop_lifecycle::DestroyLocalProp uses, and the seam
// consumes it first. It must PRECEDE the destroy, because the seam fires inside it.
// UnmarkKnownKeyedProp stays as tracker hygiene.
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
        // Distance FIRST -- three flops reject nearly every candidate -- and secondaryOk only for
        // the few within 1 cm: the predicate may be O(k), the hostPos-phase exclusion scanning the
        // free-savePos list, and running it per candidate made the two-phase re-bind's worst case
        // candidates x entries x list size. Both call-site predicates are pure, so the order does
        // not matter.
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
