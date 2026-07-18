// coop/net/session_streams.cpp -- the scalar per-channel pose/state STREAMS for
// Session (extracted from session.cpp 2026-07-18; the file sat at 1208 LOC, past
// the 800 soft cap, and every new stream channel grew it further).
//
// Owns the NINE scalar stream channels end-to-end at the Session layer:
//   per-peer:    PoseSnapshot / PropPose / RagdollPose / HandPose / DeskCursorPose
//   host-single: ClockPose / DeskSimPose / DishPose / ReelPose
// as three surface groups, ALL bodies verbatim from session.cpp:
//   - the game-thread publishers  (Set*)         -- local slots under localMutex_
//   - the game-thread readers     (TryGet*)      -- remote slots under remoteMutex_
//   - the net-thread receive-store (StoreStreamPacket) -- HandleMessage's grouped
//     scalar case labels delegate here (the session_npc.cpp Store* precedent)
//   - the net-thread send fan-out (SendStreamsTick)    -- NetThread's step 3,
//     including the npc/worldactor/trashcarry batch stamps (their Serialize*
//     bodies stay in their own TUs; this is the one per-tick fan-out loop).
// The BATCH channels (npc/worldactor/trashcarry/voice) keep their own TUs.
// Mutex discipline is UNCHANGED from the inline version: local* under
// localMutex_, remote* under remoteMutex_.

#include "coop/net/session.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#pragma warning(pop)

#include <cmath>
#include <cstring>
#include <mutex>

