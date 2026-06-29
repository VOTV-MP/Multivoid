// coop/keypad_sync.h -- password-keypad (ApasswordLock_C) mirror sync (protocol v33).
//
// Gameplay/network layer (principle 7): owns the wire protocol, the per-tick state
// poll, the receiver apply, the key->actor index, the deferred-apply retry, and the
// connect-snapshot. Talks to the engine ONLY through ue_wrap::passwordlock.
//
// WHY ITS OWN MODULE (not the interactable_sync toggle Channel): a keypad is NOT a
// 2-state toggle. It carries a typed digit BUFFER plus three state bools, and -- proven
// by 3 autonomous synth-probe rounds 2026-06-04 -- its native accept verb is UNREACHABLE
// by us (Open/open2/SetActive/isButtonUsed/processKeys are all inert even with the
// buffer filled + focusOn). The v31 attempt to force it into the toggle Channel
// ("poll isAcc + replay Open(want)") fail-cycled exactly because Open is a submit verb.
//
// THE MIRROR (RULE 1, MTA input-replication; v59 submit mirror 2026-06-11):
//   SENDER  (every peer, per net-pump tick): poll each indexed keypad's {inPassword, active};
//           broadcast KeypadSyncPayload on a change, CLASSIFIED into a KeypadEvent: a SHORT
//           (<5 digit) code's native submit edge (active flip + buffer cleared, not reset
//           mode) stamps Accept/Deny; everything else is a plain None state mirror. len>=5
//           codes need no event -- the BP AUTO-submits at Len>=5 (uber @2398), so the digit
//           replay runs the native validator on every peer already.
//   RECEIVER: resolve by Key; None -> replay inputNumber(digit) for the typed-buffer DELTA
//           (native display+beep+auto-submit) + active write + upd() repaint; Accept/Deny ->
//           run the keypad's OWN native Open(Active) chain (PL::CallOpen: accept/deny sound,
//           LED, buffer clear, LOCK-state propagation to the pair keypad + gated door) --
//           the cross-peer replication of the accept/cancel press. Echo-broken by priming
//           lastKnown to the pre-chain state.
//   NO door drive: a native accept UNLOCKS the door (door.active=true); it never OPENS it
//           (the 4s-doorOpen chain is a scripted trigger entry, not the player accept).
//           Opening/closing the unlocked door is a normal E press -- the door channel's
//           job. (The deleted 2026-06-11 trio -- buffer==password accept + auto-ForceOpen +
//           g_unlocked green latch + permanent SuppressHostHeldDoor -- was the "opens on
//           the last digit, stuck green, can never close again" bug.)
//   NO isAcc/isDeny MIRROR (removed 2026-06-06): the BP disassembly proved isAcc/isDeny are
//           crosshair-HOVER flags, not accept/deny state -- writing both onto a mirror was the
//           non-native green+red "PURPLE" the user reported. The LED colour is power-driven and
//           already equal across peers from the same world; we do not drive it.
//
// RE: research/findings/votv-keypad-door-BP-disassembly-2026-06-06.md (+ the 2026-06-11
// uber re-read in research/bp_reflection/_passwordlock_uber_full.txt).

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct KeypadSyncPayload;
}  // namespace coop::net

namespace coop::keypad_sync {

// Resolve the passwordLock_C class + build the key->actor index; store the session
// pointer. Idempotent; retried every net-pump tick until the BP class loads. Game thread.
void Install(coop::net::Session* session);

// Receiver entry: a KeypadState packet arrived (payload already memcpy'd + range-checked
// by event_feed). Resolves the keypad by Key and applies on the game thread (deferring
// if it has not streamed in yet). Called from event_feed's reliable drain loop.
void OnReliable(const coop::net::KeypadSyncPayload& payload, uint8_t senderPeerSlot);

// HOST-only: snapshot the current state of every indexed keypad to a freshly connected
// client `peerSlot` (so an in-progress / already-unlocked keypad matches on join). The
// receiver idempotently skips already-matching ones. Net-pump connect edge. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Per-tick pump: throttled index rebuild + deferred-apply retry, then the sender poll.
// Call every net-pump tick on the game thread.
void Tick();

// Session teardown: clear the per-session index + dedup + pending state.
void OnDisconnect();

}  // namespace coop::keypad_sync
