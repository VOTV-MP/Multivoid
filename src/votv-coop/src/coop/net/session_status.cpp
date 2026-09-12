// coop/net/session_status.cpp -- the connection state machine: the GNS status callback
// (Connecting, Connected, closed), the pending band an unproved socket waits in, admission into a
// seat, the per-slot teardown shared by every close path, kick and ban.

#include "coop/net/session.h"

#include <cstdio>

#include "coop/config/config.h"           // ResolveInt, the wire knobs
#include "coop/config/config_registry.h"  // rows::net_sendbuf_kb / rows::net_sendrate_kbs
#include "coop/net/connect_history.h"     // the per-address connection cap at the accept edge
#include "coop/net/net_clock.h"           // NowMs, the net layer's one steady clock
#include "coop/net/peer_admission.h"      // the exchange state a pending entry owns
#include "coop/player/players_registry.h"
#include "ue_wrap/core/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>  // SteamNetworkingUtils(), the per-connection wire knobs
#pragma warning(pop)

namespace coop::net {

// The per-connection send buffer configured when the net.sendbuf_kb knob is 0; one constant,
// shared with the sendBufBytes_ mirror.
constexpr int kDefaultSendBufBytes = 4 * 1024 * 1024;

namespace {

// The lanes for a newly seated peer; on failure reliable sends collapse to lane 0 (functional, no
// priority routing).
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
    // Per-connection wire knobs for the delivery drill and slow-link simulation; 0 leaves the
    // defaults. SendRateMin and Max are set to the same value, as the GNS header instructs for a
    // fixed rate; this build has no bandwidth estimation, so the effective rate is the ping-at-init
    // estimate clamped to the global Min/Max set at init, and this pin overrides that in both
    // directions.
    auto* utils = SteamNetworkingUtils();
    const long bufKb = coop::config::ResolveInt(coop::config_registry::rows::net_sendbuf_kb);
    if (bufKb > 0) {
        utils->SetConnectionConfigValueInt32(hConn, k_ESteamNetworkingConfig_SendBufferSize,
                                             static_cast<int32>(bufKb) * 1024);
        UE_LOGW("net: send buffer PINNED to %ld KB for h=0x%08x (drill knob net.sendbuf_kb)",
                bufKb, static_cast<unsigned>(hConn));
    } else {
        // GNS's 512 KB default is smaller than a join burst (~740 KB of PropSpawns plus the connect
        // replay), so the backlog engaged on every join; 4 MB makes it the exception, a real slow
        // link. The backlog stays the correctness net either way.
        utils->SetConnectionConfigValueInt32(hConn, k_ESteamNetworkingConfig_SendBufferSize,
                                             kDefaultSendBufBytes);
    }
    const long rateKbs = coop::config::ResolveInt(coop::config_registry::rows::net_sendrate_kbs);
    if (rateKbs > 0) {
        utils->SetConnectionConfigValueInt32(hConn, k_ESteamNetworkingConfig_SendRateMin,
                                             static_cast<int32>(rateKbs) * 1024);
        utils->SetConnectionConfigValueInt32(hConn, k_ESteamNetworkingConfig_SendRateMax,
                                             static_cast<int32>(rateKbs) * 1024);
        UE_LOGW("net: send rate PINNED to %ld KB/s for h=0x%08x (drill knob net.sendrate_kbs)",
                rateKbs, static_cast<unsigned>(hConn));
    }
}

// The host's accept policy, shared by both accept sites (the Connecting edge, and the Connected
// branch when GNS skips Connecting): the ban list, then the per-address connection cap. None
// accepts; otherwise the code the refused peer is told, with whyOut as the close text. Both run
// before AcceptConnection, so a refusal costs no handshake and no band entry. The ban filter is
// fail-closed: with a filter installed and no resolvable remote IP the connection is refused as
// AcceptFailed; direct UDP populates m_addrRemote by the Connecting edge, so only a genuinely
// unresolvable peer is refused. MTA checks its join flood at the join packet, inside the
// password check (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:1916-1919),
// so a wrong-password join is not counted there; ours counts every accepted connection,
// because what the cap protects is the accept and the save capture behind the seat, not the
// join alone. The cap counts here only where the transport already knows an address; on the
// internet lane no route exists yet at this edge, so `countedOut` stays false and the proof
// counts the arrival (peer_admission), by the address its route reports by then or by the
// identity it proves over a relay, never by one merely claimed.
EndReason AcceptPolicy(ISteamNetworkingSockets* sockets, HSteamNetConnection hConn,
                       Session::AcceptFilterFn filter, char* whyOut, int whyLen,
                       bool* countedOut) {
    if (whyOut && whyLen > 0) whyOut[0] = '\0';
    *countedOut = false;
    char ip[SteamNetworkingIPAddr::k_cchMaxString] = {};
    SteamNetConnectionInfo_t cinfo{};
    if (sockets->GetConnectionInfo(hConn, &cinfo)) {
        cinfo.m_addrRemote.ToString(ip, sizeof(ip), /*bWithPort*/false);
    }
    if (filter) {
        if (!ip[0]) {
            UE_LOGW("net: incoming connection has no resolvable remote IP -- "
                    "rejecting (fail-closed ban check)");
            if (whyOut && whyLen > 0)
                std::snprintf(whyOut, static_cast<size_t>(whyLen), "no resolvable remote address");
            return EndReason::AcceptFailed;
        }
        if (!filter(ip, whyOut, whyLen)) return EndReason::Banned;
    }
    connect_history::Key key;
    if (connect_history::KeyFromAddressBytes(cinfo.m_addrRemote.m_ipv6, key)) {
        *countedOut = true;
        const connect_history::Verdict v = connect_history::Connects().Note(key, NowMs());
        if (v.refused) {
            const auto& policy = connect_history::Connects().GetPolicy();
            if (whyOut && whyLen > 0)
                std::snprintf(whyOut, static_cast<size_t>(whyLen),
                              "%d connections from your address in the last %llu s; try "
                              "again in %llu s", v.count,
                              static_cast<unsigned long long>(policy.windowMs / 1000),
                              static_cast<unsigned long long>((v.retryMs + 999) / 1000));
            UE_LOGW("net: connect cap -- %s made %d connections in the last %llu s; refusing "
                    "for %llu s [%s]", ip, v.count,
                    static_cast<unsigned long long>(policy.windowMs / 1000),
                    static_cast<unsigned long long>((v.retryMs + 999) / 1000),
                    Describe(EndReason::ConnectFlood).id);
            return EndReason::ConnectFlood;
        }
    }
    return EndReason::None;
}

}  // namespace

