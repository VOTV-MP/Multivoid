// coop/event_dispatch_state.cpp -- the keyed device-state reliable-kind case
// bodies (the KeyedToggle family, KeypadState, PowerControlState, AtvState,
// DroneState, WindowCleanState, GrimeState, TrashPileState, KerfurConvert,
// email/signal/comp/voice), extracted VERBATIM from event_feed.cpp's Update
// switch (2026-06-11 modularity extraction; see coop/event_dispatch.h). The
// CLIENT->HOST intent/request cases moved to event_dispatch_intent.cpp
// (2026-07-10 soft-cap extraction).

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/interactables/atv_sync.h"
#include "coop/interactables/comp_sync.h"
#include "coop/interactables/console_state_sync.h"
#include "coop/interactables/desk_input_sync.h"
#include "coop/interactables/desk_snd_fx.h"
#include "coop/interactables/dish_sync.h"
#include "coop/interactables/signal_catch_sync.h"
#include "coop/interactables/tape_caddy_sync.h"  // v114 (L7): ReelSlot
#include "coop/world/daily_task_sync.h"          // v114 (L7): TaskNewState
#include "coop/player/sleep_sync.h"
#include "coop/interactables/device_occupancy.h"
#include "coop/world/email_sync.h"
#include "coop/items/player_inventory_sync.h"  // v73 inventory blob receiver
#include "coop/interactables/signal_sync.h"
#include "coop/voice/voice_chat.h"
#include "coop/interactables/drone_sync.h"
#include "coop/interactables/grime_sync.h"
#include "coop/interactables/interactable_sync.h"
#include "coop/creatures/kerfur_convert.h"
#include "coop/interactables/keypad_sync.h"
#include "coop/interactables/power_sync.h"
#include "coop/props/trash_pile_sync.h"
#include "coop/interactables/turbine_sync.h"
#include "coop/interactables/window_sync.h"

#include "ue_wrap/log.h"
#include "ue_wrap/types.h"  // ue_wrap::FVector (ThrowIntent camFwd)

#include <cmath>
#include <cstring>

