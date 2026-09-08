// coop/dev/force_overdestroy_test.h -- dev-only deterministic injection of the in-window pile
// MASS-UNCLAIM over-destroy, so the claim sweep's completeness floor can be proved against a real
// wipe instead of waited for.
//
// The injection, `[dev] force_chippile_unclaim=1` on the HOST: the host skips expressing every
// chipPile for the session. The joiner still seeds its own native chipPiles deterministically, but
// with no host expression they stay UNCLAIMED, so its claim sweep dooms them en masse -- exactly
// the condition an under-expressing host produces by accident, now on demand.
//
// The second flag is the control. `[dev] disable_completeness_floor=1` makes the client's sweep
// skip the floor, so the same binary with the injection on WIPES the piles with that flag and KEEPs
// them without it, isolating the floor as the only variable. Host-only in effect, since
// BuildPropSpawnPayload_ runs on the expressing host alone and the flag gate suffices. Diagnostics
// infrastructure, RULE-2-exempt: ini-gated, and the keys are absent from shipped configs.
#pragma once

namespace coop::dev::force_overdestroy_test {

// True when `[dev] force_chippile_unclaim=1`: the host must SKIP expressing this
// chipPile (BuildPropSpawnPayload_ returns false for it). Latched + logged once.
bool HostSkipChipPileExpression();

// True when `[dev] disable_completeness_floor=1`: the CLIENT's claim sweep SKIPS the completeness
// floor, which is the no-floor half of the control above. Dev-only, ini-gated, RULE-2-exempt, and
// absent from any shipped config. Latched and logged once.
bool FloorDisabledForTest();

}  // namespace coop::dev::force_overdestroy_test
