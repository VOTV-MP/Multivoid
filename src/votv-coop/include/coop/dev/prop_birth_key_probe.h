// coop/dev/prop_birth_key_probe.h -- the client place/birth seam instrumented. Read-only, dev-only.
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
// off, every entry point is a predicate test and a return. Game thread only.
#pragma once

#include <string>

namespace coop::dev::prop_birth_key_probe {

bool IsEnabled();

// A spawn entered the pending vector. `seamKey` is the Key as it read at FinishSpawningActor
// return, before any drain tick waited for loadData -- empty or None when there was none. The
// probe keeps the string rather than a flag because a Key that is PRESENT at the seam and a Key
// that is the actor's FINAL identity are different facts: a spawn can carry a freshly minted key
// there and take its saved one a statement later, and only comparing the two tells them apart.
void NoteEnqueue(void* actor, const std::wstring& cls, const std::wstring& seamKey,
                 bool containerExtract);

// The pending vector was full, so this spawn was never enqueued.
void NotePendingCapHit(void* actor);

// The park FIFO evicted `key` to stay under its cap; no place had consumed it.
void NoteParkEvict(const std::wstring& key);

// The Key read back at drain tick `tries` (0 = it was already there at the seam).
void NoteKeyReadable(void* actor, int tries);

// An entry left the pending vector. `verdict` names the exit; `tries` is the drain ticks it waited.
void NoteDrainExit(void* actor, const char* verdict, int tries, const std::wstring& key);

// Print the tallies and the histogram. Totals are cumulative, so each print is the whole run so
// far, and teardown prints the last.
void EmitVerdict();

// Pump: prints once a period has passed and something has been recorded since. Driven by a tick
// rather than by the probe's own events, because the seam fires in bursts and then goes quiet --
// an event-driven timer never reaches its own deadline on the run this instruments.
void Tick();

}  // namespace coop::dev::prop_birth_key_probe
