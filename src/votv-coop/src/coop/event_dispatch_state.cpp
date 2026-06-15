// coop/event_dispatch_state.cpp -- the keyed device-state reliable-kind case
// bodies (the KeyedToggle family, KeypadState, PowerControlState, AtvState,
// DroneState, OrderRequest, WindowCleanState, GrimeState, TrashPileState,
// DoorOpenRequest), extracted VERBATIM from event_feed.cpp's Update switch
// (2026-06-11 modularity extraction; see coop/event_dispatch.h).

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/atv_sync.h"
#include "coop/comp_sync.h"
#include "coop/console_state_sync.h"
#include "coop/signal_catch_sync.h"
#include "coop/sleep_sync.h"
#include "coop/device_occupancy.h"
#include "coop/email_sync.h"
#include "coop/player_inventory_sync.h"  // v73 inventory blob receiver
#include "coop/signal_sync.h"
#include "coop/voice/voice_chat.h"
#include "coop/drone_sync.h"
#include "coop/grime_sync.h"
#include "coop/interactable_sync.h"
#include "coop/kerfur_convert.h"
#include "coop/kerfur_command.h"
#include "coop/keypad_sync.h"
#include "coop/order_sync.h"
#include "coop/power_sync.h"
#include "coop/trash_pile_sync.h"
#include "coop/turbine_sync.h"
#include "coop/window_sync.h"

#include "ue_wrap/log.h"

#include <cmath>
#include <cstring>

namespace coop::event_feed {

void HandleStateEvent(net::Session& session,
                      const net::Session::ReliableMessage& msg) {
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
        // RE: research/findings/votv-doors-and-lightswitches-RE-2026-05-25.md.
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
    case net::ReliableKind::OrderRequest: {
        // v49 (2026-06-09): delivery-drone ECONOMY -- a CLIENT forwards a laptop shop order to the
        // HOST (the delivery authority). VARIABLE-LENGTH (OrderRequestHeader + packed items); the
        // host assembles chunks per (senderSlot, orderId) then re-commits via the native
        // makeAnOrder. order_sync::OnReliable fully range-checks the payload + no-ops on a client
        // (host-only ingest). RE: votv-delivery-drone-RE-and-coop-sync-design-2026-06-03.md.
        // OrderRequest is CLIENT->HOST: a valid sender is a CLIENT slot (1..kMaxPeers-1). Slot 0
        // is the host (which never sends its own order as a request -- it is the delivery
        // authority); an out-of-range / not-yet-assigned slot must NOT be routed under a 0xFF
        // sentinel that would create host assembly state in a shared bucket. Drop at the boundary.
        if (msg.senderPeerSlot < 1 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: OrderRequest from invalid senderPeerSlot=%d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        coop::order_sync::OnReliable(msg.payload, static_cast<int>(msg.payloadLen),
                                     static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
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
        // v64 (reshaped v70): the desk's live scalars. Claim-owner cadence /
        // any-peer button edges (host-relayed) / host adopt snapshot; the
        // holder-authority + adopt trust gates live in OnDeskState.
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
        // v64 increment 2: chunked email rows (producer-symmetric, host-relayed).
        // Assembly + echo-proof shadow registration live in email_sync::OnReliable.
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: EmailAppend payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
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
    case net::ReliableKind::DoorOpenRequest: {
        // v32 (2026-06-04): client->host door open/close REQUEST. Doors are HOST-
        // authoritative; only the host honors this. The host applies it (real lock/
        // jam guards) and its poll broadcasts the authoritative DoorState back to all.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: DoorOpenRequest received on a client -- dropping");
            break;
        }
        if (msg.payloadLen < sizeof(net::KeyedTogglePayload)) {
            UE_LOGW("event_feed: DoorOpenRequest payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KeyedTogglePayload));
            break;
        }
        net::KeyedTogglePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.action != 0 && p.action != 1) {
            UE_LOGW("event_feed: DoorOpenRequest action=%u out of range -- dropping",
                    static_cast<unsigned>(p.action));
            break;
        }
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::interactable_sync::OnDoorOpenRequest(p, senderSlot);
        break;
    }
    case net::ReliableKind::KerfurConvertRequest: {
        // v67: client->host kerfur on/off conversion request. Host-authoritative
        // (the DoorOpenRequest shape); the handler validates the element + class
        // + the BP kill-guard, runs the real verb, and converges the BP-internal
        // spawn/destroy side effects onto the wire (coop::kerfur_convert).
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: KerfurConvertRequest received on a client -- dropping");
            break;
        }
        if (msg.payloadLen < sizeof(net::KerfurConvertPayload)) {
            UE_LOGW("event_feed: KerfurConvertRequest payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KerfurConvertPayload));
            break;
        }
        net::KerfurConvertPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.toProp != 0 && p.toProp != 1) {
            UE_LOGW("event_feed: KerfurConvertRequest toProp=%u out of range -- dropping",
                    static_cast<unsigned>(p.toProp));
            break;
        }
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::kerfur_convert::OnConvertRequest(p, senderSlot);
        break;
    }
    case net::ReliableKind::KerfurCommand: {
        // v74: client->host kerfur radial-menu command (follow/idle/patrol/...). Host-authoritative;
        // the handler validates the element + kerfur class + the BP kill-guard, then runs the verb
        // (or, for Follow, starts the host-side MoveTo loop toward the REQUESTING player's body --
        // senderSlot). coop::kerfur_command.
        if (session.role() != net::Role::Host) {
            UE_LOGW("event_feed: KerfurCommand received on a client -- dropping");
            break;
        }
        if (msg.payloadLen < sizeof(net::KerfurCommandPayload)) {
            UE_LOGW("event_feed: KerfurCommand payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KerfurCommandPayload));
            break;
        }
        net::KerfurCommandPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::kerfur_command::OnCommandRequest(p, senderSlot);
        break;
    }
    default:
        break;  // not a state-family kind (caller routes only family members)
    }
}

}  // namespace coop::event_feed
