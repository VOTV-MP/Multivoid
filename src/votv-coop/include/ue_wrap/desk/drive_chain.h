// ue_wrap/drive_chain.h -- the drive-chain engine wrappers (v119 L5):
// AdriveSlot_C (desk play/comp ChildActor slots + the eraser's slot),
// Aprop_drive_C (the Data_0 payload @ "data_0"), Aprop_driveRack_C (the
// 16-row storage arrays + gen()).
//
// Fact base: votv-drive-chain-L5-impl-DESIGN-2026-07-18.md +
// votv-signal-chain-units-RE-2026-07-16.md §5. Principle 7: reflection
// access + struct layouts + UFunction thunks ONLY -- no coop/network state
// (coop/interactables/drive_sync owns the lanes).
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
// Safe pre-resolve (false until the class loads).
bool IsDriveClass(void* cls);
bool IsRackClass(void* cls);
bool IsSlotClass(void* cls);
bool IsGamemodeClass(void* cls);

// The resolved prop_drive_C class (null pre-resolve) -- spawn/refund callers
// skip a FindClass walk.
void* DriveClass();

// The live slot ACTOR for a role (desk slots via the cached desk actor's
// obj_driveSlot_play/comp fields; eraser via a signalDriveEraser_C census ->
// its driveSLot_obj field). Cached + liveness-checked. Null while unresolved.
void* SlotActor(int role);

// Reverse: the role of a slot actor caught as a 0x45 verb ctx (-1 unknown --
// e.g. a rack-internal or not-yet-resolved slot). Pure cached-pointer compare
// (safe in the VM bracket).
int RoleOfSlotActor(void* slotActor);

// slot.drive (null = empty). Also isRecentlyDetached for diagnostics.
void* SlotDrive(void* slotActor);
bool  SlotRecentlyDetached(void* slotActor);

// Reflected FSM verbs (the proven authors). ScopedWireApply is the CALLER's
// job. putDriveIn takes the drive ACTOR.
bool CallPutDriveIn(void* slotActor, void* driveActor);
bool CallDrivePulledOut(void* slotActor);

// The deterministic eject-latch completion (design R3): if the ejected drive
// no longer overlaps the slot's drivePort (or is null/dead), complete the
// EndOverlap transition the FSM is waiting for: isRecentlyDetached = false.
// No-op when still overlapping (the organic EndOverlap will clear it).
void CompleteEjectLatch(void* slotActor, void* driveActor);

// Drive payload (prop_drive.data_0, Fstruct_signalDataDynamic 0x70).
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
