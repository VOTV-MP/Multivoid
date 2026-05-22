// coop/net/reliable_channel.h -- reliable, ordered delivery over the UDP session.
//
// The pose channel is intentionally unreliable (drop freely, newest wins). Chat and
// system events must NOT be lost or reordered, so they ride this channel: the SAME
// socket and net thread, distinguished by MsgType::Reliable / ReliableAck (RULE 2:
// one transport, two reliability classes -- not a second socket).
//
// Design (per the code-architect review): a 2-peer LAN session carries only a
// handful of control messages, so a sliding window is overkill. This is minimal
// stop-and-wait ARQ: at most ONE unacked message in flight per direction; retransmit
// every RTO until acked; the receiver always acks (idempotent) and delivers strictly
// in order with dedup. Owned by Session (a sub-system of it); it never touches the
// engine -- it hands delivered payloads to the game thread via TryDrain.

#pragma once

#include "coop/net/protocol.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace coop::net {

// Sends a datagram to the peer (or a specific address for acks). Returns true if
// handed to the OS. Supplied by Session, which owns the Transport + peer.
using SendDatagramFn = std::function<bool(const void* data, int len)>;

class ReliableChannel {
public:
    // Game thread: queue a reliable message. Stop-and-wait, so it returns false if
    // a message is still in flight (caller may retry; control traffic is sparse).
    bool Send(ReliableKind kind, const void* payload, int payloadLen);

    // Net thread: retransmit the in-flight message if its RTO elapsed.
    void Tick(const SendDatagramFn& sendToPeer, uint64_t token);

    // Net thread: a Reliable datagram arrived. Ack it (always, via `ack`), then
    // deliver to the inbox if it is the next expected seq (else dedup/drop).
    void OnReliable(const void* data, int len, uint64_t token, const SendDatagramFn& ack);

    // Net thread: an ack arrived -- clear the in-flight message if it matches.
    void OnAck(uint32_t relSeq);

    // Game thread: pop a delivered message, if any new one arrived since last call.
    struct Message { ReliableKind kind; std::vector<uint8_t> payload; };
    bool TryDrain(Message& out);

    // Clear all state (disconnect / reconnect): a reconnecting peer restarts its
    // relSeq at 0, so stale expected/seq state must be reset or it stalls.
    void Reset();

private:
    static constexpr auto kRto = std::chrono::milliseconds(250);

    std::mutex outboxMutex_;
    bool outPending_ = false;
    uint32_t outRelSeq_ = 0;            // seq of the in-flight message
    uint32_t nextSendRelSeq_ = 0;       // next seq to assign (advances on ack)
    ReliableKind outKind_ = ReliableKind::Join;
    std::vector<uint8_t> outPayload_;
    std::chrono::steady_clock::time_point nextRetransmit_;

    std::mutex inboxMutex_;
    bool hasInbox_ = false;
    Message inbox_;
    uint32_t expectedRelSeq_ = 0;       // next seq we expect to deliver from the peer
};

}  // namespace coop::net
