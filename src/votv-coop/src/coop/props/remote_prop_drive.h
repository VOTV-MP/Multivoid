// coop/props/remote_prop_drive.h -- IMPLEMENTATION-PRIVATE: the per-slot drive cache, the hold
// gate and the drive start, shared by remote_prop.cpp (the pose Tick, the release, the teardown)
// and remote_prop_drive.cpp (which owns the state).
//
// NOT a public header (it lives under src/, not include/). Game-thread only, as the rest of
// remote_prop.

#pragma once

#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"
#include "coop/props/active_drive.h"

#include <array>
#include <cstdint>
#include <string>

namespace coop::remote_prop {

// The holds per slot, by the generation the holder mints at each grab (PropPoseSnapshot::holdGen):
// the newest one its release or stick closed, and the one its drive belongs to. A pose of a closed
// hold is the tail of a stream that already ended, perhaps with the prop frozen on the holder by a
// slot or a stick, and starts nothing; a pose of a newer hold is a new grab, whatever the drive
// cache still holds. A generation of 0 is unversioned and passes. The shape is MTA's sync time
// context, which settles a state change against sync still in flight (CElement.cpp), with one
// divergence: MTA's server also keeps one syncer per unoccupied vehicle and relays only that
// syncer's packets (CUnoccupiedVehicleSync.cpp), as the driven-prop channel's host claims a prop,
// while here the holder mints a generation per hold and nothing assigns a prop to one holder -- two
// peers grabbing one prop both stream it (docs/props.md, Known limits).
struct HoldGate {
    uint16_t closedGen   = 0;
    bool     haveClosed  = false;
    uint16_t driveGen    = 0;      // the hold the slot's drive belongs to, while there is one
    uint16_t refusedGen  = 0;      // the hold a refusal was said for (a static prop, a door the local player drags)
    uint16_t preludeGen  = 0;      // the hold the grab's prelude ran for
    uint16_t cueGen      = 0;      // the hold the grab's click and sound played for
    uint16_t tailSaidGen = 0;      // the closed hold a late pose was said for
    uint32_t relatches   = 0;      // re-latches during the drive, said at the first and at the loud mark
};

// One drive per peer slot: each client drives its own held prop, so two clients holding
// different props never race.
extern std::array<coop::active_drive::ActiveDrive, coop::players::kMaxPeers> g_drives;
extern std::array<HoldGate, coop::players::kMaxPeers> g_holds;

// Wrapping order: `a` was minted after `b`.
bool GenAfter(uint16_t a, uint16_t b);

bool IsHoldClosed(const HoldGate& h, uint16_t gen);

// A drive whose copy the game switches back to simulating this many times is being fought every
// tick, not nudged once (a welded body, a Blueprint that sets it each frame): said loudly, once.
constexpr uint32_t kRelatchLoud = 60;

// The WireKey against the drive's cached key; keys are ASCII, so a byte compare is exact.
bool KeyMatchesCache(int slot, const coop::net::WireKey& k);

// A trash body whose pose the HOST authors on this peer: a mirror we spawned, or this client's own
// save-loaded clump bound to the host's eid.
bool HostAuthorsTrashBody(void* actor);

// A prop that froze while held (a stick, a slot's insert, a mount) stays off physics on release.
bool StickHoldsPhysicsOff(void* actor);

// A door this peer's own player drags is moved by that drag on this copy, which wins at drive start
// and at every pose after it. Said once per hold.
bool LocalDragsDoor(void* door, int slot, uint16_t gen);

// The live actor a wire key names, or null. A portable key (`mv_`, a container-lane swinger) goes to that
// lane's index alone: the portable token is minted only by element::PortableWireKey and the other keys this mod
// mints start differently, so such a key is not one we made for a prop, and the prop key index would answer a miss
// with a catch-up drain on every call. Any other key asks the prop key index first and the container lane second.
// Game thread.
void* ResolveKeyedActor(const std::wstring& keyW);

// Resolve the pose's prop and start the slot's drive on it (the grab's prelude, park, drive cache).
void ResolveAndStartDrive(int slot, const coop::net::PropPoseSnapshot& pose);

}  // namespace coop::remote_prop
