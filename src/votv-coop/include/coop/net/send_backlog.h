// coop/net/send_backlog.h -- the reliable-send delivery guarantee.
//
// GNS "reliable" is ARQ only for messages that ENTER the stream. With the per-connection
// send buffer full, SendMessages refuses at enqueue (-k_EResultLimitExceeded) and, under
// bDeleteFailedMessages, deletes the message. Some sixty call sites used to ignore that
// false return, which is how a join could silently lose hundreds of prop spawns and leave
// containers permanently empty on the joiner.
//
// THE INVARIANT: a reliable send either enters the stream, enters this backlog, or the
// connection dies. Never warn-and-drop. The guarantee's scope is the connection's lifetime;
// FreeSlot at teardown discards the dead peer's backlog, its state dying with it (the MTA
// CNetServerBuffer precedent). Each slot's backlog is stamped with the hConn it opened
// under, and the drain discards the whole thing when the slot's live hConn no longer
// matches: slots recycle lowest-free with no observable absence, and person Y must never
// receive person X's queued state.

#pragma once

#include "coop/player/players_registry.h"  // kMaxPeers

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace coop::net {

class SendBacklog {
public:
    static constexpr int kLaneCount = 3;  // pinned to Lane::Count (session.cpp static_assert)

    // Fatal-bound policy (queued-until-sent or CONNECTION-FATAL, never drop):
    //  - no-progress: backlog non-empty and NOTHING departed for this long. A
    //    slow-but-DRAINING link never trips it; a truly dead link usually dies
    //    at GNS's own connected-timeout first.
    //  - byte cap: ~20x the measured worst realistic peak (the join burst is
    //    ~742 KB total; steady-state authoring is event-driven). A link this
    //    far behind is minutes stale -- kicking is honest, dropping is the bug
    //    this class exists to kill.
    static constexpr auto kNoProgress = std::chrono::seconds(30);
    static constexpr size_t kMaxBytesPerSlot = 16u * 1024u * 1024u;

    // Attempt-or-queue one complete on-wire reliable packet (PacketHeader +
    // ReliableHeader + payload, prebuilt by the caller -- the relay's rewritten
    // packets share this shape). Holds the (slot,lane) section across the GNS
    // attempt (see header comment). Returns:
    //   true  -- the packet WILL be delivered (entered the stream or the backlog);
    //   false -- it never will: the connection refused it fatally (dying/dead --
    //            teardown owns cleanup) or the args were invalid. Callers treat
    //            false exactly like today's dead-slot false.
    // Any thread. `hConn` is the slot's CURRENT connection handle.
    bool SendOrQueue(int slot, int lane, uint32_t hConn, const uint8_t* wire, int len);

    // One drain pass for a slot (net-thread tick). Re-attempts queued heads in lane-priority
    // order (High -> Normal -> Bulk); the GNS rc is the headroom read, so a refusal ends the
    // pass. `reserveGate`: the pass stops refilling once the connection's pending bytes
    // (reliable plus unreliable, the exact sum GNS's own enqueue check uses) exceed
    // sendBufBytes - kReserve, so the UnreliableNoDelay pose and voice streams keep flowing
    // through a drain episode instead of being starved for its whole length.
    void Drain(int slot, uint32_t hConn, int sendBufBytes);

    // True when the slot's backlog has tripped a fatal bound (no-progress or byte cap). Sets
    // `reason` to a static string. The caller on the net thread kicks or closes; this class
    // never touches connections beyond SendMessages.
    bool CheckFatal(int slot, const char** reason);

    // Teardown: discard everything queued for the slot. Call where
    // peerConns_[slot] is zeroed. Any thread.
    void FreeSlot(int slot);

    // net-diag: total queued bytes across the slot's lanes (0 = idle).
    size_t DepthBytes(int slot);

    // Headroom kept free for the unreliable streams during a drain episode.
    // Worst realistic concurrent unreliable is ~1.2 KB per 16 ms tick
    // (3 peers x 228 B voice frames + poses) -- 64 KB is ~30x margin.
    static constexpr int kReserve = 64 * 1024;

    // Max messages one Drain() pass re-injects: bounds the per-slot mutex hold, and so the
    // cross-lane wait, to sub-millisecond; the next net-thread pass continues. 256 messages at
    // about 200 passes a second far exceeds any burst.
    static constexpr int kDrainPassCap = 256;

private:
    struct LaneQ {
        std::deque<std::vector<uint8_t>> q;
        size_t bytes = 0;
    };
    struct SlotQ {
        std::mutex mu;
        uint32_t hConn = 0;  // the connection this backlog belongs to (0 = none)
        LaneQ lanes[kLaneCount];
        size_t totalBytes = 0;
        // Progress tracking for the no-progress bound: armed while non-empty.
        std::chrono::steady_clock::time_point lastProgress{};
        bool episodeOpen = false;      // D6: fold the episode into 2 log lines
        uint64_t episodeQueued = 0;    // messages absorbed this episode
        size_t episodePeakBytes = 0;
        bool fatal = false;
        const char* fatalReason = nullptr;
        bool dyingLogged = false;  // fold the repeated dying-connection lines
    };
    // Reset q to a fresh state under mu (caller holds mu).
    void ResetLocked_(SlotQ& s);

    SlotQ slots_[coop::players::kMaxPeers];
};

}  // namespace coop::net
