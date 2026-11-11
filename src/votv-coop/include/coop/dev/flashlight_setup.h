// coop/dev/flashlight_setup.h -- Autotest helpers to set up a working flashlight.
//
// The autotest needs to programmatically give the player a flashlight, install a
// charged battery, and if needed equip the item, so that calls to
// AmainPlayer_C::updateFlashlight() actually toggle the world light.
//
// These helpers are autotest-only. The shipping coop sync does NOT call them --
// a save from normal play already has the flashlight equipped. They exist solely
// to make the autonomous LAN test deterministic.

#pragma once

namespace coop::dev::flashlight_setup {

// Ask the BP to add an Aprop_equipment_flashlight_C to the local player's
// inventory, through AmainPlayer_C::addPropToPlayer(FName), whose FName is the
// UClass's own name. Whether that also auto-equips is BP behaviour we have not
// pinned down. Returns true if the call dispatched. Game thread only.
bool GiveFlashlight(void* mainPlayer);

// Write a full charge (100) into the worn flashlight's own `battery`, inside its
// inventory entry (ue_wrap/actors/inventory.h WriteWornItemProperty). Returns true
// on success; false when no flashlight is worn. Game thread only.
bool SetBatteryFull(void* mainPlayer);

// High-level: ensure flashlight is equipped + battery is full. Reads
// mainPlayer.equipped_flashlight to decide whether GiveFlashlight is needed.
// Logs the verified state. Game thread only.
bool EnsureFlashlightReady(void* mainPlayer);

}  // namespace coop::dev::flashlight_setup
