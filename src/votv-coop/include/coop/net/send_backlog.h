// coop/net/send_backlog.h -- the reliable-send delivery guarantee (R-4b).
//
// GNS "reliable" is ARQ only for messages that ENTER the stream: when the
// per-connection send buffer is full, SendMessages refuses the message at
// enqueue (-k_EResultLimitExceeded) and, with bDeleteFailedMessages, deletes
// it. Before this class existed ~60 call sites ignored that false return --
// measured in the field as 485 silently-lost PropSpawns in one join minute
// (snapshot 2,607/3,093 applied, 282 containers permanently empty on the
// joiner), and locally as 956 multi-kind losses in one pinned-buffer smoke.
//
// THE INVARIANT: a reliable send either enters the stream, enters this
// backlog, or the connection dies. Never warn-and-drop. Guarantee scope is
// the connection's lifetime: FreeSlot at teardown discards the dead peer's
// backlog (its state dies with it -- MTA CNetServerBuffer precedent).
//
// Shape: one FIFO per (slot, lane) -- GNS's ordering domain is the LANE, so a
// per-slot QUEUE would collapse lane independence (a Bulk join-burst backlog
// must not block a High-lane TeleportClient). FIFO-once-nonempty: while a
// (slot,lane) backlog is non-empty, every later send for that (slot,lane)
// appends behind it -- enforced by holding the slot's section across
// [empty-check -> GNS attempt -> on-refusal append], which is what keeps the
// order across BOTH producer threads (game-thread authors, net-thread relay).
// THE MUTEX IS PER-SLOT (covering its 3 lanes) BY CHOICE -- coarser than the
// ordering domain (audit 2026-08-23): correctness needs only per-lane
// atomicity, but one lock per slot avoids three-way lock-order surface for
// zero measured need; the cross-lane hold is bounded by kDrainPassCap below
// (a drain pass is at most 256 sends, ~sub-ms), so a High send never waits on
// a whole Bulk episode.
//
// Each slot's backlog is stamped with the hConn it opened under; the drain
// discards the whole backlog when the slot's live hConn no longer matches
// (slots recycle lowest-free with no observable absence -- person Y must
// never receive person X's queued state, even if a teardown path missed).
//
// Design of record (6-round /qf):
// research/findings/network/votv-reliable-delivery-guarantee-DESIGN-2026-08-23.md

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

    // One drain pass for a slot (net-thread tick). Re-attempts queued heads in
    // lane-priority order (High -> Normal -> Bulk); the GNS rc is the headroom
    // read -- a refusal ends the pass. `reserveGate` (D8): the pass stops
    // refilling once the connection's pending bytes (reliable + unreliable,
    // the exact sum GNS's enqueue check uses) exceed sendBufBytes - kReserve,
    // so the UnreliableNoDelay pose/voice streams keep flowing during a drain
    // episode instead of being starved for its whole length.
    void Drain(int slot, uint32_t hConn, int sendBufBytes);

    // D3: true when the slot's backlog has tripped a fatal bound (no-progress
    // or byte cap). Sets `reason` (static string). The caller (net thread)
    // kicks/closes; this class never touches connections beyond SendMessages.
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

    // Max messages one Drain() pass re-injects (audit WARN-1): bounds the
    // per-slot mutex hold (and the cross-lane wait) to sub-ms; the next
    // net-thread pass continues. 256 * ~200 passes/s far exceeds any burst.
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
        bool dyingLogged = false;  // fold the dying-conn lines (audit WARN-2)
    };
    // Reset q to a fresh state under mu (caller holds mu).
    void ResetLocked_(SlotQ& s);

    SlotQ slots_[coop::players::kMaxPeers];
};

}  // namespace coop::net
