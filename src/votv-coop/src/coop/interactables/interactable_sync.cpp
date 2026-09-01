// coop/interactable_sync.cpp -- see coop/interactable_sync.h. The per-feature
// ADAPTERS + the public facade for the keyed-interactable sync (door / light-switch /
// container-lid / garage / appliance state).
//
// The GENERIC replication engine (the Adapter vtable + the Channel class with its
// key->actor index, deferred-apply/retry, echo-suppress, connect-snapshot, and the
// HostAuth door hold-register) lives in coop/interactable_channel.h -- it crossed the
// 800-LOC soft cap once the 4th/5th feature landed, so the engine moved to its own
// header (RULE 2026-05-25) and this TU keeps only the small per-feature specifics:
// which class, which Key offset, which open/close UFunction (one Adapter each), the
// kind->Channel router, the client E-press observer, and the Install/Tick/... facade
// the net-pump + event_feed call. Adding a feature is an adapter + a few lines here.

#include "coop/interactables/interactable_sync.h"
#include "coop/interactables/interactable_channel.h"  // the generic engine: Adapter + Channel (+ ProbeLog, R alias, WireKey usings)

#include "ue_wrap/devices/appliance.h"     // the 6-class save-actor toggle family (faucet/sink/shower/kitchen/serverBox/wallunit_tapes)
#include "ue_wrap/devices/door.h"
#include "ue_wrap/devices/door_box.h"      // v62 lockers + drone-console hinged doors
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
// NOTE: the `R` (reflection) alias, ProbeLog(), the WireKey<->wstring usings, the
// throttle/TTL/settle constants, the Adapter struct, and the Channel class all live in
// coop/interactable_channel.h now (RULE 2 -- one definition). They are in scope here
// because this TU includes that header inside the same coop::interactable_sync namespace.

// ---- Adapters (file-static; must precede the Channel instances) ----------
// Replay a switch press for its VISUAL half. Defined below (it needs the channels for the
// role read); declared here because the light adapter's apply lambda names it.
bool ApplySwitchPresentation(void* sw);

