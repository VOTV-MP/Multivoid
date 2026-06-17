// coop/dev/restore_vitals.h -- refill food/sleep/health on both peers'
// UsaveSlot_C simultaneously. (coffeePower is intentionally excluded -- writing
// it to 100 triggers a screen-shake post-coffee BP side-effect; see commit
// 5421d6f for the user-retest finding.)
//
// Driven by the ImGui dev menu (Player > Vitals > "Restore vitals"). The legacy
// F3 hotkey was RETIRED 2026-06-02 (RULE [[feedback-dev-features-in-imgui-menu]]:
// dev features live in the F1 menu, not ad-hoc hotkeys).
//
// Direction: any peer (host OR client) can trigger it -- Restore() applies
// locally AND broadcasts a RestoreVitals reliable so the remote peer restores
// too. Echo is safe: applying max-out twice is idempotent (stays at max).

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::restore_vitals {

// Cache the Session pointer so Restore() can broadcast the RestoreVitals packet.
// Called once from harness boot. Mirrors prop_lifecycle::SetSession.
void SetSession(coop::net::Session* session);

// Menu action (Player > Vitals): refill the local player's vitals AND broadcast
// a RestoreVitals reliable so the remote peer's vitals refill too. Safe to call
// off the game thread (the local apply is posted to it; the broadcast is
// wire-thread-safe).
void Restore();

// Menu action (Player > Vitals): set THIS peer's STAMINA LOW (10) for testing the tired/exhausted
// state (can't-sprint, the low-energy HUD/effects). VOTV has no "stamina" scalar -- the energy meter is
// `sleep` (low sleep -> mainPlayer.isExhausted), so this writes ONLY saveSlot.sleep, leaving food + health
// alone. LOCAL ONLY -- no broadcast: the v19 vitals DISPLAY stream (PoseSnapshot sleepFrac) already mirrors
// the low value to peers' nameplates, so setting only the tester's value is the right scope. Host-only via
// dev_gate (dev verbs are host-only). Safe off the game thread (the write is posted to it).
void SetStaminaLow();

// Receiver: max-out food/sleep/health on the local UsaveSlot_C (coffeePower
// intentionally excluded -- see header comment).
// Called from event_feed.cpp on incoming ReliableKind::RestoreVitals, AND
// by Restore() locally. Game thread only.
void ApplyLocally();

}  // namespace coop::dev::restore_vitals
