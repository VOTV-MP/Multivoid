// coop/dev/sleep_probe.h -- DEV-ONLY autonomous exerciser for the v71 sleep
// gate (ini sleep_probe=1; OFF by default; never ships enabled). Runs on BOTH
// peers of the LAN smoke and walks the whole gate through its phases with the
// game's own entry/exit verbs, so the smoke log proves the feature end-to-end
// without hands:
//
//   CLIENT  T+15s after connect: reflected gamemode.sleep(bed) -- expect the
//           host log 'sleep_sync: tally 1/2 in bed' + the client's own
//           'WAITING -- dilation undone'.
//   HOST    T+25s: reflected gamemode.sleep(bed) -- expect 'ACCELERATE' on
//           both peers (dilation 20, feed line).
//   HOST    T+40s: reflected gamemode.wakeup() (a manual interrupt) -- expect
//           'END (natural=0)' on both peers + 'Sleep interrupted' feed lines.
//
// NOT dev_gate-gated: this is a harness scenario driver (it must run on the
// smoke CLIENT), not an interactive cheat surface; the ini key is the gate.

#pragma once

namespace coop::dev::sleep_probe {

void Install();
void Tick(bool isConnected, bool isHost);

}  // namespace coop::dev::sleep_probe
