#include "coop/event_feed.h"

#include "coop/element/player.h"
#include "coop/element/registry.h"

#include "coop/balance_sync.h"
#include "coop/interactable_sync.h"
#include "coop/join_progress.h"
#include "coop/atv_sync.h"
#include "coop/drone_sync.h"
#include "coop/order_sync.h"
#include "coop/keypad_sync.h"
#include "coop/item_activate.h"
#include "coop/net/session.h"
#include "coop/power_sync.h"
#include "coop/player_damage.h"
#include "coop/npc_mirror.h"
#include "coop/net_pump.h"
#include "coop/player_handshake.h"
#include "coop/save_transfer.h"
#include "coop/players_registry.h"
#include "coop/remote_player.h"
#include "coop/remote_prop.h"
#include "coop/remote_prop_spawn.h"
#include "coop/trash_collect_sync.h"
#include "coop/trash_pile_sync.h"
#include "coop/grime_sync.h"
#include "coop/firefly_sync.h"
#include "coop/sky_sync.h"
#include "coop/time_sync.h"
#include "coop/weather_sync.h"
#include "coop/window_sync.h"
#include "coop/dev/restore_vitals.h"
#include "coop/dev/teleport_client.h"
#include "ue_wrap/game_thread.h"
#include "coop/chat_feed.h"
#include "ue_wrap/log.h"
#include "ue_wrap/sdk_profile.h"

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