// --- the pending (unadmitted) band ---
// session.h says why it is a band and not a per-slot flag: three seats, and an unadmitted socket
// holding one would let three silent sockets lock the lobby.

int Session::ParkPending(uint32_t hConn) {
    for (int i = 0; i < kMaxPending; ++i) {
        uint32_t expected = 0;
        if (pendingConns_[i].compare_exchange_strong(expected, hConn)) {
            pendingSinceMs_[i].store(NowMs(), std::memory_order_release);
            return i;
        }
    }
    // Band full: evict rather than refuse the arrival. An unproved socket has no standing over an
    // arriving one, and the band's occupancy is attacker-timed (open eight sockets, say nothing),
    // so a refusal turns every real friend away until the sweep. Progress first, age second: an
    // entry with an open exchange is never evicted for one that has said nothing, so a flood must
    // complete a real exchange per socket. The band itself does not count by address: the P2P
    // edge has no remote address, and one household NAT would share a row. The per-address
    // connection cap lives one step earlier, in AcceptPolicy, where an address exists.
    int oldest = -1;
    uint64_t oldestMs = 0;
    for (int pass = 0; pass < 2 && oldest < 0; ++pass) {
        const bool wantSilent = (pass == 0);
        for (int i = 0; i < kMaxPending; ++i) {
            if (wantSilent && coop::net::peer_admission::HostHasOpenExchange(i)) continue;
            const uint64_t t = pendingSinceMs_[i].load(std::memory_order_acquire);
            if (oldest < 0 || t < oldestMs) { oldestMs = t; oldest = i; }
        }
    }
    if (oldest < 0) oldest = 0;  // unreachable: pass 2 considers every entry
    const uint32_t victim = pendingConns_[oldest].exchange(hConn);
    pendingSinceMs_[oldest].store(NowMs(), std::memory_order_release);
    coop::net::peer_admission::HostForgetPending(oldest);
    if (victim != 0) {
        UE_LOGW("net: pending band full -- evicting the OLDEST un-proved socket "
                "0x%08x (idx %d, %llu ms) to make room for the arrival [%s]",
                static_cast<unsigned>(victim), oldest,
                static_cast<unsigned long long>(NowMs() - oldestMs),
                Describe(EndReason::TooSlowToProve).id);
        if (auto* sockets = SteamNetworkingSockets())
            sockets->CloseConnection(static_cast<HSteamNetConnection>(victim),
                                     ToTransportEnd(EndReason::TooSlowToProve),
                                     "too slow to prove your identity", false);
    }
    return oldest;
}

