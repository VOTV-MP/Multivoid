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

// Read the group's `IsActive` bool into `on`. False if the read could not be
// made (null / not resolved); leaves `on` untouched on failure.
bool TryReadActive(void* root, bool& on);

// SetActive(on) -- turns the whole group on/off (fans out to all lamps). MUST run
// on the game thread. False on null / unresolved UFunction.
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

}  // namespace ue_wrap::lightswitch
