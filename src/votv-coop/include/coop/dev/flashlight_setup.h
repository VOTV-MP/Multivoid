// coop/dev/flashlight_setup.h -- Autotest helpers to set up a working flashlight.
//
// Phase 5F autotest needs to programmatically give the player a flashlight,
// install a charged battery, and (if needed) equip the item so that calls
// to AmainPlayer_C::updateFlashlight() actually toggle the world light.
// RE: research/findings/votv-inventory-equip-battery-RE-2026-05-26.md.
//
// These helpers are autotest-only. The shipping coop sync does NOT call
// them -- the user's save already has the flashlight equipped in normal
// play. They exist solely to make the autonomous LAN test deterministic.

#pragma once

namespace coop::dev::flashlight_setup {

// Spawn an Aprop_equipment_flashlight_C and add it to the local player's
// inventory via AmainPlayer_C::addPropToPlayer(FName). Whether this also
// auto-equips depends on VOTV BP behavior (F-INV-2 open flag).
// Returns true if the call dispatched. Game thread only.
bool GiveFlashlight(void* mainPlayer);

// Write a full charge into the live saveSlot:
//   saveSlot.battery @0x0100 = 1.0f
//   saveSlot.flashlightBattery @0x0890 = UClass*(prop_batts_C)
// Both fields are looked up by name via reflection (offset
// FindPropertyOffset on saveSlot_C), so a recook that shifts BP
// offsets still works. Returns true on success. Game thread only.
bool SetBatteryFull(void* mainPlayer);

// High-level: ensure flashlight is equipped + battery is full. Reads
// mainPlayer.hasFlashlight to decide whether GiveFlashlight is needed.
// Logs the verified state. Game thread only.
bool EnsureFlashlightReady(void* mainPlayer);

}  // namespace coop::dev::flashlight_setup