void Session::SweepPending() {
    // The band's deadline: an entry older than kPendingDeadlineMs is closed.
    const uint64_t now = NowMs();
    for (int i = 0; i < kMaxPending; ++i) {
        const uint32_t hConn = pendingConns_[i].load(std::memory_order_acquire);
        if (hConn == 0) continue;
        const uint64_t since = pendingSinceMs_[i].load(std::memory_order_acquire);
        if (since == 0 || now - since < kPendingDeadlineMs) continue;
        UE_LOGW("net: PENDING %d (h=0x%08x) never proved its identity in %llu ms "
                "-- closing [%s]", i, static_cast<unsigned>(hConn),
                static_cast<unsigned long long>(now - since),
                Describe(EndReason::IdentityProofTimedOut).id);
        pendingConns_[i].store(0, std::memory_order_release);
        pendingSinceMs_[i].store(0, std::memory_order_release);
        coop::net::peer_admission::HostForgetPending(i);
        if (auto* sockets = SteamNetworkingSockets())
            sockets->CloseConnection(static_cast<HSteamNetConnection>(hConn),
                                     ToTransportEnd(EndReason::IdentityProofTimedOut),
                                     "identity proof timed out", false);
    }
}

void Session::RetirePending(int pendIdx, uint32_t hConn, EndReason code, const char* reason) {
    if (pendIdx >= 0 && pendIdx < kMaxPending) {
        // Clear by handle, not blindly: a failed CAS means the entry is already someone else's.
        uint32_t expected = hConn;
        if (pendingConns_[pendIdx].compare_exchange_strong(expected, 0)) {
            pendingSinceMs_[pendIdx].store(0, std::memory_order_release);
            coop::net::peer_admission::HostForgetPending(pendIdx);
        }
    }
    if (auto* sockets = SteamNetworkingSockets())
        sockets->CloseConnection(static_cast<HSteamNetConnection>(hConn),
                                 ToTransportEnd(code),
                                 reason ? reason : Describe(code).text, /*bEnableLinger*/false);
}

void Session::ReleasePending(uint32_t hConn) {
    if (hConn == 0) return;
    for (int i = 0; i < kMaxPending; ++i) {
        uint32_t cur = hConn;
        if (pendingConns_[i].compare_exchange_strong(cur, 0)) {
            pendingSinceMs_[i].store(0, std::memory_order_release);
            // The exchange state dies with the entry: the band recycles indices, and a row left
            // open would let the next socket at this index answer the previous socket's challenge.
            coop::net::peer_admission::HostForgetPending(i);
        }
    }
}

void Session::SetProvedGuidForSlot(int slot, const std::string& guid) {
    if (slot < 0 || slot >= kMaxPeers) return;
    std::lock_guard<std::mutex> lk(provedGuidMutex_);
    provedGuidBySlot_[slot] = guid;
}

std::string Session::ProvedGuidForSlot(int slot) const {
    if (slot < 0 || slot >= kMaxPeers) return {};
    std::lock_guard<std::mutex> lk(provedGuidMutex_);
    return provedGuidBySlot_[slot];
}

int Session::AdmitPending(int pendingIdx, uint32_t hConn) {
    // The band index must still belong to this connection; not reachable today (ParkPending runs
    // only under RunCallbacks, never between two messages of a batch), but a reassigned index would
    // lose the new occupant's entry and the sweep that closes it.
    if (pendingIdx >= 0 && pendingIdx < kMaxPending &&
        pendingConns_[pendingIdx].load(std::memory_order_acquire) != hConn) {
        return -1;
    }
    // The seat is spent here and nowhere else, in the accept edge's old order: the generation is
    // minted before the peerConns_ store, so any observer that can see the connection can see who
    // owns it.
    const int slot = FindFreePeerSlotForClient();
    if (slot < 0) return -1;  // lobby genuinely full of ADMITTED players
    auto* sockets = SteamNetworkingSockets();
    if (!sockets) return -1;
    sockets->SetConnectionUserData(static_cast<HSteamNetConnection>(hConn), slot);
    peerGenBySlot_[slot].store(MintPeerGeneration(), std::memory_order_release);
    // GEN: mint -- the admitted peer takes the slot (the generation store is the line above).
    peerConns_[slot].store(hConn);
    // The peer is now entitled to everything a connected peer gets.
    FinishPeerConnected(slot, hConn);
    if (pendingIdx >= 0 && pendingIdx < kMaxPending) {
        pendingConns_[pendingIdx].store(0, std::memory_order_release);
        pendingSinceMs_[pendingIdx].store(0, std::memory_order_release);
    }
    if (state_.load() == ConnState::Disconnected) state_.store(ConnState::Handshaking);
    UE_LOGI("net: ADMITTED pending conn 0x%08x -> slot %d (%d/%d seated)",
            static_cast<unsigned>(hConn), slot, connectedPeerCount(), kMaxPeers - 1);
    return slot;
}

