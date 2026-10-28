// coop/dispatch/event_dispatch_state.cpp -- the keyed device-state reliable-kind case bodies:
// the keyed toggle family, the keypad, the power panel, the ATV, the drone, the window and
// grime scalars, the trash pile counters, the kerfur convert, the device claim, the PC user's tab, sleep, email,
// inventory, voice, the host's server-scope settings, a machine's local dev grants, and an admin's stat orders and queries. The client-to-host intent cases live in
// event_dispatch_intent.cpp and the signal-pipeline cases in event_dispatch_signal.cpp; see
// coop/dispatch/event_dispatch.h.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/interactables/atv_sync.h"
#include "coop/player/sleep_sync.h"
#include "coop/interactables/device_occupancy.h"
#include "coop/world/email_sync.h"
#include "coop/items/player_inventory_sync.h"  // the inventory blob receiver
#include "coop/voice/voice_chat.h"
#include "coop/interactables/drone_sync.h"
#include "coop/interactables/grime_sync.h"
#include "coop/interactables/interactable_sync.h"
#include "coop/interactables/cord_sync.h"
#include "coop/interactables/swinger_join_pose.h"  // HOST->CLIENT an open lid's rest place
#include "coop/creatures/kerfur_convert_client.h"
#include "coop/creatures/kerfus_state.h"  // HOST->CLIENT a Kerfus's on, charging and energy
#include "coop/interactables/keypad_sync.h"
#include "coop/interactables/laptop_tab.h"  // HOST->CLIENT the PC user's tab
#include "coop/world/power_panel.h"
#include "coop/session/grants_sync.h"  // this machine's local dev grants, from the host
#include "coop/player/stat_orders.h"  // an admin's orders to a player's stats and effects, and their answers
#include "coop/session/server_settings_sync.h"  // the host's server-scope rows on a client
#include "coop/props/container_contents_sync.h"  // the container stack slice lane
#include "coop/props/trash_pile_sync.h"
#include "coop/interactables/turbine_sync.h"
#include "coop/interactables/window_stroke_sync.h"
#include "coop/interactables/window_sync.h"

