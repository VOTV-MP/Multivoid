// coop/event_feed.cpp -- the per-tick reliable-message DRAIN: per-slot join/leave
// edges, then the exhaustive ReliableKind dispatch switch. The kind->handler table
// lives HERE (one greppable place); the case BODIES for the three big families
// (entity lifecycle / keyed device state / ambient world events) live in
// event_dispatch_{entity,state,world}.cpp (2026-06-11 modularity extraction --
// this file had reached 1388 LOC, 112 from the 1500 hard cap). Core session /
// handshake / snapshot / dev-key cases stay inline below.

#include "coop/event_feed.h"

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/balance_sync.h"
#include "coop/chat_feed.h"
#include "coop/interactable_sync.h"
#include "coop/join_progress.h"
#include "coop/net/session.h"
#include "coop/subsystems.h"
#include "coop/npc_adoption.h"  // v75: OnSnapshotComplete (deferred-adoption ghost-sweep gate)
#include "coop/player_damage.h"
#include "coop/wisp_tear_mirror.h"  // v72 Killer Wisp: WispGrab/WispTear receivers
#include "coop/player_handshake.h"
#include "coop/players_registry.h"
#include "coop/remote_player.h"
#include "coop/remote_prop_spawn.h"
#include "coop/save_transfer.h"
#include "coop/dev/restore_vitals.h"
#include "coop/dev/teleport_client.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace coop::event_feed {

namespace {

// Per-slot connect/disconnect edge detector. The Join-sent latch and
// per-slot nicknames live in coop::player_handshake (C5 extraction
// 2026-05-29); event_feed only needs the prior-connected bit to fire
// the "<X> left the game" hud message on the disconnect transition.
std::array<bool, net::kMaxPeers> g_lastConnectedBySlot{};

}  // namespace

void SetLocalNickname(const std::wstring& nick) {
    coop::player_handshake::SetLocalNickname(nick);
}

void OnSessionStart() {
    // File-scope per-slot state persists across Session::Stop()/Start() in
    // the same process. The harness only resets its own `g_wasConnected`
    // bit on each Start; we own the corresponding event_feed state and
    // reset it here so a restart of the session sees clean per-slot
    // edge-detector input.
    g_lastConnectedBySlot.fill(false);
    coop::player_handshake::Reset();
    coop::chat_feed::Reset();  // drop any prior session's lingering event lines
}