int Session::FindFreePeerSlotForClient() {
    // Host: the lowest unoccupied client slot; slot 0 is the host itself.
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
    // remoteMutex_ held by the caller.
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return;
    hasRemote_[peerSlot] = false;
    lastRemoteSeq_[peerSlot] = 0;
    remoteStamp_[peerSlot] = 0;
    lastReadStamp_[peerSlot] = 0;
    hasRemoteProp_[peerSlot] = false;
    lastRemotePropSeq_[peerSlot] = 0;
    remotePropStamp_[peerSlot] = 0;
    lastReadPropStamp_[peerSlot] = 0;
    // The ragdoll slot too, so a reconnecting peer inherits no stale stream.
    hasRemoteRagdoll_[peerSlot] = false;
    lastRemoteRagdollSeq_[peerSlot] = 0;
    remoteRagdollStamp_[peerSlot] = 0;
    lastReadRagdollStamp_[peerSlot] = 0;
    // The hand-item slot, the same reason.
    hasRemoteHand_[peerSlot] = false;
    lastRemoteHandSeq_[peerSlot] = 0;
    remoteHandStamp_[peerSlot] = 0;
    lastReadHandStamp_[peerSlot] = 0;
    // Clear the latched senderEpoch so the next connection on this slot re-latches on its first
    // packet; a reconnecting peer's fresh epoch would otherwise fail the compare.
    expectedEpoch_[peerSlot] = 0;
}

int Session::connectedPeerCount() const {
    // Only peers whose lanes are configured: counting a slot set at Connecting but not yet ready
    // delays the aggregate Disconnected transition and fans a snapshot out to a half-open
    // connection.
    int n = 0;
    for (int i = 0; i < kMaxPeers; ++i) {
        if (peerConns_[i].load() != 0 && peerLanesConfigured_[i].load()) ++n;
    }
    return n;
}

int Session::pendingPeerCount() const {
    // The unadmitted band: a socket sits here for the whole challenge round trip, during which
    // connectedPeerCount() reads zero, so "nobody is here" is the sum.
    int n = 0;
    for (int i = 0; i < kMaxPending; ++i) {
        if (pendingConns_[i].load(std::memory_order_acquire) != 0) ++n;
    }
    return n;
}

// Everything a peer gets the moment it is entitled to be one: the lanes, the send-buffer mirror,
// the flag IsSlotReady() reads, and the host's AssignPeerSlot. A shared function, not a copy: on
// a host the seat is spent in AdmitPending, long after the Connected callback, and a copy left in
// that callback once admitted a peer without ever sending AssignPeerSlot.
void Session::FinishPeerConnected(int slot, uint32_t hConn) {
    ConfigureLanesForPeer(hConn);
    // Mirror the buffer size the connection runs with (the knob or the default); the backlog
    // drain's reserve gate is computed against it.
    {
        const long bufKb =
            coop::config::ResolveInt(coop::config_registry::rows::net_sendbuf_kb);
        sendBufBytes_ = (bufKb > 0) ? static_cast<int>(bufKb) * 1024
                                    : kDefaultSendBufBytes;
    }
    // The ready flag flips only after ConfigureConnectionLanes returns, so a reader sees the slot
    // ready only once the per-kind lane mapping is live; the release pairs with IsSlotReady's load.
    peerLanesConfigured_[slot].store(true, std::memory_order_release);
    if (state_.load() != ConnState::Connected) {
        state_.store(ConnState::Connected);
    }
    UE_LOGI("net: peer slot %d CONNECTED (%s, h=0x%08x)",
            slot, cfg_.role == Role::Host ? "host" : "client",
            static_cast<unsigned>(hConn));
    // The host tells the client which slot it holds: the host is the only authority on slot
    // assignment, so two clients cannot collide. Net thread; SendReliableToSlot is thread-safe.
    if (cfg_.role == Role::Host) {
        AssignPeerSlotPayload p{};
        p.slot = static_cast<uint8_t>(slot);
        // The host's local Player Element id rides along so the client registers the host's mirror
        // in slot 0. Read on the net thread: net_pump allocates the host's Element every tick and
        // it lives for the session, so the read is well-defined unless the client connects in the
        // boot window before the first pump tick, when it reads kInvalidId and the client falls
        // back to non-mirror routing.
        p.hostElementId = coop::players::Registry::Get().LocalPlayerElementId();
        if (!SendReliableToSlot(slot, ReliableKind::AssignPeerSlot, &p, sizeof(p))) {
            UE_LOGW("net: SendReliableToSlot(AssignPeerSlot=%d) failed", slot);
        } else {
            UE_LOGI("net: sent AssignPeerSlot slot=%d hostElementId=0x%08x to client",
                    slot, p.hostElementId);
        }
    }
}

