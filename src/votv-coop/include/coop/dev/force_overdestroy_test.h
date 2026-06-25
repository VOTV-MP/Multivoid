// coop/dev/force_overdestroy_test.h -- dev-only DETERMINISTIC injection of the
// docs/piles/10 in-window pile MASS-UNCLAIM over-destroy, to PROVE the Phase 0
// completeness floor (docs/COOP_STABLE_ID_SIDECAR.md S4) on a real wipe.
//
// THE INJECTION (ini-gated `[dev] force_chippile_unclaim=1`, HOST side): the host
// SKIPS expressing every chipPile this session. The joiner still seeds its ~870
// native chipPiles (its seed-walk is deterministic -- 871 seeded at 13:21:28),
// but with no host expression they stay UNCLAIMED -> its claim sweep dooms them
// en masse. That is EXACTLY the 11:16 condition (host under-expressed the piles),
// now reproduced on demand instead of waiting on the non-deterministic race.
//
// THE PROOF (controlled before/after, SAME flag):
//   BEFORE = baseline binary (NO floor) + flag -> the sweep WIPES ~870 piles
//            (confirms the flag genuinely creates the catastrophe).
//   AFTER  = floor binary + flag -> the host's INDEPENDENT census still counts
//            ~870 (snapshot_census walks GUObjectArray, not the expression path),
//            so the floor KEEPs them: `completeness FLOOR kept ~870` + piles
//            survive.
//
// HOST-ONLY: BuildPropSpawnPayload_ runs only on the expressing host, so the flag
// gate alone suffices (no role plumbing). RULE-2-exempt: diagnostics/test infra,
// gated, never in a release path (the ini key is absent in shipped configs).
#pragma once

namespace coop::dev::force_overdestroy_test {

// True when `[dev] force_chippile_unclaim=1`: the host must SKIP expressing this
// chipPile (BuildPropSpawnPayload_ returns false for it). Latched + logged once.
bool HostSkipChipPileExpression();

}  // namespace coop::dev::force_overdestroy_test