void Update(net::Session& session, void* localPlayer) {
    // Push each peer's OWN RTT to its puppet so the nameplate shows "<nick> (<ping>ms)"
    // and the scoreboard the per-row ping. Session samples per-slot RTT ~1 Hz from GNS
    // m_nPing; rttMsForSlot returns -1 until a slot is sampled (nameplate then shows no
    // ms suffix) and 0 on a sub-millisecond LAN link (shown as "<1ms").
    for (int slot = 0; slot < net::kMaxPeers; ++slot) {
        RemotePlayer* p = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
        if (p) p->SetPing(session.rttMsForSlot(slot));
    }

    // Per-slot Join announcement + per-slot "left the game" hud. The
    // Join payload is built lazily on first need: in steady state
    // (all peers have received our Join) the joinPayload vector is
    // never constructed, so we don't pay the ToUtf8 + heap-alloc cost
    // every 8 ms at 125 Hz NetPumpTick.
    std::vector<uint8_t> joinPayload;
    bool joinPayloadBuilt = false;
    for (int slot = 0; slot < net::kMaxPeers; ++slot) {
        // Two distinct edges:
        // - LEFT toast / disconnect cleanup: IsSlotConnected (cleared in
        //   the Closed callback) is the right signal.
        // - Send Join: must wait for IsSlotReady (lanes configured in the
        //   Connected callback), otherwise SendReliableToSlot's m_idxLane
        //   rides GNS lane 0 instead of the assigned Normal lane,
        //   undermining PR-3's head-of-line isolation for the first
        //   reliable message per peer. N-4 (2026-05-29 audit).
        const bool slotConnected = session.IsSlotConnected(slot);
        const bool slotReady = session.IsSlotReady(slot);
        if (g_lastConnectedBySlot[slot] && !slotConnected) {
            coop::chat_feed::Push(
                coop::player_handshake::NicknameForSlot(slot) + L" left the game");
            coop::player_handshake::OnSlotDisconnected(slot);
            // HostAuth doors: release any door this departing peer was holding open (a door
            // still held by another peer stays open; one whose last holder just left closes).
            coop::interactable_sync::OnPeerLeft(slot);
        }
        if (slotReady) {
            coop::player_handshake::MaybeSendJoinToSlot(
                session, slot, joinPayload, joinPayloadBuilt);
        }
        g_lastConnectedBySlot[slot] = slotConnected;
    }

    // Drain delivered reliable messages. The switch is the ONE kind->handler
    // table; the three Handle*Event families re-switch internally on msg.kind
    // (see event_dispatch.h for the family membership).
    net::Session::ReliableMessage msg;
    while (session.TryGetReliable(msg)) {
        switch (msg.kind) {
        case net::ReliableKind::Join: {
            coop::player_handshake::HandleJoinMessage(session, msg);
            break;
        }
        case net::ReliableKind::SaveTransferRequest: {
            // v56: a menu-mode joiner asks for the world save. Host-only intake;
            // the stream itself is pumped by net_pump (save_transfer::TickHost).
            if (session.role() == net::Role::Host &&
                msg.senderPeerSlot >= 1 && msg.senderPeerSlot < net::kMaxPeers) {
                coop::save_transfer::OnRequest(msg.senderPeerSlot);
            }
            break;
        }
        case net::ReliableKind::SaveTransferBegin: {
            // v56: the blob header (client side). Chunks bypass this inbox
            // entirely (the session bulk sink).
            if (msg.payloadLen < sizeof(net::SaveTransferBeginPayload)) break;
            net::SaveTransferBeginPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            coop::save_transfer::OnBegin(p);
            break;
        }
        case net::ReliableKind::ClientWorldReady: {
            // v56: the joiner's world is up + registry-coherent -- open the
            // world-ready send gate and run the connect replay NOW (this
            // replaces the pre-v56 connect-edge replay).
            if (session.role() == net::Role::Host &&
                msg.senderPeerSlot >= 1 && msg.senderPeerSlot < net::kMaxPeers) {
                session.MarkSlotWorldReady(msg.senderPeerSlot, true);
                coop::subsystems::ConnectReplayForSlot(msg.senderPeerSlot);
            }
            break;
        }

        // ---- entity lifecycle + held items (event_dispatch_entity.cpp) ----
        case net::ReliableKind::PropRelease:
        case net::ReliableKind::PropSpawn:
        case net::ReliableKind::PropDestroy:
        case net::ReliableKind::PropConvert:
        case net::ReliableKind::PropStickState:
        case net::ReliableKind::EntitySpawn:
        case net::ReliableKind::EntityDestroy:
        case net::ReliableKind::ItemActivate: {
            HandleEntityEvent(session, msg, localPlayer);
            break;
        }

        case net::ReliableKind::RestoreVitals: {
            // 2026-05-25 LATE +5h (F3 dev key): peer pressed F3 to refill
            // food/sleep/health/coffeePower. No payload to validate -- the
            // action is fixed. Idempotent so an echo bounce is harmless.
            // RestoreVitals is a dev-key path (host presses F3); receiver
            // must enforce host-only origin or any peer could trivially
            // nullify hunger/survival tension.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: RestoreVitals from non-host senderPeerSlot=%d "
                        "-- dropping (host-only dev-key origin)",
                        msg.senderPeerSlot);
                break;
            }
            ue_wrap::game_thread::Post([] { ::coop::dev::restore_vitals::ApplyLocally(); });
            break;
        }
        case net::ReliableKind::PlayerDamage: {
            // vitals Inc3-WIRE: the host detected an enemy hitting THIS peer's puppet
            // and relayed the damage for us to apply to our OWN player. Host-only
            // origin (only the host runs enemies); a client never legitimately sends
            // it, and it is not in the relay whitelist so it is never forwarded. The
            // owner-side targetElementId check + the apply live in player_damage.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: PlayerDamage from non-host senderPeerSlot=%d "
                        "-- dropping (host-only combat origin)", msg.senderPeerSlot);
                break;
            }
            if (msg.payloadLen < sizeof(net::PlayerDamagePayload)) {
                UE_LOGW("event_feed: PlayerDamage payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::PlayerDamagePayload));
                break;
            }
            net::PlayerDamagePayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            coop::player_damage::OnWireDamage(p);
            break;
        }
        case net::ReliableKind::WispGrab: {
            // v72 Killer Wisp: the host's killerwisp false-grabbed THIS peer's puppet; the
            // host neutralized its own death and tells us to ragdoll-die for real after a
            // fixed delay. Host-only origin (only the host runs the wisp + the detect);
            // wisp_tear_mirror self-verifies the addressed Player Element id. Not relayed.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: WispGrab from non-host senderPeerSlot=%d -- dropping "
                        "(host-only wisp origin)", msg.senderPeerSlot);
                break;
            }
            if (msg.payloadLen < sizeof(net::WispGrabPayload)) {
                UE_LOGW("event_feed: WispGrab payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::WispGrabPayload));
                break;
            }
            net::WispGrabPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            const uint8_t sender = msg.senderPeerSlot;
            ue_wrap::game_thread::Post([p, sender] { coop::wisp_tear_mirror::OnWispGrab(p, sender); });
            break;
        }
        case net::ReliableKind::WispTear: {
            // v72 Killer Wisp: play the fatality tear on the LOCAL wisp mirror + (deferred)
            // hold the victim's puppet. Host-only origin; resolves the wisp by element id;
            // not relayed.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: WispTear from non-host senderPeerSlot=%d -- dropping "
                        "(host-only wisp origin)", msg.senderPeerSlot);
                break;
            }
            if (msg.payloadLen < sizeof(net::WispTearPayload)) {
                UE_LOGW("event_feed: WispTear payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::WispTearPayload));
                break;
            }
            net::WispTearPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            const uint8_t sender = msg.senderPeerSlot;
            ue_wrap::game_thread::Post([p, sender] { coop::wisp_tear_mirror::OnWispTear(p, sender); });
            break;
        }
        case net::ReliableKind::BalanceSync: {
            // v30 shared balance: the host broadcast its canonical Points -- mirror it.
            // Host-only origin (the host owns the balance); a client never sends it.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: BalanceSync from non-host senderPeerSlot=%d -- dropping",
                        msg.senderPeerSlot);
                break;
            }
            if (msg.payloadLen < sizeof(net::BalancePayload)) {
                UE_LOGW("event_feed: BalanceSync payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::BalancePayload));
                break;
            }
            net::BalancePayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            coop::balance_sync::ApplyFromHost(p.value);  // no-op on the host (authoritative)
            break;
        }
        case net::ReliableKind::BalanceDelta: {
            // v30 shared balance: a client requested a credit (the +1000 dev button) --
            // the host applies it via AddPoints (its poll re-broadcasts the new total).
            // Client->host; OnDeltaRequest no-ops unless we are the host.
            if (msg.payloadLen < sizeof(net::BalancePayload)) {
                UE_LOGW("event_feed: BalanceDelta payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::BalancePayload));
                break;
            }
            net::BalancePayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            coop::balance_sync::OnDeltaRequest(p.value);
            break;
        }
        case net::ReliableKind::TeleportClient: {
            // 2026-05-25 LATE +5h (F4 dev key): host snapshotted its pose and
            // sent it; client applies to local mainPlayer. Host echo is
            // a no-op below.
            if (msg.payloadLen < sizeof(net::TeleportClientPayload)) {
                UE_LOGW("event_feed: TeleportClient payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::TeleportClientPayload));
                break;
            }
            // TeleportClient is host-only. Without a senderPeerSlot trust
            // gate, in a 3-peer session client-slot-1 could craft a
            // TeleportClient targeting client-slot-2 (host's PollGroup
            // fan-out would deliver it) -- a positional griefing/exploit
            // vector at LAN scale, hard exploit at internet scale.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: TeleportClient from non-host senderPeerSlot=%d "
                        "-- dropping (host-only)",
                        msg.senderPeerSlot);
                break;
            }
            net::TeleportClientPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            // Trust-boundary validation (same defensive pattern as the
            // PropRelease velocity check): reject NaN/Inf before the engine
            // call. UE's K2_TeleportTo with a NaN location asserts inside
            // FSweepData::ClampSweepParameters.
            const float vals[6] = {p.locX, p.locY, p.locZ, p.rotPitch, p.rotYaw, p.rotRoll};
            bool finite = true;
            for (float v : vals) { if (!std::isfinite(v)) { finite = false; break; } }
            if (!finite) {
                UE_LOGW("event_feed: TeleportClient payload non-finite -- dropping");
                break;
            }
            // AABB bound (audit-fix 2026-05-25 LATE +5h): finite-check alone
            // allows extreme-but-finite coords (e.g. 1e30) that would still
            // assert inside the engine's teleport math. Mirror the project's
            // own kMaxCoord = 1.0e6f trust boundary from ValidatePose /
            // PropSpawnPayload receiver -- one consistent magnitude rule for
            // any world-position payload. Rotations don't need a magnitude
            // bound because FRotator components are angles (any value is
            // normalized inside K2_TeleportTo).
            if (std::fabs(p.locX) > net::kMaxCoord ||
                std::fabs(p.locY) > net::kMaxCoord ||
                std::fabs(p.locZ) > net::kMaxCoord) {
                UE_LOGW("event_feed: TeleportClient location out of bounds (%.1f,%.1f,%.1f) -- dropping",
                        p.locX, p.locY, p.locZ);
                break;
            }
            // Host echo gate: if WE are the host, this packet originated from
            // us (broadcast bounced back via the reliable channel). Applying
            // would teleport host to its own pose -- harmless but pointless.
            // Skip explicitly so we don't accidentally collide with whatever
            // host was doing the moment it pressed F4.
            if (session.role() == net::Role::Host) {
                UE_LOGI("event_feed: TeleportClient self-echo on host -- no-op");
                break;
            }
            ::coop::dev::teleport_client::ApplyArgs args{
                p.locX, p.locY, p.locZ,
                p.rotPitch, p.rotYaw, p.rotRoll,
            };
            ue_wrap::game_thread::Post([args] { ::coop::dev::teleport_client::ApplyLocally(args); });
            break;
        }
        case net::ReliableKind::SnapshotBegin: {
            // v34: host opened the connect-snapshot -- flip the client loading screen from
            // "Connecting" to a determinate "Receiving world X/N" bar. Host-only, like
            // TeleportClient: a client peer must not be able to spoof another's loading UI.
            if (msg.payloadLen < sizeof(net::SnapshotBeginPayload)) {
                UE_LOGW("event_feed: SnapshotBegin payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::SnapshotBeginPayload));
                break;
            }
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: SnapshotBegin from non-host senderPeerSlot=%d -- dropping",
                        msg.senderPeerSlot);
                break;
            }
            if (session.role() == net::Role::Host) break;  // self-echo guard (host doesn't load-screen)
            net::SnapshotBeginPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            coop::join_progress::BeginSnapshot(p.propTotal);
            // P2 (2026-06-10): arm the connect-snapshot claim set. Every
            // PropSpawn dispatched below (inline, same drain, same lane ->
            // strictly after this) claims the client actor it binds; the
            // SnapshotComplete sweep destroys the unclaimed RNG-divergent
            // locals so the client adopts the host's world layout.
            coop::remote_prop_spawn::BeginClaimTracking();
            break;
        }
        case net::ReliableKind::SnapshotComplete: {
            // v34: host finished draining the world snapshot (the LAST Lane::Bulk message
            // after every PropSpawn) -- lift the loading screen. THE hide edge.
            if (msg.payloadLen < sizeof(net::SnapshotEndPayload)) {
                UE_LOGW("event_feed: SnapshotComplete payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::SnapshotEndPayload));
                break;
            }
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: SnapshotComplete from non-host senderPeerSlot=%d -- dropping",
                        msg.senderPeerSlot);
                break;
            }
            if (session.role() == net::Role::Host) break;
            coop::join_progress::Complete();
            // P2 (2026-06-10): the snapshot drained -- every host prop has
            // claimed its client actor. Destroy the unclaimed RNG-divergent
            // locals (the client's own fresh-New-Game litter the host does
            // not have). Inline on the GT drain, after ALL claims by lane
            // ordering.
            coop::remote_prop_spawn::DestroyUnclaimedDivergentProps(localPlayer);
            // v75: the connect snapshot is fully delivered, so every save-persisted EntitySpawn
            // (the kerfur) has arrived + armed its deferred adoption. Tell npc_adoption: once those
            // pending adoptions converge it may run its one-shot ghost sweep (the NPC analogue of
            // DestroyUnclaimedDivergentProps above) without risking a not-yet-armed local twin.
            coop::npc_adoption::OnSnapshotComplete();
            break;
        }

        // ---- keyed device state (event_dispatch_state.cpp) ----
        case net::ReliableKind::DoorState:
        case net::ReliableKind::LightState:
        case net::ReliableKind::ContainerState:
        case net::ReliableKind::GarageDoorState:
        case net::ReliableKind::ApplianceState:
        case net::ReliableKind::LockerDoorState:
        case net::ReliableKind::KeypadState:
        case net::ReliableKind::PowerControlState:
        case net::ReliableKind::AtvState:
        case net::ReliableKind::AtvRelease:
        case net::ReliableKind::DroneState:
        case net::ReliableKind::OrderRequest:
        case net::ReliableKind::WindowCleanState:
        case net::ReliableKind::GrimeState:
        case net::ReliableKind::TrashPileState:
        case net::ReliableKind::TurbineState:
        case net::ReliableKind::DeviceClaim:
        case net::ReliableKind::SkySignalState:
        case net::ReliableKind::SkySignalCatch:
        case net::ReliableKind::DeskState:
        case net::ReliableKind::DishAimState:
        case net::ReliableKind::EmailAppend:
        case net::ReliableKind::EmailDelete:
        case net::ReliableKind::SavedSignalAppend:
        case net::ReliableKind::SavedSignalDelete:
        case net::ReliableKind::CompState:
        case net::ReliableKind::CompData:
        case net::ReliableKind::VoiceState:
        case net::ReliableKind::DoorOpenRequest:
        case net::ReliableKind::KerfurConvertRequest:
        case net::ReliableKind::KerfurCommand:  // v74 host-authoritative kerfur menu command relay
        case net::ReliableKind::DeskLogLine:   // v70 (route fix 2026-06-13: missed at ship -- the idle smokes never exercised the wire)
        case net::ReliableKind::SleepState:    // v71
        case net::ReliableKind::PlayerInventoryBlob: {  // v73 per-player inventory stream
            HandleStateEvent(session, msg);
            break;
        }

        // ---- ambient / world events (event_dispatch_world.cpp) ----
        case net::ReliableKind::FireflySpawn:
        case net::ReliableKind::InventoryPickup:
        case net::ReliableKind::ChatMessage:
        case net::ReliableKind::TimeSync:
        case net::ReliableKind::SkyState:
        case net::ReliableKind::RedSky:
        case net::ReliableKind::LightningStrike:
        case net::ReliableKind::WeatherState: {
            HandleWorldEvent(session, msg);
            break;
        }

        case net::ReliableKind::AssignPeerSlot: {
            coop::player_handshake::HandleAssignPeerSlot(session, msg);
            break;
        }
        case net::ReliableKind::PlayerJoined: {
            // PR-FOUNDATION Tier 2 T2-1: host-relayed cross-peer identity.
            coop::player_handshake::HandlePlayerJoined(session, msg);
            break;
        }
        default: {
            // Audit-fix 2026-05-25 LATE +5h: log-and-drop unknown ReliableKind
            // values instead of silently discarding. A peer running a newer
            // protocol could send a kind we don't yet handle; this surfaces
            // the gap in the log rather than letting it look like nothing
            // happened. Existing pattern across the project.
            UE_LOGW("event_feed: unknown ReliableKind %u -- dropping",
                    static_cast<unsigned>(msg.kind));
            break;
        }
        }
    }
}

}  // namespace coop::event_feed
