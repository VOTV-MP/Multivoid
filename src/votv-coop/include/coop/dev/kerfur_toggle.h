// coop/dev/kerfur_toggle.h -- DEV/TEST: programmatic kerfur turn_off/turn_on.
//
// The radial-menu kerfur conversion is EX_LocalVirtualFunction -- invisible to our ProcessEvent
// hook engine -- and needs an actual player at the menu, so the CLIENT conversion-adopt path
// (kerfur_convert claim/park, npc_mirror adopt, the prop fuzzy match) had no autonomous-test
// coverage. This trigger calls the conversion VERB (kerfurOmega_C::dropKerfurProp /
// prop_kerfurOmega_C::spawnKerfuro) directly through reflection on the nearest kerfur, which is
// the same BP body the menu runs, so the local game converts exactly as a real toggle would and
// the poll detects, claims and adopts it.
//
// NOT dev_gate'd: a toggle is a NATIVE client action taken from the menu, not a dev cheat-spawn,
// and the whole point is to exercise the CLIENT toggle. Env + master gated
// (VOTVCOOP_KERFUR_TOGGLE_TRIGGER + [dev] enabled), so it is dead in production. The
// `kerfurtoggle` scenario creates the trigger file on the client after a host kerfur has
// mirrored over.

#pragma once

namespace coop::dev::kerfur_toggle {

// Start the trigger-FILE watcher thread IF env VOTVCOOP_KERFUR_TOGGLE_TRIGGER is
// set. No-op otherwise. Gated by [dev] enabled (master). Call once from harness boot.
void Init();

}  // namespace coop::dev::kerfur_toggle
