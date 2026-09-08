// coop/interactables/atv_condition_sync.h -- the ATV CONDITION lane: the author's
// tires/spare/dirt/fuel/health ride the AtvState payload's condition block, and a mirror is
// OVERWRITTEN from the author, re-deriving visuals through the game's own per-facet reducers
// on change edges. The impulse neuter makes that safe -- a mirror's own accrual is held at
// zero, so an overwrite cannot race an irreversible act.
//
// AUTHORITY: accumulators apply from any legitimate author, PRESENCE (tiresMask, hasSpare)
// only from host-authored packets, because a client-authored eject ships a mask bit whose
// paired prop_atvWheel_C birth cannot travel, so applying it would turn a retained-wheel
// divergence into host-PERSISTED item loss. docs/vehicles.md carries it as a known limit.
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

// Apply a received condition block to this entry's actor. Honors tiresValid, the
// presence-authority rule (senderSlot==0), the skipTireUpdate defer guard, and the
// last-expressed change edges. Game thread.
void ApplyPayload(coop::atv_sync::AtvEntry& e, const coop::net::AtvStatePayload& p,
                  uint8_t senderSlot);

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