const Adapter g_doorAdapter = {
    "door", coop::net::ReliableKind::DoorState,
    &ue_wrap::door::EnsureResolved,
    &ue_wrap::door::IsDoor,
    &ue_wrap::door::GetKeyString,
    // INTENT reader (not isOpened): the host broadcasts a door at swing-START, so
    // a host-opened door mirrors frame-perfect on clients instead of lagging the
    // ~0.5 s swing-completion (2026-06-13 host->client open-lag fix; client opens
    // were already instant via the InpActEvt_use input-edge request).
    &ue_wrap::door::TryReadOpenIntent,
    // Receiver-apply (host state on this peer): FORCE-SNAP, not doorOpen/doorClose. The open is
    // a tick-gated animation that FREEZES when this peer's player is far from the door (probe-
    // proven 2026-06-04: isOpened never sets), so doorOpen() can't reliably mirror a door the
    // local player isn't standing at. ForceOpen/ForceClose complete the state via the move
    // timeline + move__FinishedFunc regardless of proximity. (If the local player IS near it
    // snaps rather than animates -- correctness over a swing; visual polish is a later pass.)
    [](void* a, bool on) -> bool { ue_wrap::door::SmartApply(a, on); return true; },
    // HostAuth hooks (doors only):
    &ue_wrap::door::SuppressClientAutonomy,
    &ue_wrap::door::RestoreClientAutonomy,
    // Host applies a CLIENT request: FORCE-SNAP too. The host has no local player at the door
    // (only the client's puppet), so BOTH doorOpen(bypassCheck=false) [denied -- needs a local
    // interactor] AND doorOpen(bypassCheck=true) [animation freezes when the host player is far]
    // fail to open it ("the door is closed on host"). Force-snap is the only thing that opens a
    // door the host player isn't next to. The client already enforced the lock locally, so
    // trusting its validated edge and snapping the host door is correct + authoritative.
    [](void* a, bool on) -> bool { ue_wrap::door::SmartApply(a, on); return true; },
    // HOST held-door suppression (cycle fix): mute the host's own autoclose while a client
    // holds this door open; restore + close on release.
    &ue_wrap::door::SuppressHostHeldDoor,
    &ue_wrap::door::ReleaseHostHeldDoor,
    // Open gate: the host applies a client's open only if the door's own engine would (power on,
    // not jammed, not superClosed) -- so coop never opens a door SP keeps shut.
    &ue_wrap::door::CanOpen,
    // The two facets that used to ride implicitly on Mode::HostAuth (split 2026-09-01). Doors
    // are the ONLY feature that wants either: their autoclose fights an applied state, so a
    // client open is a HOLD; and their apply is an ~0.5 s animation, so it needs finishing.
    /*holdRegister*/ true,
    /*TickApply*/    &ue_wrap::door::TickSmartApply,
};
const Adapter g_lightAdapter = {
    // Re-keyed to the SWITCH (was the lightRoot) so the receiver replays use() -> the
    // switch FLIPS VISUALLY on the peer AND its lights fan out, in one BP call. use()
    // toggles; the channel only applies when cur != want (ApplyResolved's cur==want
    // idempotent guard), so for a 2-state bool "toggle when different" == an absolute set,
    // and double-delivery is safe (applies are GT-serialized + use() updates A
    // synchronously -- lightswitch_probe proved A flips 0->1 right after the call).
    // (IDA 2026-06-04: the lightRoot.SetActive observer never fired -- BP-internal.)
    //
    // THE OLD "HANDS-ON TO VERIFY" HERE IS ANSWERED, by bytecode, 2026-09-01: use() ends in
    // an UNCONDITIONAL `a := Not_PreBool(a)` with no branch around it, so it toggles BOTH
    // directions and "apply when cur != want" really is an absolute set for this bool. No
    // switch-level set verb is needed. (PR #12 proposed forcing `a` by raw memory write to
    // fix a one-way toggle that does not exist; it was declined -- see below.)
    //
    // WHAT THIS LANE DOES *NOT* COVER, measured the same day and NOT a defect of this code:
    // `a` is the switch's PRESENTATION bit (mesh + sound) and is not save-persistent. The
    // state a player SEES is trigger_lightRoot_C::isActive, which this lane never touches
    // and nothing else in the tree reconciles either. use() reaches it via
    // runTrigger(self,0), which is GATED on the root's `active` -- so on a peer whose gate
    // is shut the switch flips and the lights do not, `a` still agrees cross-peer, and this
    // poll is structurally blind to the difference forever. A population census of the paks
    // (3,522 uassets) finds THIRTEEN blueprints that can move a group -- powerControl,
    // mainGamemode, ticker_flickerer, ui_cheatMenu, and every trigger_eventer / solarBoom /
    // fakeLmaos / breakDish, because a lightRoot IS a trigger -- so the honest fix is to
    // sync the GROUP source-agnostically, not to enumerate causes. Measure it first with
    // ini `lightgroup_census=1` on both peers (coop/dev/light_group_census.h).
    "light", coop::net::ReliableKind::LightState,
    &ue_wrap::lightswitch::EnsureSwitchResolved,
    &ue_wrap::lightswitch::IsLightSwitch,
    &ue_wrap::lightswitch::GetSwitchKeyString,
    &ue_wrap::lightswitch::TryReadSwitchA,
    // Receiver-apply: replay use() so the peer's switch flips and clicks. On a CLIENT this
    // runs PRESENTATION-ONLY -- ApplySwitchPresentation shuts the group's gate for exactly the
    // duration of the call, so use()'s internal runTrigger(root,0) cannot move `isActive`.
    // Without that the same press would be written by TWO lanes (this one via the toggle, the
    // group lane absolutely) with no defined order inside a game-thread batch. On the HOST the
    // full use() runs on purpose: replaying the client's switch edge IS how a client's press
    // becomes an authoritative group change.
    [](void* a, bool /*on*/) -> bool { return ApplySwitchPresentation(a); },
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,  // Symmetric channel -- no HostAuth hooks (last = CanOpen)
};
// The light GROUP (Atrigger_lightRoot_C) -- the state a player actually SEES, and until
// 2026-09-01 the half of this feature nobody owned. The adapter above syncs the SWITCH's
// `A`; this one syncs the group's `isActive`. They are two entities on purpose, because the
// GAME keeps them decoupled: use() toggles `A` unconditionally but reaches the lamps only
// through runTrigger(root,0), which opens with `IFNOT(active) POP`. So on a peer whose gate
// is shut the switch flips, the click plays, and no light moves -- and since `A` then agrees
// cross-peer, a poll on `A` can never see the difference again. (`A` is also not
// save-persistent; the group's state is, via buffIsActive. RE flag E-L2, open since
// 2026-05-25, is closed in ue_wrap/devices/lightswitch.h.)
//
// HOSTAUTH, not Symmetric. A pak-wide census (3,522 uassets) finds THIRTEEN blueprints that
// can move a group -- powerControl, mainGamemode, ticker_flickerer, ui_cheatMenu and every
// trigger_eventer / solarBoom / fakeLmaos / breakDish, because a lightRoot IS a trigger --
// and most are host-owned world systems. Symmetric would let a client's local ticker author
// the host's lights. The client is RECEIVE-ONLY here and there is no RequestApply: a client's
// press already reaches the host on the LightState lane, and the host's own use() is what
// produces the authoritative group change (act-as-host, COOP_SYNCER_MODEL 2b -- the intent is
// the switch edge, the host performs it).
//
// APPLY IS runTrigger(root, on ? 1 : 2) -- the game's own ABSOLUTE, UNGATED setters, both of
// which call updLig() so every lamp in lights[]/ambs[] repaints. Deliberately NOT index 0 (a
// gated toggle would re-introduce the decoupling this lane exists to fix) and deliberately
// NOT setActive, which writes the GATE and moves no lamp at all.
const Adapter g_lightGroupAdapter = {
    "lightgroup", coop::net::ReliableKind::LightGroupState,
    &ue_wrap::lightswitch::EnsureResolved,
    &ue_wrap::lightswitch::IsLightRoot,
    &ue_wrap::lightswitch::GetKeyString,
    &ue_wrap::lightswitch::TryReadActive,
    [](void* a, bool on) -> bool {
        // Skip a no-op apply. HostAuth deliberately applies UNCONDITIONALLY in general, because
        // on a DOOR a live-field match is evidence of the client's own native press racing the
        // echo rather than of idempotency (interactable_channel.h). That reasoning does not
        // transfer here: this client's own writers of isActive are gate-suppressed, so a match
        // really is a match. Without this, one connect-snapshot is 42 runTrigger calls in a
        // single frame, each fanning updLig() over its whole lights[]/ambs[] -- a join-time
        // stall and a visible full relamp of the base, for zero state change.
        bool cur = false;
        if (ue_wrap::lightswitch::TryReadActive(a, cur) && cur == on) return true;
        return ue_wrap::lightswitch::ApplyGroupState(a, on);
    },
    // No autonomy-suppression hooks: a client's native press is neutralised for the exact
    // duration of ONE input dispatch by the E-press PRE/POST pair below (the door lane's
    // idiom), never by a gate left standing. A gate left shut is a player whose light
    // switches quietly stopped working, and this project has shipped that class before.
    // No RequestApply, no hold register, no TickApply -- the apply is instant.
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};
const Adapter g_containerAdapter = {
    "container", coop::net::ReliableKind::ContainerState,
    &ue_wrap::swinger::EnsureResolved,
    &ue_wrap::swinger::IsSwinger,
    &ue_wrap::prop::GetKeyString,  // a swinger is an Aprop_C
    &ue_wrap::swinger::TryReadOpen,
    [](void* a, bool on) -> bool { return on ? ue_wrap::swinger::CallOpen(a, false) : ue_wrap::swinger::CallClose(a); },
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,  // Symmetric channel -- no HostAuth hooks (last = CanOpen)
};
// Garage door (Agarage_C). SYMMETRIC like lights/containers: it has NO sensor / autoclose
// (no auto-revert), so a symmetric poll never oscillates and the door HostAuth machinery is
// unnecessary (RULE 1 -- don't carry door complexity the garage doesn't need). Identity =
// the level-export FName (GetNameKey), NOT the save Key: the gamemode's one-shot sublevel-
// gated keying (loadObjects) can leave the garage Key=None during the host's menu->save world
// transition -> the None-key filter drops it forever (take-4 R9; host garage index 1->0, never
// recovered, while door_box's FName identity survived the SAME reload 20/20 byte-identical
// cross-peer). State = Open; the wall button just toggles Open, which the poll catches -- we
// never observe the button.
// RE: research/findings/computers-devices/votv-garage-door-button-sync-RE-2026-06-08.md;
//     R9 fix (FName identity): votv-take4-hands-on-bugs-2026-07-21.md.
const Adapter g_garageAdapter = {
    "garage", coop::net::ReliableKind::GarageDoorState,
    &ue_wrap::garage::EnsureResolved,
    &ue_wrap::garage::IsGarage,
    &ue_wrap::garage::GetNameKey,
    &ue_wrap::garage::TryReadOpen,
    [](void* a, bool on) -> bool { return ue_wrap::garage::ApplyOpen(a, on); },
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,  // Symmetric channel -- no HostAuth hooks (last = CanOpen)
};
// Appliance family (6 Aactor_save_C descendants: faucet/sink/shower/kitchen-oven/serverBox/
// wallunit-tapes). SYMMETRIC single-bool toggles -- none auto-reverts (only doors do), so a
// symmetric poll never oscillates. ONE adapter covers all six: the ue_wrap::appliance wrapper
// dispatches by class to the right bool offset + refresh verb (upd/updIsOn/SetActive) so the
// peer's mesh/FX/audio repaint, not just the field. Key = Aactor_save_C::Key @0x0230. The
// wall switches/breakers that drive these just flip the bool, which the poll catches -- we
// never observe the switch. RE: research/findings/props-lifecycle/votv-all-interactables-sweep-catalog-2026-06-08.md.
const Adapter g_applianceAdapter = {
    "appliance", coop::net::ReliableKind::ApplianceState,
    &ue_wrap::appliance::EnsureResolved,
    &ue_wrap::appliance::IsAppliance,
    &ue_wrap::appliance::GetKeyString,
    &ue_wrap::appliance::TryReadState,
    [](void* a, bool on) -> bool { return ue_wrap::appliance::ApplyState(a, on); },
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,  // Symmetric channel -- no HostAuth hooks (last = CanOpen)
};
// Hinged-door storage boxes (v62): the ~19 lockers (locker_C + the two pure
// subclasses) and the drone-call console box (droneConsole_C). SYMMETRIC: a
// bytecode scan found ZERO auto-revert writers of `opened` (only the player
// toggle and the locker's own open()), so a symmetric poll never oscillates.
// Identity = the level-export actor FName (neither class has a save Key;
// placed-actor names are deterministic cross-peer). Apply = the native verb /
// write+refresh per class, with the door.cpp verify+force-snap inside the
// wrapper (the 0.5 s swing Timeline freezes outside tick range).
// RE: research/findings/computers-devices/votv-lockers-boxes-door-RE-2026-06-11.md.
const Adapter g_doorBoxAdapter = {
    "doorbox", coop::net::ReliableKind::LockerDoorState,
    &ue_wrap::door_box::EnsureResolved,
    &ue_wrap::door_box::IsDoorBox,
    &ue_wrap::door_box::GetNameKey,
    &ue_wrap::door_box::TryReadOpened,
    [](void* a, bool on) -> bool { return ue_wrap::door_box::ApplyOpened(a, on); },
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,  // Symmetric channel -- no HostAuth hooks (last = CanOpen)
};
Channel g_door{g_doorAdapter, Channel::Mode::HostAuth};  // doors auto-revert -> host-authoritative
Channel g_light{g_lightAdapter};
// Host-authoritative -- see g_lightGroupAdapter. The client neither polls nor requests on
// this kind; it renders what the host says the world looks like.
Channel g_lightGroup{g_lightGroupAdapter, Channel::Mode::HostAuth};
Channel g_container{g_containerAdapter};
Channel g_garage{g_garageAdapter};  // garage has no auto-revert -> Symmetric (no oscillation)
Channel g_appliance{g_applianceAdapter};  // appliances have no auto-revert -> Symmetric
Channel g_doorBox{g_doorBoxAdapter};  // lockers/console have no auto-revert -> Symmetric
// Keypads (ApasswordLock_C) are NOT a toggle -- they carry a typed buffer + 3 state bools
// and their accept verb is unreachable (proven), so forcing them into this Channel was the
// v31 fail-cycle. They now live in their own coop::keypad_sync module (RULE 2: the broken
// adapter + g_keypad Channel are GONE, not disabled-in-place). KeypadState routes there
// from event_feed, not through ChannelForKind below.

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

