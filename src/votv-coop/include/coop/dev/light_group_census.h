// coop/dev/light_group_census.h -- dev-only READ-ONLY census of VOTV's light groups. The shipped
// light lane syncs Alightswitch_C::A, the switch's PRESENTATION bit (its mesh and
// sound), which is not save-persistent -- lightswitch_C has no getTriggerData/loadTriggerData pair.
// What a player SEES lives one object away, on Atrigger_lightRoot_C, which carries THREE bools:
//
//   isActive     -- the group's live on/off. updLig() pushes it to every lamp.
//   active       -- an ENABLE GATE: runTrigger(owner, 0) is `IFNOT(active) POP` before it toggles
//                   isActive, so a closed gate makes a switch press move NO lights.
//                   Save-persistent, driven by the base power panel's lights breaker
//                   (powerControl.buttonsVisibility).
//   buffIsActive -- the SAVED copy; `loadAft` does isActive := buffIsActive, then updLig().
//
// Nothing syncs or reconciles isActive or active, and thirteen cooked-pak blueprints can move a
// light group, so peer agreement is unanswerable from the shipped logs. This file only MEASURES.
// Set `lightgroup_census=1` on BOTH peers and pair the `[LGC]` lines BY KEY. Game thread only.

#pragma once

namespace coop::dev::light_group_census {

// Resolve the classes + offsets. Idempotent, retried until the blueprints load.
void Install();

// Low-rate census pass. Emits a full `[LGC] dump` on a fixed cadence and an immediate
// `[LGC] CHANGE` line the moment any watched bit moves, so a two-peer diff has both a
// baseline and the edges. Walks GUObjectArray, so it is ini-gated and rate-limited and
// must never be enabled for a performance measurement.
void Tick();

}  // namespace coop::dev::light_group_census
