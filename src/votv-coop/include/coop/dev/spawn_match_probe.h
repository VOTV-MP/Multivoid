// coop/dev/spawn_match_probe.h -- the OnSpawn fuzzy-match internals. Read-only, dev-only.
//
// A wire spawn whose key resolves to nothing falls back to a 30 cm same-class scan, adopts the
// FIRST candidate in object-array order and rekeys it to the wire key. Where several same-class
// props share that radius -- a drive rack, a shelf of discs -- which one wins is invisible: the
// shipped line names the winner alone, so a spawn that adopted the wrong neighbour and a spawn
// that adopted the right one print the same.
//
// The probe prints the whole candidate set with slot, distance, local key and mirror binding, and
// then watches each adopted actor: a destroy landing on it shortly after the bind is the ordering
// defect where a pose stream is left addressing an actor that is already gone.
//
// Observation only -- the scan returns the same actor with the probe on. Ini-gated [dev]
// spawn_match_probe=1. Game thread only.
#pragma once

#include <cstdint>
#include <string>

#include "ue_wrap/actors/prop.h"    // NearbyTrace
#include "ue_wrap/engine/engine.h"  // FVector

namespace coop::dev::spawn_match_probe {

bool IsEnabled();

// The scan has run for this wire spawn; print its candidates against the wire row.
void NoteFuzzyScan(uint32_t wireEid, const std::wstring& wireKey, const std::wstring& cls,
                   const std::wstring& propName, const ue_wrap::FVector& anchor,
                   const ue_wrap::prop::NearbyTrace& trace);

// `actor` was adopted for `wireEid` past every gate; watch it for a destroy behind the bind.
// `takenDistCm` separates a rekey at the wire spot, which is the dedupe working, from one at
// range, which is a different prop; -1 when the scan did not run.
void NoteFuzzyBound(void* actor, uint32_t wireEid, const std::wstring& wireKey, float takenDistCm);

// A wire destroy resolved `actor`. Reports the age of an adoption this destroy lands on.
void NoteDestroy(void* actor, uint32_t destroyEid);

// Print the tallies. Cumulative, so each print is the whole run so far.
void EmitVerdict();

// Pump: prints once a period has passed and something has been recorded since. Driven by a tick
// rather than by the probe's own events, because the scans arrive in a join burst and then stop --
// an event-driven timer never reaches its own deadline.
void Tick();

}  // namespace coop::dev::spawn_match_probe
