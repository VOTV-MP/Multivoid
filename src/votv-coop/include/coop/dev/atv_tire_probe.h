// coop/dev/atv_tire_probe.h -- read the ATV's TIRE STATE off both peers so the divergence can be
// OBSERVED instead of derived.
//
// A mirrored ATV runs `processTire` on its own physics impulses, decrements its own
// `tiresDurability[]`, and at zero spawns a wheel that reaches no other peer. Every link of that
// is measured from bytecode; the DEFECT ITSELF has never been observed, which is the weakest
// thing in the case for changing any code. `processTire` is dispatched EX_LocalVirtualFunction at
// all five of its sites, so it is invisible to our ProcessEvent detour and there is nothing to
// hook: the observable is not the VERB but its OUTPUT, and that output is four plain array
// properties. So this reads properties rather than attempting an interception that cannot exist.
//
// Principle 7: coop/ layer; all engine access through ue_wrap. Game thread only.

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::atv_tire_probe {

// The declared types are not guessed, and two of them would have been guessed WRONG:
//     tires            TArray<bool>    one BYTE per element
//     tiresDurability  TArray<float>   4 bytes
//     tiresDirt        TArray<float>   4 bytes
//     tiresFixes       TArray<int32>   4 bytes   <-- INT, not float
//     tiresTypes       TArray<byte>    one byte  <-- BYTE, not int
// `tiresFixes` sits between two float arrays and shares their element size, so it reads like one.
// The blueprint settles it: it subtracts from it with an integer node and round-trips it through a
// struct field named for ints. Typing it float would have punned a small int into ~1e-44 and
// printed a perfectly plausible 0.0 on both peers in every run.

// Emit one [ATVT] line for `atv`. Resolves its own offsets once, non-fatally: a
// field that will not resolve prints its sentinel forever rather than suppressing
// the line, so "unreadable" and "agrees with the other peer" stay distinguishable.
// `idx`/`key`/`ownsTick`/`n` are the caller's, so an [ATVT] line joins to the
// [ATVP] line of the same sample.
void Sample(void* atv, std::size_t idx, const wchar_t* key, bool ownsTick, std::uint32_t n);

}  // namespace coop::atv_tire_probe
