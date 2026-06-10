// coop/power_sync.h -- base POWER PANEL (ApowerControl_C) breaker mirror sync (protocol v46).
//
// Gameplay/network layer (principle 7): owns the wire protocol, the per-tick state poll, the
// receiver apply, the key->actor index, the deferred-apply retry, and the connect-snapshot.
// Talks to the engine ONLY through ue_wrap::power_control.
//
// WHY ITS OWN MODULE (not the interactable_sync toggle Channel): the panel is NOT a 2-state
// toggle -- it carries FIVE latched breaker bools (one per base subsystem). The generic Channel
// is one-bool-per-key, so 5 bools per actor don't fit. This is the same reasoning that gave the
// keypad its own module (a typed buffer, not a toggle). Structure mirrors keypad_sync (key->actor
// index with IsLiveByIndex self-heal, throttled rebuild, deferred-apply retry, silent first-sight
// prime, echo-suppress) but with a 5-bit MASK state and a panel-visual apply.
//
// SYMMETRIC: any peer flips a breaker; the receiver mirrors the panel's own visual (lever
// positions + LED particles). The base power EFFECTS (servers/doors/lightRoots) are synced by
// THEIR OWN channels, so the apply does not re-drive them. The host relays a client edge
// (IsClientRelayableReliableKind).
//
// RE: research/findings/votv-powerControl-panel-sync-RE-2026-06-08.md.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PowerPanelPayload;
}  // namespace coop::net

namespace coop::power_sync {

// Resolve ApowerControl_C + build the key->actor index; store the session pointer. Idempotent;
// retried every net-pump tick until the BP class loads. Game thread.
void Install(coop::net::Session* session);

// Receiver entry: a PowerControlState packet arrived (payload already memcpy'd + range-checked
// by event_feed). Resolves the panel by Key and applies on the game thread (deferring if it has
// not streamed in yet). Called from event_feed's reliable drain loop.
void OnReliable(const coop::net::PowerPanelPayload& payload, uint8_t senderPeerSlot);

// HOST-only: snapshot the current breaker state of every indexed panel to a freshly connected
// client `peerSlot` (so a joiner adopts the host's panel). The receiver idempotently skips
// already-matching ones. Net-pump connect edge. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Per-tick pump: throttled index rebuild + deferred-apply retry, then the sender poll. Call
// every net-pump tick on the game thread.
void Tick();

// Session teardown: clear the per-session index + dedup + pending state.
void OnDisconnect();

}  // namespace coop::power_sync
