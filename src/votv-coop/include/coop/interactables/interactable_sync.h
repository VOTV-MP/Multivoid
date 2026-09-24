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
// the engine only through ue_wrap. A channel's sender POLLS every indexed instance once per tick and
// broadcasts a delta under its cross-peer-stable key; a poll catches every writer without watching
// each. Symmetric channels poll on every peer. The two host-authoritative ones, doors and light
// groups, poll on the host alone, and a client renders them: its own player's use of a door reaches
// the host as a door verb intent (coop/interactables/door_verb_intent).

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct KeyedTogglePayload;
}  // namespace coop::net

namespace coop::interactable_sync {

// Resolve each channel's class + register its POST observers. Idempotent per
// channel; retried every net-pump tick until each BP class is loaded. Stores the
// session pointer (tracks reconnects). Game thread.
void Install(coop::net::Session* session);

// Receiver entry: a keyed state packet arrived. Resolves the live instance by key -- a
// self-healing index, deferred and retried when the instance has not streamed in yet -- and
// applies it, updating the poll baseline so the apply never echoes. The host's relay of a client's
// symmetric edge happens in the session, before this runs.
void OnReliable(uint8_t kind, const coop::net::KeyedTogglePayload& payload, uint8_t senderPeerSlot);

// The door lane's key for `door`, or "" when the lane does not index it in the current world: the
// name a door verb intent carries. Game thread.
std::wstring DoorKey(void* door);

// The live door the door lane indexes under `key` in the current world, or null. Game thread.
void* ResolveDoor(const std::wstring& key);

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