#include "ue_wrap/core/log.h"

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
    case net::ReliableKind::OvenRepairState:    // the oven's repair, one way
    case net::ReliableKind::LightGroupState:    // the light group's active flag, host-authored
    case net::ReliableKind::CameraActiveState:  // a security camera's active bit, host-authored
    case net::ReliableKind::CameraLightState:   // a security camera's spotlight bit, host-authored
    case net::ReliableKind::LockerDoorState: {  // lockers and the drone-console doors, the same shape
        // DoorState, LightGroupState and the two camera kinds are the host-authored kinds in this
        // otherwise symmetric family, so they do not get the family's any-peer-may-send treatment: a
        // client that authored one would drive the host's and every other client's doors, lights or
        // cameras, the pollution host authority prevents. The host's receive edge refuses a client's
        // copy (client_authorship.h), and the host relays none of them, so here they come from the host.
        // A peer toggled a keyed interactable (a light switch, a container lid, a garage, an
        // appliance, a locker) or repaired an oven, or the host moved a door or a light group. On a symmetric kind any
        // peer can send, and the host relays a client-originated edge to the other clients before
        // this drain runs. interactable_sync
        // routes by kind to the right channel, resolves the instance by key and applies
        // idempotently on the game thread, echo-suppressed. The keypad is not here; it carries a
        // richer payload.
        if (msg.payloadLen < sizeof(net::KeyedTogglePayload)) {
            UE_LOGW("event_feed: %d payload too short (%zu < %zu)",
                    static_cast<int>(msg.kind),
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KeyedTogglePayload));
            break;
        }
        net::KeyedTogglePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // The trust boundary: the action is a byte, but only 0 and 1 are meaningful.
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
        // HOST->CLIENTS: what a keypad did on the host's copy -- a verb every client replays, or the
        // state a chain settled on. Only the host authors it; a client's own entries go up as
        // KeypadIntent. coop::keypad_sync::OnReliable.
        if (msg.payloadLen < sizeof(net::KeypadSyncPayload)) {
            UE_LOGW("event_feed: KeypadState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KeypadSyncPayload));
            break;
        }
        net::KeypadSyncPayload kp{};
        std::memcpy(&kp, msg.payload, sizeof(kp));
        // The format: a known event, a digit that is one, a buffer of digits, a password's length. The
        // password's bytes are UTF-8, decoded strictly where the state is applied, which refuses that
        // field alone when they are not well formed (coop/text/utf8_codec).
        bool ok = kp.event <= net::kKeypadEventMax && kp.bufLen <= sizeof(kp.buf) && kp.pwLen <= sizeof(kp.pw);
        if (ok && kp.event == static_cast<uint8_t>(net::KeypadEvent::Digit)) ok = kp.arg <= 9;
        if (ok && kp.event == static_cast<uint8_t>(net::KeypadEvent::State)) ok = kp.arg <= 1;  // handed on
        for (uint8_t i = 0; ok && i < kp.bufLen; ++i) ok = kp.buf[i] <= 9;
        if (!ok) {
            UE_LOGW("event_feed: KeypadState malformed (event=%u arg=%u bufLen=%u) -- dropping",
                    static_cast<unsigned>(kp.event), static_cast<unsigned>(kp.arg),
                    static_cast<unsigned>(kp.bufLen));
            break;
        }
        coop::keypad_sync::OnReliable(kp);
        break;
    }
    case net::ReliableKind::PowerControlState: {
        // The base power panel: a client's press (client to host) or the host's canonical (host to
        // all). Role, reach and trust gates live in power_panel::OnReliable.
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
        coop::power_panel::OnReliable(pp, senderSlot);
        break;
    }
    case net::ReliableKind::AtvState: {
        // The ATV body pose. Syncer-authoritative: the machine the host assigned streams it. A client's
        // block reaches the host alone, which applies it only from the ATV's syncer and re-sends it to
        // the other clients with its own stamp; a client receives only the host's. atv_sync gates the
        // apply on both sides.
        if (msg.payloadLen < sizeof(net::AtvStatePayload)) {
            UE_LOGW("event_feed: AtvState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::AtvStatePayload));
            break;
        }
        net::AtvStatePayload ap{};
        std::memcpy(&ap, msg.payload, sizeof(ap));
        // The velocity joins the pose in this guard: it reaches a physics velocity write on a body
        // that is genuinely simulating, so a non-finite component would not merely misplace a
        // kinematic mirror, it would poison the physics.
        if (!std::isfinite(ap.x) || !std::isfinite(ap.y) || !std::isfinite(ap.z) ||
            !std::isfinite(ap.pitch) || !std::isfinite(ap.yaw) || !std::isfinite(ap.roll) ||
            !std::isfinite(ap.linVelX) || !std::isfinite(ap.linVelY) || !std::isfinite(ap.linVelZ) ||
            !std::isfinite(ap.angVelX) || !std::isfinite(ap.angVelY) || !std::isfinite(ap.angVelZ)) {
            UE_LOGW("event_feed: AtvState non-finite pose/velocity -- dropping");
            break;
        }
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::atv_sync::OnReliable(ap, senderSlot);
        break;
    }
    case net::ReliableKind::AtvAnswer: {
        // HOST->CLIENT: the host's verdict on this client's AtvSeat or AtvTakeover, with the seat and the syncer
        // it holds for that ATV. Host-authored: a client's copy is refused at the host's receive edge
        // (client_authorship.h) and the host relays none. coop::atv_sync::OnAnswer.
        if (session.role() != net::Role::Client) {
            UE_LOGW("event_feed: AtvAnswer received on the host -- dropping");
            break;
        }
        if (msg.payloadLen < sizeof(net::AtvAnswerPayload)) {
            UE_LOGW("event_feed: AtvAnswer payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::AtvAnswerPayload));
            break;
        }
        net::AtvAnswerPayload aap{};
        std::memcpy(&aap, msg.payload, sizeof(aap));
        coop::atv_sync::OnAnswer(aap);
        break;
    }
    case net::ReliableKind::AtvSpawn: {
        // The host-to-client ATV announce, named by the ATV's game Key: a client that holds no ATV under
        // that Key fresh-spawns a native one carrying it.
        // Host-authoritative; a client's copy is refused at the host's receive edge (client_authorship.h).
        if (msg.payloadLen < sizeof(net::AtvSpawnPayload)) {
            UE_LOGW("event_feed: AtvSpawn payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::AtvSpawnPayload));
            break;
        }
        net::AtvSpawnPayload asp{};
        std::memcpy(&asp, msg.payload, sizeof(asp));
        coop::atv_sync::OnAtvSpawn(asp);
        break;
    }
    case net::ReliableKind::AtvDestroy: {
        // The host-to-client ATV teardown, named by its game Key. Host-authoritative; a client's copy is
        // refused at the host's receive edge (client_authorship.h).
        if (msg.payloadLen < sizeof(net::AtvDestroyPayload)) {
            UE_LOGW("event_feed: AtvDestroy payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::AtvDestroyPayload));
            break;
        }
        net::AtvDestroyPayload adp{};
        std::memcpy(&adp, msg.payload, sizeof(adp));
        coop::atv_sync::OnAtvDestroy(adp);
        break;
    }
    case net::ReliableKind::AtvSetComplete: {
        // The host's ATV set for this joiner is complete: what it did not name is removed. Host-authoritative; a
        // client's copy is refused at the host's receive edge (client_authorship.h).
        if (msg.payloadLen < sizeof(net::AtvSetCompletePayload)) {
            UE_LOGW("event_feed: AtvSetComplete payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::AtvSetCompletePayload));
            break;
        }
        net::AtvSetCompletePayload scp{};
        std::memcpy(&scp, msg.payload, sizeof(scp));
        coop::atv_sync::OnAtvSetComplete(scp);
        break;
    }
    case net::ReliableKind::DroneState: {
        // The delivery drone body pose, a host-authoritative singleton: host to client only, refused from a client
        // at the host's receive edge. The client suppresses its own drone tick and mirrors the streamed transform.
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
    case net::ReliableKind::WindowCleanState: {
        // The base-window dirt scalar. Symmetric cooperative cleaning: any peer polls its windows
        // and broadcasts a wipe (a decrease), and the host relays a client edge. The receiver
        // applies the minimum of local and wire (a live edge only ever cleans), or the value as
        // sent on the host's connect snapshot.
        if (msg.payloadLen < sizeof(net::KeyedScalarPayload)) {
            UE_LOGW("event_feed: WindowCleanState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KeyedScalarPayload));
            break;
        }
        net::KeyedScalarPayload wp{};
        std::memcpy(&wp, msg.payload, sizeof(wp));
        // The value drives a custom primitive data float, a shader uniform: a NaN or infinity there
        // is undefined visual output and can trip a device-removed crash on some GPUs, and the
        // engine floors the clean value at 0, so a negative value is garbage too.
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
    case net::ReliableKind::WindowStroke: {
        // One sponge dab on the bay window's dirt render target, from whichever peer stroked it
        // (the host relays a client's). The values land in a canvas draw, so each is bounded here:
        // a NaN or an out-of-texture size is undefined GPU work rather than a visible mistake.
        if (msg.payloadLen < sizeof(net::WindowStrokePayload)) {
            UE_LOGW("event_feed: WindowStroke payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::WindowStrokePayload));
            break;
        }
        net::WindowStrokePayload sp{};
        std::memcpy(&sp, msg.payload, sizeof(sp));
        const bool finite = std::isfinite(sp.x) && std::isfinite(sp.y) && std::isfinite(sp.size) &&
                            std::isfinite(sp.opac) && std::isfinite(sp.col);
        if (!finite || sp.size <= 0.0f || sp.size > 512.0f || sp.x < -512.0f || sp.x > 2157.0f ||
            sp.y < -512.0f || sp.y > 1024.0f || sp.opac < 0.0f || sp.opac > 4.0f ||
            sp.col < 0.0f || sp.col > 1.0f) {
            UE_LOGW("event_feed: WindowStroke out of range (x=%.1f y=%.1f size=%.1f opac=%.2f col=%.2f)"
                    " -- dropping", sp.x, sp.y, sp.size, sp.opac, sp.col);
            break;
        }
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::window_stroke_sync::OnReliable(sp, senderSlot);
        break;
    }
    case net::ReliableKind::GrimeState: {
        // The surface grime scalar. Symmetric cooperative cleaning keyed by a quantised world
        // position (a grime decal is static); the host relays a client edge. The receiver applies
        // the minimum and repaints. A decal wiped out of existence arrives on this same kind as a
        // zero, which the minimum makes invisible; there is no separate destroy.
        if (msg.payloadLen < sizeof(net::KeyedScalarPayload)) {
            UE_LOGW("event_feed: GrimeState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KeyedScalarPayload));
            break;
        }
        net::KeyedScalarPayload gp{};
        std::memcpy(&gp, msg.payload, sizeof(gp));
        // The value drives the material's shader parameter; guard NaN and negatives like the
        // window.
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
        // The trash pile collect counters. Symmetric, keyed by the pile's save key; the host relays
        // a client's collect. The receiver applies a per-component minimum for a live edge, or the
        // values as sent for the host's adopt snapshot, trust-gated inside.
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
        // The wind-turbine driver floats. Host-authoritative at about 1 Hz, refused from a client at the
        // host's receive edge like the drone (a client never legitimately sends it, and it is not relayed). The finite
        // validation and the client-only apply live in the module.
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
        // The enterable-device occupancy claim and release, both directions on one kind: a
        // client-to-host request (arbitrated against the host claim table, trusting the transport
        // sender slot) and a host-to-all verdict (clients trust-gate to slot 0 inside). Not
        // client-relayed.
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
    case net::ReliableKind::LaptopTabState: {
        // HOST->CLIENT: the tab the PC's user is on, shown by writing the switcher's index. Host-authored: the
        // host's receive edge refuses a client's copy (client_authorship.h) and the host relays none. The slot, the
        // holder, the index and the write live in the module; the format lives here. coop::laptop_tab::OnState.
        if (session.role() != net::Role::Client) {
            UE_LOGW("event_feed: LaptopTabState received on the host -- dropping");
            break;
        }
        if (msg.payloadLen < sizeof(net::LaptopTabPayload)) {
            UE_LOGW("event_feed: LaptopTabState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::LaptopTabPayload));
            break;
        }
        net::LaptopTabPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        const uint8_t tslot = (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                                  ? static_cast<uint8_t>(msg.senderPeerSlot)
                                  : static_cast<uint8_t>(0xFF);
        coop::laptop_tab::OnState(session, p, tslot);
        break;
    }
    case net::ReliableKind::SwingerRestPose: {
        // HOST->CLIENT: where an open lid rests on the host, sent after the container lane's connect snapshot.
        // Host-authored: the host's receive edge refuses a client's copy (client_authorship.h) and the host relays
        // none. The format is checked here (finite, a location inside the world's bound, the portable key); the
        // resolve, the open check and the placement live in coop::swinger_join_pose.
        if (session.role() != net::Role::Client) {
            UE_LOGW("event_feed: SwingerRestPose received on the host -- dropping");
            break;
        }
        if (msg.payloadLen < sizeof(net::SwingerRestPosePayload)) {
            UE_LOGW("event_feed: SwingerRestPose payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::SwingerRestPosePayload));
            break;
        }
        net::SwingerRestPosePayload rp{};
        std::memcpy(&rp, msg.payload, sizeof(rp));
        const float rvals[6] = { rp.locX, rp.locY, rp.locZ, rp.rotPitch, rp.rotYaw, rp.rotRoll };
        bool rfinite = true;
        for (float v : rvals) { if (!std::isfinite(v)) { rfinite = false; break; } }
        constexpr float kMaxRestCoord = 1.0e6f;   // the bound PropSnapPos holds a position to
        if (!rfinite || std::fabs(rp.locX) > kMaxRestCoord || std::fabs(rp.locY) > kMaxRestCoord ||
            std::fabs(rp.locZ) > kMaxRestCoord) {
            UE_LOGW("event_feed: SwingerRestPose pose not finite or out of bounds (%.1f,%.1f,%.1f) -- dropping",
                    rp.locX, rp.locY, rp.locZ);
            break;
        }
        if (rp.key.len < 3 || rp.key.len > sizeof(rp.key.data) || std::memcmp(rp.key.data, "mv_", 3) != 0) {
            UE_LOGW("event_feed: SwingerRestPose key is not a portable key (len %u) -- dropping",
                    static_cast<unsigned>(rp.key.len));
            break;
        }
        coop::swinger_join_pose::OnReliable(rp);
        break;
    }
    // The signal-pipeline kinds (the sky signals, the desk, the dish, the laptop, the saved
    // signals, the refiner) are in event_dispatch_signal.cpp.
    case net::ReliableKind::SleepState: {
        // A client's rest state to the host, the host's sleeping flag and one peer's rest state to
        // the clients; the length, role, slot and field checks live in the module.
        const uint8_t sslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::sleep_sync::OnReliable(msg.payload, msg.payloadLen, sslot);
        break;
    }
    case net::ReliableKind::EmailAppend: {
        // Host-authored: the append send is gated on the host role, and clients author none.
        // Assembly and the echo-proof shadow registration live in the module.
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: EmailAppend payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        // Emails are host-authored: a client's append is refused at the host's receive edge
        // (client_authorship.h), and the kind is not relayed.
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
        // A client streams its serialised inventory to the host, chunked. Assembly and persistence
        // (the per-guid file, with a magic, a checksum and a backup, rate-limited) live in the
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
    case net::ReliableKind::ContainerContents: {
        // A world container's stack slice, bidirectional: the peer whose add or take verb fired
        // authors it, and the host arbitrates (a base-hash compare-and-swap) before applying and
        // relaying to the others, the author excluded. So the sender-slot rule is asymmetric and
        // the module owns it: a client accepts slot 0 only, the host accepts a non-zero slot only.
        // Not host-authored: under that rule a client's every extraction was silently dropped. The
        // module also owns the world-versus-personal boundary; see container_contents_sync.h.
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: ContainerContents payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        net::BlobChunkPayload cc{};
        std::memcpy(&cc, msg.payload, sizeof(cc));
        const uint8_t ccslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::props::container_contents_sync::OnContentsChunk(cc, ccslot);
        break;
    }
    case net::ReliableKind::EmailDelete: {
        // The content-keyed email delete, player-symmetric and host-relayed.
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
    case net::ReliableKind::VoiceState: {
        // The voice mute and disabled display state, player-symmetric and host-relayed.
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
    // The client-to-host requests (the door open, the kerfur convert and command, the grab and
    // throw intents, the prop drop, and the pile resync kept for its planned sender) are in
    // event_dispatch_intent.cpp.
    case net::ReliableKind::KerfurConvert: {
        // The host-to-all kerfur form-transition broadcast, the sole conversion-transition signal. Client-only
        // apply: the host converged its own conversion inline through the silent element ops, so it never applies
        // its own broadcast. Host-authoritative: a client's copy is refused at the host's receive edge
        // (client_authorship.h).
        if (msg.payloadLen < sizeof(net::KerfurConvertBroadcastPayload)) {
            UE_LOGW("event_feed: KerfurConvert payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KerfurConvertBroadcastPayload));
            break;
        }
        net::KerfurConvertBroadcastPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        coop::kerfur_convert_client::OnKerfurConvert(p, localPlayer);
        break;
    }
    case net::ReliableKind::KerfusState: {
        // The host's state for one Kerfus: on, charging, energy. Client-only apply (a client runs none of the
        // Kerfus's brain); host-authoritative, a client's copy is refused at the host's receive edge
        // (client_authorship.h). coop::kerfus_state.
        if (msg.payloadLen < sizeof(net::KerfusStatePayload)) {
            UE_LOGW("event_feed: KerfusState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::KerfusStatePayload));
            break;
        }
        net::KerfusStatePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (!std::isfinite(p.energy)) {
            UE_LOGW("event_feed: KerfusState eid=%u with a non-finite energy -- dropping", p.elementId);
            break;
        }
        coop::kerfus_state::OnState(p);
        break;
    }
    case net::ReliableKind::CordLinkState: {
        // A cord's link, one kind in three ops by direction: op 0 (a client's claim) reaches the host alone, ops 1
        // (a canonical) and 2 (the seed's end) reach a client from the host alone. The format and the direction are
        // judged here, every other check in the module; a message that fails either is counted there under its
        // reason (cord_sync.h, Drop), not logged, so a flood costs no line. coop::cord_sync::OnReliable.
        if (msg.payloadLen != sizeof(net::CordLinkPayload)) {
            coop::cord_sync::CountDropped(coop::cord_sync::Drop::Size);
            break;
        }
        net::CordLinkPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        const bool validSender = msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers;
        const bool fromClient = validSender && msg.senderPeerSlot != 0;
        const bool fromHost = validSender && msg.senderPeerSlot == 0;
        const bool direction = (p.op == 0 && session.role() == net::Role::Host && fromClient) ||
                               ((p.op == 1 || p.op == 2) && session.role() == net::Role::Client && fromHost);
        // The fields a form leaves empty are empty: an unplugged link names no socket, the seed's end carries
        // nothing, and a claim is numbered from 1 (seq 0 is a broadcast's).
        const bool fieldsAgree = !(p.sockForm == 0 && (p.sock.len != 0 || p.child.len != 0)) &&
                                 !(p.op == 2 && (p.sockForm != 0 || p.seq != 0 || p.cord.len != 0 ||
                                                 p.sock.len != 0 || p.child.len != 0)) &&
                                 !(p.op == 0 && p.seq == 0);
        using coop::cord_sync::Drop;
        const bool keyTooLong = p.cord.len > sizeof(p.cord.data) || p.sock.len > sizeof(p.sock.data) ||
                                p.child.len > sizeof(p.child.data);
        if (keyTooLong || !direction || !fieldsAgree || p.pad != 0 || p.sockForm > 2) {
            coop::cord_sync::CountDropped(keyTooLong ? Drop::Key : !direction ? Drop::Op : Drop::Field);
            break;
        }
        coop::cord_sync::OnReliable(p, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::ServerSetting:
        // The host's session value of one server-scope row; the module checks length and row, and
        // a client's copy is refused at the host's receive edge. coop::server_settings_sync.
        coop::server_settings_sync::HandleServerSetting(msg);
        break;
    case net::ReliableKind::PermissionGrants:
        // This machine's own local dev grants, from the host; the module checks length and count,
        // and a client's copy is refused at the host's receive edge. coop::session::grants_sync.
        coop::session::grants_sync::HandleGrants(session, msg);
        break;
    case net::ReliableKind::StatOrder:
        // The host's order to this player's stats or effects; the module checks the bytes, and a
        // client's copy is refused at the host's receive edge. coop::stat_orders.
        coop::stat_orders::HandleOrder(session, msg);
        break;
    case net::ReliableKind::StatOrderReply:
        // A client's answer to an order, matched to its token on the host. coop::stat_orders.
        coop::stat_orders::HandleOrderReply(session, msg);
        break;
    case net::ReliableKind::StatQuery:
        // The host's read of this player's stat table and effects. coop::stat_orders.
        coop::stat_orders::HandleQuery(session, msg);
        break;
    case net::ReliableKind::StatQueryReply:
        // A client's answer to a query, matched to its token on the host. coop::stat_orders.
        coop::stat_orders::HandleQueryReply(session, msg);
        break;
    default:
        return false;  // not a state-family kind -> event_feed tries the next family
    }
    return true;  // a state-family kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
