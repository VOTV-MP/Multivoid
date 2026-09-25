// ue_wrap/devices/appliance.h -- standalone engine access for VOTV's simple on/off APPLIANCES
// (faucet / sink / shower / kitchen-oven / serverBox / wallunit-tapes). Principle-7
// engine-wrapper layer (no network/coop state). coop::interactable_sync drives the sync
// through here via ONE Adapter -- this wrapper is the per-class dispatch.
//
// All six are Aactor_save_C descendants carrying a single bool on/off toggle and a no-arg
// refresh verb (upd/updIsOn), except serverBox, whose setter visual(bool) writes its bool.
// None has a sensor / autoclose -> none auto-reverts -> they sync SYMMETRICALLY (like
// lights/garage), each sent at the class's own verb (coop/interactables/toggle_verbs). Identity =
// the inherited Aactor_save_C::Key (save-persistent, cross-peer stable). The per-class bool name
// and the apply verb live in ONE table, appliance.cpp's g_descs, and a name that does not resolve
// leaves its class out: sink fires updIsOn() then upd() because its BP fires both, serverBox goes
// through visual(bool).

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::appliance {

// Resolve each leaf class as it streams in: its UClass, the Key it inherits from Aactor_save_C, its
// bool offset and its refresh verb. Lazy + best-effort: returns true once any class has resolved
// (the family can operate); a save lacking one class just never indexes it. Idempotent. Game
// thread.
bool EnsureResolved();

// True iff `obj`'s class is (a descendant of) any of the six appliance classes. Cheap
// (pointer compares + one hierarchy walk over the resolved set); false until resolved.
bool IsAppliance(void* obj);

// The appliance's Aactor_save_C::Key as a wide string ("" on failure or for a class not in the set,
// L"None" if unkeyed).
std::wstring GetKeyString(void* a);

// Read the appliance's per-class on/off bool into `on`. False if the read could not be made
// (null / class not in the set / not resolved); leaves `on` untouched on failure.
bool TryReadState(void* a, bool& on);

// Drive the appliance to `on`: serverBox via visual(bool); the rest by direct-writing the
// bool then calling the no-arg refresh verb (upd/updIsOn) so the mesh/FX/audio repaint from
// the new state. MUST run on the game thread. False on null / unresolved.
bool ApplyState(void* a, bool on);

// A tap's toggle, as its getActionOptions offers it while a player aims at it: a faucet's and a
// sink's action 5 negates their bool with no other condition (their bytecode; a faucet's 7 takes the
// tap off, and a shower's 5 toggles only while its useType is 0).
inline constexpr uint8_t kTapToggleAction = 5;

// True iff `obj` is a faucet_C or a sink_C, once its class has resolved. Game thread.
bool IsTap(void* obj);

// The appliance's own action verb as a player's interaction dispatches it: actionOptionIndex with
// `player` and `action`. False when the class has no such verb or the call did not run. Game thread.
bool CallAction(void* a, void* player, uint8_t action);

// The kitchen oven's repair. Its `fixed` goes false to true once, in fix(): fix() sets it, repaints
// the oven and closes the repair widget where one is open; the widget's last step calls it, and
// loadData calls it for a saved repair. Nothing sets it back (kitchen_C's and UI_oven_C's bytecode).
// True iff `obj` is a kitchen_C, once its class has resolved. Game thread.
bool IsOven(void* obj);

// Read the oven's `fixed` into `fixed`. False if the read could not be made. Game thread.
bool TryReadOvenFixed(void* oven, bool& fixed);

// Run the oven's own fix(). False when the class has no such verb or the call did not run. Game
// thread.
bool CallOvenFix(void* oven);

// A drill's fixture, never the game's path: write the oven's `fixed` and repaint it (upd), so a drill
// can start from a broken oven where the save had repaired it. Game thread.
bool WriteOvenFixedForDrill(void* oven, bool fixed);

}  // namespace ue_wrap::appliance
