// coop/dev/restore_vitals.h -- refill food, sleep and health on both peers' UsaveSlot_C at once.
// coffeePower is deliberately excluded: writing it to 100 triggers a post-coffee screen shake in
// the blueprint.
//
// Driven by the ImGui dev menu (Player > Vitals), and HOST-ONLY through dev_gate like every dev
// verb: solo or hosting, Restore() applies locally AND broadcasts a RestoreVitals reliable so the
// other peers refill too. A connected client is refused at the call, and its packet is dropped
// again at the receiver -- refilling your own vitals in someone else's game is a survival cheat.
// The echo is safe, since maxing out twice is idempotent.

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

// Menu action (Player > Vitals): set THIS peer's stamina low (10) to test the tired and exhausted
// state -- no sprint, the low-energy HUD and effects. VOTV has no stamina scalar; the energy
// meter is `sleep` (low sleep sets mainPlayer.isExhausted), so this writes only saveSlot.sleep
// and leaves food and health alone. LOCAL ONLY, no broadcast: the vitals display stream
// (PoseSnapshot sleepFrac) already mirrors the low value to peers' nameplates, so setting the
// tester's own value is the right scope. Host-only through dev_gate. Safe off the game thread --
// the write is posted to it.
void SetStaminaLow();

// Receiver: max-out food/sleep/health on the local UsaveSlot_C (coffeePower
// intentionally excluded -- see header comment).
// Called from event_feed.cpp on incoming ReliableKind::RestoreVitals, AND
// by Restore() locally. Game thread only.
void ApplyLocally();

}  // namespace coop::dev::restore_vitals