void Session::HandleConnStatusChanged(void* info) {
    auto* cb = static_cast<SteamNetConnectionStatusChangedCallback_t*>(info);
    const HSteamNetConnection hConn = cb->m_hConn;
    const auto oldState = cb->m_eOldState;
    const auto newState = cb->m_info.m_eState;
    auto* sockets = SteamNetworkingSockets();

    // --- host: an incoming connection ---
    if (cfg_.role == Role::Host &&
        oldState == k_ESteamNetworkingConnectionState_None &&
        newState == k_ESteamNetworkingConnectionState_Connecting) {
        // The accept policy before the accept: the ban list and the connection cap, at the edge
        // that costs no slot and no handshake.
        char why[128] = {};
        bool counted = false;
        const EndReason refusal =
            AcceptPolicy(sockets, hConn, acceptFilter_, why, sizeof(why), &counted);
        if (refusal != EndReason::None) {
            const char* text = why[0] ? why : Describe(refusal).text;
            UE_LOGW("net: rejecting incoming connection [%s] '%s'", Describe(refusal).id, text);
            sockets->CloseConnection(hConn, ToTransportEnd(refusal), text,
                                     /*bEnableLinger*/false);
            return;
        }
        const EResult rc = sockets->AcceptConnection(hConn);
        if (rc != k_EResultOK) {
            UE_LOGW("net: AcceptConnection rc=%d [%s]", static_cast<int>(rc),
                    Describe(EndReason::AcceptFailed).id);
            sockets->CloseConnection(hConn, ToTransportEnd(EndReason::AcceptFailed),
                                     "accept failed", false);
            return;
        }
        // No seat is spent here: the connection is parked, and the seat is spent in AdmitPending
        // and nowhere else (a seat spent at accept gave anyone who opened a socket a player number,
        // a puppet in every world and the whole person fan-out, since the roster births a row on
        // IsSlotReady alone). Never fails: a full band evicts its oldest unproved entry.
        const int pend = ParkPending(hConn);
        // The edge's own verdict rides the band entry: the proof counts what the edge could not.
        peer_admission::HostMarkCountedAtEdge(pend, counted);
        // Tagged with the pending id, outside [1, kMaxPeers), so the one drain site routes this
        // connection's traffic to the admission handler and nowhere else.
        sockets->SetConnectionUserData(hConn, kPendingTag | pend);
        // Add to the host's PollGroup so we drain all clients with one call.
        const uint32_t hPoll = hPollGroup_.load();
        if (hPoll != 0) {
            sockets->SetConnectionPollGroup(hConn, static_cast<HSteamNetPollGroup>(hPoll));
        }
        UE_LOGI("net: host accepted client into PENDING %d (h=0x%08x) -- no seat until admitted",
                pend, static_cast<unsigned>(hConn));
        return;
    }

    // --- both roles: transitions on an existing connection ---

    // Client: ICE is negotiating a path to the host (P2P only); the join screen names it.
    if (cfg_.role == Role::Client &&
        newState == k_ESteamNetworkingConnectionState_FindingRoute) {
        linkStage_.store(static_cast<uint8_t>(LinkStage::FindingRoute),
                         std::memory_order_release);
        return;
    }

    if (newState == k_ESteamNetworkingConnectionState_Connected) {
        int slot = FindPeerSlotForConn(hConn);
        // GNS may skip the None to Connecting transition (its header says so); on a host such a
        // connection has no band entry yet, so it is parked here.
        if (slot < 0 && cfg_.role == Role::Host) {
            // The second accept site runs the same policy as the first: park, never seat. A
            // connection reaching Connected with no slot is either already parked (the normal path)
            // or one where GNS skipped Connecting, which never met the accept-edge ban filter, so
            // the filter runs here.
            if (!IsPendingConn(hConn)) {
                char why[128] = {};
                bool counted = false;
                const EndReason refusal =
                    AcceptPolicy(sockets, hConn, acceptFilter_, why, sizeof(why), &counted);
                if (refusal != EndReason::None) {
                    const char* text = why[0] ? why : Describe(refusal).text;
                    UE_LOGW("net: rejecting late-register connection [%s] '%s'",
                            Describe(refusal).id, text);
                    sockets->CloseConnection(hConn, ToTransportEnd(refusal), text,
                                             /*bEnableLinger*/false);
                    return;
                }
                const int pend = ParkPending(hConn);
                peer_admission::HostMarkCountedAtEdge(pend, counted);
                sockets->SetConnectionUserData(hConn, kPendingTag | pend);
                const uint32_t hPoll = hPollGroup_.load();
                if (hPoll != 0) {
                    sockets->SetConnectionPollGroup(hConn, static_cast<HSteamNetPollGroup>(hPoll));
                }
                UE_LOGI("net: late-parked PENDING %d (Connecting was skipped, h=0x%08x)",
                        pend, static_cast<unsigned>(hConn));
            }
            // The lanes are configured at admission, not here: an unadmitted peer has no seat, and
            // the exchange rides raw sends that need only the handle.
            return;
        }
        if (slot < 0) {
            UE_LOGW("net: Connected on unknown connection h=0x%08x (role=%s)",
                    static_cast<unsigned>(hConn),
                    cfg_.role == Role::Host ? "host" : "client");
            return;
        }
        // Here role == Client and slot == 0 (a host's inbound connections are parked above). The
        // link is not finished here: the client opens the admission exchange and finishes the link
        // in FinishClientLink when the host's AssignPeerSlot proves it was admitted, so "the socket
        // is up" triggers nothing IsSlotReady gates.
        linkStage_.store(static_cast<uint8_t>(LinkStage::SocketUp), std::memory_order_release);
        if (!peer_admission::ClientOnConnected(*this, hConn)) {
            // Say why on the flee path: a silent close reads as "the host vanished", the one thing
            // it is not.
            LeaveHost(EndReason::IdentityExchange,
                      "could not start the identity exchange with this host");
        }
        return;
    }

    if (newState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
        newState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
        const int slot = FindPeerSlotForConn(hConn);
        // A pending connection has no slot, so the slot teardown below is a no-op for it; its band
        // entry is freed here, unconditionally (releasing a handle that was never parked is a
        // no-op).
        ReleasePending(hConn);
        UE_LOGW("net: peer slot %d closed (oldState=%d end=%d [%s] reason='%s')",
                slot, static_cast<int>(oldState), cb->m_info.m_eEndReason,
                Describe(FromTransportEnd(cb->m_info.m_eEndReason)).id,
                cb->m_info.m_szEndDebug);
        // Client, slot 0: the host closed our connection (kick, ban, quit, crash); stash GNS's
        // reason so net_pump can say why before fleeing.
        if (cfg_.role == Role::Client && slot == 0) {
            {   std::lock_guard<std::mutex> lk(hostCloseMutex_);
                // Do not overwrite a reason we set ourselves (a refused exchange names why); GNS's
                // reason for a close we initiated is the generic one.
                if (hostClose_.code == EndReason::None) {
                    hostClose_.code = FromTransportEnd(cb->m_info.m_eEndReason);
                    hostClose_.text = cb->m_info.m_szEndDebug;
                }
            }
            // The exchange dies with the link: a `proved` flag surviving into the next connection
            // would seat us on an unchallenged host.
            peer_admission::ClientReset();
        }
        if (slot >= 0) {
            // GEN: clear -- deferred to the END of this close path, after the inbox erase: its
            // drop to 0 is what tells the ledger the slot emptied, and clearing it here would let
            // a still-queued reliable from this peer dispatch after the teardown.
            peerConns_[slot].store(0);
            peerLanesConfigured_[slot].store(false, std::memory_order_release);
            // The departing peer's queued reliable state dies with it.
            backlog_.FreeSlot(slot);
            relayEligible_[slot].store(0, std::memory_order_release);
        }
        // A terminal state requires CloseConnection to release the handle (the GNS header).
        sockets->CloseConnection(hConn, 0, nullptr, false);

        // Per-slot reset, so a reconnecting peer (seq from 0) is not stale-dropped.
        { std::lock_guard<std::mutex> lk(remoteMutex_);
          if (slot >= 0) ResetPeerRemoteState(slot); }

        // Drop the reliables still queued from the departing peer: a PropSpawn from a ghost landing
        // after the slot cleared could never be destroyed.
        if (slot >= 0) {
            std::lock_guard<std::mutex> lk(reliableInboxMutex_);
            for (auto it = reliableInbox_.begin(); it != reliableInbox_.end();) {
                if (it->senderPeerSlot == slot) it = reliableInbox_.erase(it);
                else ++it;
            }
        }

        // The generation clear is the last write of the close path, after the inbox erase (under a
        // different mutex), so a reader that sees 0 sees an inbox already drained of this peer.
        if (slot >= 0) {
            peerGenBySlot_[slot].store(0, std::memory_order_release);
            SetProvedGuidForSlot(slot, std::string());  // the identity dies with the seat
        }

        // Aggregate state: Connected while any peer remains, otherwise everything is cleared.
        if (connectedPeerCount() == 0) {
            // A full disconnect goes to Disconnected, not Handshaking, which the reconnect UI and
            // the harness poll for.
            state_.store(ConnState::Disconnected);
            linkStage_.store(static_cast<uint8_t>(LinkStage::Idle), std::memory_order_release);
            { std::lock_guard<std::mutex> lk(remoteMutex_);
              for (int i = 0; i < kMaxPeers; ++i) ResetPeerRemoteState(i); }
            { std::lock_guard<std::mutex> lk(reliableInboxMutex_); reliableInbox_.clear(); }
            for (auto& r : rttMsBySlot_) r.store(-1, std::memory_order_relaxed);  // per-slot RTT reset
            UE_LOGI("net: all peers gone -- session back to Disconnected");
        }
    }
}

