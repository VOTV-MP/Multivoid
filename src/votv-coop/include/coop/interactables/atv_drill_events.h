// coop/interactables/atv_drill_events.h -- [dev] the decision events of the ATV host path, for the ATV drill (coop/dev/atv_gate_drill).
//
// Each ATV mechanism that a drill judges says, at the point where its outcome is decided, one record: a request opened or
// not, an answer arriving, a state applied, a block admitted, a request judged, a module refunded. The drill reads the
// records and decides on them, never on a clock; a time bound in the drill can only call a run INVALID. The records are
// kept only while the `atv_gate_drill` row is on (a static compare at each emit point), 32 per event, newest over oldest.
// Written on the game thread, read from the drill's thread under one lock.
//
// The fields of a record by event are the comments on `Event`.

#pragma once

#include <cstdint>

namespace coop::atv_drill_events {

struct Rec {
    uint32_t ord = 0;   // the order this record was made in, across every event
    int32_t i0 = 0, i1 = 0, i2 = 0;
    float f0 = 0.f, f1 = 0.f, f2 = 0.f;
};

enum Event : int {
    kAsked,        // CLIENT: a request opened for the player's act. i0 op, i1 reqId, f0 amount
    kUngated,      // CLIENT: an act's POST ran and no request was opened for it. i0 op
    kAnswer,       // CLIENT: the host's answer. i0 reqId, i1 reason, i2 closed the last open id, f0 fuel, f1 health, f2 module count
    kStateApplied, // CLIENT: a host state written onto the copy. i0 stateSeq, i1 source (0 state, 1 answer, 2 adoption), i2 module count, f0 fuel, f1 health
    kBlock,        // a condition block's vitals rule. i0 from the host, i1 ack matched, f0 copy fuel before, f1 after, f2 the block's own
    kControls,     // a block's brake, lights and turbo bits taken. i0 mask changed, i1 mask now equal to the block's
    kHornPlayed,   // a horn edge sounded on this copy for another peer. i0 down
    kHornHandled,  // HOST: a horn press from a slot that is not the syncer. i0 slot, i1 refused
    kAccepted,     // HOST: a request applied. i0 op, i1 slot, i2 stateSeq after, f0 fuel, f1 health, f2 module count
    kPublished,    // HOST: a state sent to every client. i0 stateSeq, i1 module count, f0 fuel, f1 health
    kRefund,       // HOST: a duplicate fit's module. i0 spawned
    kEventCount
};

// Record one event (a no-op while the row is off).
void Note(Event e, int32_t i0 = 0, int32_t i1 = 0, int32_t i2 = 0, float f0 = 0.f, float f1 = 0.f, float f2 = 0.f);

// How many of `e` were recorded; a cursor taken now reads only what follows.
uint32_t Count(Event e);

// The next record of `e` after `cursor` (advanced past it), false when there is none. A cursor older than the ring jumps to
// the oldest kept record.
bool Next(Event e, uint32_t& cursor, Rec& out);

// Whether a record of `e` after `cursor` and before order `beforeOrd` satisfies `pred`; the cursor is left where it was.
bool AnyBefore(Event e, uint32_t cursor, uint32_t beforeOrd, bool (*pred)(const Rec&, int32_t arg), int32_t arg);


}  // namespace coop::atv_drill_events
