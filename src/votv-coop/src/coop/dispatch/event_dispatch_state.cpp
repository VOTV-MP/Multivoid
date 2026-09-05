// coop/dispatch/event_dispatch_state.cpp -- the keyed device-state reliable-kind case bodies:
// the keyed toggle family, the keypad, the power panel, the ATV, the drone, the window and
// grime scalars, the trash pile counters, the kerfur convert, the device claim, sleep, email,
// inventory and voice. The client-to-host intent cases live in event_dispatch_intent.cpp and
// the signal-pipeline cases in event_dispatch_signal.cpp; see coop/dispatch/event_dispatch.h.

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
#include "coop/creatures/kerfur_convert_client.h"
#include "coop/interactables/keypad_sync.h"
#include "coop/interactables/power_sync.h"
#include "coop/props/container_contents_sync.h"  // the container stack slice lane
#include "coop/props/trash_pile_sync.h"
#include "coop/interactables/turbine_sync.h"
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
    case net::ReliableKind::LightGroupState:    // the light group's active flag, host-authored
    case net::ReliableKind::LockerDoorState: {  // lockers and the drone-console doors, the same shape
        // LightGroupState is the one host-authored kind in this otherwise symmetric family, so it
        // does not get the family's any-peer-may-send treatment: a client that authored it would
        // drive the host's and every other client's lights, the pollution host authority prevents.
        // The channel's receive performs no role or sender check of its own, so the drop is here,
        // at the family boundary; refused on a receiving client too, since the host relays nothing
        // on this kind, so a packet from a non-host slot is not ours either way.
        if (msg.kind == net::ReliableKind::LightGroupState && msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: LightGroupState from slot %d refused -- this kind is host-authored",
                    msg.senderPeerSlot);
            return true;  // claimed by this family, deliberately not applied
        }
        // A peer toggled a keyed interactable (a base door, a light group, a container lid, a
        // garage, an appliance). Symmetric: any peer can send, and the host relays a
        // client-originated edge to the other clients before this drain runs. interactable_sync
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
        // The password-keypad input mirror. Symmetric: any peer polls the entered digits and
        // broadcasts on a buffer change, and the host relays a client edge. The receiver replays
        // the digit input for the delta, which drives the keypad's own native validator (so the
        // host accepts a client's code itself, input replication), and an accept or deny event runs
        // the native open chain, the short-code submit mirror.
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
        // The base power-panel breakers. Symmetric: any peer polls its panels' press bools and
        // broadcasts on a change, and the host relays a client edge. The receiver writes the bools
        // and refreshes the panel's own visual; the base power effects sync through their own door,
        // light and server channels. Five bools per actor, so its own module rather than the
        // one-bool toggle channel.
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
        // The ATV body pose. Occupant-authoritative: the seated driver streams, and the host relays
        // a client driver's pose to the other clients. Trust the edge (any peer may legitimately
        // send the ATV it drives); atv_sync gates the apply and ignores a pose for an ATV this peer
        // is itself driving.
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
    case net::ReliableKind::AtvRelease: {
        // The authority-lost edge: the sender no longer authors this ATV. It carries the key and
        // nothing else; the receiver clears the seat and the author (atv_sync ignores it if this
        // peer is itself the author). There is no velocity to validate, since there is no un-freeze
        // to hand a launch velocity to. The host relays a client's release to the other clients,
        // like the pose.
        if (msg.payloadLen < sizeof(net::AtvReleasePayload)) {
            UE_LOGW("event_feed: AtvRelease payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::AtvReleasePayload));
            break;
        }
        net::AtvReleasePayload arp{};
        std::memcpy(&arp, msg.payload, sizeof(arp));
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::atv_sync::OnAtvRelease(arp, senderSlot);
        break;
    }
    case net::ReliableKind::AtvSpawn: {
        // The host-to-client runtime-ATV announce: the client fresh-spawns a native ATV it has no
        // save twin of (spawned at runtime from the props table; nothing sells an ATV).
        // Host-authoritative, slot 0 only.
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
        // The host-to-client runtime-ATV teardown. Host-authoritative, slot 0 only.
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
        // The delivery drone body pose, a host-authoritative singleton: host to client only,
        // trust-gated to slot 0. The client suppresses its own drone tick and mirrors the streamed
        // transform.
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
    case net::ReliableKind::GrimeState: {
        // The surface grime scalar. Symmetric cooperative cleaning keyed by a quantised world
        // position (a grime decal is static); the host relays a client edge. The receiver applies
        // the minimum and repaints. A decal destroy is deferred (see grime_sync.h): grime streams
        // in and out, so a vanished decal is not a reliable destroy signal.
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
        // The wind-turbine driver floats. Host-authoritative at about 1 Hz, trust-gated to slot 0
        // like the drone (a client never legitimately sends it, and it is not relayed). The finite
        // validation and the client-only apply live in the module.
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
    // The signal-pipeline kinds (the sky signals, the desk, the dish, the laptop, the saved
    // signals, the refiner) are in event_dispatch_signal.cpp.
    case net::ReliableKind::SleepState: {
        // The sleep gate: a peer's report to the host, and the host's tally, accelerate and end to
        // all; the role and slot trust gates live in the module.
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
    case net::ReliableKind::EmailAppend: {
        // Host-authored: the append send is gated on the host role, and clients author none.
        // Assembly and the echo-proof shadow registration live in the module.
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: EmailAppend payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        // The authority-boundary drop: a client append reaching the host is a protocol violation,
        // dropped so one client-side regression cannot re-pollute the shared inbox; the kind is not
        // relayed either.
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
    // throw intents, the pile resync, the prop drop) are in event_dispatch_intent.cpp.
    case net::ReliableKind::KerfurConvert: {
        // The host-to-all kerfur form-transition broadcast, the sole conversion-transition signal.
        // Client-only apply: the host converged its own conversion inline through the silent
        // element ops, so it never applies its own broadcast. Host-authoritative: slot 0 only.
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
        coop::kerfur_convert_client::OnKerfurConvert(p, localPlayer);
        break;
    }
    default:
        return false;  // not a state-family kind -> event_feed tries the next family
    }
    return true;  // a state-family kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
