// coop/interactables/atv_condition_sync.h -- the ATV CONDITION lane (#4 of the 17.14 pair):
// the author's tires/spare/dirt/fuel/health ride AtvStatePayload's v147 block; a mirror is
// OVERWRITTEN from the author and re-derives visuals through the game's own per-facet
// reducers on change edges. #5 (the impulse neuter) is what makes this safe: a mirror's own
// accrual is held at zero, so an overwrite can never race an irreversible act.
//
// AUTHORITY (qf round 2, the pass's biggest catch): ACCUMULATORS apply from any legitimate
// author; PRESENCE (tiresMask, hasSpare) is consumed ONLY from host-authored packets
// (senderSlot 0 -- driver, idle syncer, adopt seed). A client-author eject ships a mask bit
// whose paired prop_atvWheel_C birth structurally cannot travel (the express seam is
// host-only, the wheel key is a per-peer random mint), so applying it would convert a
// retained-wheel divergence into host-PERSISTED item loss. The refused client-eject
// direction is a REGISTERED crutch (docs/CRUTCHES.md, C1 family) pending the act-as-host
// intent lane (ATV.md 17.5); the (b2) acceptance arm ASSERTS the divergence so no all-green
// sheet hides it.
//
// EDGES (qf rounds 1/3/4/5): change groups are computed against a LAST-EXPRESSED baseline
// (seeded from the ACTOR at first apply; advanced only when a verb actually runs), never
// against last packet or live-local -- a per-packet baseline starves updDirt forever on slow
// drift, and a zero seed would fire updTires' measured-UNCONDITIONAL BreakConstraint x8 on a
// settled correct rig. updTires and updSpareTire both chain updDirt internally (measured), so
// the dirt group's expression advances with them.
//
// Everything here is game-thread (called from ReadPayload fills and OnReliable applies).

#pragma once

#include <cstdint>

namespace coop::net { struct AtvStatePayload; }
namespace coop::atv_sync { struct AtvEntry; }

namespace coop::atv_condition_sync {

// Fill the payload's v147 condition block from the live actor. Sets tiresValid=1 on a
// complete read; leaves the memset ZEROS + tiresValid=0 when the actor cannot be read
// (mask 0 is the LEGAL all-ejected state, so absence carries its own bit -- receivers
// touch nothing on 0). Logs the first failure once.
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

// Apply-site counters (OURS -- blind to the game's own processKeys/native reducer calls by
// construction; the acceptance arms' ==0/==1 assertions read exactly these).
struct Counters {
    unsigned long long applied = 0;            // condition blocks applied (fields written)
    unsigned long long updTiresCalled = 0;
    unsigned long long updDirtCalled = 0;
    unsigned long long updSpareCalled = 0;
    unsigned long long updHealthCalled = 0;
    unsigned long long presenceSkippedDiffering = 0;  // non-host presence that DIFFERED (b2)
    unsigned long long deferred = 0;           // applies that deferred verbs (skipTireUpdate)
    unsigned long long invalidBlocks = 0;      // tiresValid==0 received
};
Counters ReadCounters();

}  // namespace coop::atv_condition_sync
