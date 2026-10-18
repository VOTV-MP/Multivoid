// coop/interactables/interactable_sync.cpp -- see coop/interactables/interactable_sync.h. The
// per-feature adapters and the facade for the keyed-interactable sync (doors, light switches, light
// groups, container lids, the garage, appliances, lockers). The generic engine (the Adapter vtable
// and the Channel with its key index, deferred apply, echo suppression and connect snapshot) is
// coop/interactables/interactable_channel.h; this TU holds one adapter per feature, the
// kind-to-channel router and the Install, Tick and event facade.

#include "coop/interactables/interactable_sync.h"
#include "coop/interactables/interactable_channel.h"  // the generic engine: Adapter and Channel

#include "ue_wrap/devices/appliance.h"     // the six-class save-actor toggle family
#include "ue_wrap/devices/door.h"
#include "ue_wrap/devices/door_box.h"      // lockers and the drone-console box
#include "ue_wrap/devices/garage.h"
#include "ue_wrap/devices/lightswitch.h"
#include "ue_wrap/devices/security_camera.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"          // GetKeyString for swinger (it is an Aprop_C)
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/actors/swinger.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_physics.h"

#include <chrono>
#include <string>
#include <unordered_map>

namespace coop::interactable_sync {
namespace {

// The reflection alias, ProbeLog, the WireKey conversions, the constants, Adapter and Channel are
// in scope from coop/interactables/interactable_channel.h, included inside this namespace.

// The door and light group lanes' own applies, while they run: a client refuses every state verb on
// a door and every runTrigger on a group but these (coop/interactables/door_state_verbs,
// lightgroup_verbs). Game-thread serial.
void* g_applyingDoor = nullptr;
void* g_applyingGroup = nullptr;
void* g_applyingContainerMotion = nullptr;

struct ApplyMark {
    void*& slot;
    void* outer;
    ApplyMark(void*& s, void* actor) : slot(s), outer(s) { slot = actor; }
    ~ApplyMark() { slot = outer; }
};

const Adapter g_doorAdapter = {
    "door", coop::net::ReliableKind::DoorState,
    &ue_wrap::door::EnsureResolved,
    &ue_wrap::door::IsDoor,
    &ue_wrap::door::GetKeyString,
    // The intent reader, not isOpened: the host broadcasts a door at swing start, so a host-opened
    // door mirrors at once instead of lagging the swing.
    &ue_wrap::door::TryReadOpenIntent,
    // The receiver apply is the door's own swing wherever this peer ticks the door, and a
    // force-snap where the swing froze out of tick range (SmartApply and its verify).
    [](void* a, bool on) -> bool {
        ApplyMark mark(g_applyingDoor, a);
        ue_wrap::door::SmartApply(a, on);
        return true;
    },
    // The apply is a swing that needs finishing.
    &ue_wrap::door::TickSmartApply,
    nullptr,
    // The dev probe says a door's swing fields beside a SHADOW MISS.
    &ue_wrap::door::DescribeSwing,
};
const Adapter g_lightAdapter = {
    // Keyed on the switch, so the receiver replays use() and the switch flips visibly with its
    // click in one BP call. use() toggles, and it ends in an unconditional negation of `a`, so
    // "apply when cur != want" is an absolute set for this bool and a double delivery is safe. What
    // this lane does not cover: `a` is the switch's presentation bit, not save-persistent; the
    // state a player sees is the group's isActive, which use() reaches through runTrigger(self, 0),
    // gated on the root's `active`, so on a peer whose gate is shut the switch flips and no light
    // moves while `a` still agrees cross-peer. Thirteen blueprints can move a group (powerControl,
    // mainGamemode, the flickerer, the cheat menu, every eventer and a lightRoot is a trigger), so
    // the group is synced source-agnostically by the adapter below; ini lightgroup_census=1
    // measures it.
    "light", coop::net::ReliableKind::LightState,
    &ue_wrap::lightswitch::EnsureSwitchResolved,
    &ue_wrap::lightswitch::IsLightSwitch,
    &ue_wrap::lightswitch::GetSwitchKeyString,
    &ue_wrap::lightswitch::TryReadSwitchA,
    // The receiver replays use() so the switch flips and clicks. On a client that is presentation
    // only: the runTrigger use() ends in is refused there, since the group is the host's to move
    // (coop/interactables/lightgroup_verbs). On the host the full use() runs: replaying the client's
    // switch edge is how its press becomes an authoritative group change.
    [](void* a, bool /*on*/) -> bool { return ue_wrap::lightswitch::CallUse(a); },
};
// The light group (Atrigger_lightRoot_C), the state a player sees. The switch adapter syncs the
// switch's `a`, this one the group's isActive: the game keeps them decoupled (use() toggles `a`
// unconditionally but reaches the lamps only through runTrigger(root, 0), which opens with a
// gate on `active`), and the group's state is what the save persists. Host-authoritative:
// thirteen blueprints can move a group and most are host-owned world systems, so a symmetric
// channel would let a client's local flickerer author the host's lights. The client is
// receive-only with no request: its press reaches the host on the LightState lane, and the
// host's own use() produces the group change, which the host sends at the group's runTrigger. The
// apply is runTrigger(root, on ? 1 : 2), the game's own absolute ungated setters, both of which
// repaint every lamp; not index 0 (a gated toggle would re-introduce the decoupling) and not
// setActive (it writes the gate and moves no lamp).
const Adapter g_lightGroupAdapter = {
    "lightgroup", coop::net::ReliableKind::LightGroupState,
    &ue_wrap::lightswitch::EnsureResolved,
    &ue_wrap::lightswitch::IsLightRoot,
    &ue_wrap::lightswitch::GetKeyString,
    &ue_wrap::lightswitch::TryReadActive,
    [](void* a, bool on) -> bool {
        // A no-op apply is skipped: a client's own writers of isActive are refused, so a match is a
        // match, and one connect snapshot was otherwise 42 runTrigger calls in one frame, each
        // repainting a whole group, for no state change.
        bool cur = false;
        if (ue_wrap::lightswitch::TryReadActive(a, cur) && cur == on) return true;
        ApplyMark mark(g_applyingGroup, a);
        return ue_wrap::lightswitch::ApplyGroupState(a, on);
    },
    nullptr,  // no TickApply: runTrigger lands the state in its own body
};
const Adapter g_containerAdapter = {
    "container", coop::net::ReliableKind::ContainerState,
    &ue_wrap::swinger::EnsureResolved,
    &ue_wrap::swinger::IsSwinger,
    &ue_wrap::prop::GetKeyString,  // a swinger is an Aprop_C
    &ue_wrap::swinger::TryReadOpen,
    [](void* a, bool on) -> bool { return on ? ue_wrap::swinger::CallOpen(a, false) : ue_wrap::swinger::CallClose(a); },
};
// The garage door (Agarage_C), symmetric: no sensor and no autoclose, so a symmetric poll never
// oscillates. Its identity is the level-export FName, not the save key: a garage that misses
// the gamemode's sublevel-gated keying pass keeps the class default "garageDoor", which every
// garage instance shares, and the host was seen losing its garage identity through a
// menu-to-save reload while the FName came through the same reload byte-identical on both
// peers. Its Open moves only in its runTrigger (the wall button's call), past its load.
const Adapter g_garageAdapter = {
    "garage", coop::net::ReliableKind::GarageDoorState,
    &ue_wrap::garage::EnsureResolved,
    &ue_wrap::garage::IsGarage,
    &ue_wrap::garage::GetNameKey,
    &ue_wrap::garage::TryReadOpen,
    [](void* a, bool on) -> bool { return ue_wrap::garage::ApplyOpen(a, on); },
};
// The appliance family (six Aactor_save_C descendants: faucet, sink, shower, kitchen oven,
// serverBox, wall-unit tapes), symmetric single-bool toggles with no auto-revert. One adapter:
// ue_wrap::appliance dispatches by class to the right bool offset and refresh verb, so the peer's
// mesh, FX and audio repaint. Key is Aactor_save_C::Key. Each class's own verb writes its bool (its
// actionOptionIndex, a server box's visual), where the peer that ran it sends it.
const Adapter g_applianceAdapter = {
    "appliance", coop::net::ReliableKind::ApplianceState,
    &ue_wrap::appliance::EnsureResolved,
    &ue_wrap::appliance::IsAppliance,
    &ue_wrap::appliance::GetKeyString,
    &ue_wrap::appliance::TryReadState,
    [](void* a, bool on) -> bool { return ue_wrap::appliance::ApplyState(a, on); },
    nullptr,
    &ue_wrap::appliance::ResolvedClassCount,
};
// The kitchen oven's repair (kitchen_C's `fixed`), symmetric and one way: fix() sets it and nothing
// sets it back, so the peer that repaired sends it; a receiver writes it and repaints on 1, fix()'s
// work without its screen (appliance::WriteOvenFixed), and refuses 0 (the channel reads the device
// after an apply, so a refusal is said and is no delta). Keyed as the appliance family keys the
// oven, by Aactor_save_C::Key.
const Adapter g_ovenAdapter = {
    "oven", coop::net::ReliableKind::OvenRepairState,
    &ue_wrap::appliance::EnsureResolved,
    &ue_wrap::appliance::IsOven,
    &ue_wrap::appliance::GetKeyString,
    &ue_wrap::appliance::TryReadOvenFixed,
    [](void* a, bool on) -> bool { return on && ue_wrap::appliance::WriteOvenFixed(a, true); },
    nullptr,
    &ue_wrap::appliance::ResolvedClassCount,
};
// The hinged-door boxes: the lockers (locker_C and its two subclasses) and the drone-console box.
// Symmetric: nothing auto-reverts `opened` but the player toggle and the locker's own open().
// Identity is the level-export FName (neither class has a save Key; placed-actor names are
// deterministic cross-peer). The apply is the native verb or a write plus refresh per class, with
// the verify-and-force-snap inside the wrapper (the swing timeline freezes outside tick range).
const Adapter g_doorBoxAdapter = {
    "doorbox", coop::net::ReliableKind::LockerDoorState,
    &ue_wrap::door_box::EnsureResolved,
    &ue_wrap::door_box::IsDoorBox,
    &ue_wrap::door_box::GetNameKey,
    &ue_wrap::door_box::TryReadOpened,
    [](void* a, bool on) -> bool { return ue_wrap::door_box::ApplyOpened(a, on); },
    nullptr,
    &ue_wrap::door_box::ResolvedFamilyCount,
};
// A security camera's two saved bits, one adapter each because the channel carries a bool. Host-authoritative:
// four writers beside the player's use move `active` (a wisp, the gray events, the cursed camera's night roll,
// a save's load) and most are host-owned world systems, so a symmetric lane would let a client's copy of one
// author the host's camera. MTA takes an unoccupied vehicle's engine bit from its client sync owner, which is
// not taken here (reference/mtasa-blue/Server/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp:442-447); the
// shape taken is a server-held bit carried to joiners in the add packet
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:7260-7274 and
// reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CEntityAddPacket.cpp:571). The client renders the
// host's bit and asks for a change as a CameraToggleIntent (coop/interactables/camera_toggle_intent). Keyed by the
// lane's key for the camera (its portable identity, else its prop key), which ue_wrap::security_camera withholds
// from a camera that carries its class's default. The `active` apply writes the bit, then updState() when the
// write turns the camera on (the click the toggling peer heard), else upd(); a cursed camera's tick never clicks.
// Either apply ends by repainting a laptop camera panel that has the camera selected.
bool ApplyCameraActive(void* cam, bool on) {
    namespace sc = ue_wrap::security_camera;
    bool cur = false;
    if (!sc::ReadActive(cam, cur) || !sc::WriteActive(cam, on)) return false;
    const bool ok = (!cur && on && !sc::IsCursed(cam)) ? sc::CallUpdState(cam) : sc::CallUpd(cam);
    sc::RefreshLaptopsShowing(cam);
    return ok;
}

bool ApplyCameraLight(void* cam, bool on) {
    namespace sc = ue_wrap::security_camera;
    if (!sc::WriteLight(cam, on)) return false;
    const bool ok = sc::CallUpd(cam);
    sc::RefreshLaptopsShowing(cam);
    return ok;
}

const Adapter g_cameraActiveAdapter = {
    "camera_active", coop::net::ReliableKind::CameraActiveState,
    &ue_wrap::security_camera::EnsureResolved,
    &ue_wrap::security_camera::IsCameraClass,
    &ue_wrap::security_camera::KeyOf,
    &ue_wrap::security_camera::ReadActive,
    &ApplyCameraActive,
};
const Adapter g_cameraLightAdapter = {
    "camera_light", coop::net::ReliableKind::CameraLightState,
    &ue_wrap::security_camera::EnsureResolved,
    &ue_wrap::security_camera::IsCameraClass,
    &ue_wrap::security_camera::KeyOf,
    &ue_wrap::security_camera::ReadLight,
    &ApplyCameraLight,
};
Channel g_door{g_doorAdapter, Channel::Mode::HostAuth};  // doors auto-revert: host-authoritative
Channel g_light{g_lightAdapter};
// Host-authoritative: the client neither polls nor requests on this kind.
Channel g_lightGroup{g_lightGroupAdapter, Channel::Mode::HostAuth};
Channel g_container{g_containerAdapter};
Channel g_garage{g_garageAdapter};  // no auto-revert: symmetric
Channel g_appliance{g_applianceAdapter};  // no auto-revert: symmetric
Channel g_doorBox{g_doorBoxAdapter};  // no auto-revert: symmetric
Channel g_oven{g_ovenAdapter};  // one way: whoever repairs it
Channel g_cameraActive{g_cameraActiveAdapter, Channel::Mode::HostAuth};
Channel g_cameraLight{g_cameraLightAdapter, Channel::Mode::HostAuth};
// Keypads (ApasswordLock_C) are not a toggle (a typed buffer and three state bools, with an
// accept verb this engine cannot reach) and live in coop::keypad_sync; KeypadState routes there
// from event_feed, not through ChannelForKind.

// Every channel, once: each list below walks this one, in this order.
Channel* const kChannels[] = {&g_door, &g_light, &g_lightGroup, &g_container, &g_garage, &g_appliance, &g_doorBox,
                              &g_oven, &g_cameraActive, &g_cameraLight};

Channel* ChannelForKind(coop::net::ReliableKind k) {
    for (Channel* ch : kChannels)
        if (ch->Kind() == k) return ch;
    return nullptr;
}

// Each channel is sent at the verbs that write its state (door_state_verbs, lightgroup_verbs,
// toggle_verbs). A write no verb makes, a save's load, reaches a joiner in the connect snapshot; the
// dev probe (channel_shadow_probe) polls for one and says it, and sends nothing.

// The receiver index: the channels register as scan-hub consumers, and the hub builds every index
// on its own sliced cadence.
void IndexChannels() {
    for (Channel* ch : kChannels) ch->RegisterWithScanHub();
}

}  // namespace

void Install(coop::net::Session* session) {
    for (Channel* ch : kChannels) ch->SetSession(session);
    IndexChannels();              // build the key->actor index (edges and the probe name by it; receivers resolve by it)
}

void OnReliable(uint8_t kind, const coop::net::KeyedTogglePayload& payload, uint8_t senderPeerSlot) {
    if (Channel* ch = ChannelForKind(static_cast<coop::net::ReliableKind>(kind)))
        ch->OnReliable(payload, senderPeerSlot);
}

std::wstring DoorKey(void* door) { return g_door.KeyForActor(door); }

void* ResolveDoor(const std::wstring& key) {
    if (key.empty() || !ue_wrap::door::EnsureResolved()) return nullptr;
    return g_door.ActorForKey(key);
}

void OnDoorStateVerb(void* door) { g_door.OnLocalEdge(door); }

std::wstring LightGroupKey(void* root) { return g_lightGroup.KeyForActor(root); }

void OnLightGroupVerb(void* root) { g_lightGroup.OnLocalEdge(root); }

void OnLightSwitchVerb(void* sw) { g_light.OnLocalEdge(sw); }

void OnGarageVerb(void* garage) { g_garage.OnLocalEdge(garage); }

void OnApplianceVerb(void* appliance) { g_appliance.OnLocalEdge(appliance); }

void OnDoorBoxVerb(void* box) { g_doorBox.OnLocalEdge(box); }

void OnContainerVerb(void* swinger) { g_container.OnLocalEdge(swinger); }

void OnContainerMotion(void* swinger, const ue_wrap::FVector2D& xy) {
    if (!swinger || swinger == g_applyingContainerMotion) return;
    auto* s = g_container.GetSession();
    if (!s || !s->connected()) return;
    const std::wstring key = g_container.KeyForActor(swinger);
    if (key.empty()) return;
    coop::net::SwingerMotionPayload p{};
    WireKeyFromString(key, p.key);
    p.event = 0;
    p.mouseX = xy.X;
    p.mouseY = xy.Y;
    s->SendReliable(coop::net::ReliableKind::SwingerMotion, &p, sizeof(p));
}

void OnContainerDrop(void* swinger) {
    if (!swinger || swinger == g_applyingContainerMotion) return;
    OnContainerVerb(swinger);  // the release may have crossed the game's opened threshold
    auto* s = g_container.GetSession();
    if (!s || !s->connected()) return;
    const std::wstring key = g_container.KeyForActor(swinger);
    void* mesh = ue_wrap::prop::GetStaticMesh(swinger);
    if (key.empty() || !mesh) return;
    const ue_wrap::FRotator rotation = ue_wrap::engine::GetComponentWorldRotation(mesh);
    const ue_wrap::prop::VelocityState velocity = ue_wrap::prop::GetPhysicsVelocity(swinger);
    if (!velocity.ok) return;
    coop::net::SwingerMotionPayload p{};
    WireKeyFromString(key, p.key);
    p.event = 1;
    p.pitch = rotation.Pitch;
    p.yaw = rotation.Yaw;
    p.roll = rotation.Roll;
    p.angularX = velocity.angularDegS.X;
    p.angularY = velocity.angularDegS.Y;
    p.angularZ = velocity.angularDegS.Z;
    p.linearX = velocity.linearCmS.X;
    p.linearY = velocity.linearCmS.Y;
    p.linearZ = velocity.linearCmS.Z;
    s->SendReliable(coop::net::ReliableKind::SwingerMotion, &p, sizeof(p));
}

void OnContainerMotion(const coop::net::SwingerMotionPayload& payload) {
    const std::wstring key = StringFromWireKey(payload.key);
    if (key.empty() || !ue_wrap::swinger::EnsureResolved()) return;
    void* actor = g_container.ActorForKey(key);
    if (!actor) return;
    ApplyMark mark(g_applyingContainerMotion, actor);
    if (payload.event == 0) {
        ue_wrap::swinger::CallMouseAssign(actor, ue_wrap::FVector2D{payload.mouseX, payload.mouseY});
        return;
    }
    if (payload.event != 1) return;
    ue_wrap::swinger::CallDropped(actor);
    void* mesh = ue_wrap::prop::GetStaticMesh(actor);
    if (!mesh) return;
    ue_wrap::engine::SetComponentWorldRotation(mesh, ue_wrap::FRotator{payload.pitch, payload.yaw, payload.roll});
    ue_wrap::engine::SetComponentSimulatePhysics(mesh, true);
    ue_wrap::engine::SetComponentLinearVelocity(mesh, payload.linearX, payload.linearY, payload.linearZ);
    ue_wrap::engine::SetComponentAngularVelocity(mesh, payload.angularX, payload.angularY, payload.angularZ);
}

void OnOvenVerb(void* oven) { g_oven.OnLocalEdge(oven); }

void OnCameraVerb(void* cam) {
    g_cameraActive.OnLocalEdge(cam);
    g_cameraLight.OnLocalEdge(cam);
}

std::wstring CameraKey(void* cam) { return g_cameraActive.KeyForActor(cam); }

void* ResolveCamera(const std::wstring& key) {
    if (key.empty() || !ue_wrap::security_camera::EnsureResolved()) return nullptr;
    return g_cameraActive.ActorForKey(key);
}

bool ApplyingLightGroup(void* root) { return root && root == g_applyingGroup; }

bool ApplyingDoor(void* door) { return door && door == g_applyingDoor; }

std::wstring ApplianceKey(void* a) { return g_appliance.KeyForActor(a); }
std::wstring GarageKey(void* g) { return g_garage.KeyForActor(g); }
std::wstring DoorBoxKey(void* box) { return g_doorBox.KeyForActor(box); }
std::wstring ContainerKey(void* swinger) { return g_container.KeyForActor(swinger); }
std::wstring OvenKey(void* oven) { return g_oven.KeyForActor(oven); }

std::wstring LightSwitchKey(void* sw) { return g_light.KeyForActor(sw); }

void QueueConnectBroadcastForSlot(int peerSlot) {
    for (Channel* ch : kChannels) ch->QueueConnectBroadcastForSlot(peerSlot);
}

void Tick() {
    for (Channel* ch : kChannels) ch->Tick();
    ue_wrap::door_box::TickVerify();  // force-snap far-frozen locker/console swings
}

void OnDisconnect() {
    for (Channel* ch : kChannels) ch->OnDisconnect();
    ue_wrap::door_box::OnDisconnect();  // drop the mid-swing verify entries
}

}  // namespace coop::interactable_sync
