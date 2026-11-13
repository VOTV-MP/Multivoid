// coop/dev/vitals_keepalive.h -- [dev] periodic vitals refills for autonomous long-exposure harness runs
// (RULE-2-exempt diagnostics tooling). Two halves, each with its own knob.
//
// HOST BROADCAST, `vitals_keepalive_sec=N` (0 or absent is OFF, floor 30 s; host and solo only, through dev_gate): idle
// harness peers DIE (the client at about 18 minutes, the host at about 80) and the RNG census needs a multi-hour life, so
// this re-fires the proven restore_vitals path (local apply plus the RestoreVitals broadcast: every peer refills) and logs
// the pre-refill food, sleep and health, so the idle-death cause adjudicates itself from the drain curve.
//
// PER-PEER NEEDS, `fill_needs=1` (env VOTVCOOP_FILL_NEEDS): EACH peer writes its own food and sleep to 100 every 20 s
// through ue_wrap/actors/vitals, local and off the wire, so a long drill's player does not faint at 0. A drill about food,
// sleep or fainting leaves it off.

#pragma once

namespace coop::dev::vitals_keepalive {

// Called from dev_lanes TickDrills (game thread). A static-latched read per knob when both are off.
void Tick();

}  // namespace coop::dev::vitals_keepalive
