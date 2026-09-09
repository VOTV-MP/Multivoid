// coop/interactables/interactable_sync.h -- keyed interactable open/close/on-off state sync. ONE
// replication engine drives seven features through a shared Channel, with no per-feature copy:
//   - DoorState (9):         base doors     (Adoor_C, open intent, host-authoritative)
//   - LightState (10):       light switches (Alightswitch_C::use, replayed on receipt)
//   - ContainerState (11):   container lids (Aprop_swinger_C::Open / Close)
//   - GarageDoorState (33):  the garage     (Agarage_C, keyed by level-export name)
//   - ApplianceState (35):   the save-actor appliance family
//   - LockerDoorState (50):  lockers and the drone-console box (level-export name)
//   - LightGroupState (129): light groups   (runTrigger on the root, host-authoritative)
//
// Gameplay/network layer (principle 7): it owns the wire protocol, the sender polls, the receiver
// apply, the per-channel key index, the deferred-apply retry and the connect snapshot, and reaches
// the engine only through ue_wrap. The model is SYMMETRIC: each peer POLLS every indexed instance
// once per tick and broadcasts a delta under its cross-peer-stable key. Polling, not a UFunction
// observer, because the verbs are BP-internal and a poll catches EVERY writer.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct KeyedTogglePayload;
}  // namespace coop::net

namespace coop::interactable_sync {

// Resolve each channel's class + register its POST observers. Idempotent per
// channel; retried every net-pump tick until each BP class is loaded. Stores the
// session pointer (tracks reconnects). Game thread.
void Install(coop::net::Session* session);

// Receiver entry: a DoorState / LightState / ContainerState packet arrived. Resolves the live
// instance by Key -- a self-healing index, deferred and retried when the instance has not streamed
// in yet -- and applies idempotently, updating the poll baseline so the apply never echoes. On the
// host this also RELAYS a client-originated edge to the other clients.
void OnReliable(uint8_t kind, const coop::net::KeyedTogglePayload& payload, uint8_t senderPeerSlot);

// HOST receiver entry for a client's DoorOpenRequest (doors are host-authoritative).
// The host applies the requested open/close honoring the real lock/jam guards; its poll
// then broadcasts the authoritative DoorState back. Trust-gated on senderPeerSlot != 0
// (a client). Called from event_feed's reliable drain loop.
void OnDoorOpenRequest(const coop::net::KeyedTogglePayload& payload, uint8_t senderPeerSlot);

// HOST-only: a single peer disconnected -- drop its hold on every door it was keeping
// open (doors still held by OTHER peers stay open; a door whose last holder just left
// closes). Robust per-peer cleanup for N-peer sessions, distinct from OnDisconnect's
// all-peers-gone full clear. Called from event_feed's per-slot disconnect edge. Game thread.
void OnPeerLeft(int peerSlot);

// HOST-only: snapshot the FULL current state (open AND closed / on AND off) of
// every indexed instance (all channels) to a freshly connected client `peerSlot`.
// A joiner loads its own save, so a host-turned-OFF switch (from a saved/default
// ON) must be pushed too -- the receiver idempotently skips already-matching ones.
// Called from the net-pump connect edge. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Per-tick retry pump for deferred applies (instances still streaming in). No-op
// when nothing is deferred; otherwise THROTTLED per channel. Call every net-pump
// tick on the game thread.
void Tick();

// Session teardown: clear every channel's per-session dedup + pending state.
void OnDisconnect();

}  // namespace coop::interactable_sync
