// coop/interactables/atv_drill_events.h -- [dev] the decision events of the ATV host path and the ATV riders, for the ATV drills (coop/dev/atv_gate_drill, coop/dev/atv_riders_drill).
//
// Each ATV mechanism that a drill judges says, at the point where its outcome is decided, one record: a request opened or
// not, an answer arriving, a state applied, a block admitted, a request judged, a module refunded, a seat or a rider changed,
// a body seated, a kerfur riding. The drill reads the records and decides on them, never on a clock; a time bound in the
// drill can only call a run INVALID. The records are kept only while the `atv_gate_drill` or the `atv_riders_drill` row is on
// (a static compare at each emit point), 256 per event, newest over oldest.
// Written on the game thread, read from the drill's thread under one lock.
//
// The fields of a record by event are the comments on `Event`.

#pragma once

#include <cstdint>
#include <string>

namespace coop::atv_drill_events {

struct Rec {
    uint32_t ord = 0;   // the order this record was made in, across every event
    int32_t i0 = 0, i1 = 0, i2 = 0, i3 = 0;
    float f0 = 0.f, f1 = 0.f, f2 = 0.f;
};

enum Event : int {
    kAsked,        // CLIENT: a request opened for the player's act. i0 op, i1 reqId, f0 amount
    kUngated,      // CLIENT: an act's POST ran and no request was opened for it. i0 op
    kAnswer,       // CLIENT: the host's answer. i0 reqId, i1 reason, i2 closed the last open id, f0 fuel, f1 health, f2 module count
    kStateApplied, // CLIENT: a host state written onto the copy. i0 stateSeq, i1 source (0 state, 1 answer, 2 adoption), i2 module count, f0 fuel, f1 health
    kBlock,        // a condition block's vitals rule. i0 from the host, i1 ack matched, f0 copy fuel before, f1 after, f2 the block's own
    kControls,     // a block's brake, lights, turbo and horn bits taken. i0 mask changed, i1 mask now equal to the block's (the horn: its replay ran), i2 mask this block made the copy change (the horn: its replay ran), f0 the block's control bits
    kAccepted,     // HOST: a request applied. i0 op, i1 slot, i2 stateSeq after, f0 fuel, f1 health, f2 module count
    kPublished,    // HOST: a state sent to every client. i0 stateSeq, i1 module count, f0 fuel, f1 health
    kRefund,       // HOST: a duplicate fit's module. i0 spawned
    kStateGated,   // CLIENT: a state arrived while the open-request gate was shut. i0 stateSeq, i1 applied anyway, f0 the state's fuel, f1 the copy's fuel after
    kUnadopted,    // CLIENT: a state arrived for an ATV with no copy. i0 stateSeq, i1 kept, i2 KeyTag
    kAdopted,      // CLIENT: a copy entered the index. i0 a kept state applied, i1 KeyTag, i2 stateSeq, f0 fuel, f1 health, f2 module count
    kPark,         // HOST: a request's turn. i0 slot, i1 phase (0 parked for the slot's world, 1 answered after a park, 2 answered with the slot not ready)
    kPoseApplied,  // a pose block corrected the copy. i0 KeyTag, i1 role (0 the host, applying the syncing client's block; 1 a client, taking a placed block), i2 the actor's object slot, f0 the block's riderEid, f1 1 when it came from the ATV's syncer
    kSeatChange,   // a seat or rider change of an entry was announced. i0 occupantBefore<<8|occupantAfter, i1 riderAfter, i2 subscribers that ran, i3 KeyTag, f0 riderBefore
    kBodySeat,     // a body owner's outcome for a slot. i0 slot, i1 (0 released, 1 attached, 2 held no body, 3 resolve failed), i2 KeyTag, f0 the body's distance from the ATV root (cm, at an attach), f1 isOnAtv (0/1), f2 collision (0/1)
    kRide,         // a ride owner's outcome for an eid. i0 eid, i1 (0 off, 1 rides, 2 held unbound, 3 held hidden, 4 held no atv, 5 refused class), i2 KeyTag, f0 the kerfur's distance to sitarrow (cm, at a ride), f1 at an off: the mirror was still attached when the clear began (1/0)
    kStampBuilt,   // HOST: a block was built. i0 KeyTag, i1 riderEid, i2 1 = the re-send of a syncer's block
    kInvite,       // HOST: the kerfur steps of a client's seat edge. i0 KeyTag, i1 (1 invited, 0 released, 2 turned back), i2 kerfur count
    kMirrorCancel, // CLIENT: the game's own call on a kerfur mirror was cancelled. i0 (0 getOffCar, 1 tryToOccupyCar), i1 eid
    kRiderFact,    // HOST: the kerfur's own seat. i0 KeyTag, i1 eid (0 at the getOffCar PRE), i2 1 at the sitOnCar POST, f0 at that POST the kerfur's distance to sitarrow (cm, -1 when unread)
    kRideCleared,  // CLIENT: the rider subscriber's npc_ride::Clear for an eid returned. i0 eid, i1 KeyTag, i2 what still held after it (1 a ride row kept, 2 the pose held by the ride owner, 4 the mirror attached to an actor)
    kEventCount
};

// Whether the drill row is on, resolved once: every emit point asks, so a run without the drill costs one compare. An emit
// point whose arguments cost a read or a hash asks it first.
bool On();

// A pose block of the ATV under `key` was applied to `actor`'s copy (kPoseApplied), on the host or on a client. A no-op while
// the module is off; the key's hash and the actor's slot are read only when it is on. `riderEid` and `fromSyncer` are the
// block's own (a client's: the host's word and its syncer bit; the host's: zero and false).
void NotePoseApplied(const std::wstring& key, void* actor, bool host, uint32_t riderEid, bool fromSyncer);

// A number that names an ATV key in a record (kUnadopted, kAdopted), the same on every peer.
int32_t KeyTag(const std::wstring& key);

// Record one event (a no-op while the row is off).
void Note(Event e, int32_t i0 = 0, int32_t i1 = 0, int32_t i2 = 0, float f0 = 0.f, float f1 = 0.f, float f2 = 0.f,
          int32_t i3 = 0);

// How many of `e` were recorded; a cursor taken now reads only what follows.
uint32_t Count(Event e);

// The next record of `e` after `cursor` (advanced past it), false when there is none. A cursor older than the ring jumps to
// the oldest kept record.
bool Next(Event e, uint32_t& cursor, Rec& out);

// Whether a record of `e` after `cursor` and before order `beforeOrd` satisfies `pred`; the cursor is left where it was.
bool AnyBefore(Event e, uint32_t cursor, uint32_t beforeOrd, bool (*pred)(const Rec&, int32_t arg), int32_t arg);


}  // namespace coop::atv_drill_events