// Replay a switch press for its VISUAL half only. use() is the game's own verb and does three
// things: fires the group trigger, toggles `A`, and repaints the mesh + plays the click. On a
// CLIENT we want the last two and must NOT have the first, because the group is host-owned and
// a second writer with no defined ordering is how a transient becomes a permanent inversion.
// Rather than hand-copy use()'s presentation half (a copy of a BP body drifts -- that is
// exactly how power_control's ApplyPress ended up mirroring a panel while silently dropping
// one of its four fan-out targets), pull the lever the BP already gates on: shut the group's
// `active` for the duration of the call. Same idiom as the door lane's E-press PRE/POST pair.
//
// The gate is ALWAYS restored to the value it had, never to a hardcoded true: the door lane
// shipped exactly that bug in 2026-06-12 ("opens and shuts instantly") because a keypad-locked
// door legitimately has its gate shut. Here a shut gate means the base power panel's lights
// breaker is off, which is just as legitimate.
bool ApplySwitchPresentation(void* sw) {
    auto* s = g_light.GetSession();
    const bool isClient = s && s->connected() && s->role() == coop::net::Role::Client;
    if (!isClient) return ue_wrap::lightswitch::CallUse(sw);  // HOST: the full press, group and all

    void* const root = ue_wrap::lightswitch::ResolveSwitchRoot(sw);
    if (!root) return ue_wrap::lightswitch::CallUse(sw);  // no group reachable -> nothing to gate

    // RAII, not a straight-line restore: CallUse goes through ProcessEvent, whose fault is caught
    // at the task/detour boundary and unwinds straight PAST a manual restore. `active` is
    // save-persistent, so a leaked shut gate is written into the player's save and follows them
    // into single-player -- every switch in that group flips and clicks and moves no light,
    // permanently. /EHa means the unwind still runs this destructor.
    ue_wrap::lightswitch::ScopedGroupGateShut hold(root);
    if (!hold.shut()) {
        // The guard is NOT in force (unresolved offset, or the write did not take). Say so once
        // rather than proceeding as if it were: the write failing CLOSED is not the same claim as
        // the suppression having HAPPENED, and here it fails OPEN -- use() will also move
        // isActive, which the host owns.
        static bool s_warned = false;
        if (!s_warned) { s_warned = true;
            UE_LOGW("light: group gate unavailable -- a client's switch replay will also move "
                    "isActive, which the host owns. The group lane still corrects it, but the "
                    "press double-moves visibly."); }
    }
    return ue_wrap::lightswitch::CallUse(sw);
}

