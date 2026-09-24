// coop/interactables/interactable_sync.cpp -- see coop/interactables/interactable_sync.h. The
// per-feature adapters and the facade for the keyed-interactable sync (doors, light switches, light
// groups, container lids, the garage, appliances, lockers). The generic engine (the Adapter vtable
// and the Channel with its key index, deferred apply, echo suppression and connect snapshot) is
// coop/interactables/interactable_channel.h; this TU holds one adapter per feature, the
// kind-to-channel router, a client's switch-press observers and the Install, Tick and event facade.

#include "coop/interactables/interactable_sync.h"
#include "coop/interactables/interactable_channel.h"  // the generic engine: Adapter and Channel

#include "ue_wrap/devices/appliance.h"     // the six-class save-actor toggle family
#include "ue_wrap/devices/door.h"
#include "ue_wrap/devices/door_box.h"      // lockers and the drone-console box
#include "ue_wrap/engine/engine.h"        // ReadMainPlayerLookAtActor (the E-press door target)
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/devices/garage.h"
#include "ue_wrap/devices/lightswitch.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"          // GetKeyString for swinger (it is an Aprop_C)
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"   // MainPlayerClass + the InpActEvt_use input-action fn
#include "ue_wrap/actors/swinger.h"

#include <chrono>
#include <string>
#include <unordered_map>

