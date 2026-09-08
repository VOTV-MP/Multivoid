// coop/power_sync.h -- base POWER PANEL (ApowerControl_C) breaker mirror.
//
// Gameplay/network layer (principle 7): the wire packet, the per-tick sender poll, the receiver
// apply, the deferred-apply retry and the connect snapshot. It reaches the engine only through
// ue_wrap::power_control, and its key->actor index is rebuilt in this module's own pass-complete
// callback, from the shared object-scan hub's pass.
//
// Its own module rather than an interactable_sync toggle Channel because the panel is not a
// two-state toggle: it carries FIVE latched breaker bools, one per base subsystem, packed into a
// 5-bit mask, where the generic Channel is one bool per key. The keypad's typed buffer made the
// same call.
//
// Symmetric -- any peer flips a breaker and every receiver mirrors the panel's own visual, the
// lever positions and the LED particles. The base power EFFECTS (servers, doors, lightRoots) ride
// their own channels, so the apply does not re-drive them; the host relays a client's edge.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PowerPanelPayload;
}  // namespace coop::net

namespace coop::power_sync {

// Store the session pointer and register the power-panel consumer with the shared object-scan
// hub, which resolves ApowerControl_C and keeps the key->actor index. Idempotent; re-entered
// every net-pump tick. Game thread.
void Install(coop::net::Session* session);

// Receiver entry for a PowerControlState packet. Resolves the panel by Key and applies the mask
// on the game thread, deferring the apply if the actor has not streamed in yet. Called from
// event_feed's reliable drain loop.
void OnReliable(const coop::net::PowerPanelPayload& payload, uint8_t senderPeerSlot);

// HOST-only: snapshot the current breaker state of every indexed panel to a freshly connected
// client `peerSlot`, so a joiner adopts the host's panels. The receiver idempotently skips the
// already-matching ones. Net-pump connect edge. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Per-tick pump: the throttled deferred-apply retry, then the sender poll. The index itself is
// refreshed on the scan hub's own cadence, not here. Call every net-pump tick on the game thread.
void Tick();

// Session teardown: clear the key->actor index, the last-known masks and the pending applies.
void OnDisconnect();

}  // namespace coop::power_sync