bool Session::KickWithToken(int peerSlot, uint32_t expectedGeneration, EndReason code,
                            const char* reason) {
    if (peerSlot < 1 || peerSlot >= kMaxPeers) return false;
    if (expectedGeneration == 0) return false;  // an empty-slot token can never authorize a kick
    const uint32_t hConnAtCapture = peerConns_[peerSlot].load();
    if (hConnAtCapture == 0) return false;
    // Compare the captured token against the live authority; stale means refuse.
    const uint32_t liveGen = peerGenBySlot_[peerSlot].load(std::memory_order_acquire);
    if (liveGen != expectedGeneration) {
        UE_LOGW("net: kick/ban on slot %d REFUSED -- the captured occupant (gen %u) is gone; "
                "the slot now holds gen %u", peerSlot,
                static_cast<unsigned>(expectedGeneration), static_cast<unsigned>(liveGen));
        return false;
    }
    // GEN: clear -- the claim only; the generation itself is cleared at the end of KickClaimed's
    // teardown, after the inbox erase, exactly like the other two close paths. Claim by handle,
    // not by slot: the generation check can go stale between these two instructions (the net
    // thread closes and re-accepts), and a plain exchange(0) would hand us the successor's
    // connection; the CAS fails on a different handle.
    uint32_t claimed = hConnAtCapture;
    if (!peerConns_[peerSlot].compare_exchange_strong(claimed, 0)) {
        UE_LOGW("net: kick/ban on slot %d REFUSED -- the connection changed under us", peerSlot);
        return false;
    }
    return KickClaimed(peerSlot, hConnAtCapture, code, reason);
}

