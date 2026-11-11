// ue_wrap/desk/drive_chain.h -- the drive-chain engine wrappers: driveSlot_C (the desk play/comp
// ChildActor slots and the eraser's slot), prop_drive_C (the data_0 payload, an
// Fstruct_signalDataDynamic) and prop_driveRack_C (the 16-row storage arrays + gen()).
//
// Reflection access, struct layouts and UFunction thunks ONLY -- no coop or network state;
// coop/interactables/drive_sync, drive_payload_sync and drive_rack_sync own the lanes; the eraser's own
// press and show are ue_wrap/desk/drive_eraser.
//
// Game thread throughout (UObject access + reflected dispatch).

#pragma once

#include "ue_wrap/desk/signal_dynamic.h"

#include <cstdint>

namespace ue_wrap::drive_chain {

// Slot roles on the wire (DriveSlotStatePayload.role).
inline constexpr int kRoleDeskPlay = 0;
inline constexpr int kRoleDeskComp = 1;
inline constexpr int kRoleEraser   = 2;
inline constexpr int kRoleCount    = 3;

// Resolve classes + offsets + UFunctions (throttled lazy retry; idempotent).
// True once the core set (slot class, drive class, data_0 offset, slot fns)
// resolved. Game thread.
bool EnsureResolved();

// Class predicates (for the freshBirth whitelist + verb-ctx discrimination).
// Safe pre-resolve (false until the class loads). IsRackClass looks the rack class up per call
// (one index lookup, game thread).
bool IsDriveClass(void* cls);
bool IsRackClass(void* cls);

// The resolved prop_drive_C class (null pre-resolve) -- spawn/refund callers
// skip a FindClass walk.
void* DriveClass();

// The live slot ACTOR for a role (desk slots via the cached desk actor's
// obj_driveSlot_play/comp fields; the eraser's via the world's signalDriveEraser_C
// (world_singleton) -> its driveSLot_obj field). Cached by slot, serial and world.
// Null while unresolved.
void* SlotActor(int role);

// Reverse: the role of a slot actor caught as a watched verb's instance (-1 unknown --
// e.g. a rack-internal or not-yet-resolved slot). Pure cached-pointer compare
// (safe in the VM bracket).
int RoleOfSlotActor(void* slotActor);

// slot.drive (null = empty). Also isRecentlyDetached for diagnostics.
void* SlotDrive(void* slotActor);

// Reflected FSM verbs (the proven authors). ScopedWireApply is the CALLER's
// job. putDriveIn takes the drive ACTOR.
bool CallPutDriveIn(void* slotActor, void* driveActor);
bool CallDrivePulledOut(void* slotActor);

// The slot's drivePort, the box its overlap takes a drive by, and whether `actor` overlaps it. Null / false when the
// slot or its port is gone. Game thread (IsOverlappingActor is a reflected call).
void* Port(void* slotActor);
bool PortOverlaps(void* slotActor, void* actor);

// [dev] the slot's eject latch (isRecentlyDetached, which only the ejected drive's EndOverlap clears) and its port's
// collision (GetCollisionEnabled: 0 none, 1 query only, 2 physics only, 3 both; -1 unread). Game thread.
bool ReadSlotLatch(void* slotActor, bool& recentlyDetached, int& portCollision);

// [dev] a drill's fixture: set or clear the slot's eject latch, which the port's entry reads before it takes a drive.
// False when the slot or the field is not resolved. Game thread.
bool WriteSlotLatch(void* slotActor, bool recentlyDetached);

// The deterministic eject-latch completion: if the ejected drive no longer overlaps the slot's
// drivePort (or is null/dead), complete the EndOverlap transition the FSM is waiting for --
// isRecentlyDetached = false. No-op when still overlapping (the organic EndOverlap will clear it).
void CompleteEjectLatch(void* slotActor, void* driveActor);

// Drive payload (prop_drive.data_0, Fstruct_signalDataDynamic).
bool ReadDriveRow(void* driveActor, ue_wrap::signal_dynamic::Row& out);
bool WriteDriveRow(void* driveActor, const ue_wrap::signal_dynamic::Row& in);
bool CallDriveUpd(void* driveActor);  // LED/hover refresh (plain, reflected-safe)

// Rack storage (prop_driveRack: data[16] rows + has[16] bools + gen()).
inline constexpr int kRackSlots = 16;
struct RackRow {
    bool has = false;
    ue_wrap::signal_dynamic::Row row;
};
// Read all 16 (false if arrays unresolved/malformed). Num!=16 tolerated
// (reads min(Num,16); reports actual in outNum).
bool ReadRack(void* rackActor, RackRow out[kRackSlots], int& outNum);
// Write ONE row (has=false writes the empty row). Same pin doctrine as
// signal_dynamic::WriteStructLive.
bool WriteRackRow(void* rackActor, int idx, const RackRow& in);
bool CallRackGen(void* rackActor);  // the pure visual re-derive from `data`

}  // namespace ue_wrap::drive_chain
