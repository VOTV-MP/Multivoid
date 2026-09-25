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
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"          // GetKeyString for swinger (it is an Aprop_C)
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/actors/swinger.h"

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
    // The host sends a door at its state verbs (coop/interactables/door_state_verbs).
    /*edgeFed*/ true,
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
    // The host sends a group at its runTrigger (coop/interactables/lightgroup_verbs).
    /*edgeFed*/ true,
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
// peers. The wall button toggles Open, which the poll catches.
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
// mesh, FX and audio repaint. Key is Aactor_save_C::Key. The switches and breakers that drive
// them just flip the bool, which the poll catches.
const Adapter g_applianceAdapter = {
    "appliance", coop::net::ReliableKind::ApplianceState,
    &ue_wrap::appliance::EnsureResolved,
    &ue_wrap::appliance::IsAppliance,
    &ue_wrap::appliance::GetKeyString,
    &ue_wrap::appliance::TryReadState,
    [](void* a, bool on) -> bool { return ue_wrap::appliance::ApplyState(a, on); },
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
};
Channel g_door{g_doorAdapter, Channel::Mode::HostAuth};  // doors auto-revert: host-authoritative
Channel g_light{g_lightAdapter};
// Host-authoritative: the client neither polls nor requests on this kind.
Channel g_lightGroup{g_lightGroupAdapter, Channel::Mode::HostAuth};
Channel g_container{g_containerAdapter};
Channel g_garage{g_garageAdapter};  // no auto-revert: symmetric
Channel g_appliance{g_applianceAdapter};  // no auto-revert: symmetric
Channel g_doorBox{g_doorBoxAdapter};  // no auto-revert: symmetric
// Keypads (ApasswordLock_C) are not a toggle (a typed buffer and three state bools, with an
// accept verb this engine cannot reach) and live in coop::keypad_sync; KeypadState routes there
// from event_feed, not through ChannelForKind.

Channel* ChannelForKind(coop::net::ReliableKind k) {
    switch (k) {
    case coop::net::ReliableKind::DoorState:      return &g_door;
    case coop::net::ReliableKind::LightState:     return &g_light;
    case coop::net::ReliableKind::LightGroupState:return &g_lightGroup;
    case coop::net::ReliableKind::ContainerState: return &g_container;
    case coop::net::ReliableKind::GarageDoorState:return &g_garage;
    case coop::net::ReliableKind::ApplianceState: return &g_appliance;
    case coop::net::ReliableKind::LockerDoorState:return &g_doorBox;
    default:                                      return nullptr;
    }
}

// A polled channel's sender is a per-tick poll of each state field (Channel::PollAndBroadcast),
// which catches every writer without watching each one; the door channel is sent at the door's own
// state verbs (coop/interactables/door_state_verbs) and the light group channel at its runTrigger
// (coop/interactables/lightgroup_verbs), each with the poll as its shadow probe.

// The receiver index: the channels register as scan-hub consumers, and the hub builds every index
// on its own sliced cadence.
void IndexChannels() {
    g_door.RegisterWithScanHub();
    g_light.RegisterWithScanHub();
    g_lightGroup.RegisterWithScanHub();
    g_container.RegisterWithScanHub();
    g_garage.RegisterWithScanHub();
    g_appliance.RegisterWithScanHub();
    g_doorBox.RegisterWithScanHub();
}

}  // namespace

void Install(coop::net::Session* session) {
    g_door.SetSession(session);
    g_light.SetSession(session);
    g_lightGroup.SetSession(session);
    g_container.SetSession(session);
    g_garage.SetSession(session);
    g_appliance.SetSession(session);
    g_doorBox.SetSession(session);
    IndexChannels();              // build the key->actor index (sender polls it; receiver resolves by it)
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

bool ApplyingLightGroup(void* root) { return root && root == g_applyingGroup; }

bool ApplyingDoor(void* door) { return door && door == g_applyingDoor; }

std::wstring ApplianceKey(void* a) { return g_appliance.KeyForActor(a); }

std::wstring LightSwitchKey(void* sw) { return g_light.KeyForActor(sw); }

void QueueConnectBroadcastForSlot(int peerSlot) {
    g_door.QueueConnectBroadcastForSlot(peerSlot);
    g_light.QueueConnectBroadcastForSlot(peerSlot);
    g_lightGroup.QueueConnectBroadcastForSlot(peerSlot);
    g_container.QueueConnectBroadcastForSlot(peerSlot);
    g_garage.QueueConnectBroadcastForSlot(peerSlot);
    g_appliance.QueueConnectBroadcastForSlot(peerSlot);
    g_doorBox.QueueConnectBroadcastForSlot(peerSlot);
}

void Tick() {
    g_door.Tick();
    g_light.Tick();
    g_lightGroup.Tick();
    g_container.Tick();
    g_garage.Tick();
    g_appliance.Tick();
    g_doorBox.Tick();
    ue_wrap::door_box::TickVerify();  // force-snap far-frozen locker/console swings
}

void OnDisconnect() {
    g_door.OnDisconnect();
    g_light.OnDisconnect();
    g_lightGroup.OnDisconnect();
    g_container.OnDisconnect();
    g_garage.OnDisconnect();
    g_appliance.OnDisconnect();
    g_doorBox.OnDisconnect();
    ue_wrap::door_box::OnDisconnect();  // drop the mid-swing verify entries
}

}  // namespace coop::interactable_sync