bool Session::GetPeerAddressWithToken(int peerSlot, uint32_t expectedGeneration,
                                      char* out, int outLen) const {
    if (out && outLen > 0) out[0] = '\0';
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
    if (expectedGeneration == 0) return false;
    if (peerGenBySlot_[peerSlot].load(std::memory_order_acquire) != expectedGeneration)
        return false;
    return GetPeerAddress(peerSlot, out, outLen);
}

bool Session::Kick(int peerSlot, EndReason code, const char* reason) {
    // Slot 0 is the host itself, never kickable.
    if (peerSlot < 1 || peerSlot >= kMaxPeers) return false;
    // GEN: clear -- deferred to the end of the teardown, exactly as the ClosedByPeer path does.
    // Claim the slot atomically so a concurrent ClosedByPeer on the net thread and this kick
    // cannot both run the teardown (0 back means someone already closed it). This site is an
    // exchange, not a store: a census of `.store(` alone MISSES it, and missing it would leave a
    // kicked slot holding a live generation, so the ledger would never see the row empty.
    const uint32_t hConn = peerConns_[peerSlot].exchange(0);
    if (hConn == 0) return false;
    return KickClaimed(peerSlot, hConn, code, reason);
}

// A slot's send backlog tripped a fatal bound (no progress with a non-empty queue, or the byte
// cap): queued until sent or connection-fatal, never warn-and-drop, so the connection is closed
// and the backlog dies with it. Host: kick the slot; client: slot 0 is the host link, the same
// teardown. A draining link never gets here (progress resets the timer).
void Session::FatalCloseSlot(int slot, const char* reason) {
    UE_LOGE("net: send backlog FATAL for slot %d -- %s; closing the connection [%s] "
            "(delivery guarantee: never silently drop)", slot, reason ? reason : "?",
            Describe(cfg_.role == Role::Host ? EndReason::HostBacklogFatal
                                             : EndReason::ClientBacklogFatal).id);
    if (cfg_.role == Role::Host) {
        Kick(slot, EndReason::HostBacklogFatal, reason);
        return;
    }
    if (slot != 0) return;  // a client only owns its host link
    LeaveHost(EndReason::ClientBacklogFatal, reason ? reason : "send backlog fatal");
}

