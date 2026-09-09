// coop/interactables/interactable_sync.cpp -- see coop/interactables/interactable_sync.h. The
// per-feature adapters and the facade for the keyed-interactable sync (doors, light switches, light
// groups, container lids, the garage, appliances, lockers). The generic engine (the Adapter vtable
// and the Channel with its key index, deferred apply, echo suppression, connect snapshot and hold
// register) is coop/interactables/interactable_channel.h; this TU holds one adapter per feature,
// the kind-to-channel router, the client E-press observers and the Install, Tick and event facade.

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
    // The receiver apply force-snaps rather than calling doorOpen or doorClose: the open is a
    // tick-gated animation that freezes when this peer's player is far from the door (isOpened
    // never sets), and the force verbs complete the state through the timeline regardless of
    // proximity. Near the player it snaps rather than animates.
    [](void* a, bool on) -> bool { ue_wrap::door::SmartApply(a, on); return true; },
    // The HostAuth hooks, doors only.
    &ue_wrap::door::SuppressClientAutonomy,
    &ue_wrap::door::RestoreClientAutonomy,
    // The host applies a client request by force-snap too: with only the client's puppet at the
    // door, doorOpen is denied without a local interactor and freezes with the host player far. The
    // client enforced the lock locally, so its validated edge is trusted.
    [](void* a, bool on) -> bool { ue_wrap::door::SmartApply(a, on); return true; },
    // The host's own autoclose is muted while a client holds the door open; restored and closed on
    // release.
    &ue_wrap::door::SuppressHostHeldDoor,
    &ue_wrap::door::ReleaseHostHeldDoor,
    // The open gate: the host applies a client's open only if the door's own logic would (power on,
    // not jammed, not superClosed), so coop never opens a door single-player keeps shut.
    &ue_wrap::door::CanOpen,
    // The two facets only doors want: their autoclose fights an applied state, so a client open is
    // a hold; and their apply is an animation that needs finishing.
    /*holdRegister*/ true,
    /*TickApply*/    &ue_wrap::door::TickSmartApply,
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
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,  // symmetric: no HostAuth hooks
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
        // A no-op apply is skipped. HostAuth applies unconditionally in general, since on a door a
        // live-field match is evidence of the client's own native press racing the echo; here the
        // client's own writers of isActive are gate-suppressed, so a match is a match, and one
        // connect snapshot was otherwise 42 runTrigger calls in one frame, each repainting a whole
        // group, for no state change.
        bool cur = false;
        if (ue_wrap::lightswitch::TryReadActive(a, cur) && cur == on) return true;
        return ue_wrap::lightswitch::ApplyGroupState(a, on);
    },
    // No autonomy-suppression hooks: a client's native press is neutralised for one input dispatch
    // by the E-press PRE and POST pair below, never by a gate left standing (a gate left shut is a
    // player whose switches quietly stopped working). No request, no hold register, no TickApply.
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};
const Adapter g_containerAdapter = {
    "container", coop::net::ReliableKind::ContainerState,
    &ue_wrap::swinger::EnsureResolved,
    &ue_wrap::swinger::IsSwinger,
    &ue_wrap::prop::GetKeyString,  // a swinger is an Aprop_C
    &ue_wrap::swinger::TryReadOpen,
    [](void* a, bool on) -> bool { return on ? ue_wrap::swinger::CallOpen(a, false) : ue_wrap::swinger::CallClose(a); },
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,  // symmetric: no HostAuth hooks
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
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,  // symmetric: no HostAuth hooks
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
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,  // symmetric: no HostAuth hooks
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
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,  // symmetric: no HostAuth hooks
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

// The sender is per-tick state polling (Channel::PollAndBroadcast), not a UFunction observer: the
// per-verb edges (doorOpen, the root's SetActive, the swinger's Open, the switch's use) dispatch
// through ProcessInternal and bypass the ProcessEvent detour, so a POST observer never fires.
// Polling the state field catches every writer (a press, an NPC auto-open, a keypad unlock, a
// script) uniformly.

// The client's door request on an E-press: the door's own verbs are BP-internal, so the one
// observable use edge is AmainPlayer_C::InpActEvt_use on the local player. The POST observer
// reads the actor the player aimed at and, for a door the lane indexes, sends a toggle request;
// the host applies it with its real guards and broadcasts the authoritative state. Client only;
// puppets process no input.
bool g_useInputObserverInstalled = false;

// The door whose Active gate the PRE observer cleared for the current dispatch, with the real
// prior value the POST observer restores (one slot: PRE and POST are one game-thread dispatch
// apart). The client's native press chain is BP-internal and toggled the local door in parallel
// with the host request, and the host's echo was then swallowed as "already in that state";
// clearing Active, the BP's own CanOpen gate, for the body of the dispatch makes the native chain
// a no-op, so the client door moves only on host echoes (MTA's non-authority never advances state
// from its own simulation). The restore writes the saved value, never true: a keypad-locked door
// has Active false, and restoring true re-powered the lock client-side.
void* g_useInputActiveCleared = nullptr;
bool  g_useInputActivePrior  = true;

// The same lever for a light switch: the native use() ends in runTrigger(root, 0), which would
// move a group the host owns, so the group's `active` is shut for the body of the dispatch and
// the press stays presentation-only; the lights move when the host's LightGroupState lands.
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
    // A leaked door first: if the prior dispatch's BP body faulted, the POST never ran and the door
    // would stay Active false forever.
    if (g_useInputActiveCleared) {
        ue_wrap::door::SetActive(g_useInputActiveCleared, g_useInputActivePrior);
        g_useInputActiveCleared = nullptr;
    }
    RestoreLightGateIfCleared();
    if (!self) return;
    auto* s = g_door.GetSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return;  // CLIENT-only
    void* const aimed = ue_wrap::engine::ReadMainPlayerLookAtActor(self);
    if (!aimed) return;

    // A light switch: the group's gate shut, so the native use() is presentation-only.
    if (ue_wrap::lightswitch::EnsureSwitchResolved() && ue_wrap::lightswitch::IsLightSwitch(aimed)) {
        void* const root = ue_wrap::lightswitch::ResolveSwitchRoot(aimed);
        if (!root) return;                                   // no group reachable: native behaviour stays
        const std::wstring gk = ue_wrap::lightswitch::GetKeyString(root);
        if (gk.empty() || gk == L"None") return;             // unkeyed group: no lane owns it, leave it alone
        if (!ue_wrap::lightswitch::GroupGateAvailable()) return;  // cannot gate -> do not pretend we did
        g_useInputGatePrior   = ue_wrap::lightswitch::GetGroupGate(root);
        ue_wrap::lightswitch::SetGroupGate(root, false);
        if (ue_wrap::lightswitch::GetGroupGate(root)) return;     // the write did not take; record nothing
        g_useInputGateCleared = root;
        return;
    }

    if (!ue_wrap::door::EnsureResolved()) return;
    void* door = aimed;
    if (!ue_wrap::door::IsDoor(door)) return;
    // The gate decision asks whether a lane owns this door, answered by whether the channel indexes
    // it, not by whether the game gave it a Key: a door indexed under a portable identity has a
    // game Key that names nothing cross-peer, and a keyed door not yet indexed has no lane to wait
    // for. Gating on the index makes "the native open was suppressed" and "the host will echo" the
    // same condition; gating on the raw Key shut a door's own opening for a request that could
    // never resolve.
    const std::wstring key = g_door.KeyForActor(door);
    if (key.empty()) return;  // not indexed by this lane: native behaviour stays
    g_useInputActivePrior = ue_wrap::door::GetActive(door);  // the REAL gate value to restore
    ue_wrap::door::SetActive(door, false);  // close the BP CanOpen gate for THIS dispatch
    g_useInputActiveCleared = door;
}

