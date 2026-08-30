// coop/dev/atv_tire_probe.h -- read the ATV's TIRE STATE off both peers so the
// divergence can be OBSERVED instead of derived. 2026-08-30.
//
// WHY IT IS ITS OWN FILE, AND ITS OWN LINE. The defect this exists to see was
// worked out entirely from bytecode (docs/vehicles/ATV.md 17.7/17.8): a mirrored
// ATV runs `processTire` on its own physics impulses, decrements its own
// `tiresDurability[]`, and at zero spawns a wheel that reaches no other peer. Every
// link of that is measured; the DEFECT ITSELF has never been observed, which is
// the weakest thing in the case for changing any code. `processTire` is dispatched
// `EX_LocalVirtualFunction` at all five of its sites, so it is invisible to our
// ProcessEvent detour and there is nothing to hook -- the observable is not the
// VERB but its OUTPUT, and the output is four plain array properties. That is the
// project's standing rule (CLAUDE.md: verify behaviour by diffing observable
// STATE, not just hooks), and it is why this reads properties rather than
// attempting an interception that cannot exist.
//
// Separate TU because `coop/dev/atv_probe.cpp` is 758 LOC against a 800 soft cap
// and tire state is a distinct concern from the pose/rig sampling that file owns
// (folder-per-concept + one-feature-per-file). It borrows nothing from that file
// but the ATV pointer and the ownership flag its caller already computed.
//
// THE DECLARED TYPES ARE NOT GUESSED, and two of them would have been guessed
// WRONG. From research/bp_reflection/ATV.json:
//     tires            TArray<BoolProperty>   ElementSize 1   (one BYTE per element)
//     tiresDurability  TArray<FloatProperty>  ElementSize 4
//     tiresDirt        TArray<FloatProperty>  ElementSize 4
//     tiresFixes       TArray<IntProperty>    ElementSize 4   <-- INT, not float
//     tiresTypes       TArray<ByteProperty>   ElementSize 1   <-- BYTE, not int
// `tiresFixes` sits between two float arrays and reads like one; typing it float
// would have punned a small int into ~1e-44 and printed a perfectly plausible 0.0
// on both peers in every run -- which is exactly the defect a post-ship audit
// found in this same probe's `tirescount` on 2026-08-30, hours before this file.
// See [[lesson-a-sentinel-guards-the-failure-you-imagined-not-the-one-you-get]].
//
// Principle 7: coop/ layer; all engine access through ue_wrap. Game thread only.

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::atv_tire_probe {

// Emit one [ATVT] line for `atv`. Resolves its own offsets once, non-fatally: a
// field that will not resolve prints its sentinel forever rather than suppressing
// the line, so "unreadable" and "agrees with the other peer" stay distinguishable.
// `idx`/`key`/`ownsTick`/`n` are the caller's, so an [ATVT] line joins to the
// [ATVP] line of the same sample.
void Sample(void* atv, std::size_t idx, const wchar_t* key, bool ownsTick, std::uint32_t n);

}  // namespace coop::atv_tire_probe
