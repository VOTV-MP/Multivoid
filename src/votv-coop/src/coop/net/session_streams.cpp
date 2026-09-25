// coop/net/session_streams.cpp -- the scalar per-channel pose and state STREAMS for Session.
//
// Owns the nine scalar stream channels end to end at the Session layer:
//   per-peer:    PoseSnapshot / PropPose / RagdollPose / HandPose / DeskCursorPose
//   host-single: ClockPose / DeskSimPose / DishPose / ReelPose
// in four surfaces:
//   - the game-thread publishers  (Set*)         -- what this peer sends under localMutex_
//     (coop/net/outbound_streams.h), reset whole at Start
//   - the game-thread readers     (TryGet*)      -- the received streams under remoteMutex_,
//     kept by owner (coop/net/remote_streams.h)
//   - the net-thread receive-store (StoreStreamPacket) -- HandleMessage's grouped scalar
//     case labels delegate here
//   - the net-thread send fan-out (SendStreamsTick) -- one per-tick loop, including the
//     npc/worldactor/trashcarry batch stamps, whose Serialize* bodies live in their own TUs
// The BATCH channels (npc, worldactor, trashcarry, voice) keep their own TUs.

#include "coop/player/movement_ledger.h"
#include "coop/net/session.h"
#include "ue_wrap/core/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#pragma warning(pop)

#include <cmath>
#include <cstring>
#include <mutex>

