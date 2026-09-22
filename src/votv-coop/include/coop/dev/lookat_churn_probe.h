// coop/dev/lookat_churn_probe.h -- how often the local player's look-at result changes while the
// aim holds still, and which field changes.
//
// AmainPlayer_C::LookAtFunction re-traces every tick and compares the answer against the five
// fields it kept from the frame before. When any one of them differs it stores the new set, calls
// buildActionList() and re-opens the hovertext; the player sees the interaction UI reset and come
// back. An aim held on a resting prop should therefore produce ZERO changes after the first one,
// and every change this probe counts past that is one visible flicker.
//
// The reading is the PERIOD. A trace that loses its target for a frame, a collider that is
// re-registered and an actor whose own state is rewritten all look the same to the player, and are
// told apart by which field moved and how regularly. So the probe keeps one episode per aimed-at
// actor and reports the intervals inside it, not a total. Read-only: five field reads per tick, no
// dispatch, and a single bool read when the flag is off.

#pragma once

namespace coop::dev::lookat_churn_probe {

bool IsEnabled();

// Sample the local player's look-at set and record what moved. Game thread, once per pump tick.
void Tick();

// The totals and the reading, printed on a period while anything is recorded and once more at
// session end, since a rig kills its peers rather than disconnecting them.
void EmitVerdict();

// Drop the episode and the tallies: a verdict that spans two sessions is the sum of two runs.
void OnDisconnect();

}  // namespace coop::dev::lookat_churn_probe
