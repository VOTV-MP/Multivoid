// coop/dev/gnatives_probe.h -- dev-only frequency+cost counter for the EX_Local*
// dispatch substrate (docs/COOP_VM_DISPATCH_PLAN.md, gate 2.2).
//
// Gated on ini `gnatives_probe=1`. THROWAWAY / diagnostics (RULE 2 exempt): it
// measures whether wrapping GNatives[0x45] (EX_LocalVirtualFunction) and
// GNatives[0x46] (EX_LocalFinalFunction) with the substrate's wrapper shape adds
// <= 0.1 ms/frame. It swaps the two slots with a wrapper that does the SAME added
// work the real substrate will (non-destructive operand peek + a full 16-slot
// watch-table miss = cost upper bound) + a per-thread-class counter + a sampled
// RDTSC bracket, then tail-calls the original handler.
//
// Measured facts it is built on (research/findings/world-systems/
// votv-vm-dispatch-RE-2026-07-13.md): GNatives base resolved by AOB on a dispatch
// site; FFrame +0x18 = Object, +0x20 = Code; handler ABI (rcx=Context, rdx=&Stack,
// r8=Result), returns a value the dispatcher ignores but we preserve.
//
// It does NOT ship (RULE 3 dev probe) and is retired with the substrate work.
// A dedicated dumper thread logs GT/worker dispatch rates + avg cycles + the
// derived per-frame cost once per second; window boundaries (boot / join-load /
// steady / pile-burst / solo-SP) are read off the log timestamps.

#pragma once

namespace coop::dev::gnatives_probe {

// Resolve GNatives (AOB), validate, and swap the two EX_Local* slots with the
// counting wrapper; spawn the 1/s dumper. No-op unless `gnatives_probe=1`.
// Call once from the harness boot setup, AFTER reflection is resolved. Self-latches.
void Init();

}  // namespace coop::dev::gnatives_probe
