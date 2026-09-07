// coop/keypad_sync.h -- password-keypad (ApasswordLock_C) mirror sync.
//
// Gameplay/network layer (principle 7): owns the wire protocol, the per-tick state poll, the
// receiver apply, the key-to-actor index, the deferred-apply retry and the connect snapshot. Talks
// to the engine ONLY through ue_wrap::passwordlock.
//
// It is its own module rather than an interactable_sync toggle Channel because a keypad is not a
// two-state toggle: it carries a typed digit BUFFER plus three state bools, and its native accept
// verb is unreachable from outside -- Open, open2, SetActive, isButtonUsed and processKeys are all
// inert even with the buffer filled and focusOn set. Forcing it into the toggle Channel ("poll
// isAcc, replay Open(want)") fail-cycles, because Open is a submit verb, not a state.
//
// The mirror replicates INPUT, the shape MTA uses. SENDER, every peer, per net-pump tick: poll each
// indexed keypad's {inPassword, active} and broadcast a KeypadSyncPayload on a change, classified
// into a KeypadEvent.

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

// Receiver entry: a KeypadState packet arrived (payload already memcpy'd and range-checked by
// event_feed). Resolves the keypad by Key and applies on the game thread, deferring if it has not
// streamed in yet. Called from event_feed's reliable drain loop.
//
// A None state mirror replays inputNumber(digit) for the typed-buffer DELTA, which gives the native
// display, beep and auto-submit, then writes `active` and repaints with upd(). Accept or Deny runs
// the keypad's OWN native Open(Active) chain through PL::CallOpen -- sound, LED, buffer clear, and
// the lock-state propagation that writes pair.active and door.active -- which is what replicates
// the press across peers. The echo is broken by priming lastKnown to the pre-chain state.
//
// Two things this deliberately does not drive. The door: a native accept UNLOCKS it by writing
// door.active and never opens it, and opening an unlocked door is an ordinary E press, the door
// channel's job. And isAcc/isDeny: the blueprint sets them from which component the crosshair hit,
// to pick the interaction prompt, so they are hover flags rather than accept or deny state --
// writing both onto a mirror is what once rendered a keypad green and red at once.
void OnReliable(const coop::net::KeypadSyncPayload& payload, uint8_t senderPeerSlot);

// HOST-only: snapshot the current state of every indexed keypad to a freshly connected
// client `peerSlot` (so an in-progress / already-unlocked keypad matches on join). The
// receiver idempotently skips already-matching ones. Net-pump connect edge. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Per-tick pump: throttled index rebuild and deferred-apply retry, then the sender poll. A short
// (under five digit) code's native submit edge -- `active` flips and the buffer clears without a
// reset -- stamps Accept or Deny; everything else is a plain None state mirror. A code of five
// digits or more needs no event at all, because the blueprint auto-submits as soon as the buffer
// reaches that length, so replaying the digits runs the native validator on every peer already.
// Call every net-pump tick on the game thread.
void Tick();

// Session teardown: clear the per-session index + dedup + pending state.
void OnDisconnect();

}  // namespace coop::keypad_sync
