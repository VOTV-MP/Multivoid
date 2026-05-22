#include "coop/net/reliable_channel.h"

#include "ue_wrap/log.h"

#include <cstring>

namespace coop::net {

bool ReliableChannel::Send(ReliableKind kind, const void* payload, int payloadLen) {
    if (payloadLen < 0 || payloadLen > kMaxReliablePayload) {
        UE_LOGW("net: reliable Send rejected (len=%d)", payloadLen);
        return false;
    }
    std::lock_guard<std::mutex> lk(outboxMutex_);
    if (outPending_) return false;  // stop-and-wait: one in flight at a time
    outPending_ = true;
    outRelSeq_ = nextSendRelSeq_;
    outKind_ = kind;
    outPayload_.assign(static_cast<const uint8_t*>(payload),
                       static_cast<const uint8_t*>(payload) + payloadLen);
    nextRetransmit_ = std::chrono::steady_clock::now();  // send on the next Tick
    return true;
}

void ReliableChannel::Tick(const SendDatagramFn& sendToPeer, uint64_t token) {
    // Build the datagram under the lock, then send OUTSIDE it: never hold a mutex
    // across socket I/O (so the game thread's Send can't block behind a stalled send).
    uint8_t buf[kMaxPacketBytes];
    int total = 0;
    {
        std::lock_guard<std::mutex> lk(outboxMutex_);
        if (!outPending_) return;
        const auto now = std::chrono::steady_clock::now();
        if (now < nextRetransmit_) return;
        auto* hdr = reinterpret_cast<PacketHeader*>(buf);
        WriteHeader(*hdr, MsgType::Reliable, outRelSeq_, token);
        auto* rh = reinterpret_cast<ReliableHeader*>(buf + sizeof(PacketHeader));
        std::memset(rh, 0, sizeof(*rh));
        rh->kind = static_cast<uint8_t>(outKind_);
        rh->payloadLen = static_cast<uint16_t>(outPayload_.size());
        std::memcpy(buf + sizeof(PacketHeader) + sizeof(ReliableHeader), outPayload_.data(), outPayload_.size());
        total = static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader) + outPayload_.size());
        nextRetransmit_ = now + kRto;
    }
    sendToPeer(buf, total);
}

void ReliableChannel::OnReliable(const void* data, int len, uint64_t token, const SendDatagramFn& ack) {
    if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader))) return;
    PacketHeader hdr;
    std::memcpy(&hdr, data, sizeof(hdr));
    ReliableHeader rh;
    std::memcpy(&rh, static_cast<const uint8_t*>(data) + sizeof(PacketHeader), sizeof(rh));
    const int payloadLen = static_cast<int>(rh.payloadLen);
    if (payloadLen > kMaxReliablePayload) return;  // uint16 -> always >= 0
    if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(ReliableHeader)) + payloadLen) return;

    // Decide accept/ack under the lock; SEND the ack outside it (never socket I/O
    // while holding a mutex). Stop-and-wait, so:
    //  - seq < expected  : already delivered -> ack again (recovers a lost ack), no re-deliver.
    //  - seq == expected & inbox free : deliver + advance + ack (the one new message).
    //  - seq == expected & inbox FULL : drop without ack -> the sender keeps
    //    retransmitting until the game thread drains, so nothing is lost (the bug
    //    was: acking/advancing on a retransmit of an undrained delivery -> stall).
    //  - seq > expected  : a gap (impossible in stop-and-wait) -> drop, no ack.
    bool doAck = false;
    {
        std::lock_guard<std::mutex> lk(inboxMutex_);
        const int32_t d = static_cast<int32_t>(hdr.seq - expectedRelSeq_);
        if (d < 0) {
            doAck = true;  // duplicate of an already-delivered message
        } else if (d == 0 && !hasInbox_) {
            inbox_.kind = static_cast<ReliableKind>(rh.kind);
            inbox_.payload.assign(
                static_cast<const uint8_t*>(data) + sizeof(PacketHeader) + sizeof(ReliableHeader),
                static_cast<const uint8_t*>(data) + sizeof(PacketHeader) + sizeof(ReliableHeader) + payloadLen);
            hasInbox_ = true;
            ++expectedRelSeq_;
            doAck = true;
        }
        // d == 0 && hasInbox_ (inbox full) or d > 0 (gap): no ack, no deliver.
    }
    if (doAck) {
        PacketHeader a;
        WriteHeader(a, MsgType::ReliableAck, hdr.seq, token);
        ack(&a, sizeof(a));
    }
}

void ReliableChannel::OnAck(uint32_t relSeq) {
    std::lock_guard<std::mutex> lk(outboxMutex_);
    if (outPending_ && relSeq == outRelSeq_) {
        outPending_ = false;
        ++nextSendRelSeq_;
    }
}

bool ReliableChannel::TryDrain(Message& out) {
    std::lock_guard<std::mutex> lk(inboxMutex_);
    if (!hasInbox_) return false;
    out = inbox_;
    hasInbox_ = false;
    return true;
}

void ReliableChannel::Reset() {
    { std::lock_guard<std::mutex> lk(outboxMutex_);
      outPending_ = false; outRelSeq_ = 0; nextSendRelSeq_ = 0; outPayload_.clear(); }
    { std::lock_guard<std::mutex> lk(inboxMutex_);
      hasInbox_ = false; expectedRelSeq_ = 0; inbox_.payload.clear(); }
}

}  // namespace coop::net