// PR-FOUNDATION-1 (2026-05-29): role-range validation for an inbound
// eid-carrying packet. Without this, a malicious client peer can stamp
// ItemActivate/RedSky/Lightning/Weather with the HOST's senderElementId;
// Registry::Get resolves to the host's Player Element and the receiver
// applies the packet's effect under host identity. The range partition
// makes that impersonation detectable at the wire boundary: host-role
// packets MUST carry host-range eids; client-role packets MUST carry
// peer-range eids. Returns true when the eid is in-range OR when no
// compare is possible (0 sentinel / invalid sender slot). Logs +
// returns false on out-of-range.
//
// v14 / v15 also performed an 8-bit syncContext compare here (the
// VerifySenderContext function). v16 PR-FOUNDATION-1b retired that
// layer entirely: per-peer stale-generation defense now lives in
// Session::HandleMessage's senderEpoch latch, applied uniformly to
// EVERY inbound packet by the transport layer (not per-payload by the
// receiver dispatch). This helper kept the role-range half because
// it's a wire-format trust boundary (the eid range is the sender's
// claimed role), independent of the stale-gen defense.
bool VerifySenderEidRange(int senderPeerSlot,
                          uint32_t senderElementId,
                          const char* kind) {
    if (senderElementId == 0u ||
        senderElementId == coop::element::kInvalidId) {
        return true;  // 0 sentinel; no compare. peer-slot fallback applies.
    }
    if (senderPeerSlot < 0) return true;  // unknown sender; skip range check
    const bool senderIsHost = (senderPeerSlot == 0);
    if (!coop::element::Registry::IsAllowedSenderEid(
            senderIsHost, senderElementId)) {
        UE_LOGW("event_feed: %s senderElementId=0x%08x out of allowed %s "
                "range (senderPeerSlot=%d) -- dropping (role impersonation?)",
                kind, senderElementId,
                senderIsHost ? "host" : "peer",
                senderPeerSlot);
        return false;
    }
    return true;
}

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

    // Drain delivered reliable messages.
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
                coop::net_pump::RunConnectReplayForSlot(msg.senderPeerSlot);
            }
            break;
        }
        case net::ReliableKind::PropRelease: {
            // v5: peer released a held prop. Dispatch to remote_prop which
            // re-enables SimulatePhysics + sets linear/angular velocity, and
            // fires Aprop_C.thrown if the launch crosses the throw threshold.
            if (msg.payloadLen < sizeof(net::PropReleasePayload)) {
                UE_LOGW("event_feed: PropRelease payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::PropReleasePayload));
                break;
            }
            // senderPeerSlot threads into remote_prop::OnRelease for routing
            // into g_drives[slot]. Range-check before use so a malformed -1
            // or out-of-range value can't OOB the slot array.
            if (msg.senderPeerSlot < 0 || msg.senderPeerSlot >= net::kMaxPeers) {
                UE_LOGW("event_feed: PropRelease invalid senderPeerSlot=%d -- dropping",
                        msg.senderPeerSlot);
                break;
            }
            net::PropReleasePayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            // Trust-boundary validation: a NaN/Inf or absurd-magnitude velocity
            // reaches UPrimitiveComponent::SetPhysicsLinearVelocity ->
            // SetPhysicsAngularVelocityInDegrees -> PhysX UB. Reject before
            // dispatch.
            const float vals[6] = {p.linVelX, p.linVelY, p.linVelZ,
                                   p.angVelX, p.angVelY, p.angVelZ};
            bool finite = true;
            for (float v : vals) {
                if (!std::isfinite(v)) { finite = false; break; }
            }
            if (!finite) {
                UE_LOGW("event_feed: PropRelease velocity non-finite -- dropping");
                break;
            }
            // Linear velocity bound: realistic throws peak at a few thousand
            // cm/s. 1e6 cm/s = 10 km/s -- well beyond any legitimate throw and
            // below any value that would teleport a body to infinity in one
            // tick. Angular velocity bound: a fast tumble is ~3600 deg/s
            // (10 rps); 1e6 is generous headroom.
            constexpr float kMaxLinVel = 1.0e6f;
            constexpr float kMaxAngVel = 1.0e6f;
            if (std::fabs(p.linVelX) > kMaxLinVel ||
                std::fabs(p.linVelY) > kMaxLinVel ||
                std::fabs(p.linVelZ) > kMaxLinVel ||
                std::fabs(p.angVelX) > kMaxAngVel ||
                std::fabs(p.angVelY) > kMaxAngVel ||
                std::fabs(p.angVelZ) > kMaxAngVel) {
                UE_LOGW("event_feed: PropRelease velocity out of bounds (lin=(%.1f,%.1f,%.1f) ang=(%.1f,%.1f,%.1f)) -- dropping",
                        p.linVelX, p.linVelY, p.linVelZ,
                        p.angVelX, p.angVelY, p.angVelZ);
                break;
            }
            remote_prop::OnRelease(msg.senderPeerSlot, p, localPlayer);
            break;
        }
        case net::ReliableKind::PropSpawn: {
            // v5 Bug C: peer dropped an inventory item -- spawn a matching
            // Aprop_X_C locally so subsequent PropPose updates resolve.
            if (msg.payloadLen < sizeof(net::PropSpawnPayload)) {
                UE_LOGW("event_feed: PropSpawn payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::PropSpawnPayload));
                break;
            }
            net::PropSpawnPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            // Trust-boundary validation: any of the 18 floats (loc/rot/scale +
            // 2 vel vectors) NaN/Inf or out-of-bound -> SpawnActor +
            // SetPhysics* could crash PhysX. Reject before dispatch.
            const float vals[18] = {
                p.locX, p.locY, p.locZ,
                p.rotPitch, p.rotYaw, p.rotRoll,
                p.scaleX, p.scaleY, p.scaleZ,
                p.initLinVelX, p.initLinVelY, p.initLinVelZ,
                p.initAngVelX, p.initAngVelY, p.initAngVelZ,
                0.f, 0.f, 0.f  // pad
            };
            bool finite = true;
            for (int i = 0; i < 15; ++i) {  // skip the 3 pad slots
                if (!std::isfinite(vals[i])) { finite = false; break; }
            }
            if (!finite) {
                UE_LOGW("event_feed: PropSpawn floats non-finite -- dropping");
                break;
            }
            constexpr float kMaxCoord = 1.0e6f;
            constexpr float kMaxVel   = 1.0e6f;
            if (std::fabs(p.locX) > kMaxCoord || std::fabs(p.locY) > kMaxCoord ||
                std::fabs(p.locZ) > kMaxCoord) {
                UE_LOGW("event_feed: PropSpawn location out of bounds (%.1f, %.1f, %.1f)",
                        p.locX, p.locY, p.locZ);
                break;
            }
            if (std::fabs(p.initLinVelX) > kMaxVel || std::fabs(p.initLinVelY) > kMaxVel ||
                std::fabs(p.initLinVelZ) > kMaxVel ||
                std::fabs(p.initAngVelX) > kMaxVel || std::fabs(p.initAngVelY) > kMaxVel ||
                std::fabs(p.initAngVelZ) > kMaxVel) {
                UE_LOGW("event_feed: PropSpawn velocity out of bounds");
                break;
            }
            // Clamp class/key lengths defensively (they're uint8 but the
            // sender could lie). 63/31 are the struct caps.
            if (p.className.len > 63) {
                UE_LOGW("event_feed: PropSpawn className.len=%u > 63 -- dropping", p.className.len);
                break;
            }
            if (p.key.len > 31) {
                UE_LOGW("event_feed: PropSpawn key.len=%u > 31 -- dropping", p.key.len);
                break;
            }
            // PR-FOUNDATION-1 (2026-05-29): elementId range trust. The
            // sender's role determines which allocation range its
            // PropSpawn.elementId is allowed to land in (host -> host
            // range; client -> peer range per the A2 v12 contract:
            // chipPile/clump/trashBits broadcast client->host so a
            // client-sourced PropSpawn carries a peer-range eid). A
            // packet carrying an eid outside its sender's permitted
            // range is forged or relay-loop bugged and must be dropped
            // at the boundary before it reaches RegisterPropMirror and
            // collides with the receiver's own allocator. Closes
            // D2-2 / E-1's PropSpawn gap.
            //
            // Fork B 2a (2026-06-10): the HOST side is relaxed to EITHER
            // range. A snapshot bracket RE-EXPRESSES existing entities --
            // including client-born ones the host holds as mirror Elements
            // (the drain never filters by origin) -- and a re-bracket
            // (cave travel / host save-load) must re-express a client's
            // own dropped items back to it or the widened adoption sweep
            // would destroy them as unclaimed. Same argument already
            // shipped for PropDestroy ("a destroy is NOT an allocation --
            // it references an EXISTING shared entity") and matches the
            // documented MarkPropElement intent ("clients route them as
            // MIRROR entries which is correct"). A CLIENT sender may still
            // only allocate in its own peer range.
            if (msg.senderPeerSlot >= 0) {
                const bool senderIsHost = (msg.senderPeerSlot == 0);
                const bool ok = senderIsHost
                    ? (coop::element::Registry::IsAllowedHostAllocatedEid(p.elementId) ||
                       coop::element::Registry::IsAllowedPeerAllocatedEid(p.elementId))
                    : coop::element::Registry::IsAllowedPeerAllocatedEid(p.elementId);
                if (!ok) {
                    UE_LOGW("event_feed: PropSpawn elementId=0x%08x out of allowed "
                            "%s range (senderPeerSlot=%d) -- dropping",
                            p.elementId,
                            senderIsHost ? "host(any)" : "peer",
                            msg.senderPeerSlot);
                    break;
                }
            }
            // (v15 added a senderContext compare here for stale-generation
            // defense; v16 PR-FOUNDATION-1b moved that to the packet
            // header's senderEpoch latched in Session::HandleMessage.)
            // intermediate-variant classes that the receiver doesn't want
            // (mushroom7_C growing state). Host-authoritative growth
            // pipeline -- the mature variant (mushroom_C) will arrive when
            // host's transform-timer fires. Mirrors the role==Client +
            // IsClientSuppressedPropClass check in harness.cpp::
            // GrabObserver_Aprop_Init_POST so the suppression is symmetric:
            // never spawn locally AND never accept wire spawns of these.
            {
                std::wstring cls;
                cls.reserve(p.className.len);
                for (uint8_t i = 0; i < p.className.len; ++i) {
                    cls.push_back(static_cast<wchar_t>(static_cast<unsigned char>(p.className.data[i])));
                }
                if (cls == ue_wrap::profile::name::PropMushroomGrowingClass) {
                    UE_LOGI("event_feed: PropSpawn drop -- intermediate-variant class '%ls' suppressed on this peer (host-authoritative; mature variant will arrive when host transforms)",
                            cls.c_str());
                    break;
                }
            }
            remote_prop_spawn::OnSpawn(p, msg.senderPeerSlot, localPlayer);
            // v34: advance the join loading-screen bar. No-op unless a join snapshot is
            // in progress (join_progress is in the Receiving phase) -- so live PropSpawns
            // outside a join cost a single relaxed atomic load and return.
            coop::join_progress::NotePropApplied();
            break;
        }
        case net::ReliableKind::PropDestroy: {
            // v5 Inc2: peer destroyed a prop -- delete the matching local
            // actor (resolved via FindByKeyString). Receiver-side
            // K2_DestroyActor is echo-suppressed via the incoming-destroy
            // set so it doesn't bounce back to the sender.
            //
            // TRUST BOUNDARY: with bidirectional destroy, CLIENT can command
            // HOST to destroy any prop by wire-Key. Acceptable for LAN coop
            // (trusted peers); review before Internet coop -- a malicious
            // client could replay crafted Keys to destroy host's quest items.
            // Mitigation if needed: authority model (host validates destroy
            // requests against current world state / quest progress).
            if (msg.payloadLen < sizeof(net::PropDestroyPayload)) {
                UE_LOGW("event_feed: PropDestroy payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::PropDestroyPayload));
                break;
            }
            net::PropDestroyPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            if (p.key.len > 31) {
                UE_LOGW("event_feed: PropDestroy key.len=%u > 31 -- dropping", p.key.len);
                break;
            }
            // elementId range trust. UNLIKE PropSpawn, a destroy is NOT an allocation -- it
            // references an EXISTING shared entity, which the OTHER peer may have allocated.
            // The grab-destroy model is symmetric: whoever grabs a shared chipPile broadcasts
            // its destroy by eid, and that eid is in the ORIGINAL owner's range, not the
            // grabber's. So we must accept EITHER range here (was: IsAllowedSenderEid(role) ->
            // it dropped a client's PropDestroy of a host-owned pile = the cross-grab clump DUPE,
            // proven 2026-06-09 host log "0x918 out of allowed peer range"). We still reject a
            // genuinely invalid id (0 / kInvalidId / out of both ranges) so UnregisterPropMirror /
            // OnDestroy never see a forged out-of-bounds eid. (Trust note above already documents
            // that a LAN peer can command any prop destroy -- this is consistent with that model.)
            if (p.elementId != 0 && p.elementId != coop::element::kInvalidId &&
                !coop::element::Registry::IsAllowedHostAllocatedEid(p.elementId) &&
                !coop::element::Registry::IsAllowedPeerAllocatedEid(p.elementId)) {
                UE_LOGW("event_feed: PropDestroy elementId=0x%08x not a valid allocated id "
                        "(out of both ranges) -- dropping", p.elementId);
                break;
            }
            // (v15 also had a senderContext compare here -- moved to
            // header senderEpoch in v16 PR-FOUNDATION-1b.)
            // 2026-05-25 cross-peer destroy: pass localPlayer so OnDestroy
            // can release a held PHC grab (mainPlayer.grabbing_actor ==
            // doomed) before K2_DestroyActor. Prevents UPhysicsHandle
            // Component::TickComponent reading a dangling GrabbedComponent
            // ptr next frame.
            // v52: if this destroy targets a pile WE are death-watching (the OTHER peer grabbed the
            // shared pile), drop our watch entry first so the local teardown below doesn't make our
            // own liveness sweep re-broadcast the (wire-induced) death back to the origin.
            if (p.elementId != 0 && p.elementId != coop::element::kInvalidId) {
                coop::trash_collect_sync::NotifyPileConsumed(p.elementId);
            }
            // v57: same echo guard for the trashBitsPile counter channel, keyed -- a wire
            // destroy of a dispenser pile must not re-broadcast from OUR death-watch.
            if (p.key.len > 0) {
                std::wstring dkey;
                dkey.reserve(p.key.len);
                for (uint8_t i = 0; i < p.key.len && i < 31; ++i)
                    dkey.push_back(static_cast<wchar_t>(static_cast<unsigned char>(p.key.data[i])));
                coop::trash_pile_sync::NotifyWireDestroy(dkey);
            }
            remote_prop::OnDestroy(p, localPlayer);
            break;
        }
        case net::ReliableKind::PropConvert: {
            // v52: the ATOMIC trash-clump ball->pile swap. Destroy the mirror ball by oldEid +
            // spawn the authoritative pile by newEid in one handler (remote_prop::OnConvert), then
            // enrol the spawned mirror pile in OUR death-watch so a local re-grab of it propagates
            // the destroy by identity. Same trust-boundary validation as PropSpawn/PropDestroy:
            // finite transform + in-range eids (both old + new must be within the sender's range).
            if (msg.payloadLen < sizeof(net::PropConvertPayload)) {
                UE_LOGW("event_feed: PropConvert payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::PropConvertPayload));
                break;
            }
            net::PropConvertPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            const float cvals[6] = { p.locX, p.locY, p.locZ,
                                     p.rotPitch, p.rotYaw, p.rotRoll };
            bool cfinite = true;
            for (float v : cvals) { if (!std::isfinite(v)) { cfinite = false; break; } }
            if (!cfinite) {
                UE_LOGW("event_feed: PropConvert floats non-finite -- dropping");
                break;
            }
            constexpr float kMaxConvCoord = 1.0e6f;
            if (std::fabs(p.locX) > kMaxConvCoord || std::fabs(p.locY) > kMaxConvCoord ||
                std::fabs(p.locZ) > kMaxConvCoord) {
                UE_LOGW("event_feed: PropConvert location out of bounds (%.1f, %.1f, %.1f)",
                        p.locX, p.locY, p.locZ);
                break;
            }
            if (p.pileClass.len > 63) {
                UE_LOGW("event_feed: PropConvert pileClass.len=%u > 63 -- dropping", p.pileClass.len);
                break;
            }
            if (msg.senderPeerSlot >= 0) {
                const bool senderIsHost = (msg.senderPeerSlot == 0);
                if (!coop::element::Registry::IsAllowedSenderEid(senderIsHost, p.oldEid) ||
                    !coop::element::Registry::IsAllowedSenderEid(senderIsHost, p.newEid)) {
                    UE_LOGW("event_feed: PropConvert eids (old=0x%08x new=0x%08x) out of allowed "
                            "%s range (senderPeerSlot=%d) -- dropping",
                            p.oldEid, p.newEid, senderIsHost ? "host" : "peer", msg.senderPeerSlot);
                    break;
                }
            }
            // OnConvert spawns the pile via remote_prop_spawn::OnSpawn, which enrols every
            // chipPile mirror in the death-watch itself -- so no separate WatchPile here. A null
            // return = the pile spawn failed (e.g. a transient FindClass miss before the chipPile
            // BP class loaded); rare, but log it since the re-grab destroy won't propagate.
            if (!remote_prop::OnConvert(p, localPlayer, msg.senderPeerSlot)) {
                UE_LOGW("event_feed: PropConvert newEid=%u pile spawn FAILED -- a re-grab of this "
                        "pile won't propagate its destroy", p.newEid);
            }
            break;
        }
        case net::ReliableKind::EntitySpawn: {
            // Host-broadcast NPC spawn. Validate size + className.len at the
            // trust boundary here; npc_mirror::OnEntitySpawn does the full
            // per-field validation (finite + bounds + allowlist + dedup).
            // UFunction calls inside OnEntitySpawn are game-thread only, so
            // dispatch via GT::Post.
            if (msg.payloadLen < sizeof(net::EntitySpawnPayload)) {
                UE_LOGW("event_feed: EntitySpawn payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::EntitySpawnPayload));
                break;
            }
            // EntitySpawn is host-authoritative. Without a senderPeerSlot
            // trust gate, a malicious client could flood EntitySpawn packets
            // with crafted className strings, forcing R::FindClass
            // GUObjectArray walks on the host's game thread per packet (CPU
            // amplification).
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: EntitySpawn from non-host senderPeerSlot=%d "
                        "-- dropping (NPC sync is host-only)",
                        msg.senderPeerSlot);
                break;
            }
            net::EntitySpawnPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            if (p.className.len > 63) {
                UE_LOGW("event_feed: EntitySpawn className.len=%u > 63 -- dropping",
                        p.className.len);
                break;
            }
            net::EntitySpawnPayload pCopy = p;
            ue_wrap::game_thread::Post([pCopy] {
                ::coop::npc_mirror::OnEntitySpawn(pCopy);
            });
            break;
        }
        case net::ReliableKind::EntityDestroy: {
            // Host-broadcast NPC destroy. Dispatch to npc_mirror::
            // OnEntityDestroy (game-thread UFunction call).
            if (msg.payloadLen < sizeof(net::EntityDestroyPayload)) {
                UE_LOGW("event_feed: EntityDestroy payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::EntityDestroyPayload));
                break;
            }
            // Host-authoritative -- reject non-host senders. A malicious
            // client could otherwise destroy any NPC element id it learned
            // from a legitimate EntitySpawn.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: EntityDestroy from non-host senderPeerSlot=%d "
                        "-- dropping (NPC sync is host-only)",
                        msg.senderPeerSlot);
                break;
            }
            net::EntityDestroyPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            net::EntityDestroyPayload pCopy = p;
            ue_wrap::game_thread::Post([pCopy] {
                ::coop::npc_mirror::OnEntityDestroy(pCopy);
            });
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
            // Trust-boundary validation (same defensive pattern as
            // PropRelease velocity check at line 196 above): reject NaN/Inf
            // before the engine call. UE's K2_TeleportTo with a NaN
            // location asserts inside FSweepData::ClampSweepParameters.
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
            break;
        }
        case net::ReliableKind::ItemActivate: {
            // Phase 5F flashlight (and future radio/torch/lamp) -- peer's
            // item state changed and produces a WORLD effect both peers
            // must see. For Case (b) flashlight: apply to the puppet's
            // light_R. RE doc: research/findings/votv-flashlight-RE-
            // 2026-05-25.md.
            if (msg.payloadLen < sizeof(net::ItemActivatePayload)) {
                UE_LOGW("event_feed: ItemActivate payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::ItemActivatePayload));
                break;
            }
            net::ItemActivatePayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            // Trust-boundary: state is a uint8 but only 0/1 are valid.
            if (p.state != 0 && p.state != 1) {
                UE_LOGW("event_feed: ItemActivate state=%u out of range -- dropping",
                        static_cast<unsigned>(p.state));
                break;
            }
            // Reserved flag bits must be zero. A future bit added in v15+
            // would otherwise be silently triggerable by a peer on an older
            // build.
            if (p.flags & ~coop::net::kItemActivateFlag_HasActorKey) {
                UE_LOGW("event_feed: ItemActivate flags=0x%02x has reserved bits "
                        "set -- dropping",
                        static_cast<unsigned>(p.flags));
                break;
            }
            // Intensity + cone angles are passed directly to UE light
            // component setters. Without finite + magnitude
            // checks, a peer can send NaN/Inf (UB inside the renderer)
            // or 1e30 (blinding white screen). Matches the validator
            // pattern every other float-bearing reliable kind uses.
            if (!std::isfinite(p.intensity) ||
                !std::isfinite(p.outerConeAngle) ||
                !std::isfinite(p.innerConeAngle)) {
                UE_LOGW("event_feed: ItemActivate floats non-finite "
                        "(intensity=%.2f outer=%.2f inner=%.2f) -- dropping",
                        p.intensity, p.outerConeAngle, p.innerConeAngle);
                break;
            }
            constexpr float kMaxItemIntensity = 1.0e6f;  // unitless ~10 normal range
            constexpr float kMaxConeAngle     = 180.0f;  // physical degree ceiling
            if (std::fabs(p.intensity) > kMaxItemIntensity ||
                std::fabs(p.outerConeAngle) > kMaxConeAngle ||
                std::fabs(p.innerConeAngle) > kMaxConeAngle) {
                UE_LOGW("event_feed: ItemActivate floats out of bounds "
                        "(intensity=%.2f outer=%.2f inner=%.2f) -- dropping",
                        p.intensity, p.outerConeAngle, p.innerConeAngle);
                break;
            }
            // Self-echo guard via ElementId equality (uint32 compare). The
            // wire field is the SENDER's local Player Element id; if it
            // equals our own local Player Element id, this packet is a
            // loopback bounce. The peer-slot fallback only fires when
            // senderElementId is the 0/unset sentinel (boot/seed
            // pre-handshake sender). With a valid senderElementId, the
            // ElementId compare is authoritative -- gating the peer-slot
            // compare on it prevents an N-peer reassignment race from
            // mis-classifying a legitimate cross-peer packet as a self
            // loopback (e.g. a packet from a peer at slot X arriving before
            // our own AssignPeerSlot reassigned us off slot X).
            const auto selfEid =
                coop::players::Registry::Get().LocalPlayerElementId();
            const bool selfEchoByEid =
                (p.senderElementId != 0u &&
                 p.senderElementId != coop::element::kInvalidId &&
                 selfEid != coop::element::kInvalidId &&
                 p.senderElementId == selfEid);
            const uint8_t selfPeerId = coop::players::Registry::Get().LocalPeerId();
            const bool senderElementIdMissing =
                (p.senderElementId == 0u ||
                 p.senderElementId == coop::element::kInvalidId);
            const bool selfEchoByPeerSlotFallback =
                (senderElementIdMissing &&
                 msg.senderPeerSlot >= 0 &&
                 static_cast<uint8_t>(msg.senderPeerSlot) == selfPeerId);
            if (selfEchoByEid || selfEchoByPeerSlotFallback) {
                UE_LOGI("event_feed: ItemActivate self-echo "
                        "(senderElementId=0x%08x senderPeerSlot=%d via=%s) -- dropping",
                        p.senderElementId, msg.senderPeerSlot,
                        selfEchoByEid ? "eid" : "peerSlot-fallback");
                break;
            }
            // PR-FOUNDATION-1 (2026-05-29): role-range trust boundary on
            // senderElementId. v16 (PR-FOUNDATION-1b) replaces v14's
            // syncContext compare with the Session-layer senderEpoch
            // latch (applied before HandleMessage dispatches here).
            if (!VerifySenderEidRange(msg.senderPeerSlot, p.senderElementId,
                                       "ItemActivate")) {
                break;
            }
            // Resolve senderElementId -> peer slot via Registry::Get. Falls
            // back to msg.senderPeerSlot when the mirror hasn't been
            // established yet (early boot before Join/AssignPeerSlot landed).
            uint8_t resolvedSlot = coop::players::kPeerIdUnknown;
            if (p.senderElementId != 0u &&
                p.senderElementId != coop::element::kInvalidId) {
                auto* el = coop::element::Registry::Get().Get(p.senderElementId);
                if (el && el->GetType() == coop::element::ElementType::Player) {
                    resolvedSlot =
                        static_cast<coop::element::Player*>(el)->PeerSlot();
                }
            }
            if (resolvedSlot >= net::kMaxPeers) {
                if (msg.senderPeerSlot >= 0 &&
                    msg.senderPeerSlot < net::kMaxPeers) {
                    resolvedSlot = static_cast<uint8_t>(msg.senderPeerSlot);
                } else {
                    UE_LOGW("event_feed: ItemActivate could not resolve sender "
                            "slot (senderElementId=0x%08x senderPeerSlot=%d) -- dropping",
                            p.senderElementId, msg.senderPeerSlot);
                    break;
                }
            }
            // The puppet may be null if this packet beat the first
            // PoseSnapshot (puppet spawned lazily on first pose; ItemActivate
            // rides the reliable channel and CAN arrive first under a
            // connect-edge burst). ApplyToPuppetOrDefer stashes the payload
            // when the puppet isn't ready; TickConnect drains it once the
            // puppet appears in the registry.
            //
            // Audit C1 (2026-05-27): capture only resolvedSlot + payload by
            // value into the lambda; re-fetch puppet INSIDE the lambda. The
            // raw void* would risk UAF because Destroy() can run on the
            // game thread between this post and the lambda dispatch,
            // recycling the GUObjectArray slot.
            net::ItemActivatePayload pCopy = p;
            const uint8_t peerSlotCopy = resolvedSlot;
            ue_wrap::game_thread::Post([peerSlotCopy, pCopy] {
                ::coop::RemotePlayer* rp =
                    ::coop::players::Registry::Get().Puppet(peerSlotCopy);
                void* puppetNow = (rp && rp->valid()) ? rp->GetActor() : nullptr;
                ::coop::item_activate::ApplyToPuppetOrDefer(peerSlotCopy, puppetNow, pCopy);
            });
            break;
        }
        case net::ReliableKind::DoorState:
        case net::ReliableKind::LightState:
        case net::ReliableKind::ContainerState:
        case net::ReliableKind::GarageDoorState:
        case net::ReliableKind::ApplianceState: {
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
            // client's code itself -- MTA input-replication). No isAcc/isDeny (hover flags, the
            // old PURPLE -- removed v35). RE: votv-keypad-door-BP-disassembly-2026-06-06.md.
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
        case net::ReliableKind::FireflySpawn: {
            // v51 (2026-06-09): PEER-SYMMETRIC ambient firefly. Any peer may originate one
            // (each runs its own spawner near its OWN camera + shares); the host relays a
            // client's spawn to the other clients (IsClientRelayableReliableKind). No trust
            // gate -- a cosmetic transient particle. The origin never receives its own send,
            // so OnReliable always materialises ANOTHER peer's firefly at its world position.
            if (msg.payloadLen < sizeof(net::FireflySpawnPayload)) {
                UE_LOGW("event_feed: FireflySpawn payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::FireflySpawnPayload));
                break;
            }
            net::FireflySpawnPayload fp{};
            std::memcpy(&fp, msg.payload, sizeof(fp));
            coop::firefly_sync::OnReliable(fp);
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
        case net::ReliableKind::TimeSync: {
            // v36 (2026-06-07): HOST-authoritative world clock (time-of-day). HOST->client; the
            // client applies it to its cycle (OnReliable no-ops on the host defensively).
            // Trust gate (like every other host-only kind): only slot 0 (the host) may set the clock.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: TimeSync from non-host senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
                break;
            }
            if (msg.payloadLen < sizeof(net::TimeSyncPayload)) {
                UE_LOGW("event_feed: TimeSync payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::TimeSyncPayload));
                break;
            }
            net::TimeSyncPayload tp{};
            std::memcpy(&tp, msg.payload, sizeof(tp));
            // Reject NaN/Inf / absurd values before the raw float write into the cycle struct (a NaN
            // clock -> setSunAndMoonRotation(NaN) -> black sky / FRotator assert). totalTime/day are
            // monotonic game counters (O(1e4)s per game-day); timeScale is ~1.
            if (!std::isfinite(tp.totalTime) || !std::isfinite(tp.day) || !std::isfinite(tp.timeScale) ||
                std::fabs(tp.totalTime) > 1.0e7f || std::fabs(tp.day) > 1.0e7f ||
                tp.timeScale < 0.0f || tp.timeScale > 1.0e4f) {
                UE_LOGW("event_feed: TimeSync values out of range (t=%.1f d=%.1f s=%.3f) -- dropping",
                        tp.totalTime, tp.day, tp.timeScale);
                break;
            }
            coop::time_sync::OnReliable(tp);
            break;
        }
        case net::ReliableKind::SkyState: {
            // v44 (2026-06-08): HOST-authoritative night-sky orientation + moon phase (Anewsky_C).
            // HOST->client; trust-gated to slot 0 like TimeSync. The client writes the sky mesh
            // world rotation + moonPhase (sky_sync::OnReliable no-ops on the host defensively).
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: SkyState from non-host senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
                break;
            }
            if (msg.payloadLen < sizeof(net::SkyStatePayload)) {
                UE_LOGW("event_feed: SkyState payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::SkyStatePayload));
                break;
            }
            net::SkyStatePayload sp{};
            std::memcpy(&sp, msg.payload, sizeof(sp));
            // NaN/Inf guard before the raw float writes (SetComponentWorldRotation(NaN) -> FRotator
            // assert / garbage transform). Rotations are bounded angles; moonPhase is a material
            // scalar. (sky_sync::OnReliable re-checks defensively too.)
            if (!std::isfinite(sp.skyPitch) || !std::isfinite(sp.skyYaw) ||
                !std::isfinite(sp.skyRoll) || !std::isfinite(sp.moonPhase)) {
                UE_LOGW("event_feed: SkyState non-finite floats -- dropping");
                break;
            }
            coop::sky_sync::OnReliable(sp);
            break;
        }
        case net::ReliableKind::RedSky: {
            // Phase 5W Inc-fix-2 (2026-05-27): one-shot/toggle red-sky
            // story-event sync. Host's POST observer on spawnRedSky +
            // redSky.set caught the change; broadcast it. Receiver
            // invokes the same chain on its local gamemode.
            if (msg.payloadLen < sizeof(net::RedSkyPayload)) {
                UE_LOGW("event_feed: RedSky payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::RedSkyPayload));
                break;
            }
            net::RedSkyPayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            if (session.role() == net::Role::Host) {
                UE_LOGI("event_feed: RedSky received on host -- dropping");
                break;
            }
            // v13 (A4 2026-05-29): host trust-bound. RedSky is host-only;
            // a non-host senderPeerSlot is a protocol violation.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: RedSky from non-host senderPeerSlot=%d "
                        "(senderElementId=0x%08x) -- dropping",
                        msg.senderPeerSlot, p.senderElementId);
                break;
            }
            // PR-FOUNDATION-1: role-range trust on senderElementId.
            // (v14 syncContext compare replaced by Session-layer senderEpoch in v16.)
            if (!VerifySenderEidRange(msg.senderPeerSlot, p.senderElementId,
                                       "RedSky")) {
                break;
            }
            if (p.state != 0 && p.state != 1) {
                UE_LOGW("event_feed: RedSky state=%u out of range -- dropping",
                        static_cast<unsigned>(p.state));
                break;
            }
            net::RedSkyPayload pCopy = p;
            ue_wrap::game_thread::Post([pCopy] {
                ::coop::weather_sync::ApplyRedSky(pCopy);
            });
            break;
        }
        case net::ReliableKind::LightningStrike: {
            // Phase 5W Inc2 (2026-05-27): discrete strike event. Host's
            // POST observer on BeginDeferredActorSpawnFromClass caught
            // an AlightningStrike_C spawn (BP-internal SpawnActor inside
            // AdaynightCycle_C::timerLightning) and broadcast the
            // strike's world location. Client suppressed its own
            // timerLightning via Inc1's interceptor so no local strike
            // happened; this packet drives the visual.
            if (msg.payloadLen < sizeof(net::LightningStrikePayload)) {
                UE_LOGW("event_feed: LightningStrike payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::LightningStrikePayload));
                break;
            }
            net::LightningStrikePayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            if (session.role() == net::Role::Host) {
                UE_LOGI("event_feed: LightningStrike received on host -- dropping");
                break;
            }
            // v13 (A4 2026-05-29): host trust-bound. LightningStrike is
            // host-only; a non-host senderPeerSlot is a protocol violation.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: LightningStrike from non-host "
                        "senderPeerSlot=%d (senderElementId=0x%08x) -- dropping",
                        msg.senderPeerSlot, p.senderElementId);
                break;
            }
            // PR-FOUNDATION-1: role-range trust on senderElementId.
            // (v14 syncContext compare replaced by Session-layer senderEpoch in v16.)
            if (!VerifySenderEidRange(msg.senderPeerSlot, p.senderElementId,
                                       "LightningStrike")) {
                break;
            }
            // Trust boundary: validate loc finite + within sane bounds.
            if (!std::isfinite(p.locX) || !std::isfinite(p.locY) || !std::isfinite(p.locZ) ||
                std::fabs(p.locX) > coop::net::kMaxCoord ||
                std::fabs(p.locY) > coop::net::kMaxCoord ||
                std::fabs(p.locZ) > coop::net::kMaxCoord) {
                UE_LOGW("event_feed: LightningStrike loc out of bounds (%.0f, %.0f, %.0f) -- dropping",
                        p.locX, p.locY, p.locZ);
                break;
            }
            net::LightningStrikePayload pCopy = p;
            ue_wrap::game_thread::Post([pCopy] {
                ::coop::weather_sync::ApplyLightningStrike(pCopy);
            });
            break;
        }
        case net::ReliableKind::WeatherState: {
            // Phase 5W Inc1 (2026-05-26): host-authoritative weather state.
            // Sender = host. Receiver looks up local AdaynightCycle_C and
            // invokes the cycle's mutator UFunctions to apply each delta.
            // See coop/weather_sync.cpp::ApplyFromHost for the full apply
            // logic + research/findings/votv-weather-DESIGN-2026-05-26.md.
            if (msg.payloadLen < sizeof(net::WeatherStatePayload)) {
                UE_LOGW("event_feed: WeatherState payload too short (%zu < %zu)",
                        static_cast<size_t>(msg.payloadLen), sizeof(net::WeatherStatePayload));
                break;
            }
            net::WeatherStatePayload p{};
            std::memcpy(&p, msg.payload, sizeof(p));
            // Self-echo guard: weather is host->client only; if our role
            // says we ARE the host, a WeatherState packet must be a loopback
            // bounce (we'd never send to ourselves but defensive). Drop.
            if (session.role() == net::Role::Host) {
                UE_LOGI("event_feed: WeatherState received on host -- dropping "
                        "(host is the authority; no inbound from client)");
                break;
            }
            // v13 (A4 2026-05-29): host trust-bound. WeatherState is
            // host-only; a non-host senderPeerSlot is a protocol violation.
            if (msg.senderPeerSlot != 0) {
                UE_LOGW("event_feed: WeatherState from non-host "
                        "senderPeerSlot=%d (senderElementId=0x%08x) -- dropping",
                        msg.senderPeerSlot, p.senderElementId);
                break;
            }
            // PR-FOUNDATION-1: role-range trust on senderElementId.
            // (v14 syncContext compare replaced by Session-layer senderEpoch in v16.)
            if (!VerifySenderEidRange(msg.senderPeerSlot, p.senderElementId,
                                       "WeatherState")) {
                break;
            }
            // Trust-boundary: validate EVERY float the receiver writes into engine memory
            // is finite + within a sane range. Rain scalars are unitless [0, ~10] (lc/dc
            // chance up to ~120 observed); fog density ~[0, 15]; wind ~[0, 50] -- a generous
            // (-1e3, 1e3) catches garbage/NaN without clamping any legit value. v43 added
            // the 4 wind floats (written raw into the wind actor via directionalwind::Write)
            // AND the v24 fog/rain floats that were applied unvalidated before -- same
            // trust boundary, all in one check now (audit 2026-06-08).
            const float vals[] = {
                p.rainStrength, p.rainLightningChance, p.rainDeactivateChance, p.rainWindSpeed,
                p.rain, p.finalFogDensity, p.fogAlpha, p.fogStrength,
                p.windSpeedBg, p.windStrengthBg, p.windSpeedRain, p.windStrengthRain
            };
            bool bad = false;
            for (float v : vals) {
                if (!std::isfinite(v) || std::fabs(v) > 1.0e3f) { bad = true; break; }
            }
            if (bad) {
                UE_LOGW("event_feed: WeatherState floats out of bounds (rain=%.2f lc=%.2f dc=%.2f "
                        "ws=%.2f fog=%.2f windBg=%.2f/%.2f windRain=%.2f/%.2f) -- dropping",
                        p.rainStrength, p.rainLightningChance, p.rainDeactivateChance,
                        p.rainWindSpeed, p.finalFogDensity, p.windSpeedBg, p.windStrengthBg,
                        p.windSpeedRain, p.windStrengthRain);
                break;
            }
            net::WeatherStatePayload pCopy = p;
            ue_wrap::game_thread::Post([pCopy] {
                ::coop::weather_sync::ApplyFromHost(pCopy);
            });
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
