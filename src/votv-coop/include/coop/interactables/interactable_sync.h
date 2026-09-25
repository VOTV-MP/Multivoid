// coop/interactables/interactable_sync.h -- keyed interactable open/close/on-off state sync. ONE
// replication engine drives seven features through a shared Channel, with no per-feature copy:
//   - DoorState (9):         base doors     (Adoor_C, open intent, host-authoritative)
//   - LightState (10):       light switches (Alightswitch_C::use, replayed on receipt)
//   - ContainerState (11):   container lids (Aprop_swinger_C::Open / Close)
//   - GarageDoorState (33):  the garage     (Agarage_C, keyed by level-export name)
//   - ApplianceState (35):   the save-actor appliance family
//   - LockerDoorState (50):  lockers and the drone-console box (level-export name)
//   - LightGroupState (129): light groups   (runTrigger on the root, host-authoritative)
// Gameplay/network layer (principle 7): the wire protocol, the senders, the receiver apply, the key
// index, the deferred-apply retry and the connect snapshot; the engine only through ue_wrap. A
// sender polls every indexed instance each tick -- on every peer for a symmetric channel, on the
// host for light groups -- except doors, which the host sends at their state verbs
// (coop/interactables/door_state_verbs). The model: docs/devices.md.

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

// Host: a door's state verb just ran on `door` (coop/interactables/door_state_verbs); the door lane
// sends the state it left when that changed. Game thread.
void OnDoorStateVerb(void* door);

// Whether the door lane's own apply is running on `door` now: a client's copy of a door refuses
// every doorOpen and doorClose but that one. Game thread.
bool ApplyingDoor(void* door);

// The appliance lane's key for `a`, or "" when the lane does not index it in the current world.
// Game thread.
std::wstring ApplianceKey(void* a);

// The light switch lane's key for `sw`, or "" when the lane does not index it in the current world.
// Game thread.
std::wstring LightSwitchKey(void* sw);

// The light group lane's key for `root`, or "" when the lane does not index it in the current world.
// Game thread.
std::wstring LightGroupKey(void* root);

// Host: a group's runTrigger just ran on `root` (coop/interactables/lightgroup_verbs); the group lane
// sends the state it left when that changed. Game thread.
void OnLightGroupVerb(void* root);

// Any peer: a switch's use() just ran on `sw` (coop/interactables/toggle_verbs); the switch lane
// sends the `a` it left when that changed, unless the lane's own apply ran it. Game thread.
void OnLightSwitchVerb(void* sw);

// Any peer: a garage's runTrigger just ran on `garage` (coop/interactables/toggle_verbs); the garage
// lane sends the Open it left when that changed. Game thread.
void OnGarageVerb(void* garage);

// Any peer: an appliance's action or its server box's visual just ran on `appliance`
// (coop/interactables/toggle_verbs); the appliance lane sends the bool it left when that changed.
// Game thread.
void OnApplianceVerb(void* appliance);

// Whether the light group lane's own apply is running on `root` now: a client's copy of a group
// refuses every runTrigger but that one. Game thread.
bool ApplyingLightGroup(void* root);

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
