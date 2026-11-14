// coop/interactables/atv_seat_table.h -- the one writer of an ATV entry's seat, syncer and rider, and the list of
// subscribers told when one of them changes. Every write of AtvEntry::seatOccupant, seatSyncer, seatRider and
// seatRiderActor goes through SetSeat or SetRider (CI refuses a write anywhere else), so a change is announced
// whichever route made it: a judgement, a stamp, an answer, a departure, or the entry's own end (Exit).
// Game thread only. The state is the entries it is given and the subscriber array.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace coop::atv_sync { struct AtvEntry; }

namespace coop::atv_seat_table {

// What one change was. `key` points into the entry and is valid for the call only. `actorIdx` is the object index of
// `actor` as the entry holds it, for a liveness test that never reads the actor's own memory. `riderActorBefore` is the
// riding kerfur's actor the entry held before the call (compared, never dereferenced); null on a client.
struct Change {
    const std::wstring* key;
    void* actor;
    int32_t actorIdx;
    uint8_t occupantBefore, occupantAfter, syncerBefore, syncerAfter;
    uint32_t riderBefore, riderAfter;
    void* riderActorBefore;
    const char* why;
};

using Subscriber = void (*)(const Change&);

// Registers a subscriber, called in registration order on every change. Capacity 4; the same pointer twice is ignored
// (true); a full list returns false and says so once.
bool OnChange(Subscriber fn);

// Writes the seat and the syncer. `forgetHorn` true drops the memory of the horn's level unconditionally; false only
// when the syncer changed. On a change the host owes a stamp, then every subscriber is called. A subscriber may cause a
// nested change, delivered in full inside the outer one; after its own call a subscriber reads the entry, not the outer
// record.
void SetSeat(coop::atv_sync::AtvEntry& e, uint8_t occupant, uint8_t syncer, const char* why, bool forgetHorn);

// Writes the rider (the riding kerfur's element id, 0 = none; the host also its actor). On a change of the id the host
// owes a stamp, then every subscriber is called.
void SetRider(coop::atv_sync::AtvEntry& e, uint32_t eid, void* actor, const char* why);

// How many entries the host holds with a rider named. Any thread (a relaxed load): a worker that learns an NPC was retired
// asks it before posting the game-thread work that would end that NPC's ride, and posts only when it is not zero.
int HostRiderCount();

// The entry's end: the seat free, the syncer the host, no rider. Run before an entry is erased or replaced.
void Exit(coop::atv_sync::AtvEntry& e, const char* why);

// The live actor filed under `key` (null when there is none) and its index; looked up at each call.
void* ResolveAtv(const std::wstring& key, int32_t& atvIdx);

// The seat transform of seat `seatIndex` of the ATV under `key`: index 0 is the driver's, any other index is false.
bool ResolveSeat(const std::wstring& key, uint8_t seatIndex, void*& atv, int32_t& atvIdx, ue_wrap::FVector& loc,
                 ue_wrap::FRotator& rot);

}  // namespace coop::atv_seat_table