namespace coop::net {

// --- game-thread publishers (verbatim) --------------------------------------

void Session::SetLocalPose(const PoseSnapshot& pose) {
    std::lock_guard<std::mutex> lk(localMutex_);
    localPose_ = pose;
    hasLocal_ = true;
}

void Session::SetLocalPropPose(bool set, const PropPoseSnapshot& pose) {
    std::lock_guard<std::mutex> lk(localMutex_);
    hasLocalProp_ = set;
    if (set) localPropPose_ = pose;
}

void Session::SetLocalRagdollPose(bool set, const RagdollPoseSnapshot& pose) {
    std::lock_guard<std::mutex> lk(localMutex_);
    hasLocalRagdoll_ = set;
    if (set) localRagdollPose_ = pose;
}

void Session::SetLocalHandPose(bool set, const HandPoseSnapshot& pose) {
    std::lock_guard<std::mutex> lk(localMutex_);
    hasLocalHand_ = set;
    if (set) localHandPose_ = pose;
}

void Session::SetLocalDeskCursor(bool set, const DeskCursorPoseSnapshot& pose) {
    std::lock_guard<std::mutex> lk(localMutex_);
    hasLocalDeskCursor_ = set;
    if (set) localDeskCursor_ = pose;
}

void Session::SetHostClock(bool set, const TimeSyncPayload& clock) {
    std::lock_guard<std::mutex> lk(localMutex_);
    hasLocalHostClock_ = set;
    if (set) localHostClock_ = clock;
}

void Session::SetHostDeskSim(bool set, const DeskSimSnapshot& sim) {
    std::lock_guard<std::mutex> lk(localMutex_);
    hasLocalDeskSim_ = set;
    if (set) localDeskSim_ = sim;
}

void Session::SetHostDishPose(const DishPoseBody& body) {
    std::lock_guard<std::mutex> lk(localMutex_);
    localDishPose_ = body;
    dishPoseDirty_ = true;  // one-shot: the net thread sends once + clears
}

void Session::SetHostReelPose(const ReelPosePayload& body) {  // v114 (L7)
    std::lock_guard<std::mutex> lk(localMutex_);
    localReelPose_ = body;
    reelPoseDirty_ = true;  // one-shot: the net thread sends once + clears
}

// --- game-thread readers (verbatim) -----------------------------------------

bool Session::TryGetRemotePose(int peerSlot, PoseSnapshot& out, bool* outIsNew) {
    if (state_.load() != ConnState::Connected) return false;
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (!hasRemote_[peerSlot]) return false;
    out = remotePoses_[peerSlot];
    if (outIsNew) *outIsNew = (remoteStamp_[peerSlot] != lastReadStamp_[peerSlot]);
    lastReadStamp_[peerSlot] = remoteStamp_[peerSlot];
    return true;
}

bool Session::TryGetRemotePropPose(int peerSlot, PropPoseSnapshot& out, bool* outIsNew) {
    if (state_.load() != ConnState::Connected) return false;
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (!hasRemoteProp_[peerSlot]) return false;
    out = remotePropPoses_[peerSlot];
    if (outIsNew) *outIsNew = (remotePropStamp_[peerSlot] != lastReadPropStamp_[peerSlot]);
    lastReadPropStamp_[peerSlot] = remotePropStamp_[peerSlot];
    return true;
}

bool Session::TryGetRemoteRagdollPose(int peerSlot, RagdollPoseSnapshot& out, bool* outIsNew) {
    if (state_.load() != ConnState::Connected) return false;
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (!hasRemoteRagdoll_[peerSlot]) return false;
    out = remoteRagdollPoses_[peerSlot];
    if (outIsNew) *outIsNew = (remoteRagdollStamp_[peerSlot] != lastReadRagdollStamp_[peerSlot]);
    lastReadRagdollStamp_[peerSlot] = remoteRagdollStamp_[peerSlot];
    return true;
}

bool Session::TryGetRemoteHandPose(int peerSlot, HandPoseSnapshot& out, bool* outIsNew) {
    if (state_.load() != ConnState::Connected) return false;
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (!hasRemoteHand_[peerSlot]) return false;
    out = remoteHandPoses_[peerSlot];
    if (outIsNew) *outIsNew = (remoteHandStamp_[peerSlot] != lastReadHandStamp_[peerSlot]);
    lastReadHandStamp_[peerSlot] = remoteHandStamp_[peerSlot];
    return true;
}

bool Session::TryGetRemoteDeskCursor(int peerSlot, DeskCursorPoseSnapshot& out, bool* outIsNew) {
    if (state_.load() != ConnState::Connected) return false;
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (!hasRemoteDeskCursor_[peerSlot]) return false;
    out = remoteDeskCursors_[peerSlot];
    if (outIsNew) *outIsNew = (remoteDeskCursorStamp_[peerSlot] != lastReadDeskCursorStamp_[peerSlot]);
    lastReadDeskCursorStamp_[peerSlot] = remoteDeskCursorStamp_[peerSlot];
    return true;
}

bool Session::TryGetHostClock(TimeSyncPayload& out, bool* outIsNew) {
    if (state_.load() != ConnState::Connected) return false;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (!hasRemoteHostClock_) return false;
    out = remoteHostClock_;
    if (outIsNew) *outIsNew = (remoteHostClockStamp_ != lastReadHostClockStamp_);
    lastReadHostClockStamp_ = remoteHostClockStamp_;
    return true;
}

bool Session::TryGetHostDeskSim(DeskSimSnapshot& out, bool* outIsNew) {
    if (state_.load() != ConnState::Connected) return false;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (!hasRemoteDeskSim_) return false;
    out = remoteDeskSim_;
    if (outIsNew) *outIsNew = (remoteDeskSimStamp_ != lastReadDeskSimStamp_);
    lastReadDeskSimStamp_ = remoteDeskSimStamp_;
    return true;
}

bool Session::TryGetHostReelPose(ReelPosePayload& out, bool* outIsNew) {  // v114 (L7)
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (!hasRemoteReelPose_) return false;
    out = remoteReelPose_;
    if (outIsNew) *outIsNew = (remoteReelPoseStamp_ != lastReadReelPoseStamp_);
    lastReadReelPoseStamp_ = remoteReelPoseStamp_;
    return true;
}

bool Session::TryGetHostDishPose(DishPoseBody& out, bool* outIsNew) {
    if (state_.load() != ConnState::Connected) return false;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    if (!hasRemoteDishPose_) return false;
    out = remoteDishPose_;
    if (outIsNew) *outIsNew = (remoteDishPoseStamp_ != lastReadDishPoseStamp_);
    lastReadDishPoseStamp_ = remoteDishPoseStamp_;
    return true;
}

// --- net-thread receive-store: the 9 scalar stream cases (verbatim bodies) ---
// Called from HandleMessage's grouped case labels AFTER the header parse, the
// epoch latch, and the routeSlot derivation -- exactly the point the inline
// switch cases ran at. `return` here == the old `return` from HandleMessage
// (nothing followed the switch).

void Session::StoreStreamPacket(MsgType type, int routeSlot, int peerSlot,
                                const void* data, int len, uint32_t seq) {
    switch (type) {
    case MsgType::PoseSnapshot: {
        if (len < static_cast<int>(sizeof(PosePacket))) return;
        PosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        if (!ValidatePose(pkt.pose)) return;
        {
            std::lock_guard<std::mutex> lk(remoteMutex_);
            if (hasRemote_[routeSlot] &&
                static_cast<int32_t>(seq - lastRemoteSeq_[routeSlot]) <= 0) {
                break;  // stale/duplicate for this origin slot; still relayed? no -- a stale packet need not propagate
            }
            remotePoses_[routeSlot] = pkt.pose;
            lastRemoteSeq_[routeSlot] = seq;
            hasRemote_[routeSlot] = true;
            ++remoteStamp_[routeSlot];
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
            if (hasRemoteProp_[routeSlot] &&
                static_cast<int32_t>(seq - lastRemotePropSeq_[routeSlot]) <= 0) {
                break;
            }
            remotePropPoses_[routeSlot] = pkt.pose;
            lastRemotePropSeq_[routeSlot] = seq;
            hasRemoteProp_[routeSlot] = true;
            ++remotePropStamp_[routeSlot];
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
            if (hasRemoteRagdoll_[routeSlot] &&
                static_cast<int32_t>(seq - lastRemoteRagdollSeq_[routeSlot]) <= 0) {
                break;
            }
            remoteRagdollPoses_[routeSlot] = pkt.pose;
            lastRemoteRagdollSeq_[routeSlot] = seq;
            hasRemoteRagdoll_[routeSlot] = true;
            ++remoteRagdollStamp_[routeSlot];
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
            if (hasRemoteHand_[routeSlot] &&
                static_cast<int32_t>(seq - lastRemoteHandSeq_[routeSlot]) <= 0) {
                break;
            }
            remoteHandPoses_[routeSlot] = pkt.pose;
            lastRemoteHandSeq_[routeSlot] = seq;
            hasRemoteHand_[routeSlot] = true;
            ++remoteHandStamp_[routeSlot];
        }
        // Host relay: forward this client's hand pose to every OTHER client.
        if (cfg_.role == Role::Host) {
            RelayUnreliableToOtherClients(peerSlot, data, len);
        }
        break;
    }
    case MsgType::DeskCursorPose: {  // v109: coords-panel live cursor (sibling of HandPose)
        if (len < static_cast<int>(sizeof(DeskCursorPosePacket))) return;
        DeskCursorPosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        // Trust-boundary sanitize: viewCoordinate is screen-space (no fixed magnitude
        // bound), so a finite check is the sane floor before it reaches WriteCursorOnly.
        if (!std::isfinite(pkt.pose.viewX) || !std::isfinite(pkt.pose.viewY)) return;
        {
            std::lock_guard<std::mutex> lk(remoteMutex_);
            if (hasRemoteDeskCursor_[routeSlot] &&
                static_cast<int32_t>(seq - lastRemoteDeskCursorSeq_[routeSlot]) <= 0) {
                break;
            }
            remoteDeskCursors_[routeSlot] = pkt.pose;
            lastRemoteDeskCursorSeq_[routeSlot] = seq;
            hasRemoteDeskCursor_[routeSlot] = true;
            ++remoteDeskCursorStamp_[routeSlot];
        }
        // Host relay: forward this client's cursor to every OTHER client.
        if (cfg_.role == Role::Host) {
            RelayUnreliableToOtherClients(peerSlot, data, len);
        }
        break;
    }
    case MsgType::ClockPose: {  // v109 (design F): HOST->all world-clock snapshot (single value, newest-wins)
        if (len < static_cast<int>(sizeof(ClockPosePacket))) return;
        // Host is authoritative -- it owns the clock and never applies a received one (a self-echo
        // via the relay can't reach it: this kind is host-originated + not relayed, but guard anyway).
        if (cfg_.role == Role::Host) break;
        ClockPosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        {
            std::lock_guard<std::mutex> lk(remoteMutex_);
            if (hasRemoteHostClock_ &&
                static_cast<int32_t>(seq - lastRemoteHostClockSeq_) <= 0) {
                break;  // older/duplicate snapshot -- keep the newer one
            }
            remoteHostClock_ = pkt.clock;
            lastRemoteHostClockSeq_ = seq;
            hasRemoteHostClock_ = true;
            ++remoteHostClockStamp_;
        }
        break;
    }
    case MsgType::DeskSimPose: {  // v111: HOST->all download-sim output vector (single value, newest-wins)
        if (len < static_cast<int>(sizeof(DeskSimPosePacket))) return;
        // Host owns the sim and never applies a received one (host-originated + not relayed; guard anyway).
        if (cfg_.role == Role::Host) break;
        DeskSimPosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        {
            std::lock_guard<std::mutex> lk(remoteMutex_);
            if (hasRemoteDeskSim_ &&
                static_cast<int32_t>(seq - lastRemoteDeskSimSeq_) <= 0) {
                break;  // older/duplicate snapshot -- keep the newer one
            }
            remoteDeskSim_ = pkt.sim;
            lastRemoteDeskSimSeq_ = seq;
            hasRemoteDeskSim_ = true;
            ++remoteDeskSimStamp_;
        }
        break;
    }
    case MsgType::DishPose: {  // v113 (L4): HOST->all dish-pose row batch (newest-wins)
        if (len < static_cast<int>(sizeof(DishPosePacket))) return;
        if (cfg_.role == Role::Host) break;  // host-originated; never applied locally
        DishPosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        if (pkt.body.count > kMaxDishes) return;
        {
            std::lock_guard<std::mutex> lk(remoteMutex_);
            if (hasRemoteDishPose_ &&
                static_cast<int32_t>(seq - lastRemoteDishPoseSeq_) <= 0) {
                break;  // older/duplicate batch -- keep the newer one
            }
            remoteDishPose_ = pkt.body;
            lastRemoteDishPoseSeq_ = seq;
            hasRemoteDishPose_ = true;
            ++remoteDishPoseStamp_;
        }
        break;
    }
    case MsgType::ReelPose: {  // v114 (L7): HOST->all reel corrector (newest-wins)
        if (len < static_cast<int>(sizeof(ReelPosePacket))) return;
        if (cfg_.role == Role::Host) break;  // host-originated; never applied locally
        ReelPosePacket pkt;
        std::memcpy(&pkt, data, sizeof(pkt));
        {
            std::lock_guard<std::mutex> lk(remoteMutex_);
            if (hasRemoteReelPose_ &&
                static_cast<int32_t>(seq - lastRemoteReelPoseSeq_) <= 0) {
                break;  // older/duplicate -- keep the newer one
            }
            remoteReelPose_ = pkt.body;
            lastRemoteReelPoseSeq_ = seq;
            hasRemoteReelPose_ = true;
            ++remoteReelPoseStamp_;
        }
        break;
    }
    default:
        break;
    }
}

// --- net-thread step-3 stream fan-out (verbatim from NetThread) --------------
// `now` is computed ONCE in the NetThread shell (step 4's net-diag shares the
// same timestamp) and passed in; the cadence time_points live in the shell as
// net-thread locals and are advanced here by reference. Wrapper lines (the ONLY
// non-verbatim lines): the function head, the sockets re-fetch, and the two
// cadence constexprs hoisted in from the old NetThread prologue.

void Session::SendStreamsTick(std::chrono::steady_clock::time_point now,
                              std::chrono::milliseconds sendInterval,
                              std::chrono::steady_clock::time_point& nextSend,
                              std::chrono::steady_clock::time_point& nextClockSend,
                              std::chrono::steady_clock::time_point& nextDeskSimSend,
                              uint64_t& sendFails) {
    auto* sockets = SteamNetworkingSockets();
    constexpr auto kClockSendInterval = std::chrono::milliseconds(500);
    constexpr auto kDeskSimSendInterval = std::chrono::milliseconds(100);  // v111: ~10 Hz

    if (state_.load() == ConnState::Connected && now >= nextSend) {
        PoseSnapshot local;
        bool have;
        PropPoseSnapshot localProp;
        bool haveProp;
        RagdollPoseSnapshot localRagdoll;
        bool haveRagdoll;
        HandPoseSnapshot localHand;
        bool haveHand;
        DeskCursorPoseSnapshot localDeskCursor;
        bool haveDeskCursor;
        TimeSyncPayload localHostClock;
        bool haveHostClock;
        DeskSimSnapshot localDeskSim;
        bool haveDeskSim;
        DishPoseBody localDishPose;
        bool dishPoseDue;
        ReelPosePayload localReelPose;
        bool reelPoseDue;
        { std::lock_guard<std::mutex> lk(localMutex_);
          local = localPose_; have = hasLocal_;
          localProp = localPropPose_; haveProp = hasLocalProp_;
          localRagdoll = localRagdollPose_; haveRagdoll = hasLocalRagdoll_;
          localHand = localHandPose_; haveHand = hasLocalHand_;
          localDeskCursor = localDeskCursor_; haveDeskCursor = hasLocalDeskCursor_;
          localHostClock = localHostClock_; haveHostClock = hasLocalHostClock_;
          localDeskSim = localDeskSim_; haveDeskSim = hasLocalDeskSim_;
          // v113 (L4): dirty one-shot -- the GT sweep owns the cadence; consume the flag.
          localDishPose = localDishPose_;
          dishPoseDue = dishPoseDirty_ && cfg_.role == Role::Host;
          dishPoseDirty_ = false;
          // v114 (L7): reel corrector -- same dirty one-shot shape.
          localReelPose = localReelPose_;
          reelPoseDue = reelPoseDirty_ && cfg_.role == Role::Host;
          reelPoseDirty_ = false; }
        // v109 (design F): the clock rides its OWN 500 ms throttle, and only the HOST
        // originates it. Computed once here so the per-peer fan-out below sends the same
        // snapshot to every peer this round (and nextClockSend advances once, after).
        const bool clockDue = haveHostClock && cfg_.role == Role::Host && now >= nextClockSend;
        const bool deskSimDue = haveDeskSim && cfg_.role == Role::Host && now >= nextDeskSimSend;
        // v37: serialize the live NPC pose batch ONCE (same body for every peer; only the
        // per-peer header seq differs). SerializeLocalNpcBatch (session_npc.cpp) reads
        // localNpcBatch_ under localMutex_ + writes the body after the leading PacketHeader,
        // returning 0 when there is no batch to send this tick (no intermediate copy).
        uint8_t npcBuf[kNpcPoseDatagramMax];
        const int npcMsgLen = SerializeLocalNpcBatch(npcBuf);
        // v80 (B3b): the live WorldActor pose batch, serialized ONCE like the NPC batch (host-only
        // producer -- SerializeLocalWorldActorBatch returns 0 on a client / when no actors stream).
        uint8_t waBuf[kWorldActorPoseDatagramMax];
        const int waMsgLen = SerializeLocalWorldActorBatch(waBuf);
        // v85 (Increment 2): the carried-trash-clump pose batch, serialized ONCE (host-only producer
        // -- SerializeLocalTrashCarryBatch returns 0 on a client / when no clump is carried).
        uint8_t tcBuf[kTrashCarryPoseDatagramMax];
        const int tcMsgLen = SerializeLocalTrashCarryBatch(tcBuf);
        if (have || haveProp || haveRagdoll || haveHand || haveDeskCursor || clockDue || deskSimDue ||
            dishPoseDue || reelPoseDue || npcMsgLen > 0 || waMsgLen > 0 || tcMsgLen > 0) {
            for (int i = 0; i < kMaxPeers; ++i) {
                const uint32_t hConn = peerConns_[i].load();
                if (hConn == 0) continue;
                if (have) {
                    PosePacket pkt{};
                    WriteHeader(pkt.header, MsgType::PoseSnapshot,
                                sendSeq_.fetch_add(1), ownEpoch_);
                    pkt.pose = local;
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, &pkt, sizeof(pkt),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    if (rc == k_EResultOK) net_stats::AddSent(sizeof(pkt)); else ++sendFails;
                }
                if (haveProp) {
                    PropPosePacket pkt{};
                    WriteHeader(pkt.header, MsgType::PropPose,
                                sendSeq_.fetch_add(1), ownEpoch_);
                    pkt.pose = localProp;
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, &pkt, sizeof(pkt),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    if (rc == k_EResultOK) net_stats::AddSent(sizeof(pkt)); else ++sendFails;
                }
                if (haveRagdoll) {
                    RagdollPosePacket pkt{};
                    WriteHeader(pkt.header, MsgType::RagdollPose,
                                sendSeq_.fetch_add(1), ownEpoch_);
                    pkt.pose = localRagdoll;
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, &pkt, sizeof(pkt),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    if (rc == k_EResultOK) net_stats::AddSent(sizeof(pkt)); else ++sendFails;
                }
                if (haveHand) {  // v109: hand-item view-relative transform (while holding)
                    HandPosePacket pkt{};
                    WriteHeader(pkt.header, MsgType::HandPose,
                                sendSeq_.fetch_add(1), ownEpoch_);
                    pkt.pose = localHand;
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, &pkt, sizeof(pkt),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    if (rc == k_EResultOK) net_stats::AddSent(sizeof(pkt)); else ++sendFails;
                }
                if (haveDeskCursor) {  // v109: coords-panel live cursor (while desk-claimed + moving)
                    DeskCursorPosePacket pkt{};
                    WriteHeader(pkt.header, MsgType::DeskCursorPose,
                                sendSeq_.fetch_add(1), ownEpoch_);
                    pkt.pose = localDeskCursor;
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, &pkt, sizeof(pkt),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    if (rc == k_EResultOK) net_stats::AddSent(sizeof(pkt)); else ++sendFails;
                }
                if (npcMsgLen > 0) {  // v37: NPC pose batch -- body built once above; stamp the header per-peer
                    PacketHeader npcHdr{};  // build + memcpy (npcBuf is uint8_t[]; no misaligned PacketHeader lvalue)
                    WriteHeader(npcHdr, MsgType::EntityPose, sendSeq_.fetch_add(1), ownEpoch_);
                    std::memcpy(npcBuf, &npcHdr, sizeof(npcHdr));
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, npcBuf, static_cast<uint32_t>(npcMsgLen),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    if (rc == k_EResultOK) net_stats::AddSent(static_cast<uint32_t>(npcMsgLen)); else ++sendFails;
                }
                if (waMsgLen > 0) {  // v80 (B3b): WorldActor pose batch -- body built once above; stamp the header per-peer
                    PacketHeader waHdr{};
                    WriteHeader(waHdr, MsgType::WorldActorPose, sendSeq_.fetch_add(1), ownEpoch_);
                    std::memcpy(waBuf, &waHdr, sizeof(waHdr));
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, waBuf, static_cast<uint32_t>(waMsgLen),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    if (rc == k_EResultOK) net_stats::AddSent(static_cast<uint32_t>(waMsgLen)); else ++sendFails;
                }
                if (tcMsgLen > 0) {  // v85 (Increment 2): trash-clump carry batch -- body built once above; stamp per-peer
                    PacketHeader tcHdr{};
                    WriteHeader(tcHdr, MsgType::TrashCarryPose, sendSeq_.fetch_add(1), ownEpoch_);
                    std::memcpy(tcBuf, &tcHdr, sizeof(tcHdr));
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, tcBuf, static_cast<uint32_t>(tcMsgLen),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    if (rc == k_EResultOK) net_stats::AddSent(static_cast<uint32_t>(tcMsgLen)); else ++sendFails;
                }
                if (clockDue) {  // v109 (design F): HOST world-clock snapshot -- same body to every peer
                    ClockPosePacket pkt{};
                    WriteHeader(pkt.header, MsgType::ClockPose, sendSeq_.fetch_add(1), ownEpoch_);
                    pkt.clock = localHostClock;
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, &pkt, sizeof(pkt),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    if (rc == k_EResultOK) net_stats::AddSent(sizeof(pkt)); else ++sendFails;
                }
                if (deskSimDue) {  // v111: HOST download-sim output vector -- same body to every peer
                    DeskSimPosePacket pkt{};
                    WriteHeader(pkt.header, MsgType::DeskSimPose, sendSeq_.fetch_add(1), ownEpoch_);
                    pkt.sim = localDeskSim;
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, &pkt, sizeof(pkt),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    if (rc == k_EResultOK) net_stats::AddSent(sizeof(pkt)); else ++sendFails;
                }
                if (dishPoseDue) {  // v113 (L4): HOST dish-pose batch -- same body to every peer
                    DishPosePacket pkt{};
                    WriteHeader(pkt.header, MsgType::DishPose, sendSeq_.fetch_add(1), ownEpoch_);
                    pkt.body = localDishPose;
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, &pkt, sizeof(pkt),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    if (rc == k_EResultOK) net_stats::AddSent(sizeof(pkt)); else ++sendFails;
                }
                if (reelPoseDue) {  // v114 (L7): HOST reel corrector -- same body to every peer
                    ReelPosePacket pkt{};
                    WriteHeader(pkt.header, MsgType::ReelPose, sendSeq_.fetch_add(1), ownEpoch_);
                    pkt.body = localReelPose;
                    const EResult rc = sockets->SendMessageToConnection(
                        hConn, &pkt, sizeof(pkt),
                        k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
                    if (rc == k_EResultOK) net_stats::AddSent(sizeof(pkt)); else ++sendFails;
                }
            }
        }
        if (clockDue) nextClockSend = now + kClockSendInterval;  // advance once per round, after the fan-out
        if (deskSimDue) nextDeskSimSend = now + kDeskSimSendInterval;
        nextSend = now + sendInterval;
    }
}

}  // namespace coop::net
