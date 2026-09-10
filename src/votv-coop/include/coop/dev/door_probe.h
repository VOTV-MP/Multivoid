// coop/dev/door_probe.h -- dev-only ground-truth probe for the base door state
// machine (ini door_probe=1). Answers, by direct observation rather than
// inference, the question that blocked door coop sync: when the host
// opens a door with no local player at it, WHAT re-closes it, and does the
// autoclose=0 + sensor-overlap-off suppression actually hold it open?
//
// Runs a scripted experiment on ONE door (key from ini door_probe_key, default
// "basedoor_entrance") and logs the full door field set EVERY tick across three
// phases: (1) doorOpen(true) alone [control], (2) doorOpen(true)+suppress
// [production], (3) settime(true) [no-animation snap]. Read the resulting log to
// see exactly when/why isOpened flips. Dev-tool only (RULE 3); never ships.

#pragma once

namespace coop::dev::door_probe {

// Resolve door_C + the watched field offsets. Idempotent; retries until the BP
// class loads. No-op unless ini door_probe=1. Game thread.
void Install();

// Drive the scripted experiment one step per call + log the door state. No-op
// unless enabled / until Install resolved. Game thread.
void Tick();

}  // namespace coop::dev::door_probe
