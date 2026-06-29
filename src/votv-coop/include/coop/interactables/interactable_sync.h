// coop/interactable_sync.h -- generic "keyed interactable open/close/on-off
// state" sync (Phase 5D, protocol v27). ONE engine of replication drives three
// features through a shared Channel (RULE 2 -- no per-feature duplication):
//   - DoorState (9):      base doors   (Adoor_C::doorOpen / doorClose)
//   - LightState (10):    light groups (Atrigger_lightRoot_C::SetActive)
//   - ContainerState (11):container lids (Aprop_swinger_C::Open / Close)
//
// Gameplay/network layer (principle 7): owns the wire protocol, the sender
// observers, the receiver apply, the per-channel key->actor index, the deferred-
// apply retry, and the connect-snapshot. Talks to the engine ONLY through the
// ue_wrap engine wrappers (ue_wrap::door / ::lightswitch / ::swinger / ::prop).
//
// Model (SYMMETRIC for all three): each peer POLLS every indexed instance's state
// field once per tick (NOT a UFunction observer -- the open/close/toggle verbs are
// BP-internal and bypass our ProcessEvent detour, IDA-proven 2026-06-04). On a
// delta it broadcasts the new state + the instance's cross-peer-stable Key; polling
// catches EVERY writer (player E-press, NPC-proximity auto-open, keypad-unlock,
// scripts), which a per-verb observer cannot. The host RELAYS a client-originated
// edge to the other clients. The receiver resolves the live instance by Key (self-
// healing index; deferred + retried if it has not streamed in yet) and idempotently
// applies, updating the poll baseline so it never echoes. On a new client's connect
// edge the HOST snapshots the FULL current state (open AND closed) of every indexed
// instance, so a joiner that loaded its own save converges to the host.
//
// RE: research/findings/votv-doors-and-lightswitches-RE-2026-05-25.md.

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

// Receiver entry: a DoorState / LightState / ContainerState packet arrived.
// `kind` is the ReliableKind (uint8). Routes to the matching channel, which
// applies on the game thread (deferring if the instance has not streamed in).
// Called from event_feed's reliable drain loop.
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
