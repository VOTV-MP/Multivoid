// coop/net/session_status.cpp -- GNS status callback + peer-slot bookkeeping.
//
// Extracted from coop/net/session.cpp at M-1 2026-05-29 to bring that file
// under the 800-LOC soft cap (was 814 LOC). The split point is the
// "connection state-machine" subsystem: everything driven by GNS's status
// callback (None->Connecting->Connected->ClosedByPeer/ProblemDetectedLocally)
// and the per-slot helpers that exclusively serve it.
//
// Stays in session.cpp:
//   - g_session (anon-ns) -- accessed only by OnConnStatusChanged
//   - OnConnStatusChanged + ConnStatusTrampoline -- the GNS bridge
//   - EnsureGnsInit + g_initMutex/g_inited -- called only from Start
//   - Lane enum + LaneForKind -- used by the SendReliable path
//   - Start / Stop / send paths / Try*Get / NetThread / HandleMessage
//
// Moves here:
//   - ConfigureLanesForPeer (anon-ns helper, only called from
//     HandleConnStatusChanged)
//   - Session::FindFreePeerSlotForClient
//   - Session::FindPeerSlotForConn
//   - Session::ResetPeerRemoteState
//   - Session::connectedPeerCount
//   - Session::HandleConnStatusChanged (the 190-LOC state-machine)
//
// All five member functions remain ordinary class members declared in
// session.h; splitting their definitions across translation units is
// the standard C++ idiom and requires no header / public-API changes.
// The single anon-ns helper that moves is exclusively called from the
// member fn that moves with it, so no cross-TU linkage is added.

#include "coop/net/session.h"

#include "coop/players_registry.h"
#include "ue_wrap/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#pragma warning(pop)