// ---- The SENDER is per-tick STATE POLLING (Channel::PollAndBroadcast, driven by
// Tick()). There is no UFunction observer: IDA-PROVEN 2026-06-04 the per-verb edges
// (Adoor_C::doorOpen, Atrigger_lightRoot_C::SetActive, Aprop_swinger_C::Open -- and even
// the switch's player_use/use) dispatch via CallFunction -> ProcessInternal (@0x141302dc0)
// and bypass our ProcessEvent detour (@0x141465930), so a POST observer never fires (the
// doors `sent=0` bug). Polling the resulting STATE field instead catches every writer --
// player E-press, NPC-proximity auto-open, keypad-unlock, scripts -- uniformly. -------

// ---- CLIENT door request on E-press (HostAuth doors) ----------------------------------
// The door's own verbs (player_use / doorOpen) dispatch BP-internally (CallFunction ->
// ProcessInternal), so a POST observer on them NEVER fires -- the client's door open would
// never reach the host (the bug: "client opening door never mirrors"). The ONE ProcessEvent-
// observable use-edge is AmainPlayer_C::InpActEvt_use (the E-press input action; grab_observer
// proves it fires). We observe THAT (POST) on the local player, read the actor the player was
// aiming at (mainPlayer.lookAtActor @0x0AA0), and -- if it is a door -- send a DoorOpenRequest
// with the door's post-press isOpened (POST = the new toggled state). The host applies it
// (real guards) + broadcasts authoritative DoorState. CLIENT-only; the host runs the real
// door logic + polls, so it needs no such hook. (Puppets are unpossessed -> they never
// process input, so this only ever fires for the local player.)
bool g_useInputObserverInstalled = false;

