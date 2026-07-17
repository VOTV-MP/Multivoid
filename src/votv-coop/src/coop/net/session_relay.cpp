// coop/net/session_relay.cpp -- host-relay topology fan-out (PR-FOUNDATION Tier 2).
//
// Extracted from coop/net/session.cpp at T2-3 (2026-05-29) when that file
// crossed the 800-LOC soft cap. This is the "host as relay hub" subsystem:
// in the star topology a packet from client A reaches only the host; these
// two methods forward A's packets to the OTHER clients so peers see each
// other. MTA shape: CGame relays puresync/RPC as it arrives.
//
// Both are Session member functions (declared in coop/net/session.h); their
// definitions live here. They are called only from Session::HandleMessage
// (session.cpp) on the net thread, host-role only.
//
// Header rewrite (common to both): the forwarded datagram's senderEpoch is
// replaced with the HOST's own epoch -- from the receiving client's POV the
// packet rides ITS host connection, whose epoch it latched, so the relayed
// copy must carry the host epoch to pass the connection-keyed v16 latch.
// senderSlot is set to the true ORIGIN connection slot so the receiver routes
// the payload to the right peer's puppet. seq + body are preserved, so
// per-origin-slot seq monotonicity holds on the receiver.

#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "session_lanes.h"  // Lane, LaneForKind (co-located private header)

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#pragma warning(pop)

#include <cstdint>
#include <cstring>

namespace coop::net {

void Session::RelayUnreliableToOtherClients(int originSlot, const void* data, int len) {
    // T2-2: forward an unreliable pose/proppose the host just received from
    // `originSlot` to every OTHER connected client.
    if (cfg_.role != Role::Host) return;
    if (len < static_cast<int>(sizeof(PacketHeader)) || len > kMaxPacketBytes) return;
    auto* sockets = SteamNetworkingSockets();
    if (!sockets) return;
    uint8_t buf[kMaxPacketBytes];
    std::memcpy(buf, data, static_cast<size_t>(len));
    auto* h = reinterpret_cast<PacketHeader*>(buf);
    h->senderEpoch = ownEpoch_;
    h->senderSlot = static_cast<uint8_t>(originSlot);
    h->_reserved2[0] = h->_reserved2[1] = h->_reserved2[2] = 0;
    for (int i = 1; i < kMaxPeers; ++i) {
        if (i == originSlot) continue;
        const uint32_t hConn = peerConns_[i].load();
        if (hConn == 0) continue;
        // v56 pre-world gate (B2): no pose relays to a joiner still at the menu.
        if (!IsSlotWorldReady(i)) continue;
        const EResult rc = sockets->SendMessageToConnection(
            hConn, buf, len, k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
        if (rc == k_EResultOK) net_stats::AddSent(static_cast<uint32_t>(len));
    }
}

void Session::RelayReliableToOtherClients(int originSlot, ReliableKind kind,
                                          const void* data, int len) {
    // T2-3: reliable sibling of the unreliable relay. Forward a peer-originated
    // gameplay reliable to every OTHER client on the reliable channel + the
    // kind's priority lane. HandleMessage has already filtered to relayable
    // kinds via IsClientRelayableReliableKind.
    //
    // DoS note: a flooding client makes the host fan out 1->(P-2) reliables.
    // Per-peer relay rate-limiting is deferred to PR-FOUNDATION-4
    // (kick/ban/ratelimit); for LAN/trusted-peer coop the existing
    // reliableInbox cap bounds the host's own queue and each receiver
    // re-validates (range + epoch), so a forged relay is dropped per-endpoint.
    if (cfg_.role != Role::Host) return;
    if (len < static_cast<int>(sizeof(PacketHeader)) || len > kMaxPacketBytes) return;
    auto* sockets = SteamNetworkingSockets();
    auto* utils = SteamNetworkingUtils();
    if (!sockets || !utils) return;
    const int laneIdx = static_cast<int>(LaneForKind(kind));
    for (int i = 1; i < kMaxPeers; ++i) {
        if (i == originSlot) continue;
        const uint32_t hConn = peerConns_[i].load();
        if (hConn == 0) continue;
        // v56 pre-world gate (B2): relayed gameplay reliables (all world-mutating
        // by the relay whitelist's nature) skip a joiner still at the menu -- its
        // world-ready replay re-derives the state.
        if (!IsSlotWorldReady(i) && !IsPreWorldSendableKind(kind)) continue;
        // One GNS-owned message per recipient (cannot share across sends).
        SteamNetworkingMessage_t* msg = utils->AllocateMessage(len);
        if (!msg) {
            UE_LOGW("net: relay AllocateMessage(%d) returned null", len);
            continue;
        }
        auto* dst = static_cast<uint8_t*>(msg->m_pData);
        std::memcpy(dst, data, static_cast<size_t>(len));
        auto* h = reinterpret_cast<PacketHeader*>(dst);
        h->senderEpoch = ownEpoch_;
        h->senderSlot = static_cast<uint8_t>(originSlot);
        h->_reserved2[0] = h->_reserved2[1] = h->_reserved2[2] = 0;
        msg->m_conn = hConn;
        msg->m_nFlags = k_nSteamNetworkingSend_Reliable;
        msg->m_idxLane = static_cast<uint16>(laneIdx);
        int64 outMsgNum = 0;
        sockets->SendMessages(1, &msg, &outMsgNum, /*bDeleteFailedMessages*/true);
        if (outMsgNum >= 0) net_stats::AddSent(static_cast<uint32_t>(len));
    }
}

}  // namespace coop::net
