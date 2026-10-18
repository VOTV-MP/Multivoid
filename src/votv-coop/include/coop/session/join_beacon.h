// coop/session/join_beacon.h -- the host's half of the join's progress: once a second, per joining
// slot, what this host is doing for that joiner and how far in. The client's half is
// coop/session/join_progress.h, which waits on this token instead of on a wall clock.
//
// The contract is silence once the bracket is delivered. An owner calls NotePhase every pass it
// works on a slot; Tick sends at most one note per slot per second and NOTHING for a slot nobody
// noted, so a host that stopped working on a joiner stops beaconing with no "I gave up" message,
// and a joiner whose token expires names the phase and the side that stopped. The owners:
// save_transfer (capture, stream), prop_snapshot::DrainChunk (deferred, streaming the bracket)
// and this module (Draining, from the close until a fresh link sample finds the queue drained).
//
// Host only (Tick no-ops on a client), game thread only: all owners run inside the net pump's
// tick. No engine calls.

#pragma once

#include "coop/net/protocol.h"  // HostJoinPhase

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::join_beacon {

// The session the notes are sent on. Once at harness boot, beside the other subsystem installs.
void SetSession(coop::net::Session* session);

// An owner did a pass of work for `slot` in `phase`. `done`/`total` are that phase's numerator and
// denominator, 0/0 for a phase that has none. Cheap by design -- one struct store, no send, no
// clock read -- because the owners call it from their per-tick loops.
void NotePhase(int slot, coop::net::HostJoinPhase phase, uint32_t done, uint32_t total);

// The slot's bracket closed (its SnapshotComplete went into the transport or the backlog) at
// `closedAtMs` on the steady clock; or it was cleared, abandoned or never opened. Pushed by
// prop_snapshot, which owns the bracket: from the close until the link's queue has drained, Tick
// beacons the Draining phase for the slot itself, so the joiner's wait reads the delivery of the
// bracket after the host has finished enqueueing it.
void NoteBracketClosed(int slot, int64_t closedAtMs);
void NoteBracketReset(int slot);

// The Draining decision's table and the joiner's renewal rule across it (join_progress::
// BeaconRenews), run once per process at the first session start. One PASS line each; false when
// any case failed.
bool RunDrainingSelftest();

// Send what is due: one note per slot noted since the last Tick, at most one ATTEMPT a second, and
// one at once when the phase changed (a joiner should see a transition without a second of lag).
// The cadence bounds attempts rather than arrivals, so a link that refuses a note does not turn
// this into a per-tick retry; the second after carries a fresher note anyway. From the net pump's
// tick, after the owners have run.
void Tick();

// The slot left mid-join, or the session ended: drop its note state so a recycled slot starts
// silent rather than inheriting a phase.
void CancelForSlot(int slot);
void OnDisconnect();

}  // namespace coop::join_beacon