void OnUseInput(void* self, void*, void*) {
    // The Active gate the PRE observer cleared is restored first, before any early return: the
    // native chain already ran, gated shut, and the host echo is the only thing that moves this
    // door now.
    if (g_useInputActiveCleared) {
        ue_wrap::door::SetActive(g_useInputActiveCleared, g_useInputActivePrior);
        g_useInputActiveCleared = nullptr;
    }
    RestoreLightGateIfCleared();
    if (!self) return;
    auto* s = g_door.GetSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return;  // CLIENT-only
    if (!ue_wrap::door::EnsureResolved()) return;
    void* door = ue_wrap::engine::ReadMainPlayerLookAtActor(self);  // the actor under the cursor at press
    const bool isDoor = (door && ue_wrap::door::IsDoor(door));
    // The diagnostic behind interactable_log (every E-press, door or not, so never unconditional):
    // no line means the observer did not fire; a null actor means the aim trace had not populated;
    // a non-door means the aim was elsewhere. The request and the host's verdict log
    // unconditionally.
    if (ProbeLog())
        UE_LOGI("door: use-input fired -- lookAtActor=%p isDoor=%d (role=client, connected)", door, isDoor ? 1 : 0);
    if (!isDoor) return;             // not aiming at a door -> not ours
    // The same key the channel indexes (Channel::KeyForActor): the index and the request must name
    // the same thing.
    std::wstring key = g_door.KeyForActor(door);
    if (key.empty()) {
        // A press the lane cannot name, and not silent: to the player it is the defect itself
        // (press E, nothing happens), and a press that emits nothing enqueues nothing for the
        // receive-side retry to cover. Two causes: the index belongs to another world generation
        // (it refills on the hub's next pass), or the door is genuinely unindexed. WARN,
        // rate-limited against a key-masher.
        static std::chrono::steady_clock::time_point s_lastGripe{};
        const auto nowG = std::chrono::steady_clock::now();
        if (nowG - s_lastGripe > std::chrono::seconds(3)) {
            s_lastGripe = nowG;
            UE_LOGW("door: E-press on %p is NOT INDEXED by the door lane -- no request sent "
                    "(indexCurrent=%d, raw game key='%ls'). The door will not open on either "
                    "peer; this is the visible symptom of an unindexed instance, not silence.",
                    door, g_door.IndexCurrent() ? 1 : 0,
                    ue_wrap::door::GetKeyString(door).c_str());
        }
        return;
    }
    // The debounce: InpActEvt_use dispatches on both the press and the release of one tap, so one
    // use fired two toggles (the host opened then closed, and the release's force-close left the
    // client's swing ajar). Repeats within 300 ms for the same door collapse into one.
    static std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> s_lastUse;  // GT-only
    const auto nowTs = std::chrono::steady_clock::now();
    if (auto it = s_lastUse.find(key); it != s_lastUse.end() && nowTs - it->second < std::chrono::milliseconds(300)) {
        UE_LOGI("door: use-input hook -> debounced repeat (press+release) key='%ls'", key.c_str());
        return;
    }
    s_lastUse[key] = nowTs;
    // A pure toggle, without reading isOpened: that flag marks the completed animation and lags the
    // swing, so at this point it holds the pre-toggle value and reports the wrong intent. The host
    // derives open versus close from its own hold record; the action field is unused.
    coop::net::KeyedTogglePayload p{};
    WireKeyFromString(key, p.key);
    p.action = 0;
    if (s->SendReliable(coop::net::ReliableKind::DoorOpenRequest, &p, sizeof(p)))
        UE_LOGI("door: use-input hook -> toggle request key='%ls'", key.c_str());
}