// A client ending its own host link: every path where we decide to leave (an admission refusal, a
// host that tried to seat us unproved, an exchange that could not start, a fatal backlog) comes
// through here. GNS posts no status callback for a connection we close, so a bare CloseConnection
// left state_ at Handshaking forever and net_pump's connect-fail edge, the only consumer of the
// reason, never fired: right on the wire, mute on screen. KickClaimed's tail downgrades the
// aggregate state, so a departure we author and one we suffer leave the session the same.
void Session::LeaveHost(EndReason code, const char* why) {
    if (cfg_.role != Role::Client) return;
    {   // FIRST WRITER WINS, matching the ClosedByPeer branch: a refusal names the
        // real cause, and whatever the teardown trips afterwards is a consequence.
        // Overwriting would hand the player the symptom instead of the reason.
        std::lock_guard<std::mutex> lk(hostCloseMutex_);
        if (hostClose_.code == EndReason::None) {
            hostClose_.code = code;
            hostClose_.text = why ? why : "";
        }
    }
    // GEN: none -- CLIENT side: slot 0 is the host LINK handle, not a peer-slot occupancy. The
    // client owns no roster generations and the flee path tears down whole.
    const uint32_t hConn = peerConns_[0].exchange(0);
    if (hConn == 0) return;  // already claimed by another path; its teardown owns it
    KickClaimed(0, hConn, code, why);
}

// The teardown for a connection whose slot the caller has already claimed (exchanged or CAS'd to
// 0), shared by the blind and the token-checked entry points.
bool Session::KickClaimed(int peerSlot, uint32_t hConn, EndReason code, const char* reason) {
    peerLanesConfigured_[peerSlot].store(false, std::memory_order_release);
    // The delivery guarantee is scoped to the connection: the queued state dies with the peer.
    backlog_.FreeSlot(peerSlot);
    relayEligible_[peerSlot].store(0, std::memory_order_release);

    if (auto* sockets = SteamNetworkingSockets()) {
        // No linger: a kick drops the peer at once. The reason rides to the peer's status callback,
        // so a kicked client can say why.
        sockets->CloseConnection(static_cast<HSteamNetConnection>(hConn),
                                 ToTransportEnd(code),
                                 reason ? reason : Describe(code).text, /*bEnableLinger*/false);
    }

    // GNS delivers no status callback for a connection we close, so the ClosedByPeer teardown is
    // replicated here; the exchange(0) above makes a racing callback's FindPeerSlotForConn return
    // -1, so the teardown runs exactly once.
    { std::lock_guard<std::mutex> lk(remoteMutex_); ResetPeerRemoteState(peerSlot); }
    { std::lock_guard<std::mutex> lk(reliableInboxMutex_);
      for (auto it = reliableInbox_.begin(); it != reliableInbox_.end();) {
          if (it->senderPeerSlot == peerSlot) it = reliableInbox_.erase(it);
          else ++it;
      } }
    // The generation clear is the last write, after the inbox erase (the ClosedByPeer path says why
    // the order matters).
    peerGenBySlot_[peerSlot].store(0, std::memory_order_release);
    // And the proved identity with it: a recycled slot must not carry its predecessor's storage
    // name.
    SetProvedGuidForSlot(peerSlot, std::string());

    // Aggregate state, as in the ClosedByPeer branch.
    if (connectedPeerCount() == 0) {
        state_.store(ConnState::Disconnected);
        linkStage_.store(static_cast<uint8_t>(LinkStage::Idle), std::memory_order_release);
        { std::lock_guard<std::mutex> lk(remoteMutex_);
          for (int i = 0; i < kMaxPeers; ++i) ResetPeerRemoteState(i); }
        { std::lock_guard<std::mutex> lk(reliableInboxMutex_); reliableInbox_.clear(); }
        for (auto& r : rttMsBySlot_) r.store(-1, std::memory_order_relaxed);  // per-slot RTT reset
    }
    UE_LOGI("net: kicked peer slot %d [%s] (reason='%s')", peerSlot, Describe(code).id,
            reason ? reason : Describe(code).text);
    return true;
}

HostClose Session::TakeHostCloseReason() {
    std::lock_guard<std::mutex> lk(hostCloseMutex_);
    HostClose r = std::move(hostClose_);
    hostClose_ = HostClose{};  // move may leave the string valid-but-unspecified; force empty
    return r;
}

bool Session::GetPeerAddress(int peerSlot, char* out, int outLen) const {
    if (!out || outLen <= 0) return false;
    out[0] = '\0';
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
    const uint32_t hConn = peerConns_[peerSlot].load();
    if (hConn == 0) return false;
    auto* sockets = SteamNetworkingSockets();
    if (!sockets) return false;
    SteamNetConnectionInfo_t info{};
    if (!sockets->GetConnectionInfo(static_cast<HSteamNetConnection>(hConn), &info)) return false;
    info.m_addrRemote.ToString(out, static_cast<size_t>(outLen), /*bWithPort*/false);
    return out[0] != '\0';
}

}  // namespace coop::net
