// coop/dev/vitals_keepalive.h -- [dev] periodic vitals refill for autonomous
// long-exposure harness runs (RULE-2-exempt diagnostics tooling).
//
// Why: idle harness peers DIE (measured 2026-07-10 census run: the idle client
// at ~18 min, the idle HOST at ~80 min -- [[lesson-copy-peer-log-before-relaunch]]),
// and the T1 RNG census needs a MULTI-HOUR client life (the long-period spawners
// arm on 30 min - 3 h cycles; a relaunch resets their timers, so cycled short
// lives can never measure them). This ticker keeps an unattended session alive
// by periodically re-firing the PROVEN restore_vitals path (local apply + the
// RestoreVitals reliable broadcast, so every connected peer refills too).
//
// It also MEASURES: each firing logs the pre-refill food/sleep/health values,
// so the idle-death cause adjudicates itself from the drain curve in the log
// (probe-don't-guess -- the 18-min death cause was never adjudicated).
//
// Gated by votv-coop.ini `vitals_keepalive_sec=N` (seconds between refills;
// 0/absent = OFF -- the shipping default). Host/solo only via dev_gate (a
// client refilling vitals in someone else's game is a cheat; the host's
// broadcast covers connected clients anyway).

#pragma once

namespace coop::dev::vitals_keepalive {

// Called from subsystems.cpp TickGameplay (game thread). Single static-latched
// bool read when the ini flag is off.
void Tick();

}  // namespace coop::dev::vitals_keepalive
