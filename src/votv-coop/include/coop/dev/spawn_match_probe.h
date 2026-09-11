// coop/dev/spawn_match_probe.h -- the OnSpawn same-class match internals. Read-only, dev-only.
//
// The match and what it may not take are docs/props.md. The shipped line names the winner alone,
// so a spawn that adopted the wrong neighbour prints exactly like one that adopted the right one.
// This prints the whole candidate set -- slot, distance, local key, mirror binding -- and then
// watches each adopted actor for a destroy landing behind the bind, which is the ordering defect
// where a pose stream is left addressing an actor that is already gone. The watch sits on the
// destroy SEAM, so it sees the death whoever caused it.
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

// `actor` is being destroyed, from the seam every destroy passes through. Reports the age of an
// adoption this destroy lands on; `destroyEid` is the element id the tracker still holds for it,
// 0 for an actor that carried none.
void NoteDestroy(void* actor, uint32_t destroyEid);

// Print the tallies. Cumulative, so each print is the whole run so far.
void EmitVerdict();

// Pump: prints once a period has passed and something has been recorded since. Driven by a tick
// rather than by the probe's own events, because the scans arrive in a join burst and then stop --
// an event-driven timer never reaches its own deadline.
void Tick();

}  // namespace coop::dev::spawn_match_probe
