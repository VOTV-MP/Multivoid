// coop/dev/vitals_keepalive.h -- [dev] periodic vitals refill for autonomous long-exposure
// harness runs (RULE-2-exempt diagnostics tooling).
//
// Why: idle harness peers DIE -- measured on a census run, the idle client at about 18
// minutes and the idle HOST at about 80 -- and the RNG census needs a MULTI-HOUR client life,
// since the long-period spawners arm on 30-minute to 3-hour cycles and a relaunch resets
// their timers, so cycled short lives can never measure them. This ticker keeps an unattended
// session alive by re-firing the proven restore_vitals path (local apply plus the
// RestoreVitals reliable broadcast, so every connected peer refills too).
//
// It also MEASURES: each firing logs the pre-refill food, sleep and health values, so the
// idle-death cause adjudicates itself from the drain curve in the log.
//
// Gated by multivoid.ini `vitals_keepalive_sec=N` (seconds between refills; 0 or absent is
// OFF, the shipping default). Host and solo only, through dev_gate.

#pragma once

namespace coop::dev::vitals_keepalive {

// Called from subsystems.cpp TickGameplay (game thread). Single static-latched
// bool read when the ini flag is off.
void Tick();

}  // namespace coop::dev::vitals_keepalive
