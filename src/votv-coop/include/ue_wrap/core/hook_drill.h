// ue_wrap/core/hook_drill.h -- make the trampoline-retirement contract falsifiable.
//
// The rule it drills lives in `hook.h` under "Retirement": lift a patch, never remove it,
// because MinHook's removal writes a free-list pointer over the trampoline's first bytes
// while a thread may still be about to return through them.
//
// A rule of that shape cannot be trusted on argument alone, and a teardown fix whose only
// evidence is a CI gate going red has shown the INSTRUMENT failing, never the defect. So this
// samples the bytes directly -- and it deliberately does NOT assert that a crash happens. The
// eight bytes the removal writes are a heap pointer: executed as code they may fault, or may
// decode into something that quietly runs. MinHook frees the block only once its used count
// reaches zero, which with a dozen live hooks it never does, so an unmap is not a reliable
// signal either. A drill keyed on a crash would be a coin flip wearing a control's clothes.

#pragma once

namespace ue_wrap::hook_drill {

// Sample a trampoline's first 8 bytes and compare against the previous sample for
// the same `slot`. Inert unless VOTVCOOP_TRAMPOLINE_DRILL=1.
//
// Those bytes are the hooked target's stolen prologue; nothing legitimate rewrites
// them for the life of the process. So:
//   first call            -> records the baseline and logs it
//   bytes CHANGED         -> RED: something freed the slot under a live pointer
//   bytes IDENTICAL       -> GREEN: the patch was lifted and the slot left intact
//
// This is a MEASUREMENT, not a gate: it logs and returns, never refuses. A teardown
// that refuses to proceed is a worse failure than the bug it is watching for.
//
// `when` labels the sample in the log (e.g. "pre-disable" / "post-disable").
// `slot` identifies which trampoline is being tracked, so several can be drilled
// independently in one run.
void SampleTrampoline(const char* when, int slot, const void* trampoline);

}  // namespace ue_wrap::hook_drill
