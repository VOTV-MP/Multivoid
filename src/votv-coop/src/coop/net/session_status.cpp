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

#include "coop/config/config.h"           // R-4b commit-0: ResolveInt for the wire knobs
#include "coop/config/config_registry.h"  // rows::net_sendbuf_kb / rows::net_sendrate_kbs
#include "coop/player/players_registry.h"
#include "ue_wrap/core/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>  // SteamNetworkingUtils() for the R-4b wire knobs
#pragma warning(pop)

namespace coop::net {

// R-4b D4: the per-connection send buffer the mod configures when the
// net.sendbuf_kb knob is 0 (see ConfigureLanesForPeer). Shared with the
// sendBufBytes_ mirror below -- ONE constant, no drift.
constexpr int kDefaultSendBufBytes = 4 * 1024 * 1024;

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
    // R-4b commit-0: per-connection wire knobs (the delivery-guarantee drill +
    // slow-link simulation; research/findings/network/
    // votv-reliable-delivery-guarantee-DESIGN-2026-08-23.md D7). 0 = leave the
    // mod's defaults. SendRateMin/Max are set to the SAME value per the GNS
    // header's own instruction ("should always be set to the same value, to
    // manually configure a specific send rate"). GNS STOCK defaults both to
    // 256 KB/s, but OUR binary overrides globally at init (session_start.cpp:
    // Min 1 MB/s / Max 25 MB/s) -- and there is no bandwidth estimation in
    // this build, so the effective rate is clamp(ping-at-init estimate, Min,
    // Max): Min forever on any internet link, Max on LAN. This per-connection
    // pin overrides that global in BOTH directions.
    auto* utils = SteamNetworkingUtils();
    const long bufKb = coop::config::ResolveInt(coop::config_registry::rows::net_sendbuf_kb);
    if (bufKb > 0) {
        utils->SetConnectionConfigValueInt32(hConn, k_ESteamNetworkingConfig_SendBufferSize,
                                             static_cast<int32>(bufKb) * 1024);
        UE_LOGW("net: send buffer PINNED to %ld KB for h=0x%08x (drill knob net.sendbuf_kb)",
                bufKb, static_cast<unsigned>(hConn));
    } else {
        // R-4b D4: the GNS default (512 KB) is STRUCTURALLY smaller than a join
        // burst (~740 KB of PropSpawns + the connect replay), so the backlog
        // engaged on every join. 4 MB makes the backlog the exception (a real
        // slow link), not the common path. The backlog remains the correctness
        // net either way.
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

// Phase 2 ban filter, shared by BOTH host accept paths (the normal
// None->Connecting edge AND the late-register path in the Connected branch when
// GNS skips Connecting). Returns true to ACCEPT the incoming connection.
//
// FAIL-CLOSED: when a filter is installed but the remote IP can't be resolved
// (GetConnectionInfo fails or yields an empty address), we REJECT -- an
// unverifiable peer must not slip past an active banlist. For direct-UDP
// connections m_addrRemote is populated by the Connecting edge (the GNS server
// example reads it there), so this only rejects genuinely-unresolvable peers.
bool AcceptAllowed(ISteamNetworkingSockets* sockets, HSteamNetConnection hConn,
                   Session::AcceptFilterFn filter) {
    if (!filter) return true;  // no banlist installed -> accept all
    char ip[SteamNetworkingIPAddr::k_cchMaxString] = {};
    SteamNetConnectionInfo_t cinfo{};
    if (sockets->GetConnectionInfo(hConn, &cinfo)) {
        cinfo.m_addrRemote.ToString(ip, sizeof(ip), /*bWithPort*/false);
    }
    if (!ip[0]) {
        UE_LOGW("net: incoming connection has no resolvable remote IP -- "
                "rejecting (fail-closed ban check)");
        return false;
    }
    return filter(ip);
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
    // v22: clear the ragdoll pelvis-physics slot too, so a reconnecting peer
    // doesn't inherit the dead generation's stale ragdoll stream.
    hasRemoteRagdoll_[peerSlot] = false;
    lastRemoteRagdollSeq_[peerSlot] = 0;
    remoteRagdollStamp_[peerSlot] = 0;
    lastReadRagdollStamp_[peerSlot] = 0;
    // v109: clear the hand-item transform slot -- same stale-generation reasoning.
    hasRemoteHand_[peerSlot] = false;
    lastRemoteHandSeq_[peerSlot] = 0;
    remoteHandStamp_[peerSlot] = 0;
    lastReadHandStamp_[peerSlot] = 0;
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
        // Ban filter (Phase 2): reject a banned (or unverifiable) remote IP
        // before we accept it. MTA does the equivalent at join time
        // (CGame.cpp:1973); doing it at the Connecting edge is earlier + cheaper
        // (no slot consumed, no handshake). Fail-closed (see AcceptAllowed).
        if (!AcceptAllowed(sockets, hConn, acceptFilter_)) {
            UE_LOGW("net: rejecting incoming connection (banned remote IP)");
            sockets->CloseConnection(hConn, k_ESteamNetConnectionEnd_App_Generic,
                                     "banned", /*bEnableLinger*/false);
            return;
        }
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
        // GEN: mint -- host accept edge. Ordered BEFORE the peerConns_ store so
        // any observer that can see the connection can also see who owns it.
        peerGenBySlot_[slot].store(MintPeerGeneration(), std::memory_order_release);
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
            // GNS skipped None->Connecting, so the accept-edge ban filter above
            // never ran for this connection. Re-run it here (Phase 2 audit fix)
            // -- otherwise a banned IP that hits this rare path would be admitted.
            if (!AcceptAllowed(sockets, hConn, acceptFilter_)) {
                UE_LOGW("net: rejecting late-register connection (banned remote IP)");
                sockets->CloseConnection(hConn, k_ESteamNetConnectionEnd_App_Generic,
                                         "banned", /*bEnableLinger*/false);
                return;
            }
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
            // GEN: mint -- the late-register accept edge (GNS skipped Connecting).
            // Same authority moment as the normal accept above.
            peerGenBySlot_[slot].store(MintPeerGeneration(), std::memory_order_release);
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
        // R-4b: mirror the buffer size the connection actually runs with --
        // the backlog drain's D8 reserve gate is computed against it. Same
        // resolve as ConfigureLanesForPeer's pin (knob or the default).
        {
            const long bufKb =
                coop::config::ResolveInt(coop::config_registry::rows::net_sendbuf_kb);
            sendBufBytes_ = (bufKb > 0) ? static_cast<int>(bufKb) * 1024
                                        : kDefaultSendBufBytes;
        }
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
        // Client: the host closed OUR connection (kick / ban / host quit / host
        // crash). Stash GNS's reason so net_pump can log WHY before fleeing to
        // the main menu. Host role ignores this (a client leaving is not us being
        // closed). slot 0 is the host on a client.
        if (cfg_.role == Role::Client && slot == 0) {
            std::lock_guard<std::mutex> lk(hostCloseMutex_);
            hostCloseReason_ = cb->m_info.m_szEndDebug;
        }
        if (slot >= 0) {
            // GEN: clear -- deferred to the END of this close path (below, after
            // the reliableInbox_ erase). The generation dropping to 0 is what
            // tells the game-thread ledger the slot emptied, and that tears down
            // the departed peer's person-state; clearing it here would let a
            // still-queued reliable from this peer dispatch AFTER the teardown.
            peerConns_[slot].store(0);
            peerLanesConfigured_[slot].store(false, std::memory_order_release);
            // R-4b: the departing peer's queued reliable state dies with it.
            backlog_.FreeSlot(slot);
            relayEligible_[slot].store(0, std::memory_order_release);  // seeds arc
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

        // GEN: clear -- the LAST write of the close path, release-ordered, and
        // deliberately AFTER the inbox erase above (which runs under a DIFFERENT
        // mutex). A reader that observes generation==0 is therefore guaranteed to
        // observe an inbox already drained of this peer.
        if (slot >= 0) peerGenBySlot_[slot].store(0, std::memory_order_release);

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
            for (auto& r : rttMsBySlot_) r.store(-1, std::memory_order_relaxed);  // per-slot RTT reset
            UE_LOGI("net: all peers gone -- session back to Disconnected");
        }
    }
}

bool Session::KickWithToken(int peerSlot, uint32_t expectedGeneration, const char* reason) {
    if (peerSlot < 1 || peerSlot >= kMaxPeers) return false;
    if (expectedGeneration == 0) return false;  // an empty-slot token can never authorize a kick
    const uint32_t hConnAtCapture = peerConns_[peerSlot].load();
    if (hConnAtCapture == 0) return false;
    // Compare the CAPTURED token against the LIVE authority. Stale -> refuse.
    const uint32_t liveGen = peerGenBySlot_[peerSlot].load(std::memory_order_acquire);
    if (liveGen != expectedGeneration) {
        UE_LOGW("net: kick/ban on slot %d REFUSED -- the captured occupant (gen %u) is gone; "
                "the slot now holds gen %u", peerSlot,
                static_cast<unsigned>(expectedGeneration), static_cast<unsigned>(liveGen));
        return false;
    }
    // Claim by HANDLE, not by slot. The generation check above can go stale
    // between these two instructions (the net thread could close and re-accept),
    // and a plain exchange(0) would then hand us the SUCCESSOR's connection --
    // which is precisely the person this whole path exists to protect. The CAS
    // closes that: a successor's accept stored a different handle, so it fails.
    // GEN: clear -- the claim; the generation itself is cleared at the end of
    // KickClaimed's teardown, after the inbox erase, exactly like the other two
    // close paths.
    uint32_t claimed = hConnAtCapture;
    if (!peerConns_[peerSlot].compare_exchange_strong(claimed, 0)) {
        UE_LOGW("net: kick/ban on slot %d REFUSED -- the connection changed under us", peerSlot);
        return false;
    }
    return KickClaimed(peerSlot, hConnAtCapture, reason);
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

bool Session::Kick(int peerSlot, const char* reason) {
    // Slot 0 is the host self -- never kickable. Bounds-reject everything else.
    if (peerSlot < 1 || peerSlot >= kMaxPeers) return false;
    // Atomically claim the slot so a concurrent natural ClosedByPeer on the net
    // thread and this kick can't both run the teardown (exchange -> 0 means we
    // own the close; a 0 result means someone already closed it).
    // GEN: clear -- deferred to the end of the teardown below, exactly as the
    // ClosedByPeer path does. (This site is an exchange, not a store: a census of
    // `.store(` alone MISSES it, and missing it would leave a kicked slot holding
    // a live generation, so the ledger would never see the row empty.)
    const uint32_t hConn = peerConns_[peerSlot].exchange(0);
    if (hConn == 0) return false;
    return KickClaimed(peerSlot, hConn, reason);
}

// R-4b D3: a slot's send backlog tripped a fatal bound (no progress for
// kNoProgress with a non-empty queue, or the byte cap). "Queued-until-sent or
// connection-fatal, never warn-and-drop": the honest exit is closing the
// connection, and the backlog dies WITH it (FreeSlot in KickClaimed's
// teardown). Host: kick the slot. Client: slot 0 is our host link -- claim it
// and run the SAME teardown (GNS delivers no callback for a connection we
// close; KickClaimed is slot-agnostic). A slow-but-DRAINING link never gets
// here -- progress resets the timer; a truly dead link normally dies at GNS's
// own connected-timeout first.
void Session::FatalCloseSlot(int slot, const char* reason) {
    UE_LOGE("net: send backlog FATAL for slot %d -- %s; closing the connection "
            "(delivery guarantee: never silently drop)", slot, reason ? reason : "?");
    if (cfg_.role == Role::Host) {
        Kick(slot, reason);
        return;
    }
    if (slot != 0) return;  // a client only owns its host link
    {   // Surface WHY on the client's own flee-to-menu path.
        std::lock_guard<std::mutex> lk(hostCloseMutex_);
        hostCloseReason_ = reason ? reason : "send backlog fatal";
    }
    const uint32_t hConn = peerConns_[0].exchange(0);
    if (hConn == 0) return;
    KickClaimed(0, hConn, reason);
}

// The teardown for a connection whose slot the caller has ALREADY claimed
// (peerConns_[peerSlot] exchanged/CAS'd to 0). Split out so the token-checked
// entry point can do a compare-exchange claim instead of a blind exchange and
// still share one teardown.
bool Session::KickClaimed(int peerSlot, uint32_t hConn, const char* reason) {
    peerLanesConfigured_[peerSlot].store(false, std::memory_order_release);
    // R-4b: the delivery guarantee is scoped to the connection's lifetime --
    // the departing peer's queued state dies with the peer.
    backlog_.FreeSlot(peerSlot);
    relayEligible_[peerSlot].store(0, std::memory_order_release);  // seeds arc

    if (auto* sockets = SteamNetworkingSockets()) {
        // No linger: an admin kick should drop the peer immediately. The reason
        // string rides to the peer's status callback (m_szEndDebug) so a kicked
        // client can surface WHY -- same channel as the protocol-mismatch close.
        sockets->CloseConnection(static_cast<HSteamNetConnection>(hConn),
                                 k_ESteamNetConnectionEnd_App_Generic,
                                 reason ? reason : "kicked", /*bEnableLinger*/false);
    }

    // GNS does not deliver a status callback to US for a connection we close,
    // so replicate the ClosedByPeer per-slot teardown here. (Even if a terminal
    // callback for this handle did race in on the net thread, the exchange(0)
    // above means FindPeerSlotForConn returns -1 there, so its teardown is
    // skipped and the only double-action would be a CloseConnection on an
    // already-closed handle -- which GNS handles idempotently. Teardown runs
    // exactly once, here.) Reset remote pose/prop/ragdoll state (so a
    // reconnecting peer's seq-from-0 isn't stale-dropped) and drop any reliable
    // messages still queued from this slot.
    { std::lock_guard<std::mutex> lk(remoteMutex_); ResetPeerRemoteState(peerSlot); }
    { std::lock_guard<std::mutex> lk(reliableInboxMutex_);
      for (auto it = reliableInbox_.begin(); it != reliableInbox_.end();) {
          if (it->senderPeerSlot == peerSlot) it = reliableInbox_.erase(it);
          else ++it;
      } }
    // GEN: clear -- last write of the teardown, after the inbox erase (see the
    // ClosedByPeer path for why the order is load-bearing).
    peerGenBySlot_[peerSlot].store(0, std::memory_order_release);

    // Aggregate state: stay Connected if any peer remains; otherwise downgrade
    // and clear everything (mirrors the ClosedByPeer branch above).
    if (connectedPeerCount() == 0) {
        state_.store(ConnState::Disconnected);
        { std::lock_guard<std::mutex> lk(remoteMutex_);
          for (int i = 0; i < kMaxPeers; ++i) ResetPeerRemoteState(i); }
        { std::lock_guard<std::mutex> lk(reliableInboxMutex_); reliableInbox_.clear(); }
        for (auto& r : rttMsBySlot_) r.store(-1, std::memory_order_relaxed);  // per-slot RTT reset
    }
    UE_LOGI("net: kicked peer slot %d (reason='%s')", peerSlot, reason ? reason : "kicked");
    return true;
}

std::string Session::TakeHostCloseReason() {
    std::lock_guard<std::mutex> lk(hostCloseMutex_);
    std::string r = std::move(hostCloseReason_);
    hostCloseReason_.clear();  // move may leave it valid-but-unspecified; force empty
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

// True for an address that can only be reached inside a local network:
// loopback, or one of the RFC1918 private IPv4 ranges. GetIPv4() returns HOST
// byte order (steamnetworkingtypes.h:1909), so the ranges are compared as
// 0xAABBCCDD literals; a real IPv6 peer yields 0 there and falls through to
// "not private", which is the correct answer for a routable v6 address.
static bool IsPrivateAddress(const SteamNetworkingIPAddr& addr) {
    if (addr.IsLocalHost()) return true;
    const uint32 v4 = addr.GetIPv4();
    if (v4 == 0) return false;                                  // not IPv4-mapped
    if ((v4 & 0xFF000000u) == 0x0A000000u) return true;         // 10.0.0.0/8
    if ((v4 & 0xFFF00000u) == 0xAC100000u) return true;         // 172.16.0.0/12
    if ((v4 & 0xFFFF0000u) == 0xC0A80000u) return true;         // 192.168.0.0/16
    if ((v4 & 0xFF000000u) == 0x7F000000u) return true;         // 127.0.0.0/8
    if ((v4 & 0xFFFF0000u) == 0xA9FE0000u) return true;         // 169.254.0.0/16 link-local
    return false;
}

// The classifier proper, split out from the connection fetch so it can be
// exercised over synthetic addresses (see RunLinkClassifySelftest).
//
// ORDER IS LOAD-BEARING, and the middle case is the one an audit caught: GNS
// documents m_addrRemote as "Might be all 0's if we don't know it, or if this is
// N/A" (steamnetworkingtypes.h) -- true on paths that are not plain direct UDP.
// An address test alone would then read a same-LAN ICE peer as `Direct`, which
// is a claim ("public, no relay") the connection never supported. So when the
// address is absent we answer from GNS's OWN flags, and if those say nothing we
// answer `Unknown` -- the whole point of this lane is that nobody prints a value
// nobody measured, and that applies to us too.
static LinkKind ClassifyLink(int infoFlags, const SteamNetworkingIPAddr& addr) {
    // Relay FIRST: a relayed path's remote address is the RELAY's, so an address
    // test there would describe the wrong hop.
    if (infoFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) return LinkKind::Relayed;
    // Loopback buffers are same-process by definition -- as local as it gets.
    if (infoFlags & k_nSteamNetworkConnectionInfoFlags_LoopbackBuffers) return LinkKind::Lan;
    if (addr.IsIPv6AllZeros()) {
        // No address to classify. GNS's `Fast` bit means "internal/localhost, or
        // the peer is on the same LAN" -- its own hedged best judgement, which is
        // still a measurement where we have none. Absent that: Unknown.
        return (infoFlags & k_nSteamNetworkConnectionInfoFlags_Fast) ? LinkKind::Lan
                                                                     : LinkKind::Unknown;
    }
    return IsPrivateAddress(addr) ? LinkKind::Lan : LinkKind::Direct;
}

bool RunLinkClassifySelftest() {
    // No port column: ClassifyLink never reads m_port, so a port field would be
    // the one column of this table with no discriminating power.
    struct Case { const char* what; const char* ip; int flags; LinkKind want; };
    // Known POSITIVES and known NEGATIVES. The negatives are what stop a
    // classifier that answers one value for everything from passing.
    static const Case kCases[] = {
        {"loopback v4",        "127.0.0.1", 0, LinkKind::Lan},
        {"rfc1918 10/8",       "10.0.0.5", 0, LinkKind::Lan},
        {"rfc1918 172.16/12",  "172.16.4.9", 0, LinkKind::Lan},
        {"rfc1918 192.168/16", "192.168.1.50", 0, LinkKind::Lan},
        {"link-local",         "169.254.7.7", 0, LinkKind::Lan},
        // NEGATIVES: 172.32 is OUTSIDE 172.16/12 and 11.x is outside 10/8 --
        // both are the classic off-by-a-mask mistakes, and both must read Direct.
        {"public 8.8.8.8",     "8.8.8.8", 0, LinkKind::Direct},
        {"public 172.32.0.1",  "172.32.0.1", 0, LinkKind::Direct},
        {"public 11.0.0.1",    "11.0.0.1", 0, LinkKind::Direct},
        // A real IPv6 peer: GetIPv4() returns 0 there, which must NOT be read as
        // 0.0.0.0-and-therefore-private.
        {"public v6",          "2606:4700::1111", 0, LinkKind::Direct},
        {"v6 loopback",        "::1", 0, LinkKind::Lan},
        // The relay flag WINS over any address, including a private one.
        {"relayed public",     "8.8.8.8",
             k_nSteamNetworkConnectionInfoFlags_Relayed, LinkKind::Relayed},
        {"relayed private",    "192.168.1.50",
             k_nSteamNetworkConnectionInfoFlags_Relayed, LinkKind::Relayed},
        // NO ADDRESS -- GNS leaves m_addrRemote all-zero on paths that are not
        // plain direct UDP. Answering `Direct` there would assert "public, no
        // relay" from nothing; these three pin the fallback ladder.
        {"no addr, no flags",  "::",              0, LinkKind::Unknown},
        {"no addr, Fast",      "::",
             k_nSteamNetworkConnectionInfoFlags_Fast, LinkKind::Lan},
        {"loopback buffers",   "::",
             k_nSteamNetworkConnectionInfoFlags_LoopbackBuffers, LinkKind::Lan},
    };
    int pass = 0, total = 0;
    for (const Case& c : kCases) {
        ++total;
        SteamNetworkingIPAddr addr{};
        addr.Clear();
        if (!addr.ParseString(c.ip)) {
            UE_LOGW("link-classify selftest: '%s' did not parse -- case '%s' SKIPPED as FAIL",
                    c.ip, c.what);
            continue;
        }
        const LinkKind got = ClassifyLink(c.flags, addr);
        if (got == c.want) { ++pass; continue; }
        UE_LOGW("link-classify selftest: '%s' (%s flags=0x%x) -> %d, expected %d",
                c.what, c.ip, static_cast<unsigned>(c.flags),
                static_cast<int>(got), static_cast<int>(c.want));
    }
    const bool ok = (pass == total);
    if (ok) UE_LOGI("link-classify selftest: PASS (%d/%d cases)", pass, total);
    else    UE_LOGE("link-classify selftest: FAIL (%d/%d cases)", pass, total);
    return ok;
}

LinkKind Session::LinkKindForSlot(int peerSlot) const {
    // v131. EVERY kind is measured FROM THE CONNECTION. The pre-v131 code
    // answered "LAN" whenever cfg_.topology was LanDirect -- a config assertion
    // that labelled a port-forwarded WAN peer "LAN" -- and split relay-vs-direct
    // by substring-matching m_szConnectionDescription, a human-readable string,
    // when GNS publishes the fact as a documented bit. Both are retired: a value
    // nobody measured is the same defect as "VIA HOST" in truer-looking words.
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return LinkKind::Unknown;
    const uint32_t hConn = peerConns_[peerSlot].load();
    if (hConn == 0) return LinkKind::Unknown;
    auto* sockets = SteamNetworkingSockets();
    if (!sockets) return LinkKind::Unknown;
    SteamNetConnectionInfo_t info{};
    if (!sockets->GetConnectionInfo(static_cast<HSteamNetConnection>(hConn), &info))
        return LinkKind::Unknown;
    return ClassifyLink(info.m_nFlags, info.m_addrRemote);
}

}  // namespace coop::net