namespace coop::net {

// --- game-thread publishers -------------------------------------------------

void Session::SetLocalPose(const PoseSnapshot& pose) {
    // Stamp the SAMPLE moment here, not the send moment in the net thread. This is the time the
    // pose was TRUE, and the receiver's freshness accounting is only as good as that; a game-thread
    // hitch would otherwise pair an old position with a fresh stamp.
    const uint32_t stateMs = NowStateTimeMs24();
    std::lock_guard<std::mutex> lk(localMutex_);
    outbound_.samples.pose.Put(true, pose);
    outbound_.samples.poseStateMs = stateMs;
}

void Session::SetLocalPropPose(bool set, const PropPoseSnapshot& pose) {
    std::lock_guard<std::mutex> lk(localMutex_);
    outbound_.samples.prop.Put(set, pose);
}

void Session::SetLocalRagdollPose(bool set, const RagdollPoseSnapshot& pose) {
    std::lock_guard<std::mutex> lk(localMutex_);
    outbound_.samples.ragdoll.Put(set, pose);
}

void Session::SetLocalHandPose(bool set, const HandPoseSnapshot& pose) {
    std::lock_guard<std::mutex> lk(localMutex_);
    outbound_.samples.hand.Put(set, pose);
}

void Session::SetLocalDeskCursor(bool set, const DeskCursorPoseSnapshot& pose) {
    std::lock_guard<std::mutex> lk(localMutex_);
    outbound_.samples.deskCursor.Put(set, pose);
}

void Session::SendHostClock(const TimeSyncPayload& clock) {
    std::lock_guard<std::mutex> lk(localMutex_);
    outbound_.samples.clock.Put(clock);
}

void Session::SetHostDeskSim(bool set, const DeskSimSnapshot& sim) {
    std::lock_guard<std::mutex> lk(localMutex_);
    outbound_.samples.deskSim.Put(set, sim);
}

void Session::SetHostDishPose(const DishPoseBody& body) {
    std::lock_guard<std::mutex> lk(localMutex_);
    outbound_.samples.dish.Put(body);
}

void Session::SetHostReelPose(const ReelPosePayload& body) {
    std::lock_guard<std::mutex> lk(localMutex_);
    outbound_.samples.reel.Put(body);
}

void Session::ResetOutboundStreams() {
    {
        std::lock_guard<std::mutex> lk(localMutex_);
        outbound_ = OutboundStreams{};
    }
    trashCarryPoses_.Reset();
    propDrivePoses_.Reset();
}

void Session::RefuseClientBatch(HostBatch kind, const char* tag, const char* msgName) {
    // Once per session: a client that keeps sending would otherwise write a line per datagram.
    if (saidClientBatch_[static_cast<size_t>(kind)].exchange(true, std::memory_order_relaxed)) return;
    UE_LOGW("%s: host received a %s batch -- only the host originates this kind, so this is a "
            "client-authored batch. Dropping (said once).", tag, msgName);
}

// --- game-thread readers ----------------------------------------------------

bool Session::TryGetRemotePose(int peerSlot, PoseSnapshot& out, bool* outIsNew) {
    return ReadOrigin(peerSlot, &OriginStreams::pose, out, outIsNew);
}

bool Session::TryGetRemotePropPose(int peerSlot, PropPoseSnapshot& out, bool* outIsNew) {
    return ReadOrigin(peerSlot, &OriginStreams::prop, out, outIsNew);
}

bool Session::TryGetRemoteRagdollPose(int peerSlot, RagdollPoseSnapshot& out, bool* outIsNew) {
    return ReadOrigin(peerSlot, &OriginStreams::ragdoll, out, outIsNew);
}

bool Session::TryGetRemoteHandPose(int peerSlot, HandPoseSnapshot& out, bool* outIsNew) {
    return ReadOrigin(peerSlot, &OriginStreams::hand, out, outIsNew);
}

bool Session::TryGetRemoteDeskCursor(int peerSlot, DeskCursorPoseSnapshot& out, bool* outIsNew) {
    return ReadOrigin(peerSlot, &OriginStreams::deskCursor, out, outIsNew);
}

bool Session::TryGetHostClock(TimeSyncPayload& out, bool* outIsNew) {
    return ReadHost(&HostStreams::clock, out, outIsNew);
}

bool Session::TryGetHostDeskSim(DeskSimSnapshot& out, bool* outIsNew) {
    return ReadHost(&HostStreams::deskSim, out, outIsNew);
}

bool Session::TryGetHostReelPose(ReelPosePayload& out, bool* outIsNew) {
    return ReadHost(&HostStreams::reel, out, outIsNew);
}

bool Session::TryGetHostDishPose(DishPoseBody& out, bool* outIsNew) {
    return ReadHost(&HostStreams::dish, out, outIsNew);
}

// A client's roster edge for a relayed slot: the occupancy that left has its late packets refused and
// its streams cleared, so no stored pose can bring its puppet back; the live one's streams start over
// when they belong to another. The host resets a slot on its own connection edges. Only a running
// session judges, asked under the lock Stop's reset takes: the ledger's own teardown empties every row
// after Stop has cleared every slot, and a context latched then would outlive the session, refusing
// that slot's packets in the next one until its first roster row. Game thread.
void Session::OnRosterOrigin(int peerSlot, bool left, uint8_t leftCtx, bool live, uint8_t liveCtx) {
    if (cfg_.role != Role::Client || peerSlot <= 0 || peerSlot >= kMaxPeers) return;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (!running_.load(std::memory_order_acquire)) return;
    if (left && originContext_.Retire(peerSlot, leftCtx)) {
        ResetOriginStreams(peerSlot);
        UE_LOGI("net: slot %d's occupancy %u left -- its relayed streams cleared, its late packets refused",
                peerSlot, static_cast<unsigned>(leftCtx));
    }
    if (live && originContext_.Conform(peerSlot, liveCtx)) {
        ResetOriginStreams(peerSlot);
        UE_LOGI("net: slot %d's roster names occupancy %u -- its relayed streams start over", peerSlot,
                static_cast<unsigned>(liveCtx));
    }
}

// --- net-thread receive-store: the nine scalar stream cases -----------------
// Called from HandleMessage's grouped case labels AFTER the header parse, the
// epoch latch, and the routeSlot derivation -- exactly the point the inline
// switch cases ran at. `return` here == the old `return` from HandleMessage
// (nothing followed the switch).

void Session::StoreStreamPacket(MsgType type, int routeSlot, int peerSlot,
                                const void* data, int len, uint32_t seq) {
    // A relayed packet names its origin's occupancy, and is stored only when that is the one the slot's
    // roster names (coop/net/origin_context.h). Each relayed store below asks under its own hold of
    // remoteMutex_, so a roster edge on the game thread cannot retire the slot between the answer and
    // the store and let a departed occupant's packet re-arm its puppet. Only a client is relayed to;
    // slot 0 is the host's own.
    std::uint8_t ctx = 0;
    if (cfg_.role == Role::Client && routeSlot != 0) {
        PacketHeader h;
        std::memcpy(&h, data, sizeof(h));
        ctx = h.originContext;
    }
    OriginStreams& origin = origin_[routeSlot];
    const auto occupancyAccepted = [&](MsgType kind) {  // under remoteMutex_
        if (originContext_.Accepts(routeSlot, ctx)) return true;
        if (kind == MsgType::PoseSnapshot)
            streamRefusals_.Refused(routeSlot, StreamRefusals::Why::OtherOccupancy, seq,
                                    origin.pose.LastSeq());
        return false;
    };
    switch (type) {
    case MsgType::PoseSnapshot: {
        if (len < static_cast<int>(sizeof(PosePacket))) return;
        PosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        if (!ValidatePose(pkt.pose)) {
            std::lock_guard<std::mutex> lk(remoteMutex_);
            streamRefusals_.Refused(routeSlot, StreamRefusals::Why::FailedValidation, seq,
                                    origin.pose.LastSeq());
            return;
        }
        {
            std::lock_guard<std::mutex> lk(remoteMutex_);
            if (!occupancyAccepted(type)) break;
            if (!origin.pose.Offer(seq, pkt.pose)) {
                streamRefusals_.Refused(routeSlot, StreamRefusals::Why::StaleSequence, seq,
                                        origin.pose.LastSeq());
                break;  // stale or a duplicate for this origin: not stored, and not relayed
            }
            streamRefusals_.Accepted(routeSlot);
        }
        // Bill this peer for the distance it just CLAIMED to have covered. After the freshness
        // check above and outside remoteMutex_, both deliberately: a reordered datagram would
        // otherwise walk the ledger's anchor backwards and then forwards and bill one metre
        // twice, and the ledger owns its own lock because its row holds correlated fields.
        // HOST ONLY: bounds apply to clients, never to the host, so a symmetric validator would
        // be a bug. `routeSlot == peerSlot` here is the authenticated connection. Measure-only
        // on this build: it records, nothing refuses.
        if (cfg_.role == Role::Host) {
            coop::movement_ledger::OnClientPose(
                *this, routeSlot,
                ue_wrap::FVector{pkt.pose.x, pkt.pose.y, pkt.pose.z},
                ReadStateTimeMs24(pkt.header));
        }
        // Host relay: forward this client's pose to every OTHER client.
        if (cfg_.role == Role::Host) {
            RelayUnreliableToOtherClients(peerSlot, data, len);
        }
        break;
    }
    case MsgType::PropPose: {
        if (len < static_cast<int>(sizeof(PropPosePacket))) return;
        PropPosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        const float vals[6] = {pkt.pose.x, pkt.pose.y, pkt.pose.z,
                               pkt.pose.pitch, pkt.pose.yaw, pkt.pose.roll};
        for (float v : vals) if (!std::isfinite(v)) return;
        if (std::fabs(pkt.pose.x) > kMaxCoord ||
            std::fabs(pkt.pose.y) > kMaxCoord ||
            std::fabs(pkt.pose.z) > kMaxCoord) return;
        if (std::fabs(pkt.pose.pitch) > 180.f ||
            std::fabs(pkt.pose.yaw)   > 180.f ||
            std::fabs(pkt.pose.roll)  > 180.f) return;
        if (pkt.pose.key.len > 31) return;
        {
            std::lock_guard<std::mutex> lk(remoteMutex_);
            if (!occupancyAccepted(type)) break;
            if (!origin.prop.Offer(seq, pkt.pose)) break;
        }
        // Host relay: forward this client's held-prop pose to every OTHER client.
        if (cfg_.role == Role::Host) {
            RelayUnreliableToOtherClients(peerSlot, data, len);
        }
        break;
    }
    case MsgType::RagdollPose: {
        if (len < static_cast<int>(sizeof(RagdollPosePacket))) return;
        RagdollPosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        // Trust-boundary sanitize (same shape as PropPose): reject NaN/Inf and
        // out-of-bounds before storing -- a velocity write of a poisoned value
        // would corrupt the receiver's PhysX state. Velocities are unbounded in
        // principle but a finite + sane-magnitude check rejects garbage; the
        // rotation goes onto SetActorRotation so it must be a finite FRotator.
        const float vals[12] = {pkt.pose.x, pkt.pose.y, pkt.pose.z,
                                pkt.pose.pitch, pkt.pose.yaw, pkt.pose.roll,
                                pkt.pose.linVelX, pkt.pose.linVelY, pkt.pose.linVelZ,
                                pkt.pose.angVelX, pkt.pose.angVelY, pkt.pose.angVelZ};
        for (float v : vals) if (!std::isfinite(v)) return;
        if (std::fabs(pkt.pose.x) > kMaxCoord ||
            std::fabs(pkt.pose.y) > kMaxCoord ||
            std::fabs(pkt.pose.z) > kMaxCoord) return;
        // Rotation must be in the canonical FRotator range (the sender normalizes
        // via NormalizeAxis) -- same guard the PropPose case applies. SetActorRotation
        // normalizes internally so an out-of-range value wouldn't crash, but reject it
        // at the trust boundary for parity with PropPose (a finite-but-huge angle from
        // a malformed/hostile datagram has no legitimate sender).
        if (std::fabs(pkt.pose.pitch) > 180.f ||
            std::fabs(pkt.pose.yaw)   > 180.f ||
            std::fabs(pkt.pose.roll)  > 180.f) return;
        {
            std::lock_guard<std::mutex> lk(remoteMutex_);
            if (!occupancyAccepted(type)) break;
            if (!origin.ragdoll.Offer(seq, pkt.pose)) break;
        }
        // Host relay: forward this client's ragdoll pose to every OTHER client.
        if (cfg_.role == Role::Host) {
            RelayUnreliableToOtherClients(peerSlot, data, len);
        }
        break;
    }
    case MsgType::HandPose: {
        if (len < static_cast<int>(sizeof(HandPosePacket))) return;
        HandPosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        // Trust-boundary sanitize (RagdollPose shape): finite + arm's-reach pos +
        // canonical rotator range; a poisoned value would go onto SetActorLocation/
        // Rotation of the display mirror.
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(pkt.pose.relPos[i]) || !std::isfinite(pkt.pose.relRot[i])) return;
            if (std::fabs(pkt.pose.relPos[i]) > 300.f) return;
            if (std::fabs(pkt.pose.relRot[i]) > 180.f) return;
        }
        {
            std::lock_guard<std::mutex> lk(remoteMutex_);
            if (!occupancyAccepted(type)) break;
            if (!origin.hand.Offer(seq, pkt.pose)) break;
        }
        // Host relay: forward this client's hand pose to every OTHER client.
        if (cfg_.role == Role::Host) {
            RelayUnreliableToOtherClients(peerSlot, data, len);
        }
        break;
    }
    case MsgType::DeskCursorPose: {  // coords-panel live cursor (sibling of HandPose)
        if (len < static_cast<int>(sizeof(DeskCursorPosePacket))) return;
        DeskCursorPosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        // Trust-boundary sanitize: viewCoordinate is screen-space (no fixed magnitude
        // bound), so a finite check is the sane floor before it reaches WriteCursorOnly.
        if (!std::isfinite(pkt.pose.viewX) || !std::isfinite(pkt.pose.viewY)) return;
        {
            std::lock_guard<std::mutex> lk(remoteMutex_);
            if (!occupancyAccepted(type)) break;
            if (!origin.deskCursor.Offer(seq, pkt.pose)) break;
        }
        // Host relay: forward this client's cursor to every OTHER client.
        if (cfg_.role == Role::Host) {
            RelayUnreliableToOtherClients(peerSlot, data, len);
        }
        break;
    }
    case MsgType::ClockPose: {  // HOST->all world-clock snapshot (single value, newest-wins)
        if (len < static_cast<int>(sizeof(ClockPosePacket))) return;
        // Host is authoritative -- it owns the clock and never applies a received one (a self-echo
        // via the relay cannot reach it: this kind is host-originated + not relayed, but guard
        // anyway).
        if (cfg_.role == Role::Host) break;
        ClockPosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        std::lock_guard<std::mutex> lk(remoteMutex_);
        host_.clock.Offer(seq, pkt.clock);  // an older or duplicate sample keeps the newer one
        break;
    }
    case MsgType::DeskSimPose: {  // HOST->all download-sim output vector (single value, newest-wins)
        if (len < static_cast<int>(sizeof(DeskSimPosePacket))) return;
        // Host owns the sim and never applies a received one (host-originated + not relayed; guard
        // anyway).
        if (cfg_.role == Role::Host) break;
        DeskSimPosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        std::lock_guard<std::mutex> lk(remoteMutex_);
        host_.deskSim.Offer(seq, pkt.sim);  // an older or duplicate snapshot keeps the newer one
        break;
    }
    case MsgType::DishPose: {  // HOST->all dish-pose row batch (newest-wins)
        if (len < static_cast<int>(sizeof(DishPosePacket))) return;
        if (cfg_.role == Role::Host) break;  // host-originated; never applied locally
        DishPosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        if (pkt.body.count > kMaxDishes) return;
        std::lock_guard<std::mutex> lk(remoteMutex_);
        host_.dish.Offer(seq, pkt.body);  // an older or duplicate batch keeps the newer one
        break;
    }
    case MsgType::ReelPose: {  // HOST->all reel corrector (newest-wins)
        if (len < static_cast<int>(sizeof(ReelPosePacket))) return;
        if (cfg_.role == Role::Host) break;  // host-originated; never applied locally
        ReelPosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        std::lock_guard<std::mutex> lk(remoteMutex_);
        host_.reel.Offer(seq, pkt.body);  // an older or duplicate one keeps the newer one
        break;
    }
    default:
        break;
    }
}

