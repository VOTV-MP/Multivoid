// coop/dev/look_probe.h -- read-only trace of what the local player's crosshair rests on.
//
// A guest sees the hint and the action menu of whatever it looks at blink out and back -- a light
// switch, the computer's power button, a kerfus -- and the host never does. The game re-derives
// AmainPlayer_C::lookAtActor from a trace every tick and draws the hint from it, so a blink is
// either that field letting go for a moment or the hint redrawn over a field that held.
//
// CAUSE: a per-tick read of lookAtActor, and script-gate watches on the HUD's rebuild bodies
// (ui_UI_C's buildActions, clearActions, openHovertext, updateSlotInv); both roles, the host as
// the control. EFFECT: a [LOOK] BLINK line when a held target lets go and comes back within half
// a second, and a [LOOK] HUD tally every 10 s of each rebuild by its caller, in milliseconds.

#pragma once

namespace coop::dev::look_probe {

bool IsEnabled();

// Per game tick. Gated by [dev] look_probe=1 or VOTVCOOP_DEV_LOOK_PROBE=1; off, one bool read.
// Game thread.
void Tick(bool isHost);

}  // namespace coop::dev::look_probe