// The door whose Active gate the PRE observer cleared for the CURRENT
// InpActEvt_use dispatch + the REAL pre-clear value; the POST observer restores
// that value. PRE/POST pair within ONE game-thread dispatch, so a single slot
// suffices. (Door stress-desync fix 2026-06-10: the client's NATIVE BP press
// chain InpActEvt_use -> player_use -> doorOpen is BP-internal -- unobservable,
// unsuppressable by hooks -- so it toggled the LOCAL door on every E in
// parallel with our host request, and the host echo then got swallowed as
// "already in that state". Clearing Active -- the BP's own CanOpen gate -- for
// the body of the dispatch makes the native chain a no-op: the client door
// moves ONLY on host echoes. MTA shape: the non-authority never advances state
// from its own simulation (CVehicle m_bAllowDoorRatioSetting); our equivalent
// lever is a field the BP already gates on, since we cannot add flags to
// assets.) The restore writes the SAVED value, never a hardcoded true: a
// keypad-LOCKED door has Active=false, and restoring true silently re-powered
// the lock client-side -- the next PRE-miss (aim trace not yet populated) let
// the native chain open a locked door, and the host's deny correction slammed
// it shut ("opens and shuts instantly", user 2026-06-12 round 2).
void* g_useInputActiveCleared = nullptr;
bool  g_useInputActivePrior  = true;

