// coop/dev/gnatives_probe.h -- dev-only frequency and cost counter for the blueprint VM's
// local-call dispatch, run ahead of a permanent dispatch substrate to price it.
//
// Gated on ini `gnatives_probe=1`; throwaway diagnostics that never ship. It swaps the
// local-virtual-call and local-final-call handlers in the VM's exec-handler table for a wrapper
// that runs the same filter shape the real substrate would: a game-thread check, then on the
// local-virtual opcode a comparison-index compare against two kerfur verbs and a nine-entry desk,
// drive and laptop family, and last a class-descendant check on the dispatch context that runs on
// EVERY enabled game-thread dispatch, not only on a name hit. It counts the dispatch rate per
// thread class, brackets a 1-in-1024 sample with the cycle counter, and tail-calls the original
// handler. `gnatives_probe_disabled=1` skips the filter, the verb resolve and the size dump, so a
// second run measures the bare tax of a swap that is never removed.
//
// A detached dumper thread reports the per-thread dispatch rates, the average sampled cycles and
// the derived per-frame cost once a second, and while enabled a signal-store size dump every 30 s.

#pragma once

namespace coop::dev::gnatives_probe {

// Resolve GNatives (the exec-handler table) by signature, validate it, swap the two local-call
// slots for the counting wrapper and start the dumper. No-op unless `gnatives_probe=1`. Call
// once from harness boot, after reflection is resolved. Self-latching.
void Init();

}  // namespace coop::dev::gnatives_probe