namespace coop::event_feed {

bool HandleStateEvent(net::Session& session,
                      const net::Session::ReliableMessage& msg,
                      void* localPlayer) {
    switch (msg.kind) {
    case net::ReliableKind::DoorState:
    case net::ReliableKind::LightState:
    case net::ReliableKind::ContainerState:
    case net::ReliableKind::GarageDoorState:
    case net::ReliableKind::ApplianceState:
    case net::ReliableKind::LockerDoorState: {  // v62: lockers + drone-console doors (same KeyedToggle shape)
        // Phase 5D (v27): a peer toggled a keyed interactable (base door /
        // light group / container lid / garage / appliance). SYMMETRIC -- any peer
        // can send; the host relays a client-originated edge to the other clients
        // (IsClientRelayableReliableKind) before this drain runs.
        // interactable_sync routes by kind to the right channel, resolves the
        // instance by Key, + idempotently applies on the GT (echo-suppressed).
        // (KeypadState is NOT here -- it carries a richer payload, its own case below.)
        // RE: research/findings/computers-devices/votv-doors-and-lightswitches-RE-2026-05-25.md.
        if (msg.payloadLen < sizeof(net::KeyedTogglePayload)) {
            UE_LOGW("event_feed: %d payload too short (%zu < %zu)",
                    static_cast<int>(msg.kind),
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KeyedTogglePayload));
            break;
        }
        net::KeyedTogglePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // Trust-boundary: action is a uint8 but only 0/1 are meaningful.
        if (p.action != 0 && p.action != 1) {
            UE_LOGW("event_feed: keyed-toggle action=%u out of range -- dropping",
                    static_cast<unsigned>(p.action));
            break;
        }
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::interactable_sync::OnReliable(static_cast<uint8_t>(msg.kind), p, senderSlot);
        break;
    }
    case net::ReliableKind::KeypadState: {
        // v35 (2026-06-06): password-keypad INPUT mirror (ApasswordLock_C). SYMMETRIC -- any
        // peer polls inPassword + broadcasts on a buffer change; the host relays a client
        // edge (IsClientRelayableReliableKind). The receiver replays inputNumber for the
        // digit delta, which drives the keypad's own native validator (so the host accepts a
        // client's code itself -- MTA input-replication); a v59 Accept/Deny event runs the
        // native Open(Active) chain (the short-code submit mirror). No isAcc/isDeny (hover
        // flags, the old PURPLE -- removed v35). RE: votv-keypad-door-BP-disassembly-2026-06-06.md.
        if (msg.payloadLen < sizeof(net::KeypadSyncPayload)) {
            UE_LOGW("event_feed: KeypadState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KeypadSyncPayload));
            break;
        }
        net::KeypadSyncPayload kp{};
        std::memcpy(&kp, msg.payload, sizeof(kp));
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::keypad_sync::OnReliable(kp, senderSlot);
        break;
    }
    case net::ReliableKind::PowerControlState: {
        // v46 (2026-06-08): base POWER PANEL breakers (ApowerControl_C). SYMMETRIC -- any peer
        // polls its panels' 5 press bools + broadcasts on a change; the host relays a client
        // edge (IsClientRelayableReliableKind). The receiver writes the bools + refreshes the
        // panel's own visual (the base power EFFECTS sync via their own door/light/server
        // channels). 5 bools per actor -> its own coop/power_sync module (doesn't fit the
        // 1-bool toggle Channel). RE: votv-powerControl-panel-sync-RE-2026-06-08.md.
        if (msg.payloadLen < sizeof(net::PowerPanelPayload)) {
            UE_LOGW("event_feed: PowerControlState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PowerPanelPayload));
            break;
        }
        net::PowerPanelPayload pp{};
        std::memcpy(&pp, msg.payload, sizeof(pp));
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::power_sync::OnReliable(pp, senderSlot);
        break;
    }
    case net::ReliableKind::AtvState: {
        // v47 (2026-06-08): ATV body pose (AATV_C). OCCUPANT-authoritative -- the seated driver
        // streams; the host relays a client driver's pose to the other clients
        // (IsClientRelayableReliableKind). Trust-the-edge (any peer may legitimately send the
        // ATV it drives); atv_sync gates the APPLY (ignores a pose for an ATV this peer is
        // itself driving). RE: votv-ATV-quadbike-RE-and-coop-sync-design-2026-06-08.md.
        if (msg.payloadLen < sizeof(net::AtvStatePayload)) {
            UE_LOGW("event_feed: AtvState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::AtvStatePayload));
            break;
        }
        net::AtvStatePayload ap{};
        std::memcpy(&ap, msg.payload, sizeof(ap));
        if (!std::isfinite(ap.x) || !std::isfinite(ap.y) || !std::isfinite(ap.z) ||
            !std::isfinite(ap.pitch) || !std::isfinite(ap.yaw) || !std::isfinite(ap.roll)) {
            UE_LOGW("event_feed: AtvState non-finite pose -- dropping");
            break;
        }
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::atv_sync::OnReliable(ap, senderSlot);
        break;
    }
    case net::ReliableKind::AtvRelease: {
        // v76 (2026-06-15): ATV grab-carry RELEASE/throw edge (companion to AtvState). The peer that
        // was the grav-hand grabber sends this once when the grab ends; the receiver re-enables the
        // mirror's physics + inherits the launch velocity (atv_sync gates the apply: a peer that is
        // itself the authority ignores it). Host relays a client grabber's release to the other
        // clients (IsClientRelayableReliableKind), same as AtvState.
        if (msg.payloadLen < sizeof(net::AtvReleasePayload)) {
            UE_LOGW("event_feed: AtvRelease payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::AtvReleasePayload));
            break;
        }
        net::AtvReleasePayload arp{};
        std::memcpy(&arp, msg.payload, sizeof(arp));
        if (!std::isfinite(arp.linVelX) || !std::isfinite(arp.linVelY) || !std::isfinite(arp.linVelZ) ||
            !std::isfinite(arp.angVelX) || !std::isfinite(arp.angVelY) || !std::isfinite(arp.angVelZ)) {
            UE_LOGW("event_feed: AtvRelease non-finite velocity -- dropping");
            break;
        }
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::atv_sync::OnAtvRelease(arp, senderSlot);
        break;
    }
    case net::ReliableKind::AtvSpawn: {
        // v77: HOST->client purchased-ATV announce. The client fresh-spawns a native AATV_C it has no
        // save-twin of (host-only economy delivery). HOST-AUTHORITATIVE -- trust only slot 0.
        if (msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: AtvSpawn from non-host senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::AtvSpawnPayload)) {
            UE_LOGW("event_feed: AtvSpawn payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::AtvSpawnPayload));
            break;
        }
        net::AtvSpawnPayload asp{};
        std::memcpy(&asp, msg.payload, sizeof(asp));
        coop::atv_sync::OnAtvSpawn(asp, 0);
        break;
    }
    case net::ReliableKind::AtvDestroy: {
        // v77: HOST->client purchased-ATV teardown. HOST-AUTHORITATIVE -- trust only slot 0.
        if (msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: AtvDestroy from non-host senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::AtvDestroyPayload)) {
            UE_LOGW("event_feed: AtvDestroy payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::AtvDestroyPayload));
            break;
        }
        net::AtvDestroyPayload adp{};
        std::memcpy(&adp, msg.payload, sizeof(adp));
        coop::atv_sync::OnAtvDestroy(adp, 0);
        break;
    }
    case net::ReliableKind::DroneState: {
        // v48 (2026-06-08): delivery drone body pose (Adrone_C). HOST-AUTHORITATIVE singleton --
        // HOST->client only; trust-gated to slot 0 (like SkyState/TimeSync). The client
        // suppresses its own drone ReceiveTick + mirrors the streamed transform. RE:
        // votv-delivery-drone-RE-and-coop-sync-design-2026-06-03.md.
        if (msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: DroneState from non-host senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::DroneStatePayload)) {
            UE_LOGW("event_feed: DroneState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DroneStatePayload));
            break;
        }
        net::DroneStatePayload dp{};
        std::memcpy(&dp, msg.payload, sizeof(dp));
        if (!std::isfinite(dp.x) || !std::isfinite(dp.y) || !std::isfinite(dp.z) ||
            !std::isfinite(dp.pitch) || !std::isfinite(dp.yaw) || !std::isfinite(dp.roll)) {
            UE_LOGW("event_feed: DroneState non-finite pose -- dropping");
            break;
        }
        coop::drone_sync::OnReliable(dp);
        break;
    }
    // (OrderRequest: moved to the INTENT family, event_dispatch_intent.cpp, 2026-07-10.)
    case net::ReliableKind::WindowCleanState: {
        // v41 (2026-06-08): base-window DIRT scalar (AbaseWindow_C::clean). SYMMETRIC
        // cooperative-clean -- any peer polls its windows + broadcasts a wipe (a decrease);
        // the host relays a client edge (IsClientRelayableReliableKind). The receiver applies
        // MIN(local, clean) (adopt==0) so a wire update only ever cleans, or VERBATIM (adopt==1,
        // host connect-snapshot only). RE: votv-dirt-window-cleaning-RE-...-2026-06-07a.md.
        if (msg.payloadLen < sizeof(net::KeyedScalarPayload)) {
            UE_LOGW("event_feed: WindowCleanState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KeyedScalarPayload));
            break;
        }
        net::KeyedScalarPayload wp{};
        std::memcpy(&wp, msg.payload, sizeof(wp));
        // Trust-boundary: value drives SetCustomPrimitiveDataFloat (a shader uniform) -- a
        // NaN/Inf there is undefined visual output and can trip a device-removed crash on some
        // GPUs. clean is FMax'd to >= 0 by the engine, so a negative value is also garbage.
        // (Same isfinite guard every other float payload in this file carries.)
        if (!std::isfinite(wp.value) || wp.value < 0.0f) {
            UE_LOGW("event_feed: WindowCleanState value=%.3f invalid -- dropping", wp.value);
            break;
        }
        if (wp.adopt != 0 && wp.adopt != 1) {
            UE_LOGW("event_feed: WindowCleanState adopt=%u out of range -- dropping",
                    static_cast<unsigned>(wp.adopt));
            break;
        }
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::window_sync::OnReliable(wp, senderSlot);
        break;
    }
    case net::ReliableKind::GrimeState: {
        // v42 (2026-06-08): surface grime dirt scalar -- SYMMETRIC cooperative-clean keyed by a
        // quantized world-position (Agrime_C is a static decal); the host relays a client edge.
        // The receiver applies MIN(local, process) + repaints. (Decal DESTROY is deferred -- see
        // grime_sync.h: grime streams in/out, so a vanished decal is NOT a reliable destroy signal.)
        if (msg.payloadLen < sizeof(net::KeyedScalarPayload)) {
            UE_LOGW("event_feed: GrimeState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KeyedScalarPayload));
            break;
        }
        net::KeyedScalarPayload gp{};
        std::memcpy(&gp, msg.payload, sizeof(gp));
        // value drives applyMaterial's shader param -- guard NaN/negative like the window.
        if (!std::isfinite(gp.value) || gp.value < 0.0f) {
            UE_LOGW("event_feed: GrimeState value=%.3f invalid -- dropping", gp.value);
            break;
        }
        if (gp.adopt != 0 && gp.adopt != 1) {  // consistency with WindowCleanState's guard
            UE_LOGW("event_feed: GrimeState adopt=%u out of range -- dropping",
                    static_cast<unsigned>(gp.adopt));
            break;
        }
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::grime_sync::OnReliable(gp, senderSlot);
        break;
    }
    case net::ReliableKind::TrashPileState: {
        // v57 (2026-06-10): trashBitsPile collect counters -- SYMMETRIC, keyed by the pile's
        // save Key; host relays a client's collect. Receiver applies per-component MIN for a
        // live edge / VERBATIM for the host adopt snapshot (trust-gated in OnReliable).
        if (msg.payloadLen < sizeof(net::TrashPileStatePayload)) {
            UE_LOGW("event_feed: TrashPileState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::TrashPileStatePayload));
            break;
        }
        net::TrashPileStatePayload tp{};
        std::memcpy(&tp, msg.payload, sizeof(tp));
        if (tp.amountA < 0 || tp.amountA > 10000 || tp.amountB < 0 || tp.amountB > 10000) {
            UE_LOGW("event_feed: TrashPileState amounts (%d,%d) out of range -- dropping",
                    static_cast<int>(tp.amountA), static_cast<int>(tp.amountB));
            break;
        }
        if (tp.adopt != 0 && tp.adopt != 1) {
            UE_LOGW("event_feed: TrashPileState adopt=%u out of range -- dropping",
                    static_cast<unsigned>(tp.adopt));
            break;
        }
        const uint8_t tpSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::trash_pile_sync::OnReliable(tp, tpSlot);
        break;
    }
    case net::ReliableKind::TurbineState: {
        // v61 (2026-06-11): wind-turbine driver floats. HOST-authoritative ~1 Hz;
        // trust-gated to slot 0 like DroneState (a client never legitimately sends
        // it, and it is not in the relay whitelist). Finite-validation + the
        // client-only apply live in turbine_sync::OnReliable.
        if (msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: TurbineState from non-host senderPeerSlot=%d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::TurbineStatePayload)) {
            UE_LOGW("event_feed: TurbineState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::TurbineStatePayload));
            break;
        }
        net::TurbineStatePayload tsp{};
        std::memcpy(&tsp, msg.payload, sizeof(tsp));
        coop::turbine_sync::OnReliable(tsp);
        break;
    }
    case net::ReliableKind::DeviceClaim: {
        // v63 (2026-06-12): enterable-device occupancy claim/release. BOTH
        // directions on one kind: client->host request (arbitrated against
        // the host claim table, trust = the TRANSPORT sender slot) and
        // host->all verdict broadcast (clients trust-gate to slot 0 inside
        // OnReliable). Not client-relayed.
        if (msg.payloadLen < sizeof(net::DeviceClaimPayload)) {
            UE_LOGW("event_feed: DeviceClaim payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DeviceClaimPayload));
            break;
        }
        net::DeviceClaimPayload dcp{};
        std::memcpy(&dcp, msg.payload, sizeof(dcp));
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::device_occupancy::OnReliable(dcp, senderSlot);
        break;
    }
    case net::ReliableKind::SkySignalState: {
        // v64 (2026-06-12): HOST-authoritative sky-signal SET snapshot parts. The
        // client-only apply + the slot-0 trust gate + part assembly live in
        // console_state_sync::OnSkySignalState.
        if (msg.payloadLen < sizeof(net::SkySignalStatePayload)) {
            UE_LOGW("event_feed: SkySignalState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::SkySignalStatePayload));
            break;
        }
        net::SkySignalStatePayload sp{};
        std::memcpy(&sp, msg.payload, sizeof(sp));
        const uint8_t sslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::console_state_sync::OnSkySignalState(sp, sslot);
        break;
    }
    case net::ReliableKind::SkySignalCatch: {
        // v70: the signal-catch consume replay. HOST validates the claim holder,
        // replays + rebroadcasts; CLIENTS replay (transport-trusted). The gates
        // live in signal_catch_sync::OnReliable.
        if (msg.payloadLen < sizeof(net::SkySignalCatchPayload)) {
            UE_LOGW("event_feed: SkySignalCatch payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::SkySignalCatchPayload));
            break;
        }
        net::SkySignalCatchPayload cp{};
        std::memcpy(&cp, msg.payload, sizeof(cp));
        const uint8_t cslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::signal_catch_sync::OnReliable(cp, cslot);
        break;
    }
    case net::ReliableKind::DishArm: {
        // v113 (L4): the download-ARM state edge (host-authored; host polarity).
        if (msg.payloadLen < sizeof(net::DishArmPayload)) {
            UE_LOGW("event_feed: DishArm payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DishArmPayload));
            break;
        }
        net::DishArmPayload ap{};
        std::memcpy(&ap, msg.payload, sizeof(ap));
        const uint8_t aslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::dish_sync::OnDishArm(ap, aslot);
        break;
    }
    case net::ReliableKind::DishSnapshot: {
        // v113 (L4): the joiner's full-24 dish pose/state seed.
        if (msg.payloadLen < sizeof(net::DishSnapshotPayload)) {
            UE_LOGW("event_feed: DishSnapshot payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DishSnapshotPayload));
            break;
        }
        net::DishSnapshotPayload sp2{};
        std::memcpy(&sp2, msg.payload, sizeof(sp2));
        const uint8_t s2slot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::dish_sync::OnDishSnapshot(sp2, s2slot);
        break;
    }
    case net::ReliableKind::DishCalib: {
        // v113 (L4): the symmetric calibration batch (apply + prime; host relays).
        if (msg.payloadLen < sizeof(net::DishCalibPayload)) {
            UE_LOGW("event_feed: DishCalib payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DishCalibPayload));
            break;
        }
        net::DishCalibPayload cp2{};
        std::memcpy(&cp2, msg.payload, sizeof(cp2));
        const uint8_t c2slot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::dish_sync::OnDishCalib(cp2, c2slot);
        break;
    }
    case net::ReliableKind::ReelSlot: {
        // v114 (L7): a caddy slot sentinel edge (presser-authored; host relays).
        if (msg.payloadLen < sizeof(net::ReelSlotPayload)) {
            UE_LOGW("event_feed: ReelSlot payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::ReelSlotPayload));
            break;
        }
        net::ReelSlotPayload rsp{};
        std::memcpy(&rsp, msg.payload, sizeof(rsp));
        const uint8_t rslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::tape_caddy_sync::OnReelSlot(rsp, rslot);
        break;
    }
    case net::ReliableKind::TaskNewState: {
        // v114 (L7): the host's saveSlot.taskNew mirror (host-authored; trust-gated in the handler).
        if (msg.payloadLen < sizeof(net::TaskNewStatePayload)) {
            UE_LOGW("event_feed: TaskNewState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::TaskNewStatePayload));
            break;
        }
        net::TaskNewStatePayload tnp{};
        std::memcpy(&tnp, msg.payload, sizeof(tnp));
        const uint8_t tslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::daily_task_sync::OnTaskNewState(tnp, tslot);
        break;
    }
    case net::ReliableKind::DeskLogLine: {
        // v70: one coords-terminal event line (producer-symmetric, host-relayed).
        if (msg.payloadLen < sizeof(net::DeskLogLinePayload)) {
            UE_LOGW("event_feed: DeskLogLine payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DeskLogLinePayload));
            break;
        }
        net::DeskLogLinePayload lp{};
        std::memcpy(&lp, msg.payload, sizeof(lp));
        const uint8_t lslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::console_state_sync::OnDeskLogLine(lp, lslot);
        break;
    }
    case net::ReliableKind::SleepState: {
        // v71: the sleep gate. Report (peer->host) / Tally / Accelerate / End
        // (host->all); the role + slot trust gates live in OnReliable.
        if (msg.payloadLen < sizeof(net::SleepStatePayload)) {
            UE_LOGW("event_feed: SleepState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::SleepStatePayload));
            break;
        }
        net::SleepStatePayload sp{};
        std::memcpy(&sp, msg.payload, sizeof(sp));
        const uint8_t sslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::sleep_sync::OnReliable(sp, sslot);
        break;
    }
    case net::ReliableKind::DeskState: {
        // v112: ADOPT-ONLY (the host->joiner connect seed). The live claimed/
        // unclaimed lanes retired to DeskInput; the adopt gate lives in
        // OnDeskState.
        if (msg.payloadLen < sizeof(net::DeskStatePayload)) {
            UE_LOGW("event_feed: DeskState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DeskStatePayload));
            break;
        }
        net::DeskStatePayload dp{};
        std::memcpy(&dp, msg.payload, sizeof(dp));
        const uint8_t dslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::console_state_sync::OnDeskState(dp, dslot);
        break;
    }
    case net::ReliableKind::DeskInput: {
        // v112: the claim-free field-granular desk INPUT delta (presser-
        // authored; host relays to all except the originator). The apply +
        // echo-prime live in desk_input_sync.
        if (msg.payloadLen < sizeof(net::DeskInputPayload)) {
            UE_LOGW("event_feed: DeskInput payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DeskInputPayload));
            break;
        }
        net::DeskInputPayload ip{};
        std::memcpy(&ip, msg.payload, sizeof(ip));
        const uint8_t islot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::desk_input_sync::OnDeskInput(ip, islot);
        break;
    }
    case net::ReliableKind::DeskScanEvent: {
        // v112: the SHIFT quick-scan happened on a peer -- replay the
        // accepted-branch effects (spawnDirs + beep) on this mirror.
        if (msg.payloadLen < sizeof(net::DeskScanEventPayload)) {
            UE_LOGW("event_feed: DeskScanEvent payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DeskScanEventPayload));
            break;
        }
        net::DeskScanEventPayload sp2{};
        std::memcpy(&sp2, msg.payload, sizeof(sp2));
        const uint8_t sslot2 =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::desk_input_sync::OnDeskScan(sp2, sslot2);
        break;
    }
    case net::ReliableKind::DeskSndFx: {
        // v115: a peer's desk audio effect (organic Play/SetActive caught at the
        // native seam) -- replay on this mirror. Symmetric, relayed.
        if (msg.payloadLen < sizeof(net::DeskSndFxPayload)) {
            UE_LOGW("event_feed: DeskSndFx payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DeskSndFxPayload));
            break;
        }
        net::DeskSndFxPayload fx{};
        std::memcpy(&fx, msg.payload, sizeof(fx));
        const uint8_t fxslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::desk_snd_fx::OnDeskSndFx(fx, fxslot);
        break;
    }
    case net::ReliableKind::DishAimState: {
        // v64: the claim owner's dish-aim stream (host-relayed). The holder gate
        // lives in OnDishAim.
        if (msg.payloadLen < sizeof(net::DishAimStatePayload)) {
            UE_LOGW("event_feed: DishAimState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DishAimStatePayload));
            break;
        }
        net::DishAimStatePayload ap{};
        std::memcpy(&ap, msg.payload, sizeof(ap));
        const uint8_t aslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::console_state_sync::OnDishAim(ap, aslot);
        break;
    }
    case net::ReliableKind::EmailAppend: {
        // HOST-AUTHORED since e5718fc6 (email_sync gates the append send on
        // role()==Host; clients author zero). Assembly + echo-proof shadow
        // registration live in email_sync::OnReliable.
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: EmailAppend payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        // Authority-boundary drop (2026-07-10 audit MEDIUM, the serverbox parity
        // shape): a client EmailAppend reaching the HOST is a protocol violation
        // post-e5718fc6 -- drop it so one client-side regression can't re-pollute
        // the shared inbox (also removed from the relay whitelist,
        // session_lanes.h).
        if (session.role() == net::Role::Host && msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: EmailAppend from client slot=%d on the HOST "
                    "(emails are host-authored) -- dropping", msg.senderPeerSlot);
            break;
        }
        net::BlobChunkPayload ep{};
        std::memcpy(&ep, msg.payload, sizeof(ep));
        const uint8_t eslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::email_sync::OnReliable(ep, eslot);
        break;
    }
    case net::ReliableKind::PlayerInventoryBlob: {
        // v73: a CLIENT streams its serialized inventory to the host (chunked). Assembly +
        // persistence (coop_players/<guid>.json, magic+FNV+.bak, rate-limited) live in the
        // module. Host-terminal; never relayed.
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: PlayerInventoryBlob payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        net::BlobChunkPayload ip{};
        std::memcpy(&ip, msg.payload, sizeof(ip));
        const uint8_t islot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::player_inventory_sync::OnReliable(ip, islot);
        break;
    }
    case net::ReliableKind::EmailDelete: {
        // v65: content-keyed email delete (player-symmetric, host-relayed).
        if (msg.payloadLen < sizeof(net::ContentHashPayload)) {
            UE_LOGW("event_feed: EmailDelete payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::ContentHashPayload));
            break;
        }
        net::ContentHashPayload dp{};
        std::memcpy(&dp, msg.payload, sizeof(dp));
        const uint8_t dslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::email_sync::OnDelete(dp, dslot);
        break;
    }
    case net::ReliableKind::SavedSignalAppend: {
        // v65: chunked saved-signal rows (producer-symmetric, host-relayed).
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: SavedSignalAppend payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        net::BlobChunkPayload cp{};
        std::memcpy(&cp, msg.payload, sizeof(cp));
        const uint8_t slot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::signal_sync::OnAppendChunk(cp, slot);
        break;
    }
    case net::ReliableKind::SavedSignalDelete: {
        // v65: content-keyed saved-signal delete (player-symmetric, host-relayed).
        if (msg.payloadLen < sizeof(net::ContentHashPayload)) {
            UE_LOGW("event_feed: SavedSignalDelete payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::ContentHashPayload));
            break;
        }
        net::ContentHashPayload hp{};
        std::memcpy(&hp, msg.payload, sizeof(hp));
        const uint8_t slot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::signal_sync::OnDelete(hp, slot);
        break;
    }
    case net::ReliableKind::CompState: {
        // v65: the decode-pane scalar stream (simulator-authoritative).
        if (msg.payloadLen < sizeof(net::CompStatePayload)) {
            UE_LOGW("event_feed: CompState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::CompStatePayload));
            break;
        }
        net::CompStatePayload sp{};
        std::memcpy(&sp, msg.payload, sizeof(sp));
        const uint8_t slot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::comp_sync::OnState(sp, slot);
        break;
    }
    case net::ReliableKind::CompData: {
        // v65: chunked comp_data_0 (change edges + host adopt).
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: CompData payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        net::BlobChunkPayload cp{};
        std::memcpy(&cp, msg.payload, sizeof(cp));
        const uint8_t slot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::comp_sync::OnDataChunk(cp, slot);
        break;
    }
    case net::ReliableKind::VoiceState: {
        // v66: voice mute/disabled display state (player-symmetric, host-relayed).
        if (msg.payloadLen < sizeof(net::VoiceStatePayload)) {
            UE_LOGW("event_feed: VoiceState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::VoiceStatePayload));
            break;
        }
        net::VoiceStatePayload vp{};
        std::memcpy(&vp, msg.payload, sizeof(vp));
        const uint8_t slot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::voice_chat::OnVoiceState(vp, slot);
        break;
    }
    // (DoorOpenRequest / KerfurConvertRequest / KerfurCommand / GrabIntent / ThrowIntent /
    // PileResyncRequest / PropDropIntent: moved to the INTENT family, event_dispatch_intent.cpp.)
    case net::ReliableKind::KerfurConvert: {
        // v78: HOST->ALL kerfur form-transition broadcast -- the SOLE conversion-transition signal
        // (kerfur redesign 10.3). CLIENT-only apply: the host converged its OWN conversion inline via
        // the silent host element ops, so it never applies its own broadcast. Host-authoritative:
        // accept only from slot 0 (the host).
        if (session.role() != net::Role::Client) {
            break;
        }
        if (msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: KerfurConvert from non-host senderPeerSlot=%d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::KerfurConvertBroadcastPayload)) {
            UE_LOGW("event_feed: KerfurConvert payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KerfurConvertBroadcastPayload));
            break;
        }
        net::KerfurConvertBroadcastPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::kerfur_convert::OnKerfurConvert(p, localPlayer);
        break;
    }
    default:
        return false;  // not a state-family kind -> event_feed tries the next family
    }
    return true;  // a state-family kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
