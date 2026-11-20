// coop/player/hand_history.h -- what each slot held in its hand, and when.
//
// A short per-slot ring of (class, from, to) entries, so the host can ask whether a slot held a class
// at any moment in the last N ms. The hand lane stamps it where it APPLIES a slot's state, never at
// receive: that lane paces its applies, so the history follows the hand the host shows. The shape is
// Source's: a per-player history the server judges an act against
// (reference/source-sdk-2013/src/game/server/player_lagcompensation.cpp). Its window there is latency plus
// interpolation, capped at 1 s (:258, :381-399); the windows asked of this one are flat: they cover the hand
// lane's pacing and, for the hand kinds, the fuse. Engine-free and wire-free; game thread only.

#pragma once

#include <cstdint>
#include <string>

namespace coop::hand_history {

// Entries kept per slot; the oldest is overwritten.
inline constexpr int kRing = 4;

// Records that `slot` now holds `cls` (has) or holds nothing (!has), at `nowMs`. The class already
// held changes nothing; a different class or an empty hand closes the open entry first.
void Note(uint8_t slot, bool has, const std::wstring& cls, uint64_t nowMs);

// True when `slot` has an entry for `cls` (a case-insensitive class name) that is open or ended at or after
// nowMs - windowMs. There is no `fromMs <= nowMs` bound, deliberately: a hold that began after `nowMs`, open or
// already closed, also counts, so a launcher taken and fired at once is judged at the shot's arrival time, before the
// hand lane applied the take.
bool HeldWithin(uint8_t slot, const wchar_t* cls, uint64_t windowMs, uint64_t nowMs);

// HeldWithin with the caller deciding which held class names count: `match(held, want)`. A caller that takes a
// class's subclasses passes a walk of the game's class chain, which this engine-free ring does not do itself.
using ClassMatch = bool (*)(const std::wstring& held, const wchar_t* want);
bool HeldWithinMatching(uint8_t slot, ClassMatch match, const wchar_t* want, uint64_t windowMs, uint64_t nowMs);

// Forgets `slot`'s entries (its leave, a session reset).
void Clear(uint8_t slot);

// The slot's entries as `cls[from..to]` items, newest last, for a refusal's diagnostic line. Game thread only.
std::string Describe(uint8_t slot);

// Once per process; logs ALL PASS / n/m checks passed, and later calls return the first verdict silently.
bool RunSelftest();

}  // namespace coop::hand_history
