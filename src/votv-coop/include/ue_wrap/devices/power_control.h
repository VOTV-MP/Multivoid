// ue_wrap/devices/power_control.h -- standalone engine access for the VOTV base POWER PANEL
// (ApowerControl_C). Principle-7 engine-wrapper layer (no network/coop state).
// coop::power_sync drives the cross-peer sync through here.
//
// The panel carries 5 LATCHED breaker bools (one per base subsystem) -- it is NOT a 2-state
// toggle, so it does not fit the generic interactable_sync Channel; coop::power_sync is its
// own module (the keypad_sync precedent). This wrapper exposes the breaker state as a single
// 5-bit MASK so the module stays state-shape-agnostic: bit0 press_coord (coordinates), bit1
// press_downl (downloading), bit2 press_play (playing), bit3 press_calc (calculating), bit4
// press_light (lights) -- the press_ FIELD order, which power_control.cpp resolves by name.
//
// Identity = the inherited AtriggerBase_C::Key (save-persistent, cross-peer stable;
// FindPropertyOffset does NOT climb to a super, so Key resolves against triggerBase_C directly,
// the same gotcha garage/appliance handle). The base power EFFECTS (servers/doors/lightRoots)
// are synced by their OWN channels -- this wrapper mirrors the PANEL's own breaker/LED visual.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::power_control {

// Resolve ApowerControl_C + the Key offset + the 5 press-bool offsets + the apply/refresh
// UFunction(s). Idempotent; true once resolved. Game thread.
bool EnsureResolved();

// True iff `obj`'s class is powerControl_C or a subclass. False if not yet resolved.
bool IsPowerControl(void* obj);

// The panel's AtriggerBase_C::Key as a wide string ("" on failure, L"None" if unkeyed).
std::wstring GetKeyString(void* p);

// Read the 5 press bools into `mask` (bit0=coord .. bit4=light, field order). False if the
// read could not be made (null / not resolved); leaves `mask` untouched on failure.
bool ReadPress(void* p, uint8_t& mask);

// Drive the panel to `mask` (bit0=coord .. bit4=light): write the 5 press bools, call
// moveLevers() to animate the levers, and set the LED particle visibility directly. Mirrors
// the PANEL only -- it does NOT re-drive the base subsystems (those are synced by their own
// channels). MUST run on the game thread. False on null / unresolved.
bool ApplyPress(void* p, uint8_t mask);

}  // namespace ue_wrap::power_control