namespace coop::net {

namespace {

// PR-3 lane plumbing applied to a freshly-connected peer (called from the
// Connected status callback). On failure, reliable sends collapse to lane 0
// (still functional, just no priority routing).
void ConfigureLanesForPeer(HSteamNetConnection hConn) {
    constexpr int kLaneCount = 3;  // matches Lane::Count in session.cpp
    const int priorities[kLaneCount] = { 0, 1, 2 };
    const uint16 weights[kLaneCount] = { 4, 2, 1 };
    const EResult rc = SteamNetworkingSockets()->ConfigureConnectionLanes(
        hConn, kLaneCount, priorities, weights);
    if (rc != k_EResultOK) {
        UE_LOGW("net: ConfigureConnectionLanes(h=0x%08x) rc=%d",
                static_cast<unsigned>(hConn), static_cast<int>(rc));
    }
}

}  // namespace

int Session::FindFreePeerSlotForClient() {
    // Host: scan client slots [1..kMaxPeers-1] for the lowest unoccupied one.
    // Slot 0 reserved for "host self" -- never holds a remote connection here.
    for (int i = 1; i < kMaxPeers; ++i) {
        if (peerConns_[i].load() == 0) return i;
    }
    return -1;
}

int Session::FindPeerSlotForConn(uint32_t hConn) {
    for (int i = 0; i < kMaxPeers; ++i) {
        if (peerConns_[i].load() == hConn) return i;
    }
    return -1;
}

void Session::ResetPeerRemoteState(int peerSlot) {
    // remoteMutex_ held by caller.
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return;
    hasRemote_[peerSlot] = false;
    lastRemoteSeq_[peerSlot] = 0;
    remoteStamp_[peerSlot] = 0;
    lastReadStamp_[peerSlot] = 0;
    hasRemoteProp_[peerSlot] = false;
    lastRemotePropSeq_[peerSlot] = 0;
    remotePropStamp_[peerSlot] = 0;
    lastReadPropStamp_[peerSlot] = 0;
    // PR-FOUNDATION-1b v16: clear the latched senderEpoch so the next
    // connection on this slot re-latches via HandleMessage's first-packet
    // path. Without this, a reconnecting peer's fresh epoch would fail
    // the compare against the dead generation's stored value.
    expectedEpoch_[peerSlot] = 0;
}

int Session::connectedPeerCount() const {
    // Count only peers whose lanes are configured (= Connected state). Counting
    // Connecting-state slots (peerConns_ set in the Connecting callback but
    // peerLanesConfigured_ not yet set in the Connected callback) delays the
    // aggregate Disconnected transition and triggers snapshot fan-out toward a
    // half-open connection.
    int n = 0;
    for (int i = 0; i < kMaxPeers; ++i) {
        if (peerConns_[i].load() != 0 && peerLanesConfigured_[i].load()) ++n;
    }
    return n;
}

void Session::HandleConnStatusChanged(void* info) {
    auto* cb = static_cast<SteamNetConnectionStatusChangedCallback_t*>(info);
    const HSteamNetConnection hConn = cb->m_hConn;
    const auto oldState = cb->m_eOldState;
    const auto newState = cb->m_info.m_eState;
    auto* sockets = SteamNetworkingSockets();

    // --- Host: accept incoming clients up to kMaxPeers-1 of them.
    if (cfg_.role == Role::Host &&
        oldState == k_ESteamNetworkingConnectionState_None &&
        newState == k_ESteamNetworkingConnectionState_Connecting) {
        const int slot = FindFreePeerSlotForClient();
        if (slot < 0) {
            UE_LOGW("net: host full (%d/%d slots) -- rejecting incoming connection",
                    kMaxPeers - 1, kMaxPeers - 1);
            sockets->CloseConnection(hConn, 0, "host full", false);
            return;
        }
        const EResult rc = sockets->AcceptConnection(hConn);
        if (rc != k_EResultOK) {
            UE_LOGW("net: AcceptConnection rc=%d", static_cast<int>(rc));
            sockets->CloseConnection(hConn, 0, "accept failed", false);
            return;
        }
        // Tag the connection with its peer slot so ReceiveMessagesOnPollGroup
        // can recover the sender in O(1) via msg->m_nConnUserData.
        sockets->SetConnectionUserData(hConn, slot);
        // Add to the host's PollGroup so we drain all clients with one call.
        const uint32_t hPoll = hPollGroup_.load();
        if (hPoll != 0) {
            sockets->SetConnectionPollGroup(hConn, static_cast<HSteamNetPollGroup>(hPoll));
        }
        peerConns_[slot].store(hConn);
        // Only demote to Handshaking if currently Disconnected. If peer-1 is
        // already Connected and peer-2 starts connecting, the aggregate state
        // must remain Connected -- otherwise pose fan-out to peer-1 pauses,
        // TryGetRemotePose returns false, event_feed re-sends Join, and
        // harness teardown can trigger for ~10-200ms of handshake.
        if (state_.load() == ConnState::Disconnected) {
            state_.store(ConnState::Handshaking);
        }
        UE_LOGI("net: host accepted client at slot %d (h=0x%08x, %d/%d connected)",
                slot, static_cast<unsigned>(hConn),
                connectedPeerCount(), kMaxPeers - 1);
        return;
    }

    // --- Both roles: state transitions on an existing connection.

    if (newState == k_ESteamNetworkingConnectionState_Connected) {
        int slot = FindPeerSlotForConn(hConn);
        // GNS may skip the None->Connecting transition in rare cases (per
        // SteamNetConnectionStatusChangedCallback_t header doc). When that
        // happens on host, the slot is unregistered. Late-register here so
        // the connection has a known slot and SetConnectionUserData lands.
        if (slot < 0 && cfg_.role == Role::Host) {
            slot = FindFreePeerSlotForClient();
            if (slot < 0) {
                UE_LOGW("net: host full at Connected (Connecting was skipped) -- closing h=0x%08x",
                        static_cast<unsigned>(hConn));
                sockets->CloseConnection(hConn, 0, "host full", false);
                return;
            }
            sockets->SetConnectionUserData(hConn, slot);
            const uint32_t hPoll = hPollGroup_.load();
            if (hPoll != 0) {
                sockets->SetConnectionPollGroup(hConn, static_cast<HSteamNetPollGroup>(hPoll));
            }
            peerConns_[slot].store(hConn);
            UE_LOGI("net: late-registered slot %d (Connecting was skipped, h=0x%08x)",
                    slot, static_cast<unsigned>(hConn));
        }
        if (slot < 0) {
            UE_LOGW("net: Connected on unknown connection h=0x%08x (role=%s)",
                    static_cast<unsigned>(hConn),
                    cfg_.role == Role::Host ? "host" : "client");
            return;
        }
        ConfigureLanesForPeer(hConn);
        // Order matters: lanes-configured flag flips ONLY after the
        // ConfigureConnectionLanes call returns, so IsSlotReady() readers
        // see the slot as ready only when the per-kind lane mapping is
        // live on the connection. Acquire/release pair below pairs with
        // the IsSlotReady() relaxed load (any subsequent send through
        // SendReliable etc. happens-before consumer dispatch).
        peerLanesConfigured_[slot].store(true, std::memory_order_release);
        if (state_.load() != ConnState::Connected) {
            state_.store(ConnState::Connected);
        }
        UE_LOGI("net: peer slot %d CONNECTED (%s, h=0x%08x)",
                slot, cfg_.role == Role::Host ? "host" : "client",
                static_cast<unsigned>(hConn));
        // Host tells the freshly-connected client which peer slot it was
        // assigned (clients no longer self-stamp peerSessionId=1; the
        // host is the only authority on slot assignment so two clients
        // can't silently collide). Status callback runs on the net
        // thread; SendReliableToSlot is thread-safe via GNS's queue.
        if (cfg_.role == Role::Host) {
            AssignPeerSlotPayload p{};
            p.slot = static_cast<uint8_t>(slot);
            // v13 (A4 2026-05-29): stamp the host's local Player Element id
            // so the client can RegisterMirror it in slot 0. Read is from
            // the net thread (this callback fires off ReceiveMessagesOnPoll
            // / SteamNetworkingSockets thread), not the game thread; the
            // host's slot-0 Element is allocated by net_pump.cpp every tick
            // (idempotent) and stays for the session lifetime, so this read
            // is well-defined unless the client connects in the
            // ~tens-of-ms boot window before the first net pump tick --
            // in which case the read returns kInvalidId, and the client
            // receiver falls back to non-mirror routing (the field's
            // contract documents 0/kInvalidId as "sender had no Element").
            // v16 PR-FOUNDATION-1b: the hostContext byte that v14 added
            // here is gone; per-peer stale-generation defense moved to the
            // packet header's senderEpoch, stamped by WriteHeader for this
            // SendReliableToSlot like every other outbound packet.
            p.hostElementId = coop::players::Registry::Get().LocalPlayerElementId();
            if (!SendReliableToSlot(slot, ReliableKind::AssignPeerSlot, &p, sizeof(p))) {
                UE_LOGW("net: SendReliableToSlot(AssignPeerSlot=%d) failed", slot);
            } else {
                UE_LOGI("net: sent AssignPeerSlot slot=%d hostElementId=0x%08x to client",
                        slot, p.hostElementId);
            }
        }
        return;
    }

    if (newState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
        newState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
        const int slot = FindPeerSlotForConn(hConn);
        UE_LOGW("net: peer slot %d closed (oldState=%d reason='%s')",
                slot, static_cast<int>(oldState), cb->m_info.m_szEndDebug);
        if (slot >= 0) {
            peerConns_[slot].store(0);
            peerLanesConfigured_[slot].store(false, std::memory_order_release);
        }
        // Per the GNS header doc on the status callback, terminal states
        // require us to CloseConnection to release the handle.
        sockets->CloseConnection(hConn, 0, nullptr, false);

        // Per-slot reset so a reconnecting peer (whose seq restarts at 0) is
        // not stale-dropped.
        { std::lock_guard<std::mutex> lk(remoteMutex_);
          if (slot >= 0) ResetPeerRemoteState(slot); }

        // Drop reliable messages still queued from the departing peer.
        // Without this a PropSpawn from a ghost peer can land in the game
        // thread AFTER the slot has been cleared, and no future PropDestroy
        // can ever arrive.
        if (slot >= 0) {
            std::lock_guard<std::mutex> lk(reliableInboxMutex_);
            for (auto it = reliableInbox_.begin(); it != reliableInbox_.end();) {
                if (it->senderPeerSlot == slot) it = reliableInbox_.erase(it);
                else ++it;
            }
        }

        // Aggregate state: stay Connected if any peer still up; otherwise
        // downgrade and clear everything.
        if (connectedPeerCount() == 0) {
            // Full disconnect goes to Disconnected, not Handshaking.
            // Reconnect UI / harness polling state()==Disconnected was
            // permanently blocked when this said Handshaking.
            state_.store(ConnState::Disconnected);
            { std::lock_guard<std::mutex> lk(remoteMutex_);
              for (int i = 0; i < kMaxPeers; ++i) ResetPeerRemoteState(i); }
            { std::lock_guard<std::mutex> lk(reliableInboxMutex_); reliableInbox_.clear(); }
            lastRttMs_.store(0);
            UE_LOGI("net: all peers gone -- session back to Disconnected");
        }
    }
}

}  // namespace coop::net