void InstallUseInputObserver() {
    if (g_useInputObserverInstalled) return;
    void* playerCls = R::FindClass(P::name::MainPlayerClass);
    if (!playerCls) return;  // retry until mainPlayer_C loads
    void* fn = R::FindFunction(playerCls, P::name::MainPlayerUseInputEventFn);
    if (!fn) {
        UE_LOGW("door: InpActEvt_use UFunction not found -- client door opens cannot be signalled");
        g_useInputObserverInstalled = true;  // no retry
        return;
    }
    if (!GT::RegisterPreObserver(fn, &OnUseInputPre)) {
        UE_LOGW("door: InpActEvt_use PRE observer register failed");
        return;
    }
    if (!GT::RegisterPostObserver(fn, &OnUseInput)) {
        UE_LOGW("door: InpActEvt_use observer register failed");
        return;
    }
    g_useInputObserverInstalled = true;
    UE_LOGI("door: InpActEvt_use PRE+POST observers installed (PRE gates the native toggle; POST restores + sends DoorOpenRequest)");
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
    InstallUseInputObserver();   // client E-press (InpActEvt_use + lookAtActor) -> DoorOpenRequest
}

void OnReliable(uint8_t kind, const coop::net::KeyedTogglePayload& payload, uint8_t senderPeerSlot) {
    if (Channel* ch = ChannelForKind(static_cast<coop::net::ReliableKind>(kind)))
        ch->OnReliable(payload, senderPeerSlot);
}

void OnDoorOpenRequest(const coop::net::KeyedTogglePayload& payload, uint8_t senderPeerSlot) {
    // Host only: a client asked to toggle a door. event_feed gates the sender slot, OnRequest
    // re-checks the role; the host applies with its real guards and its poll broadcasts the
    // authoritative state.
    if (senderPeerSlot == 0) return;  // the host never sends this to itself
    g_door.OnRequest(payload, senderPeerSlot);
}

void OnPeerLeft(int peerSlot) {
    if (peerSlot <= 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    g_door.OnPeerLeft(static_cast<uint8_t>(peerSlot));  // doors are the one channel with a hold register
}

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
