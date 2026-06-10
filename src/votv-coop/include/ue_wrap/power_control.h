// ue_wrap/power_control.h -- standalone engine access for the VOTV base POWER PANEL
// (ApowerControl_C). Principle-7 engine-wrapper layer (no network/coop state).
// coop::power_sync drives the cross-peer sync through here.
//
// The panel carries 5 LATCHED breaker bools (one per base subsystem) -- it is NOT a 2-state
// toggle, so it does not fit the generic interactable_sync Channel; coop::power_sync is its
// own module (the keypad_sync precedent). This wrapper exposes the breaker state as a single
// 5-bit MASK so the module stays state-shape-agnostic:
//   bit0 = press_coord  @0x0380   (coordinates)
//   bit1 = press_downl  @0x0381   (downloading)
//   bit2 = press_play   @0x0382   (playing)
//   bit3 = press_calc   @0x0383   (calculating)
//   bit4 = press_light  @0x0384   (lights)
// (FIELD/offset order. The native powerChanged() setter takes its 5 args in a DIFFERENT order
// -- calc,downl,coords,play,light -- so ApplyPress maps bit<->arg by NAME, not position.)
//
// Identity = the inherited AtriggerBase_C::Key @0x0260 (save-persistent, cross-peer stable;
// FindPropertyOffset does NOT climb to a super, so Key resolves against triggerBase_C directly,
// the same gotcha garage/appliance handle). The base power EFFECTS (servers/doors/lightRoots)
// are synced by their OWN channels -- this wrapper mirrors the PANEL's own breaker/LED visual.
//
// RE: research/findings/votv-powerControl-panel-sync-RE-2026-06-08.md.

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

// Drive the panel to `mask` (bit0=coord .. bit4=light): write the 5 press bools then refresh
// the panel's own visual (lever positions + LED particles). Mirrors the PANEL only -- it does
// NOT re-drive the base subsystems (those are synced by their own channels). MUST run on the
// game thread. False on null / unresolved. (Exact refresh verb(s) per the RE.)
bool ApplyPress(void* p, uint8_t mask);

}  // namespace ue_wrap::power_control
