// ue_wrap/lightswitch.h -- standalone engine access for VOTV light groups
// (Atrigger_lightRoot_C). Principle-7 engine-wrapper layer (no network/coop
// state). coop::interactable_sync drives the sync through here.
//
// A light SWITCH (Alightswitch_C) is the user-facing toggle, but the
// authoritative + save-persistent on/off state lives on the GROUP controller
// Atrigger_lightRoot_C (its SetActive fans out to every AceilingLamp_C /
// AambientLight_C in the group). Syncing at the lightRoot level captures EVERY
// source (wall switch, power board, script) with one hook -- exactly as the RE
// doc prescribes. Identity = the inherited AtriggerBase_C::Key.
//
// RE: research/findings/computers-devices/votv-doors-and-lightswitches-RE-2026-05-25.md.

#pragma once

#include <string>

namespace ue_wrap::lightswitch {

// Resolve Atrigger_lightRoot_C + the Key / IsActive offsets + the SetActive
// UFunction. Idempotent; true once resolved. Game thread.
bool EnsureResolved();

// True iff `obj`'s class is trigger_lightRoot_C or a subclass. False if not yet
// resolved.
bool IsLightRoot(void* obj);

// The light-group's AtriggerBase_C::Key as a wide string ("" on failure, L"None"
// if unkeyed).
std::wstring GetKeyString(void* root);

// THE GROUP CARRIES THREE SEPARATE BOOLS -- bytecode-measured 2026-09-01, and this
// CLOSES RE flag E-L2 (open since 2026-05-25, which could not tell them apart):
//   isActive     -- the group's LIVE on/off. `updLig()` pushes it to every lamp in
//                   `lights[]` / `ambs[]`. This is the bit a player sees.
//   active       -- an ENABLE GATE, not a state. `runTrigger(owner,0)` opens with
//                   `IFNOT(active) POP`, so with the gate shut a switch press flips the
//                   switch and moves NO lights. Save-persistent; the base power panel's
//                   lights breaker drives it (powerControl.buttonsVisibility).
//   buffIsActive -- the SAVED copy of the live state; `loadAft` runs
//                   `isActive := buffIsActive; updLig()`.
// FName lookup is case-insensitive, so L"IsActive" resolves the live state and L"Active"
// resolves the GATE. They are different properties; do not let the near-collision fuse them.

// Read the group's `isActive` bool into `on` -- the LIVE state. False if the read could
// not be made (null / not resolved); leaves `on` untouched on failure.
bool TryReadActive(void* root, bool& on);

// setActive(on) -- writes the group's ENABLE GATE (`active`) and NOTHING ELSE. Measured:
// the whole BP body is one `EX_LetBool`, member `active` := param. It does NOT touch
// `isActive` and does NOT call `updLig()`, so it moves no lamps.
//
// (This header used to claim it "turns the whole group on/off (fans out to all lamps)".
// That was false and it had no callers, so nothing was broken by it -- but it is exactly
// the kind of comment that gets trusted instead of the code. The verb that actually drives
// the lamps is `runTrigger(owner, 1)` = absolute ON and `runTrigger(owner, 2)` = absolute
// OFF, both UNGATED and both calling `updLig()`; index 0 is the gated toggle a switch uses.)
//
// MUST run on the game thread. False on null / unresolved UFunction.
bool CallSetActive(void* root, bool on);

// The SetActive UFunction pointer (for POST-observer registration). nullptr until
// EnsureResolved.
void* SetActiveFn();

// --- The light SWITCH (Alightswitch_C) -- the user-facing flip toggle ---------
// IDA-PROVEN 2026-06-04: lightRoot.SetActive (and the switch's player_use/use) are all
// BP-INTERNAL -> a POST observer never fires on a real flip. The only observable edge is
// the player's InpActEvt_use input action (coop::interactable_sync hooks it + reads
// lookAtActor). Syncing the SWITCH (not just the lightRoot) lets the RECEIVER replay the
// switch's use() so the switch FLIPS VISUALLY on the peer too -- use() flips the switch's
// `bool A` (its mesh state) AND fans out to the lights in one BP call. Identity = the
// switch's own AtriggerBase_C::Key (deterministic placed-trigger key, cross-peer stable).
bool EnsureSwitchResolved();
bool IsLightSwitch(void* obj);              // class == lightswitch_C or subclass
std::wstring GetSwitchKeyString(void* sw);  // the switch's AtriggerBase_C::Key
bool TryReadSwitchA(void* sw, bool& on);    // the switch's flip state (bool A)
bool CallUse(void* sw);                     // use() -- flips the switch visual + the lights

// --- The GROUP as a synced entity (2026-09-01) --------------------------------------
// The switch lane above syncs `A`, which use() sets from its OWN toggle -- presentation.
// The GROUP's live state is trigger_lightRoot_C::isActive, and until now nothing on either
// peer owned or reconciled it. These are the primitives the group lane drives it with.

// The lightRoot this switch fires. The BP reads `objects[0]` (the inherited
// triggerBase_C::objects array) and casts it to the int_Ttrigger interface; the older
// `Trigger` pointer field is the fallback. nullptr if neither resolves to a lightRoot.
void* ResolveSwitchRoot(void* sw);

// runTrigger(owner, index) on a lightRoot. The BP switches on index:
//   0 -> `IFNOT(active) POP` then isActive := !isActive; updLig()   (the GATED toggle a switch uses)
//   1 -> isActive := true;  updLig()                                 (absolute ON,  UNGATED)
//   2 -> isActive := false; updLig()                                 (absolute OFF, UNGATED)
// `owner` is written into the ubergraph frame and never read on any path, so we pass the
// root itself rather than inventing a caller. Game thread. False on null/unresolved.
bool CallRunTrigger(void* root, int32_t index);

// Drive the group to `on` ABSOLUTELY, through the game's own verb (index 1/2). Ungated on
// purpose: a receiver must land the authority's state even where its own gate is shut --
// that gate governs LOCAL presses, not what this peer is told the world looks like.
bool ApplyGroupState(void* root, bool on);

// Read/write the group's ENABLE GATE (`active`) -- used to make a CLIENT's native press
// presentation-only for the duration of one input dispatch, the same lever the door lane
// pulls on Adoor_C::Active. Never leave it cleared past the dispatch: a gate left shut is a
// player whose light switches silently stopped working.
bool GetGroupGate(void* root);
void SetGroupGate(void* root, bool open);

}  // namespace ue_wrap::lightswitch
