// coop/dev/lightswitch_probe.h -- dev-only RE probe for light-switch interaction sync.
//
// Gated on ini `lightswitch_probe=1`. The existing interactable_sync hooks
// Atrigger_lightRoot_C::SetActive (suspected BP-internal = never fires = no sync, the
// doors `doorOpen` trap). This probe resolves it "verify, don't guess":
//   - Installs POST observers on the CANDIDATE sender edges (Alightswitch_C::player_use,
//     ::use, ::actionOptionIndex) + the current Atrigger_lightRoot_C::SetActive -- a FIRED
//     line on a REAL flip (hands-on) tells us which edge is ProcessEvent-OBSERVABLE.
//   - Runs a ONE-SHOT synthetic flip: finds a lightswitch, reads its A (switch-flip bool
//     flip bool) + its lightRoot (via Trigger) IsActive, calls use() via reflection,
//     re-reads both. If A FLIPS + IsActive TOGGLES but lightRoot.SetActive did NOT fire ->
//     SetActive is BP-internal (the trap) AND use() does the FULL flip (switch visual +
//     lights) -> the fix = hook a player edge on the SENDER + replay use() on the RECEIVER
//     (mirrors BOTH the switch flip and the lights in one BP call). Game thread only.

#pragma once

namespace coop::dev::lightswitch_probe {

void Install();  // ini-gated, retried until lightswitch_C loads; self-latches.
void Tick();     // one-shot synthetic flip after the world settles; logs the verdict.

}  // namespace coop::dev::lightswitch_probe