// --- net-thread stream fan-out ----------------------------------------------
// `now` is computed ONCE in the NetThread shell, which shares that timestamp with its
// net-diag step, and passed in; the cadence time_points live in the shell as net-thread
// locals and are advanced here by reference.

void Session::SendStreamsTick(std::chrono::steady_clock::time_point now,
                              std::chrono::milliseconds sendInterval,
                              std::chrono::steady_clock::time_point& nextSend,
                              std::chrono::steady_clock::time_point& nextDeskSimSend,
                              uint64_t& sendFails) {
    auto* sockets = SteamNetworkingSockets();
    constexpr auto kDeskSimSendInterval = std::chrono::milliseconds(100);  // ~10 Hz

    if (state_.load() == ConnState::Connected && now >= nextSend) {
        OutboundSamples local;
        { std::lock_guard<std::mutex> lk(localMutex_);
          local = outbound_.samples;
          // The one-shot samples are taken here; the lanes that publish them own the cadence.
          outbound_.samples.clock.due = outbound_.samples.dish.due = outbound_.samples.reel.due = false; }
        const bool isHost = cfg_.role == Role::Host;
        const bool have = local.pose.set, haveProp = local.prop.set, haveRagdoll = local.ragdoll.set,
                   haveHand = local.hand.set, haveDeskCursor = local.deskCursor.set;
        const bool clockDue = local.clock.due && isHost;
        const bool dishPoseDue = local.dish.due && isHost;
        const bool reelPoseDue = local.reel.due && isHost;
        const bool deskSimDue = local.deskSim.set && isHost && now >= nextDeskSimSend;
        // Serialize the live NPC pose batch ONCE (same body for every peer; only the per-peer
        // header seq differs). SerializeLocalNpcBatch (session_npc.cpp) reads outbound_.npcBatch under
        // localMutex_ + writes the body after the leading PacketHeader, returning 0 when there is
        // no batch to send this tick (no intermediate copy).
        uint8_t npcBuf[kNpcPoseDatagramMax];
        const int npcMsgLen = SerializeLocalNpcBatch(npcBuf);
        // The live WorldActor pose batch, serialized ONCE like the NPC batch (host-only producer --
        // SerializeLocalWorldActorBatch returns 0 on a client / when no actors stream).
        uint8_t waBuf[kWorldActorPoseDatagramMax];
        const int waMsgLen = SerializeLocalWorldActorBatch(waBuf);
        // The carried-trash-clump pose batch, serialized ONCE (host-only producer --
        // SerializeLocalTrashCarryBatch returns 0 on a client / when no clump is carried).
        uint8_t tcBuf[kTrashCarryPoseDatagramMax];
        const int tcMsgLen = SerializeLocalTrashCarryBatch(tcBuf);
        // The driven-prop batch, serialized ONCE and drained from the host's pending queue
        // (host-only producer -- SerializeLocalPropDriveBatch returns 0 on a client / when no
        // driven prop moved).
        uint8_t pdBuf[kPropDrivePoseDatagramMax];
        const int pdMsgLen = SerializeLocalPropDriveBatch(pdBuf);
        if (have || haveProp || haveRagdoll || haveHand || haveDeskCursor || clockDue || deskSimDue ||
            dishPoseDue || reelPoseDue || npcMsgLen > 0 || waMsgLen > 0 || tcMsgLen > 0 ||
            pdMsgLen > 0) {
            for (int i = 0; i < kMaxPeers; ++i) {
                const uint32_t hConn = peerConns_[i].load();
                if (hConn == 0) continue;
                // One accounting for every datagram of this round: the session's traffic total,
                // the occupancy the headroom rule reads, and the rejection count the per-second
                // diagnostics report. These are the sends the reserve exists to protect, so they
                // are never gated -- they are only counted.
                auto noteSent = [&](EResult rc, int bytes) {
                    if (rc != k_EResultOK) { ++sendFails; return; }
                    net_stats::AddSent(static_cast<uint32_t>(bytes));
                    admission_.NoteHanded(i, bytes);
                    // What the rate is being spent on besides the reliable stream. The send-rate
                    // law bounds a CONNECTION-WIDE rate against measured delivery, and only the
                    // reliable half is ever acknowledged; without this term an ordinary play
                    // session -- pose and voice, acknowledged by nothing -- would measure as a
                    // link delivering nothing and be clamped to the floor.
                    rateControl_.NoteUnreliableQueued(i, bytes);
                };
                if (have) {
                    PosePacket pkt{};
                    WriteHeader(pkt.header, MsgType::PoseSnapshot,
                                sendSeq_.fetch_add(1), ownEpoch_);
                    // The ORIGIN's time for the STATE in this datagram. WriteHeader leaves 0 (= not
                    // stamped) for every lane without a reader; the pose lane has one
                    // (coop::movement_ledger on the host), so it stamps the SAMPLE time that came
                    // out of localMutex_ with the pose itself.
                    WriteStateTimeMs24(pkt.header, local.poseStateMs);
                    pkt.pose = local.pose.value;
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, &pkt, sizeof(pkt),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    noteSent(rc, static_cast<int>(sizeof(pkt)));
                }
                // A joiner still loading gets no held-prop pose, as it gets no relayed one: the
                // release and the stick that close a hold are not sent to a world that is not ready
                // either (session_lanes.h), so a pose sent now names a hold it never sees end, and
                // would drive the copy in the world it is loading.
                if (haveProp && IsSlotWorldReady(i)) {
                    PropPosePacket pkt{};
                    WriteHeader(pkt.header, MsgType::PropPose,
                                sendSeq_.fetch_add(1), ownEpoch_);
                    pkt.pose = local.prop.value;
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, &pkt, sizeof(pkt),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    noteSent(rc, static_cast<int>(sizeof(pkt)));
                }
                if (haveRagdoll) {
                    RagdollPosePacket pkt{};
                    WriteHeader(pkt.header, MsgType::RagdollPose,
                                sendSeq_.fetch_add(1), ownEpoch_);
                    pkt.pose = local.ragdoll.value;
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, &pkt, sizeof(pkt),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    noteSent(rc, static_cast<int>(sizeof(pkt)));
                }
                if (haveHand) {  // hand-item view-relative transform (while holding)
                    HandPosePacket pkt{};
                    WriteHeader(pkt.header, MsgType::HandPose,
                                sendSeq_.fetch_add(1), ownEpoch_);
                    pkt.pose = local.hand.value;
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, &pkt, sizeof(pkt),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    noteSent(rc, static_cast<int>(sizeof(pkt)));
                }
                if (haveDeskCursor) {  // coords-panel live cursor (while desk-claimed + moving)
                    DeskCursorPosePacket pkt{};
                    WriteHeader(pkt.header, MsgType::DeskCursorPose,
                                sendSeq_.fetch_add(1), ownEpoch_);
                    pkt.pose = local.deskCursor.value;
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, &pkt, sizeof(pkt),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    noteSent(rc, static_cast<int>(sizeof(pkt)));
                }
                if (npcMsgLen > 0) {  // NPC pose batch -- body built once above; stamp the header per-peer
                    PacketHeader npcHdr{};  // build + memcpy (npcBuf is uint8_t[]; no misaligned PacketHeader lvalue)
                    WriteHeader(npcHdr, MsgType::EntityPose, sendSeq_.fetch_add(1), ownEpoch_);
                    std::memcpy(npcBuf, &npcHdr, sizeof(npcHdr));
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, npcBuf, static_cast<uint32_t>(npcMsgLen),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    noteSent(rc, static_cast<int>(static_cast<uint32_t>(npcMsgLen)));
                }
                if (waMsgLen > 0) {  // WorldActor pose batch -- body built once above; stamp the header
                                     // per-peer
                    PacketHeader waHdr{};
                    WriteHeader(waHdr, MsgType::WorldActorPose, sendSeq_.fetch_add(1), ownEpoch_);
                    std::memcpy(waBuf, &waHdr, sizeof(waHdr));
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, waBuf, static_cast<uint32_t>(waMsgLen),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    noteSent(rc, static_cast<int>(static_cast<uint32_t>(waMsgLen)));
                }
                if (tcMsgLen > 0) {  // trash-clump carry batch -- body built once above; stamp per-peer
                    PacketHeader tcHdr{};
                    WriteHeader(tcHdr, MsgType::TrashCarryPose, sendSeq_.fetch_add(1), ownEpoch_);
                    std::memcpy(tcBuf, &tcHdr, sizeof(tcHdr));
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, tcBuf, static_cast<uint32_t>(tcMsgLen),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    noteSent(rc, static_cast<int>(static_cast<uint32_t>(tcMsgLen)));
                }
                // A joiner still loading gets no driven-prop pose either: the end edge that would hand
                // its copy back is not sent to a world that is not ready (session_lanes.h), so a park
                // started now could outlive the drive, and the host re-sends every driven prop's pose
                // at the ready edge (prop_drive_host::OnPeerWorldReady).
                if (pdMsgLen > 0 && IsSlotWorldReady(i)) {  // driven-prop batch -- body built once above; stamp per-peer
                    PacketHeader pdHdr{};
                    WriteHeader(pdHdr, MsgType::PropDrivePose, sendSeq_.fetch_add(1), ownEpoch_);
                    std::memcpy(pdBuf, &pdHdr, sizeof(pdHdr));
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, pdBuf, static_cast<uint32_t>(pdMsgLen),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    noteSent(rc, static_cast<int>(static_cast<uint32_t>(pdMsgLen)));
                }
                if (clockDue) {  // HOST world-clock snapshot -- same body to every peer
                    ClockPosePacket pkt{};
                    WriteHeader(pkt.header, MsgType::ClockPose, sendSeq_.fetch_add(1), ownEpoch_);
                    pkt.clock = local.clock.value;
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, &pkt, sizeof(pkt),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    noteSent(rc, static_cast<int>(sizeof(pkt)));
                }
                if (deskSimDue) {  // HOST download-sim output vector -- same body to every peer
                    DeskSimPosePacket pkt{};
                    WriteHeader(pkt.header, MsgType::DeskSimPose, sendSeq_.fetch_add(1), ownEpoch_);
                    pkt.sim = local.deskSim.value;
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, &pkt, sizeof(pkt),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    noteSent(rc, static_cast<int>(sizeof(pkt)));
                }
                if (dishPoseDue) {  // HOST dish-pose batch -- same body to every peer
                    DishPosePacket pkt{};
                    WriteHeader(pkt.header, MsgType::DishPose, sendSeq_.fetch_add(1), ownEpoch_);
                    pkt.body = local.dish.value;
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, &pkt, sizeof(pkt),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    noteSent(rc, static_cast<int>(sizeof(pkt)));
                }
                if (reelPoseDue) {  // HOST reel corrector -- same body to every peer
                    ReelPosePacket pkt{};
                    WriteHeader(pkt.header, MsgType::ReelPose, sendSeq_.fetch_add(1), ownEpoch_);
                    pkt.body = local.reel.value;
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, &pkt, sizeof(pkt),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    noteSent(rc, static_cast<int>(sizeof(pkt)));
                }
            }
        }
        if (deskSimDue) nextDeskSimSend = now + kDeskSimSendInterval;
        nextSend = now + sendInterval;
    }
}

}  // namespace coop::net
