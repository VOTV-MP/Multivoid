// coop/interactables/atv_condition_sync.h -- the ATV CONDITION lane: the syncer's
// tires/spare/dirt/fuel/health ride the AtvState payload's condition block, and a mirror is
// OVERWRITTEN from the syncer, re-deriving visuals through the game's own per-facet reducers
// on change edges. The impulse neuter makes that safe -- a mirror's own accrual is held at
// zero, so an overwrite cannot race an irreversible act.
//
// AUTHORITY: accumulators apply from the syncer the host assigned, PRESENCE (tiresMask, hasSpare)
// only from the host's own copy, never a client syncer's re-sent block, because a client-authored
// eject ships a mask bit whose paired prop_atvWheel_C birth cannot travel, so applying it would turn
// a retained-wheel divergence into host-PERSISTED item loss. docs/vehicles.md carries it as a limit.
//
// EDGES: change groups run against a LAST-EXPRESSED baseline, seeded from the ACTOR at first
// apply and advanced only when a verb runs. Against the last packet, slow drift starves
// updDirt forever; from a zero seed, updTires fires its unconditional BreakConstraint x8 on a
// settled rig. updTires and updSpareTire both chain updDirt, so the dirt group rides along.

#pragma once

#include <cstdint>

namespace coop::net { struct AtvStatePayload; }
namespace coop::atv_sync { struct AtvEntry; }

namespace coop::atv_condition_sync {

// Fill the payload's condition block from the live actor. Sets tiresValid=1 on a complete
// read; leaves the memset ZEROS and tiresValid=0 when the actor cannot be read, since mask 0
// is the LEGAL all-ejected state and absence needs its own bit. Logs the first failure once.
// Game thread, like everything here.
void FillPayload(void* actor, coop::net::AtvStatePayload& p);

// Apply a received condition block to this entry's actor. `fromHost` is whether the datagram's sender is the
// host: a block that is not, or that the host re-sent for a client syncer (kAtvFromSyncer), is clamped to the
// game's domains; presence is consumed only from the host's own copy, a connect snapshot's block included.
// Honors tiresValid, the skipTireUpdate defer guard, and the last-expressed change edges. Fuel and health follow the
// host's copy (atv_host_sync.h): on the host a client syncer's block lowers them, to the lower of the host's and its own,
// only when its ackSeq is the ATV's current state sequence, and `checkIfRunout` then runs on the host's copy; on a client
// the host's block sets them unless the ATV's open-request gate is shut, and its ackSeq is recorded as applied. Game thread.
void ApplyPayload(coop::atv_sync::AtvEntry& e, const coop::net::AtvStatePayload& p, bool fromHost);

// Idle-syncer change-gate term: true when the payload's condition block differs from the
// last block this entry actually SENT (or nothing was ever sent). NoteSent records a send.
bool CondChangedSinceLastSend(const coop::atv_sync::AtvEntry& e,
                              const coop::net::AtvStatePayload& p);
void NoteSent(coop::atv_sync::AtvEntry& e, const coop::net::AtvStatePayload& p);

// Apply-site counters: OURS, and blind by construction to the game's own processKeys and
// native reducer calls.
struct Counters {
    unsigned long long applied = 0;            // condition blocks applied (fields written)
    unsigned long long updTiresCalled = 0;
    unsigned long long updDirtCalled = 0;
    unsigned long long updSpareCalled = 0;
    unsigned long long updHealthCalled = 0;
    unsigned long long presenceSkippedDiffering = 0;  // non-host presence that DIFFERED
    unsigned long long deferred = 0;           // applies that deferred verbs (skipTireUpdate)
    unsigned long long invalidBlocks = 0;      // tiresValid==0 received
};
Counters ReadCounters();

}  // namespace coop::atv_condition_sync
