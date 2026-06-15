// coop/net/session_voice.cpp -- v66 voice-frame send/receive for Session.
//
// Extracted from session.cpp (2026-06-12) per the 800-LOC soft cap (the
// session_npc.cpp precedent; session.cpp hit 894 when the VoiceFrame path
// landed). Owns the unreliable voice STREAM (MsgType::VoiceFrame): the game
// thread fan-out send (SendVoiceFrame -- GNS send APIs are thread-safe, the
// SendReliable calling convention), the receive-side inbox store
// (StoreVoiceFrame, net thread, called from HandleMessage's case) and the
// game-thread batch drain (DrainVoiceFrames). A per-sender FIFO STREAM, not
// the newest-wins pose model: every arrival queues; ordering/loss live in the
// per-payload voice seq at the jitter buffer (coop/voice/voice_playback).
// The inbox is per-slot fixed rings (audit I-3): no net-thread heap alloc,
// per-sender overflow fairness. Mutex discipline: voiceInboxMutex_ only.

#include "coop/net/session.h"

#include "ue_wrap/log.h"

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include <cstring>
#include <mutex>

namespace coop::net {

bool Session::SendVoiceFrame(const VoiceFramePayload& frame) {
    if (frame.opusLen > kVoiceMaxOpusBytes) return false;
    const int bodyLen = kVoiceFrameHeadBytes + frame.opusLen;
    const int total = static_cast<int>(sizeof(PacketHeader)) + bodyLen;
    const uint32_t seq = sendSeq_.fetch_add(1);

    auto* sockets = SteamNetworkingSockets();
    auto* utils = SteamNetworkingUtils();

    bool anySuccess = false;
    for (int i = 0; i < kMaxPeers; ++i) {
        const uint32_t hConn = peerConns_[i].load();
        if (hConn == 0) continue;
        if (!IsSlotWorldReady(i)) continue;  // a menu-mode joiner has no listener yet
        SteamNetworkingMessage_t* msg = utils->AllocateMessage(total);
        if (!msg) continue;
        auto* buf = static_cast<uint8_t*>(msg->m_pData);
        auto* hdr = reinterpret_cast<PacketHeader*>(buf);
        WriteHeader(*hdr, MsgType::VoiceFrame, seq, ownEpoch_);
        std::memcpy(buf + sizeof(PacketHeader), &frame, bodyLen);
        msg->m_conn = hConn;
        msg->m_nFlags = k_nSteamNetworkingSend_UnreliableNoDelay;
        int64 outMsgNum = 0;
        sockets->SendMessages(1, &msg, &outMsgNum, /*bDeleteFailedMessages*/true);
        if (outMsgNum >= 0) {
            sent_.fetch_add(1);
            anySuccess = true;
        }
    }
    return anySuccess;
}

void Session::StoreVoiceFrame(int routeSlot, int peerSlot, const void* data, int len) {
    const int minLen = static_cast<int>(sizeof(PacketHeader)) + kVoiceFrameHeadBytes;
    if (len < minLen) return;
    if (routeSlot < 0 || routeSlot >= kMaxPeers) return;  // ring index safety
    const int bodyLen = len - static_cast<int>(sizeof(PacketHeader));
    VoiceFrameMsg vm{};
    vm.senderSlot = static_cast<int8_t>(routeSlot);
    const int copyLen = bodyLen > static_cast<int>(sizeof(vm.frame))
                            ? static_cast<int>(sizeof(vm.frame)) : bodyLen;
    std::memcpy(&vm.frame, static_cast<const uint8_t*>(data) + sizeof(PacketHeader), copyLen);
    if (vm.frame.opusLen > kVoiceMaxOpusBytes ||
        kVoiceFrameHeadBytes + vm.frame.opusLen > bodyLen)
        return;  // truncated/poisoned datagram
    {
        std::lock_guard<std::mutex> lk(voiceInboxMutex_);
        VoiceSlotRing& r = voiceRings_[routeSlot];
        if (r.tail - r.head >= static_cast<uint32_t>(kVoiceRingPerSlot))
            ++r.head;  // full: shed THIS sender's oldest frame only
        r.ring[r.tail % kVoiceRingPerSlot] = vm;
        ++r.tail;
    }
    // Host relay: every other client hears this speaker (receiver-side
    // attenuation silences out-of-range audio; see the MsgType doc).
    if (cfg_.role == Role::Host) {
        RelayUnreliableToOtherClients(peerSlot, data, len);
    }
}

int Session::DrainVoiceFrames(VoiceFrameMsg* out, int maxCount) {
    int n = 0;
    std::lock_guard<std::mutex> lk(voiceInboxMutex_);
    for (int slot = 0; slot < kMaxPeers && n < maxCount; ++slot) {
        VoiceSlotRing& r = voiceRings_[slot];
        while (r.head != r.tail && n < maxCount) {
            out[n++] = r.ring[r.head % kVoiceRingPerSlot];
            ++r.head;
        }
    }
    return n;
}

}  // namespace coop::net
