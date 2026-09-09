// coop/interactables/keypad_sync.h -- password-keypad (ApasswordLock_C) mirror sync.
//
// Gameplay/network layer (principle 7): owns the wire protocol, the per-tick state poll, the
// receiver apply, the key-to-actor index, the deferred-apply retry and the connect snapshot. Talks
// to the engine ONLY through ue_wrap::passwordlock.
//
// It is its own module rather than an interactable_sync toggle Channel because a keypad is not a
// two-state toggle: it carries a typed digit BUFFER plus three state bools, and its accept verb
// is a SUBMIT, not a state setter. Forcing it into that Channel -- poll isAcc, replay Open(want)
// -- fail-cycles for that reason: replaying Open re-submits the buffer, it does not restore it.
//
// Two things it deliberately does not drive. The door: a native accept UNLOCKS it by writing
// door.active, never opens it, and opening an unlocked door is an ordinary E press. And
// isAcc/isDeny, set from the component the crosshair hit to pick the prompt -- hover flags, not
// state; both written onto a mirror render green and red. The LED is power-driven, needing none.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct KeypadSyncPayload;
}  // namespace coop::net

namespace coop::keypad_sync {

// Resolve the passwordLock_C class and build the key-to-actor index; store the session pointer.
// Idempotent; retried every net-pump tick until the BP class loads. Game thread.
void Install(coop::net::Session* session);

// Receiver entry: a KeypadState packet arrived (event_feed has memcpy'd and range-checked it).
// Resolves the keypad by Key and applies on the game thread, deferring while it is still
// streaming in. Called from event_feed's reliable drain loop.
//
// A None state mirror replays inputNumber(digit) for the typed-buffer DELTA -- the native display,
// beep and auto-submit -- then writes `active` and repaints with upd(). Accept or Deny runs the
// keypad's OWN native Open(Active) chain through PL::CallOpen (sound, LED, buffer clear, and the
// lock-state propagation writing pair.active and door.active), which replicates the press across
// peers. The echo breaks by priming lastKnown to the pre-chain state.
void OnReliable(const coop::net::KeypadSyncPayload& payload, uint8_t senderPeerSlot);

// HOST-only: snapshot the current state of every indexed keypad to a freshly connected
// client `peerSlot` (so an in-progress / already-unlocked keypad matches on join). The
// receiver idempotently skips already-matching ones. Net-pump connect edge. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Per-tick pump: the throttled deferred-apply retry, then the sender poll -- the index itself is
// refreshed on the scan hub's own cadence, not here. A short
// (under five digit) code's native submit edge -- `active` flips and the buffer clears without a
// reset -- stamps Accept or Deny; everything else is a plain None state mirror. A code of five
// digits or more needs no event at all, because the blueprint auto-submits as soon as the buffer
// reaches that length, so replaying the digits runs the native validator on every peer already.
// Call every net-pump tick on the game thread.
void Tick();

// Session teardown: clear the per-session index + dedup + pending state.
void OnDisconnect();

}  // namespace coop::keypad_sync
