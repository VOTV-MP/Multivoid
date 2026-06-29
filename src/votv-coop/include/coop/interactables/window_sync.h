// coop/window_sync.h -- base-window dirt scalar sync (AbaseWindow_C::clean, the
// base's "main huge window"). Protocol v41, ReliableKind::WindowCleanState.
//
// Gameplay/network layer (principle 7): owns the wire protocol, the per-tick `clean`
// POLL, the receiver min-wins apply, the per-window Key->actor index, the deferred-apply
// retry, and the connect-snapshot. Talks to the engine ONLY through ue_wrap::base_window.
//
// Model (SYMMETRIC, MTA monotone min-register): each peer POLLS every AbaseWindow_C's
// `clean`@0x0260 once per tick (NOT a UFunction observer -- cleanSponge is BP-internal and
// bypasses our ProcessEvent detour, same as doors/keypad) and broadcasts on a DECREASE,
// keyed by the window's Aactor_save_C::Key. The receiver resolves the window by Key (self-
// healing index; deferred + retried if it has not streamed in yet) and applies
// MIN(local, wire) -- a wire update can only make a window CLEANER, so two peers wiping
// concurrently converge to the lowest value with no oscillation/regression (clean is
// monotone + inert: nothing re-raises it locally, unlike a door's autoclose -> no HostAuth
// needed). The host RELAYS a client-originated wipe to the other clients. On a new client's
// connect edge the HOST snapshots each window's current clean with adopt=1 (the receiver
// writes it VERBATIM so the joiner adopts the host's world even if its own save was cleaner).
//
// RE: research/findings/votv-dirt-window-cleaning-RE-and-coop-sync-design-2026-06-07a.md.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct KeyedScalarPayload;
}  // namespace coop::net

namespace coop::window_sync {

// Resolve the baseWindow_C class + build the Key->actor index. Idempotent; retried every
// net-pump tick until the BP class is loaded. Stores the session pointer. Game thread.
void Install(coop::net::Session* session);

// Receiver entry: a WindowCleanState packet arrived (payload already memcpy'd +
// range-checked by event_feed). Resolves the window by Key and applies MIN(local, clean)
// for a live wipe (adopt==0) or VERBATIM for a connect-snapshot (adopt==1), deferring if
// the instance has not streamed in. Called from event_feed's reliable drain loop.
void OnReliable(const coop::net::KeyedScalarPayload& payload, uint8_t senderPeerSlot);

// HOST-only: snapshot the current `clean` of every indexed window to a freshly connected
// client `peerSlot` with adopt=1 (the joiner adopts the host's world). Called from the
// net-pump connect edge. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Per-tick: poll for live `clean` decreases (broadcast wipes) + retry deferred applies
// (throttled). Call every net-pump tick on the game thread.
void Tick();

// Session teardown: clear the per-session poll baseline + pending applies.
void OnDisconnect();

}  // namespace coop::window_sync
