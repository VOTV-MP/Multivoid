// ue_wrap/core/hook_drill.h -- make the trampoline-retirement contract falsifiable.
//
// The rule this drills lives in `hook.h` under "Retirement": lift a patch, never
// remove it, because removing writes a linked-list pointer over the trampoline's
// first bytes (`[V]` minhook/src/buffer.c:43-50 + :282, reached from :702
// MH_RemoveHook) while a thread may still be about to return through them.
//
// A rule of that shape cannot be trusted on argument alone. Every comparable arc in
// this project made the defect DETERMINISTIC before believing the fix --
// VOTVCOOP_REAPER_PIN_WORLD turned an 11-flush symptom into 2, the atlas exclude
// drill, the 29-check ApplyPose selftest -- and a teardown fix whose only evidence
// is "the CI gate went red" has shown the INSTRUMENT failing and never the defect.
//
// It deliberately does NOT assert that a crash happens. `[V]` The eight bytes
// MH_RemoveHook writes are a heap pointer; executed as code they may fault or may
// decode into something that quietly runs. And `[V]` buffer.c:288-296 only
// VirtualFrees the block once usedCount hits 0, which with ~12 live hooks it never
// reaches -- so the unmap is not a reliable signal either. A drill keyed on a crash
// would be a coin flip wearing a control's clothes.

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