// The SAME lever for a light switch, and for the same reason. A client's native E-press runs
// lightswitch_C::use() BP-internally -- unobservable and unsuppressable by hooks -- and use()
// ends in runTrigger(root,0), which would move a group the HOST owns. Shutting that group's
// `active` for the body of the dispatch makes the native chain presentation-only: the switch
// still flips and clicks locally (the responsive half a player feels), and the lights move
// only when the host's LightGroupState lands. One slot suffices for the same reason the door's
// does -- PRE and POST are one game-thread dispatch apart.
void* g_useInputGateCleared = nullptr;
bool  g_useInputGatePrior   = true;

// Put back a gate the PRE observer shut. Called FIRST from both observers, so a dispatch whose
// BP body SEH-faulted (POST never ran) self-heals on the next press instead of leaving a group
// permanently deaf to its own switch -- the door lane's audit IMP-3, which applies verbatim.
void RestoreLightGateIfCleared() {
    if (!g_useInputGateCleared) return;
    ue_wrap::lightswitch::SetGroupGate(g_useInputGateCleared, g_useInputGatePrior);
    g_useInputGateCleared = nullptr;
}

void OnUseInputPre(void* self, void*, void*) {
    // Restore a LEAKED door first (audit IMP-3): if the prior dispatch's BP body
    // SEH-faulted, the POST observer never ran and the cleared door would stay
    // Active=false forever (bricked). Self-heals on the next E-press.
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

    // A LIGHT SWITCH: shut its group's gate so the native use() is presentation-only.
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
    const std::wstring key = ue_wrap::door::GetKeyString(door);
    if (key.empty() || key == L"None") return;  // unkeyed door: native behavior stays
    g_useInputActivePrior = ue_wrap::door::GetActive(door);  // the REAL gate value to restore
    ue_wrap::door::SetActive(door, false);  // close the BP CanOpen gate for THIS dispatch
    g_useInputActiveCleared = door;
}

