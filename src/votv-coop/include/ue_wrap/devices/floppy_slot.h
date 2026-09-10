// ue_wrap/devices/floppy_slot.h -- the floppy slot: the fields a VOTV device holds a disc in,
// for every device class that has one.
//
// Alaptop_C and AserverBox_C carry the same four: floppyType (negative when empty, and the index
// lib_C::floppyFromType reads a class and a mesh out of), floppyReadwrites, floppyData, and
// floppyObjectData, the JSON of the disc's save struct that the insert captures and the eject
// spawns the disc back from. The laptop adds the two its second hitbox needs. AkerfurOmega_C has
// three of the four and rebuilds a disc by property name instead; it is not a row here, because
// the robot has no lane and a slot row would have no identity to travel under.
//
// Applying a slot is more than the fields: the laptop's widget refreshes through updFloppy, and
// the box, whose mesh swap is inline in two EX_LocalVirtualFunction verbs, gets its mesh set
// here. The mesh's pose needs no help -- the eject parks it at the in-slot transform first.
//
// No network logic, no coop state (principle 7). Game thread only.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::floppy_slot {

// A device class that holds a disc in the slot above. The value is on the wire, so a row is
// appended, never renumbered.
enum class DeviceKind : uint8_t {
    Laptop    = 0,
    ServerBox = 1,
};
inline constexpr uint8_t kDeviceKindCount = 2;

struct Scalars {
    int32_t floppyType = -1;   // negative = empty
    int32_t readWrites = -1;   // the inserted disc's remaining writes
    bool    zip        = false;  // laptop only; the box refuses a zip disc before it ever inserts
};

struct Content {
    std::wstring              nametype;    // laptop only (floppyNametype)
    std::wstring              objectData;  // the disc's save struct as JSON
    std::vector<std::wstring> data;        // the disc's data rows
};

// Resolve one kind's class and members. Retried on a backoff until the world is up; true once
// every member this kind declares is in hand. Resolution is per kind, so a device the world has
// not placed cannot cost the other one its slot.
bool EnsureResolved(DeviceKind kind);

bool ReadScalars(DeviceKind kind, void* device, Scalars& out);
bool ReadContent(DeviceKind kind, void* device, Content& out);

// Alloc-free digest over the slot's raw field buffers -- the scalars, and the FString and
// TArray<FString> bytes read through their headers, with no string minted. It is the poll's
// pre-filter, so a full read runs only where the digest moved.
bool ReadDigest(DeviceKind kind, void* device, uint64_t& out);

// Receiver-side apply: the scalars raw (the game writes them raw too), the strings through an
// engine-side mint, then the kind's own refresh.
bool WriteSlot(DeviceKind kind, void* device, const Scalars& st, const Content& content);

// The empty slot: floppyType and readWrites back to -1, strings and rows emptied, refresh run.
bool ClearSlot(DeviceKind kind, void* device);

// Level change: the classes, offsets and functions are world-scoped.
void ResetCache();

}  // namespace ue_wrap::floppy_slot
