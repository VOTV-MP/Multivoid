// ue_wrap/appliance.h -- standalone engine access for VOTV's simple on/off APPLIANCES
// (faucet / sink / shower / kitchen-oven / serverBox / wallunit-tapes). Principle-7
// engine-wrapper layer (no network/coop state). coop::interactable_sync drives the sync
// through here via ONE Adapter -- this wrapper is the per-class dispatch.
//
// All six are Aactor_save_C descendants carrying a single bool on/off toggle and a no-arg
// refresh verb (upd/updIsOn), except serverBox which exposes a named SetActive(bool) setter.
// None has a sensor / autoclose -> none auto-reverts -> they sync SYMMETRICALLY (like
// lights/garage), driven by the generic Channel's state poll (the activation verbs are
// BP-internal and bypass our ProcessEvent detour, so we never observe the switch -- we poll
// the resulting bool, exactly as doors/lights/garage do). Identity = the inherited
// Aactor_save_C::Key @0x0230 (save-persistent, cross-peer stable).
//
// Per-class state field + apply verb (SDK CXXHeaderDump, Alpha 0.9.0-n):
//   faucet_C         turnon       @0x0278  upd()
//   sink_C           isOn         @0x0278  updIsOn() then upd()  (BP fires both; mirror both)
//   prop_shower_C    running_cold @0x0298  upd()
//   kitchen_C        Active       @0x02E1  upd()       (the oven on/off)
//   serverBox_C      Active       @0x03D5  SetActive(bool bNewActive)
//   wallunit_tapes_C Active       @0x0290  upd()
//
// RE: research/findings/props-lifecycle/votv-all-interactables-sweep-catalog-2026-06-08.md.

#pragma once

#include <string>

namespace ue_wrap::appliance {

// Resolve the shared Aactor_save_C::Key offset + each leaf class's UClass / bool offset /
// refresh verb. Lazy + best-effort: returns true once the Key offset is known (the family
// can operate); individual classes resolve as they stream in -- a save lacking one class
// just never indexes it. Idempotent. Game thread.
bool EnsureResolved();

// True iff `obj`'s class is (a descendant of) any of the six appliance classes. Cheap
// (pointer compares + one hierarchy walk over the resolved set); false until resolved.
bool IsAppliance(void* obj);

// The appliance's Aactor_save_C::Key as a wide string ("" on failure, L"None" if unkeyed).
std::wstring GetKeyString(void* a);

// Read the appliance's per-class on/off bool into `on`. False if the read could not be made
// (null / class not in the set / not resolved); leaves `on` untouched on failure.
bool TryReadState(void* a, bool& on);

// Drive the appliance to `on`: serverBox via SetActive(bool); the rest by direct-writing the
// bool then calling the no-arg refresh verb (upd/updIsOn) so the mesh/FX/audio repaint from
// the new state. MUST run on the game thread. False on null / unresolved.
bool ApplyState(void* a, bool on);

}  // namespace ue_wrap::appliance