namespace coop::interactable_sync {
namespace {

namespace GT = ue_wrap::game_thread;
namespace P = ue_wrap::profile;
// The reflection alias, ProbeLog, the WireKey conversions, the constants, Adapter and Channel are
// in scope from coop/interactables/interactable_channel.h, included inside this namespace.

// The adapters, ahead of the channels. ApplySwitchPresentation replays a switch press for its
// visual half; defined below (it needs the channels for the role read).
bool ApplySwitchPresentation(void* sw);

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
    [](void* a, bool on) -> bool { ue_wrap::door::SmartApply(a, on); return true; },
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
    // only: ApplySwitchPresentation shuts the group's gate for the duration of the call, so use()'s
    // runTrigger cannot move isActive, or two lanes would write the same press with no defined
    // order inside a game-thread batch. On the host the full use() runs: replaying the client's
    // switch edge is how its press becomes an authoritative group change.
    [](void* a, bool /*on*/) -> bool { return ApplySwitchPresentation(a); },
};
// The light group (Atrigger_lightRoot_C), the state a player sees. The switch adapter syncs the
// switch's `a`, this one the group's isActive: the game keeps them decoupled (use() toggles `a`
// unconditionally but reaches the lamps only through runTrigger(root, 0), which opens with a
// gate on `active`), and the group's state is what the save persists. Host-authoritative:
// thirteen blueprints can move a group and most are host-owned world systems, so a symmetric
// channel would let a client's local flickerer author the host's lights. The client is
// receive-only with no request: its press reaches the host on the LightState lane, and the
// host's own use() produces the group change. The apply is runTrigger(root, on ? 1 : 2), the
// game's own absolute ungated setters, both of which repaint every lamp; not index 0 (a gated
// toggle would re-introduce the decoupling) and not setActive (it writes the gate and moves no
// lamp).
const Adapter g_lightGroupAdapter = {
    "lightgroup", coop::net::ReliableKind::LightGroupState,
    &ue_wrap::lightswitch::EnsureResolved,
    &ue_wrap::lightswitch::IsLightRoot,
    &ue_wrap::lightswitch::GetKeyString,
    &ue_wrap::lightswitch::TryReadActive,
    [](void* a, bool on) -> bool {
        // A no-op apply is skipped: the client's own writers of isActive are gate-suppressed, so a
        // match is a match, and one connect snapshot was otherwise 42 runTrigger calls in one frame,
        // each repainting a whole group, for no state change.
        bool cur = false;
        if (ue_wrap::lightswitch::TryReadActive(a, cur) && cur == on) return true;
        return ue_wrap::lightswitch::ApplyGroupState(a, on);
    },
    // A client's native press is neutralised for one input dispatch by the E-press PRE and POST pair
    // below, never by a gate left standing (a gate left shut is a player whose switches quietly
    // stopped working). No TickApply; the poll is the sender.
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

// A switch press replayed for its visual half. use() fires the group trigger, toggles `a`, and
// repaints the mesh with the click; on a client the last two are wanted and the first is not,
// since the group is host-owned. Rather than hand-copy use()'s presentation half (a copy of a
// BP body drifts), the lever the BP already gates on is pulled: the group's `active` is shut for
// the duration of the call, the door lane's PRE/POST idiom. The gate is always restored to the
// value it had, never to true: a shut gate here means the lights breaker is off, as legitimate
// as a keypad-locked door.
bool ApplySwitchPresentation(void* sw) {
    auto* s = g_light.GetSession();
    const bool isClient = s && s->connected() && s->role() == coop::net::Role::Client;
    if (!isClient) return ue_wrap::lightswitch::CallUse(sw);  // HOST: the full press, group and all

    void* const root = ue_wrap::lightswitch::ResolveSwitchRoot(sw);
    if (!root) return ue_wrap::lightswitch::CallUse(sw);  // no group reachable -> nothing to gate

    // RAII, not a straight-line restore: CallUse goes through ProcessEvent, whose fault is caught
    // at the detour boundary and unwinds past a manual restore, and `active` is save-persistent, so
    // a leaked shut gate would follow the player into single-player with every switch in the group
    // dead. /EHa runs this destructor on that unwind.
    ue_wrap::lightswitch::ScopedGroupGateShut hold(root);
    if (!hold.shut()) {
        // The guard is not in force (an unresolved offset, or the write did not take), said once:
        // it fails open, and use() will also move isActive, which the host owns.
        static bool s_warned = false;
        if (!s_warned) { s_warned = true;
            UE_LOGW("light: group gate unavailable -- a client's switch replay will also move "
                    "isActive, which the host owns. The group lane still corrects it, but the "
                    "press double-moves visibly."); }
    }
    return ue_wrap::lightswitch::CallUse(sw);
}

// A polled channel's sender is a per-tick poll of each state field (Channel::PollAndBroadcast),
// which catches every writer without watching each one; the door channel is sent at the door's own
// state verbs (coop/interactables/door_state_verbs), with the poll as its shadow probe.

// A client's light-switch press. The native use() ends in runTrigger(root, 0), which would move a
// group the host owns, so the PRE observer on AmainPlayer_C::InpActEvt_use shuts the group's
// `active` for the body of the dispatch and the press stays presentation-only; the lights move when
// the host's LightGroupState lands. Client only; puppets process no input.
bool g_useInputObserverInstalled = false;
void* g_useInputGateCleared = nullptr;
bool  g_useInputGatePrior   = true;

// Puts back a gate the PRE observer shut; called first from both observers, so a dispatch whose
// BP body faulted (no POST) self-heals on the next press instead of leaving a group deaf to its
// switch.
void RestoreLightGateIfCleared() {
    if (!g_useInputGateCleared) return;
    ue_wrap::lightswitch::SetGroupGate(g_useInputGateCleared, g_useInputGatePrior);
    g_useInputGateCleared = nullptr;
}

void OnUseInputPre(void* self, void*, void*) {
    RestoreLightGateIfCleared();
    if (!self) return;
    auto* s = g_light.GetSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return;  // CLIENT-only
    void* const aimed = ue_wrap::engine::ReadMainPlayerLookAtActor(self);
    if (!aimed) return;
    if (!ue_wrap::lightswitch::EnsureSwitchResolved() || !ue_wrap::lightswitch::IsLightSwitch(aimed)) return;
    void* const root = ue_wrap::lightswitch::ResolveSwitchRoot(aimed);
    if (!root) return;                                   // no group reachable: native behaviour stays
    const std::wstring gk = ue_wrap::lightswitch::GetKeyString(root);
    if (gk.empty() || gk == L"None") return;             // unkeyed group: no lane owns it, leave it alone
    if (!ue_wrap::lightswitch::GroupGateAvailable()) return;  // cannot gate -> do not pretend we did
    g_useInputGatePrior   = ue_wrap::lightswitch::GetGroupGate(root);
    ue_wrap::lightswitch::SetGroupGate(root, false);
    if (ue_wrap::lightswitch::GetGroupGate(root)) return;     // the write did not take; record nothing
    g_useInputGateCleared = root;
}

void OnUseInput(void*, void*, void*) {
    // The native chain already ran, gated shut; the gate goes back.
    RestoreLightGateIfCleared();
}

void InstallUseInputObserver() {
    if (g_useInputObserverInstalled) return;
    void* playerCls = R::FindClass(P::name::MainPlayerClass);
    if (!playerCls) return;  // retry until mainPlayer_C loads
    void* fn = R::FindFunction(playerCls, P::name::MainPlayerUseInputEventFn);
    if (!fn) {
        UE_LOGW("light: InpActEvt_use UFunction not found -- a client's switch press will move the "
                "group locally too");
        g_useInputObserverInstalled = true;  // no retry
        return;
    }
    if (!GT::RegisterPreObserver(fn, &OnUseInputPre)) {
        UE_LOGW("light: InpActEvt_use PRE observer register failed");
        return;
    }
    if (!GT::RegisterPostObserver(fn, &OnUseInput)) {
        UE_LOGW("light: InpActEvt_use observer register failed");
        return;
    }
    g_useInputObserverInstalled = true;
    UE_LOGI("light: InpActEvt_use PRE+POST observers installed (PRE shuts a pressed switch's group gate; "
            "POST restores it)");
}

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
    InstallUseInputObserver();   // a client's switch press: its group's gate shut for the press
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
    // Any gate an E-press shut and never restored (a faulted BP body skips the POST) goes back, and
    // the pointer is forgotten: it would otherwise hold an actor of a torn-down world, and the next
    // press's restore would write a byte into whatever owns the address. SetGroupGate re-checks
    // liveness.
    RestoreLightGateIfCleared();
}

}  // namespace coop::interactable_sync