void OnUseInput(void* self, void*, void*) {
    // Restore the Active gate the PRE observer cleared -- FIRST, before any
    // early return below (disconnect race, debounce, ...): every exit path
    // must put back the SAVED value. The native chain already ran (gated
    // shut); the host echo is now the only thing that will move this door.
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
    // DIAGNOSTIC (gated behind interactable_log -- fires on EVERY E-press, door or not, so it
    // must NOT be unconditional: log spam on a per-input path costs FPS). Enable it when a door
    // open misbehaves: no line = observer didn't fire; lookAtActor=0 = the interaction trace had
    // not populated the aimed actor yet; non-null + isDoor=0 = aiming at a non-door. The
    // meaningful edges (toggle request sent; host opened/DENIED) are logged unconditionally below
    // + in OnRequest, so the normal path is visible without this.
    if (ProbeLog())
        UE_LOGI("door: use-input fired -- lookAtActor=%p isDoor=%d (role=client, connected)", door, isDoor ? 1 : 0);
    if (!isDoor) return;             // not aiming at a door -> not ours
    std::wstring key = ue_wrap::door::GetKeyString(door);
    if (key.empty() || key == L"None") return;
    // DEBOUNCE: AmainPlayer_C::InpActEvt_use dispatches on BOTH the press AND the release of one
    // tap (~0.3s apart), so a single "use" fires this observer TWICE -> two toggles -> the host
    // opens then immediately closes ("open-closed in 0.3s, nothing changed") and the release's
    // force-close interrupts the client's mid-open swing -> ajar. Collapse rapid repeats for the
    // same door into ONE toggle. 300ms < any deliberate re-toggle, > a tap's press-release gap.
    static std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> s_lastUse;  // GT-only
    const auto nowTs = std::chrono::steady_clock::now();
    if (auto it = s_lastUse.find(key); it != s_lastUse.end() && nowTs - it->second < std::chrono::milliseconds(300)) {
        UE_LOGI("door: use-input hook -> debounced repeat (press+release) key='%ls'", key.c_str());
        return;
    }
    s_lastUse[key] = nowTs;
    // Send a pure TOGGLE -- do NOT read isOpened here. isOpened is the animation-COMPLETED flag
    // and lags the swing, so at POST-player_use it holds the PRE-toggle value and reports the
    // WRONG intent (press-to-open read "closed" -> sent close -> host closed the door). The host
    // derives open-vs-close from its own authoritative hold record (OnRequest), which is timing-
    // robust. p.action is unused by OnRequest (every DoorOpenRequest is a toggle).
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
        g_useInputObserverInstalled = true;  // don't retry forever
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

// ---- Receiver index: R-2 -- the six channels register as shared-scan-hub consumers; the
// hub builds every index on its own sliced cadence (the one-shot RebuildIndex prime is gone
// with the per-channel walks).
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
    // HOST-only: a client asked to open/close a door. event_feed already trust-gates
    // senderPeerSlot != 0; OnRequest re-checks the host role. The host applies it (real
    // guards) and its poll broadcasts the authoritative DoorState back to everyone.
    if (senderPeerSlot == 0) return;  // the host never sends this to itself
    g_door.OnRequest(payload, senderPeerSlot);
}

void OnPeerLeft(int peerSlot) {
    if (peerSlot <= 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    g_door.OnPeerLeft(static_cast<uint8_t>(peerSlot));  // doors are the only channel with a HOLD REGISTER (g_lightGroup is HostAuth too, but
    // never enters holdOpen_ -- see Adapter::holdRegister)
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
    ue_wrap::door_box::OnDisconnect();  // drop mid-swing verify entries (audit IMPORTANT-2)
    // Put back any gate an E-press shut and never restored (a faulted BP body skips the POST
    // observer), and forget the pointer. Otherwise the slot holds an actor of a world that may be
    // torn down before the next press, and that press's restore writes a byte into whatever now
    // owns the address -- a wrong-offset write faults nowhere near where it happened. SetGroupGate
    // re-checks liveness, so this is safe even mid-teardown.
    RestoreLightGateIfCleared();
}

}  // namespace coop::interactable_sync
