// coop/dev/container_intent_drill_detail.h -- what the container intent drill's files share. A private header of
// coop/dev/container_intent_drill*.cpp (src tree, not include/).

#pragma once

#include "ue_wrap/core/types.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::net { class Session; }

namespace coop::dev::container_intent_drill::detail {

inline constexpr uint64_t kPickEveryMs = 1000;
inline constexpr uint64_t kPickFailMs  = 60000;
inline constexpr int      kMinRecords  = 2;
inline constexpr float    kPickNearCm  = 5000.f;   // the pick takes a container within this of the local player
inline constexpr float    kStandCm     = 150.f;    // this near the container, a player stands where it can open it
inline constexpr int      kWalkMaxS    = 900;      // the walk's longest budget, in seconds

// The walker's budget, in seconds, for a route of this length; and whether the route fits the longest budget.
inline int WalkSeconds(float routeCm) { return std::clamp(static_cast<int>(routeCm / 100.f) + 60, 90, kWalkMaxS); }
inline bool WalkFits(float routeCm) { return static_cast<int>(routeCm / 100.f) + 60 <= kWalkMaxS; }

// A record's identity in the inventories: addObject re-files a record with the owner's flag in bools[0], so its hash
// differs between a container and an inventory, and its class and first name do not.
struct ItemKey {
    std::wstring cls;
    std::wstring name;
    bool operator==(const ItemKey& o) const { return cls == o.cls && name == o.name; }
};

enum class Arm : uint8_t { Off, Take, Settle, Abort, Deadline, Double, Ghost, Held, Race, Two, Sale, Depart, Room,
                           PutRecord, PutHand, PutFood, PutHidden, PutNested, PutEmpty, PutHover, PutReserved, PutFull,
                           PutFullHand, PutDepart, PutDeadline, PutOwed, PutJoin, ThrowHand, ThrowGrab, ThrowHostGrab, ThrowClamp };
Arm ArmOf();
const char* ArmName();

// The put arms are their own drill (container_intent_drill_put*.cpp): neither half picks a world container.
inline bool IsPutArm(Arm a) { return a >= Arm::PutRecord && a <= Arm::PutJoin; }

// The throw arms are their own drill too (container_intent_drill_throw*.cpp): a client throws a drive at a shared backpack.
inline bool IsThrowArm(Arm a) { return a == Arm::ThrowHand || a == Arm::ThrowGrab || a == Arm::ThrowHostGrab || a == Arm::ThrowClamp; }

// The arms that throw from the hotbar hand: throwclamp is throwhand with a forged velocity.
inline bool IsHandThrow(Arm a) { return a == Arm::ThrowHand || a == Arm::ThrowClamp; }

// The arms that name the record the press takes (R) at the pick, preferring a container where it is unique.
bool NamesR();

enum class Phase { Picking, Walking, Opening, SettleWait, Pressed, Parked, Holding, Settling,
                   GhostFind, GhostWait, GhostBack, Watching, Done };

extern Phase    g_phase;
extern uint64_t g_pickStartMs;
extern uint64_t g_nextPickMs;

// The target, picked alike on both peers.
extern uint32_t        g_eid;
extern void*           g_actor;
extern int32_t         g_actorIdx;
extern ue_wrap::FVector g_pos;
extern int             g_records0;
extern uint64_t        g_r0;         // race and two: the hash of the target's record 0 at the pick
extern bool            g_r0Unique;   // and whether no other record of the target has its class and name
extern ItemKey         g_r0Key;      // the class and name of that record

const char* Role();
void Finish();
void Fail(const char* fmt, ...);
int CountOf(uint32_t eid);
bool TryPick(int& seen, int& qualified);

// The RecordHash of each record of a container actor's inventory, in order, in the form a take request carries it.
bool ContainerHashes(void* container, std::vector<uint64_t>& out);

// The ItemKey of each record of a container actor's inventory, in order.
bool ContainerKeys(void* container, std::vector<ItemKey>& out);

// The open inventory screen's container slot that names record `id`, or its first slot when `id` is negative.
void* SlotById(int32_t id);

// How many records of the local player's own inventory have this class and name; -1 when the inventory does not read.
int OwnHolds(const ItemKey& key);

// The client's depart and room arms (container_intent_drill_depart_room.cpp).
// Depart: the parked press leaves a mark (the wall clock) in the mod's data folder; a relaunched client finds it,
// removes it, and is idle when the mark is under 300 s old (the arm runs once per host session). An older mark is
// removed and the arm runs.
void DepartMarkParked();
bool DepartRejoined();
// Room: the own inventory's capacity is written to 0 until RoomRestore writes the old value back (idempotent).
bool RoomFill();
void RoomRestore();

// The host's half (container_intent_drill_host.cpp).
void HostTick(const coop::net::Session& session);
void HostReset();

// The put arms' two halves, and their state back to the start (container_intent_drill_put.cpp, _put_host.cpp).
void PutHostTick(const coop::net::Session& session);
void PutClientTick();
void PutReset();

// The throw arms' two halves, and their state back to the start (container_intent_drill_throw.cpp, _throw_host.cpp).
void ThrowHostTick(const coop::net::Session& session);
void ThrowClientTick(const coop::net::Session& session);
void ThrowReset();

}  // namespace coop::dev::container_intent_drill::detail
