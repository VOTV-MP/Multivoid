// coop/net/session_teardown.cpp -- the slot teardown family: the one close every kick, ban and
// leave runs for a connection whose slot the caller has claimed, the net thread's freeing of a slot
// an off-thread kick queued, and the session's all-peers-gone edge, which only the net thread writes.

#include "coop/net/session.h"

#include "ue_wrap/core/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#pragma warning(pop)

namespace coop::net {

// The teardown for a connection whose slot the caller has already claimed (exchanged or CAS'd to
// 0), shared by the blind and the token-checked entry points. On the net thread it is the whole
// teardown, the generation clear included: the supersede's admission, in the same pass, then finds
// the slot free. From any other thread it does every step but that clear and queues the slot, so
// that the net thread -- the only thread a receive or an admission runs on -- frees the slot after
// whatever receive was in flight for the departed peer has landed and been swept.
bool Session::KickClaimed(int peerSlot, uint32_t hConn, EndReason code, const char* reason) {
    const bool onNetThread = std::this_thread::get_id() == netThreadId_.load();
    peerLanesConfigured_[peerSlot].store(false, std::memory_order_release);
    // The delivery guarantee is scoped to the connection: the queued state dies with the peer, and
    // the link measurement with it.
    backlog_.FreeSlot(peerSlot);
    rateControl_.FreeSlot(peerSlot);
    admission_.FreeSlot(peerSlot);
    relayEligible_[peerSlot].store(0, std::memory_order_release);

    if (auto* sockets = SteamNetworkingSockets()) {
        // No linger: a kick drops the peer at once. The reason rides to the peer's status callback,
        // so a kicked client can say why.
        sockets->CloseConnection(static_cast<HSteamNetConnection>(hConn),
                                 ToTransportEnd(code),
                                 reason ? reason : Describe(code).text, /*bEnableLinger*/false);
    }

    // A connection we close is not closed by its peer, so the close path's teardown does not run for
    // it and is replicated here; the claim above makes a racing callback's FindPeerSlotForConn
    // return -1, or its own claim fail if it found the slot first, so the teardown runs exactly once.
    { std::lock_guard<std::mutex> lk(remoteMutex_); ResetPeerRemoteState(peerSlot); }
    { std::lock_guard<std::mutex> lk(reliableInboxMutex_);
      for (auto it = reliableInbox_.begin(); it != reliableInbox_.end();) {
          if (it->senderPeerSlot == peerSlot) it = reliableInbox_.erase(it);
          else ++it;
      } }
    // A client's slots 1..3 are the other players as the host relayed them, and they end with the
    // link: leaving the host resets every slot and empties the whole inbox, as its close does.
    if (cfg_.role == Role::Client && peerSlot == 0) {
        { std::lock_guard<std::mutex> lk(remoteMutex_);
          for (int i = 0; i < kMaxPeers; ++i) ResetPeerRemoteState(i); }
        { std::lock_guard<std::mutex> lk(reliableInboxMutex_); reliableInbox_.clear(); }
    }
    // The proved identity goes first: a recycled slot must not carry its predecessor's storage
    // name. The generation clear is the last per-slot write, after the inbox erase (the
    // ClosedByPeer path says why the order matters).
    SetProvedGuidForSlot(peerSlot, 0, std::string());
    if (onNetThread) {
        peerGenBySlot_[peerSlot].store(0, std::memory_order_release);
        // The all-peers-gone edge (Connected or linked to Disconnected) is written by
        // UpdateAggregateState and nowhere else; this close only marks it due.
        aggregateDue_.store(true);
    } else {
        std::lock_guard<std::mutex> lk(teardownMutex_);
        pendingFrees_.push_back(PendingFree{peerSlot});
    }
    UE_LOGI("net: kicked peer slot %d [%s] (reason='%s')", peerSlot, Describe(code).id,
            reason ? reason : Describe(code).text);
    return true;
}

// The net thread's half of an off-thread kick. A receive that was in flight when the kick claimed
// the slot may have written the departed peer's epoch or streams after the kick's own reset, and
// the receive path's connection check under the inbox mutex closes only the inbox's half of that
// window; so the slot's remote state is swept once more here, and the generation is cleared LAST.
// A successor needs generation 0 (FindFreePeerSlotForClient), so nothing of a successor is ever
// wiped.
void Session::RunPendingFrees() {
    bool any = false;
    for (;;) {
        PendingFree f;
        {
            std::lock_guard<std::mutex> lk(teardownMutex_);
            if (pendingFrees_.empty()) break;
            f = pendingFrees_.back();
            pendingFrees_.pop_back();
        }
        { std::lock_guard<std::mutex> lk(remoteMutex_); ResetPeerRemoteState(f.slot); }
        backlog_.FreeSlot(f.slot);  // again, after the inbox erase: a ClientWorldReady receipt that raced an off-thread
                                     // kick could have opened the join gate after the first free
        peerGenBySlot_[f.slot].store(0, std::memory_order_release);
        any = true;
    }
    if (any) aggregateDue_.store(true);
}

// An EDGE after a close, never a level test: a host that has not yet seated anyone has no peer
// either, and is not "back" at Disconnected. A host goes Disconnected from Connected when its last
// seated peer is gone; a client when its host link is gone. A seat admitted earlier in the same pass
// is counted by now, so a supersede (close, then admission) never flips the host.
void Session::UpdateAggregateState() {
    if (!aggregateDue_.exchange(false)) return;
    const bool allGone = cfg_.role == Role::Host
        ? (state_.load() == ConnState::Connected && connectedPeerCount() == 0)
        : (peerConns_[0].load() == 0 && state_.load() != ConnState::Disconnected);
    if (!allGone) return;
    // A full disconnect goes to Disconnected, not Handshaking, which the reconnect UI and the
    // harness poll for.
    state_.store(ConnState::Disconnected);
    linkStage_.store(static_cast<uint8_t>(LinkStage::Idle), std::memory_order_release);
    for (auto& r : rttMsBySlot_) r.store(-1, std::memory_order_relaxed);  // per-slot RTT reset
    UE_LOGI("net: all peers gone -- session back to Disconnected");
}

}  // namespace coop::net
