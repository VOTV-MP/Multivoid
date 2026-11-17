// coop/dev/end_trace.h -- TEST-ONLY measure (ini end_trace=true / env VOTVCOOP_END_TRACE; default off): what a copy does just after
// a drive End put it to sleep at the End's pose.
//
// A resting prop leaning on a support can leave its End pose after the receiver switched its physics on and put it to sleep: it
// settles, or falls through a pose that was not a valid contact. On the receiver of an End, for the first two ticks after the
// sleep: whether the body reads awake, its linear and angular velocity, and its penetration (from the lowest point of its bounds
// to the surface a short down-trace finds under it, positive when the bottom is below that surface); and about three seconds
// on, where it rests against the End pose's height. Lines start [END-TRACE]. No verdict reads it. Game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace coop::dev::end_trace {

// True when the row is set.
bool IsEnabled();

// A drive End was applied to `actor` and the copy was put to sleep at `endPose` (`slept`: the sleep call ran).
void OnEndApplied(void* actor, uint32_t eid, uint8_t gen, const ue_wrap::FVector& endPose, bool slept);

// Every tick in a world: the pending traces advance.
void Tick();

// A row of this peer's own is open on `actor` (a client author's, or a host coast's), asked every tick it is open: once a second,
// with the open_row_trace row set, says the body's awake flag and how far its pose moved since the last line. `who` names the
// role. Nothing when the row is off.
void NoteOpenRow(const char* who, uint32_t eid, uint8_t gen, void* actor);

}  // namespace coop::dev::end_trace
