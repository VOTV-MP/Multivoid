// coop/dev/prop_birth_key_probe.h -- the prop place/birth seam instrumented. Read-only, dev-only.
//
// A client's fresh keyed prop spawn is enqueued at FinishSpawningActor and drained a tick later,
// because the game restores the save Key inside loadData, after the spawn returns. Seven drain
// exits discard an entry and three of them are bounded registers -- an 8-tick key wait, a 32-entry
// pending vector, a 64-key park FIFO -- so a prop that never reaches the host leaves no record of
// which bound it hit, or whether it hit one at all.
//
// The probe tallies every exit and histograms the drain tick on which the Key became readable.
// That histogram is the measurement the wait was sized by guess: clustered at 0-2 the wait is not
// what loses props, and anything at the ceiling names the register as the loss.
//
// Observation only -- no admission changes with it on. Ini-gated [dev] prop_birth_key_probe=1;
// off, every entry point is a predicate test and a return. Game thread only. Both roles: the
// finish-spawn hook is role-gated after the birth record, not before.
#pragma once

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::prop_birth_key_probe {

bool IsEnabled();

// A spawn entered the pending vector. `seamKey` is the Key as it read at FinishSpawningActor
// return, before any drain tick waited for loadData -- empty or None when there was none. The
// probe keeps the string rather than a flag because a Key that is PRESENT at the seam and a Key
// that is the actor's FINAL identity are different facts: a spawn can carry a freshly minted key
// there and take its saved one a statement later, and only comparing the two tells them apart.
void NoteEnqueue(void* actor, const std::wstring& cls, const std::wstring& seamKey,
                 bool containerExtract);

// Any keyed-prop birth while the probe is on, whatever the session's state -- what arms the
// DESTROY line; separate from NoteEnqueue, which counts the place-detection pending vector and
// keeps its verdict's meaning.
void NoteBirth(void* actor);

// An actor this probe saw born (NoteBirth) reached the destroy seam. `caller` is the object whose
// script called K2_DestroyActor (null for a native destroy); `s` is the session as the destroy
// seam holds it (null before the first boot). The line names the caller, the session's state at
// that instant (none, connected, joining, stopped) and the world the actor belongs to beside the
// current one, which tells a world's own teardown from a script that spawned and removed the prop.
// Bounded per world and per caller: past 64 lines a `capped` line prints and the rest are counted.
void NoteDestroy(void* actor, void* caller, const coop::net::Session* s);

// The pending vector was full, so this spawn was never enqueued.
void NotePendingCapHit(void* actor);

// The park FIFO evicted `key` to stay under its cap; no place had consumed it.
void NoteParkEvict(const std::wstring& key);

// The Key read back at drain tick `tries` (0 = it was already there at the seam).
void NoteKeyReadable(void* actor, int tries);

// An entry left the pending vector. `verdict` names the exit; `tries` is the drain ticks it waited.
void NoteDrainExit(void* actor, const char* verdict, int tries, const std::wstring& key);

// The enqueue count now, and the verdict of the drain exit of the newest entry of class `cls` enqueued AFTER `mark` (empty
// while none of them has left). A drill takes the mark just before the call that causes a birth and reads that birth's
// end as the event that decides it, whatever else of the class is born meanwhile. Dev only.
uint64_t EnqueueMark();
std::string ExitSince(const std::wstring& cls, uint64_t mark);

// Print the tallies and the histogram. Totals are cumulative, so each print is the whole run so
// far, and teardown prints the last.
void EmitVerdict();

// Pump: prints once a period has passed and something has been recorded since. Driven by a tick
// rather than by the probe's own events, because the seam fires in bursts and then goes quiet --
// an event-driven timer never reaches its own deadline on the run this instruments.
void Tick();

}  // namespace coop::dev::prop_birth_key_probe
