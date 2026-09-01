// coop/dev/light_group_census.h -- dev-only READ-ONLY census of VOTV's light groups.
//
// WHY THIS EXISTS. The shipped light lane syncs Alightswitch_C::A -- the switch's
// PRESENTATION bit (its mesh + sound; bytecode-measured 2026-09-01, and NOT
// save-persistent: lightswitch_C has no getTriggerData/loadTriggerData pair). The
// state a player actually SEES lives one object away, on Atrigger_lightRoot_C, which
// carries THREE distinct bools:
//
//   isActive     -- the group's live on/off. updLig() pushes it to every lamp.
//   active       -- an ENABLE GATE. runTrigger(owner,0) is `IFNOT(active) POP` before
//                   it toggles isActive, so a closed gate makes a switch press flip the
//                   switch and move NO lights. Save-persistent; driven by the base power
//                   panel's lights breaker (powerControl.buttonsVisibility).
//   buffIsActive -- the SAVED copy; `loadAft` does isActive := buffIsActive; updLig().
//
// (That triple closes RE flag E-L2, open since 2026-05-25.)
//
// Neither isActive nor active is synced or reconciled by anything in this codebase, and
// a population census of the cooked paks (3,522 uassets) found THIRTEEN blueprints that
// can move a light group -- powerControl, mainGamemode, ticker_flickerer, ui_cheatMenu,
// and every trigger_eventer/solarBoom/fakeLmaos/breakDish, because a lightRoot IS a
// trigger. So the question "do two peers' light groups agree, and if not which bit
// broke first" is unanswerable from the shipped logs.
//
// THIS FILE ONLY MEASURES. It writes nothing to the world and calls no UFunction --
// deliberately, because the neighbouring lightswitch_probe DOES mutate (a one-shot
// synthetic use()), which would itself desync a two-peer run. Hence its own ini row.
//
// USE: set `lightgroup_census=1` in multivoid.ini on BOTH peers, reproduce (idle / flip a
// switch / throw the base power panel's lights breaker), then pair the `[LGC]` lines from
// the two logs BY KEY. A key whose isActive differs across peers is a real divergence; a
// key whose `active` differs is the gate having drifted, which makes the NEXT press
// diverge. Game thread only.

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
