// ue_wrap/desk/phys_mods.h -- engine access for the desk's PHYSICAL MODULES array.
// Principle-7 engine-wrapper layer: no network logic; coop::physmods_sync drives
// the mirror through here.
//
// physMods is a TArray<TEnumAsByte<enum_physicalModules>> on
// AanalogDScreenTest_C, fixed at 12 slots with 0 meaning empty. It is not a set:
// plugInModule refuses a type off its isModuleAllowed list and an occupied slot,
// never a type the desk already holds, so two modules of one type can sit on it.
// Three writers -- plugInModule (write into an empty slot, then K2_DestroyActor on
// the module prop), the unplug in actionOptionIndex, the E press (the module is
// reborn INTO THE HAND through lib.physModToActor and the slot goes to 0), and
// setData on save load, which calls updPhysMods. gatherData only READS it,
// marshalling the array into the save. updPhysMods() is the
// parameterless consumer re-runner and measures as a PURE function of the array
// -- per-slot socket visuals plus Contains-gated speed, lamp and shield effects,
// with no player references, spawns or audio -- so a mirror may call it.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::phys_mods {

inline constexpr int kSlots = 12;

// Resolve the physMods offset + updPhysMods on the desk class, the module
// base class (Aprop_physModule_C) and lib.physModToActor. Piggybacks
// console_desk::EnsureResolved for the desk class/instance. Throttled lazy
// retry; idempotent; game thread.
bool EnsureResolved();

// Read the live array into out[kSlots] (missing tail zero-filled; n = the
// engine array's Num, clamped). False when unresolved / no desk.
bool ReadArray(uint8_t out[kSlots]);

// Wholesale write of all kSlots elements into the EXISTING engine allocation
// (fixed-size CDO array; never realloc). False if the engine Num < kSlots.
bool WriteArray(const uint8_t in[kSlots]);

// Reflected desk updPhysMods() -- re-runs every consumer from the array.
// The caller holds the coop-side wire-apply guard.
bool CallUpdPhysMods();

// Is `cls` Aprop_physModule_C or a descendant (the per-byte module classes)?
bool IsModuleClass(void* cls);

// The desk's slot component for array index `slot` (physModSlots holds the slots' primitive
// components in array order), or null. Game thread.
void* SlotComponent(void* desk, int slot);

// The desk's plugInModule(holdActor = module, slot = SlotComponent(slot), player): what a module
// carried onto the slot runs. True when the call dispatched; the array says whether the desk took
// it. Game thread.
bool CallPlugInModule(void* desk, void* module, int slot, void* player);

// A player's E on slot `slot`, as the game runs it: the desk's lookAt with a hit on the slot, which
// leaves no panel for the press to go to, then actionOptionIndex with the same hit. On a filled slot
// that is the unplug: the module goes to `player`'s hand and the slot to 0. True when both
// dispatched. Game thread.
bool CallPressSlot(void* desk, void* player, int slot);

// lib.physModToActor(byte) -> the module class for `byte` (null on fail /
// unresolved / byte unmapped). Reflected static-library call on the lib CDO.
// It maps more than the desk takes (ATV upgrades, radar modules): IsModuleAllowed
// is the desk's own answer.
void* ClassForByte(uint8_t byte);

// Does the desk take a module of `byte` into a slot? The desk's own isModuleAllowed
// (a fixed list of types), asked once per byte and kept; a call that fails is asked
// again next time. False while unresolved. Game thread.
bool IsModuleAllowed(uint8_t byte);

// The byte a module CLASS encodes: reads the class's default object's module
// byte... NOT AVAILABLE statically-cheap; instead the reverse map is built by
// probing ClassForByte over the enum range once (cached). 0 = unknown.
uint8_t ByteForClass(void* cls);

// Session-end cache reset (instance pointers only; class-level persists).
void ResetCache();

}  // namespace ue_wrap::phys_mods
