// coop/interactables/interactable_sync.h -- keyed state sync for doors, switches, containers,
// swingers, garage doors, appliances, lockers, light groups, ovens and security cameras.
// Each channel sends state when its authoritative gameplay verb writes it; the host owns doors,
// light groups and cameras, while the peer that ran the verb owns the symmetric channels.
// The connect snapshot supplies states that came from a save rather than a verb. Engine access
// stays in ue_wrap; see docs/devices.md for the model.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap { struct FVector2D; }

namespace coop::net {
class Session;
struct KeyedTogglePayload;
struct SwingerMotionPayload;
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

// Host: one of a security camera's verbs just ran on `cam` (coop/interactables/toggle_verbs: its use, upd,
// updState, or the cursed camera's tick); both camera lanes send the bit they read when it changed. A client
// calls it too and it does nothing there. Game thread.
void OnCameraVerb(void* cam);

// The camera lane's key for `cam`, or "" when the lane does not index it in the current world: the name a
// camera toggle intent carries. Game thread.
std::wstring CameraKey(void* cam);

// The live camera the camera lane indexes under `key` in the current world, or null. Game thread.
void* ResolveCamera(const std::wstring& key);

// Whether the door lane's own apply is running on `door` now: a client's copy of a door refuses
// every doorOpen and doorClose but that one. Game thread.
bool ApplyingDoor(void* door);

// The appliance lane's key for `a`, or "" when the lane does not index it in the current world.
// Game thread.
std::wstring ApplianceKey(void* a);

// The garage, door-box, container and oven lanes' names for their devices, the same way: the index
// key, or empty when the lane does not index the device. Game thread.
std::wstring GarageKey(void* g);
std::wstring DoorBoxKey(void* box);
std::wstring ContainerKey(void* swinger);
std::wstring OvenKey(void* oven);

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

// Any peer: a locker's open or the drone console's action just ran on `box`
// (coop/interactables/toggle_verbs); the box lane sends the `opened` it left when that changed.
// Game thread.
void OnDoorBoxVerb(void* box);

// Any peer: a lid's open or close just ran on `swinger` (coop/interactables/toggle_verbs); the
// container lane sends the `opened` it left when that changed. Game thread.
void OnContainerVerb(void* swinger);
// A mouseAssign call just applied its torque input to a physical swinger hinge.
void OnContainerMotion(void* swinger, const ue_wrap::FVector2D& xy);
void OnContainerDrop(void* swinger);
void OnContainerMotion(const coop::net::SwingerMotionPayload& payload);

// Any peer: an oven's fix() just ran on `oven` (coop/interactables/toggle_verbs); the oven lane
// sends the `fixed` it left when that changed. Game thread.
void OnOvenVerb(void* oven);

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
