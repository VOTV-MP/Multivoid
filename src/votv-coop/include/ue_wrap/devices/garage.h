// ue_wrap/garage.h -- standalone engine access for the base GARAGE door
// (Agarage_C). Principle-7 engine-wrapper layer (no network/coop state).
// coop::interactable_sync drives the sync through here.
//
// Agarage_C is a self-contained, level-placed AtriggerBase_C descendant (the base's
// big roller door). Unlike Adoor_C it has NO sensor / autoclose / ReceiveTick, so it
// never auto-reverts -> it syncs SYMMETRICALLY (like lights/containers), not HostAuth.
// State = `bool Open` @0x02E8.
//
// IDENTITY = the garage's LEVEL-EXPORT FName (its placed-actor name), NOT the inherited
// AtriggerBase_C::Key. The save Key is assigned by the gamemode's ONE-SHOT, sublevel-gated
// keying pass (mainGamemode::loadObjects -> GetAllActorsWithInterface + loadTriggers, gated
// by isSublevelAllowed -- kismet-analyzer bytecode 2026-07-21); a garage instance that is
// mid-recycle / not in that snapshot during the host's menu->save world transition never
// gets keyed (Key stays None) and is dropped by the Channel's None-key filter FOREVER
// (take-4 R9: host garage index 1->0 at the reload, never recovered, while 50 doors keyed).
// The level-export FName is baked into the cooked untitled_1.umap package -> deterministic +
// cross-peer stable BY CONSTRUCTION (both peers load the identical package) and independent
// of the fragile keying. This is exactly why door_box keys lockers/console (same untitled_1
// package) by their FName; door_box's name-based keysHash was byte-identical host vs client
// through the SAME reload that broke the garage's Key identity. The wall button
// (Atrigger_button_C) just fires runTrigger -> the garage toggles Open; we sync the garage's
// resulting Open (which catches every activation source), never the button.
//
// RE: research/findings/computers-devices/votv-garage-door-button-sync-RE-2026-06-08.md;
//     R9 root+fix: votv-take4-hands-on-bugs-2026-07-21.md (R9) + the qf thread 2026-07-21.

#pragma once

#include <string>

namespace ue_wrap::garage {

// Resolve Agarage_C + the Open offset + the acivae UFunction. Idempotent; true
// once resolved. Game thread.
bool EnsureResolved();

// True iff `obj`'s class is garage_C or a subclass. False if not yet resolved.
bool IsGarage(void* obj);

// The garage's LEVEL-EXPORT FName (its placed-actor name) as a wide string ("" on failure).
// This is the cross-peer identity -- see the header note on why NOT the save Key.
std::wstring GetNameKey(void* g);

// Read the garage's `Open` bool into `open`. False if the read could not be made (null /
// not resolved); leaves `open` untouched on failure.
bool TryReadOpen(void* g, bool& open);

// Drive the garage to `open` -- idempotent (no-op if already Open==open). Writes the `Open` bool
// directly (neither settime NOR acivae writes it -- only runTrigger/loadTriggerData do -- so the
// mirror's poll baseline would otherwise go stale -> oscillation), THEN plays the NATIVE animated
// swing via acivae() (montage from 0 @0.5x + the move timeline over its full ~10s, direction read
// from the Open field we just set). NOT settime: settime SNAPS (move.SetNewTime(endpoint) + montage
// @StartingPosition=100) = the "too fast" the user saw. MUST run on the game thread. False on null /
// unresolved UFunction. RE: the garage Open-writers + animate-vs-snap bytecode passes 2026-06-09.
bool ApplyOpen(void* g, bool open);

}  // namespace ue_wrap::garage
