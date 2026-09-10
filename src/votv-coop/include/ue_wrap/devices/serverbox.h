// ue_wrap/devices/serverbox.h -- the signal server (AserverBox_C) engine wrapper: the box list the
// gamemode owns, and the floppy slot that swallows a disc.
//
// What the wrapper reaches:
//   - mainGamemode_C.servers, whose INDEX is the identity every server lane addresses a box by:
//     the level places the boxes, so both peers build the same ordering
//   - the box's own label, the FString the sign above it renders
//   - the floppy slot: the type (negative when empty), the inserted disc's remaining writes, the
//     data rows, and the JSON of the disc's whole save struct that the insert captures
//   - the two verbs a player's interaction dispatches: pocessFloppy, which casts the held object
//     and refuses a zip disc before inserting it, and ejectFloppy, which empties the slot and
//     respawns the disc when the out-timeline finishes
//
// No network logic, no coop state (principle 7). Game thread only.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::serverbox {

// Resolve the box class, the slot fields and the two verbs. Retried on a backoff until the world
// is up; true once every member is in hand. The box LIST resolves separately, so a renamed slot
// field cannot take the list away from the lane that only wants the boxes.
bool EnsureResolved();

// The gamemode's server list, in its own order. Returns the number appended, 0 before the world
// has a gamemode. The index into `out` is the cross-peer box identity. Resolves what it needs
// itself; EnsureResolved is not a precondition.
size_t ReadServers(std::vector<void*>& out);

// The box's rendered label.
std::wstring ReadName(void* box);

struct SlotState {
    int32_t floppyType    = -1;  // negative = empty
    int32_t readWrites    = -1;  // the inserted disc's remaining writes
    int32_t dataNum       = 0;   // rows of the disc's data array the insert took
    int32_t objectDataLen = 0;   // characters of the captured save JSON
};
bool ReadSlot(void* box, SlotState& out);

// The insert verb the interaction dispatches with whatever the player holds: it casts to a disc,
// refuses a zip one, and only then reaches the insert. Calling the inner insert instead would skip
// both of those and enter through a door no player has.
bool CallProcessFloppy(void* box, void* discActor);

// The eject verb. It returns before the disc exists: the slot is cleared here and the actor is
// spawned when the box's out-timeline finishes.
bool CallEjectFloppy(void* box);

}  // namespace ue_wrap::serverbox
