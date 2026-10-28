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

#include "coop/net/send_admission.h"       // the send buffer's headroom rule, one for every path
#include "coop/player/players_registry.h"  // kMaxPeers

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace coop::net {

// What became of one reliable send. The delivery guarantee only distinguishes "will arrive" from
// "never will", but the send-rate measurement needs the third fact: whether these bytes are now
// GNS's problem or still ours, since only the ones GNS took can come back as an acknowledgement.
enum class SendOutcome : uint8_t {
    Streamed,  // GNS accepted the whole packet; it is in the reliable stream
    Queued,    // the send buffer refused it; it is in this backlog and will be re-attempted
    Dropped,   // it never will be delivered: the connection is dying, or the arguments were invalid
};

class SendBacklog {
public:
    static constexpr int kLaneCount = 3;  // pinned to Lane::Count (session.cpp static_assert)

    // Fatal-bound policy (queued-until-sent or CONNECTION-FATAL, never drop):
    //  - no-progress: backlog non-empty and NOTHING departed for this long. A
    //    slow-but-DRAINING link never trips it; a truly dead link usually dies
    //    at GNS's own connected-timeout first.
    //  - byte cap: ~20x the measured worst realistic peak (the join burst was
    //    ~742 KB total when that was measured, and the device-slot seed rides
    //    it too -- one canonical per disc-holding device, so a base whose boxes
    //    all hold a disc adds its own content on top; steady-state authoring is
    //    event-driven). A link this far behind is minutes stale -- kicking is
    //    honest, dropping is the bug this class exists to kill.
    static constexpr auto kNoProgress = std::chrono::seconds(30);
    // A join gate open this long without closing logs one warning (OpenJoinGate).
    static constexpr auto kJoinGateWarn = std::chrono::seconds(10);
    static constexpr size_t kMaxBytesPerSlot = 16u * 1024u * 1024u;

    // Attempt-or-queue one complete on-wire reliable packet (PacketHeader +
    // ReliableHeader + payload, prebuilt by the caller -- the relay's rewritten
    // packets share this shape). Holds the (slot,lane) section across the GNS
    // attempt (see header comment). Streamed and Queued both mean the packet WILL
    // be delivered; Dropped means it never will, because the connection refused it
    // fatally (dying/dead -- teardown owns cleanup) or the args were invalid, and
    // callers treat it exactly like today's dead-slot false.
    // `adm` is the headroom rule: a packet it refuses queues here instead of entering the buffer,
    // which changes when a packet departs and never whether it does.
    // Any thread. `hConn` is the slot's CURRENT connection handle.
    SendOutcome SendOrQueue(int slot, int lane, uint32_t hConn, const uint8_t* wire, int len,
                            SendAdmission& adm);

    // `count` equal-length packets (`len` bytes each, contiguous in `wire`) sent to one (slot,lane)
    // under ONE acquisition of the slot's lock, in order: a join gate opened on the net thread cannot
    // split them, so they go straight or are held together. Each packet follows SendOrQueue's rules;
    // the first Dropped ends the batch. Returns Dropped if one was, else Queued if any was held, else
    // Streamed; `streamed` is how many entered GNS. The slot lock is held across up to 255 sends
    // (a blob's chunk ceiling), about the length of one Drain pass (kDrainPassCap). Any thread.
    SendOutcome SendOrQueueBatch(int slot, int lane, uint32_t hConn, const uint8_t* wire, int len,
                                 int count, SendAdmission& adm, int& streamed);

    // One drain pass for a slot (net-thread tick). Re-attempts queued heads in lane-priority
    // order (High -> Default -> Transfer); the GNS rc is the correctness backstop, so a refusal ends
    // the pass. The refill stops at the same headroom rule every send path obeys, so the
    // UnreliableNoDelay pose and voice streams keep flowing through a drain episode instead of
    // being starved for its whole length. Returns the bytes this pass handed to GNS, which is the
    // share of a slot's delivery the backlog accounts for.
    int Drain(int slot, uint32_t hConn, SendAdmission& adm);

    // True when the slot's backlog has tripped a fatal bound (no-progress or byte cap). Sets
    // `reason` to a static string. The caller on the net thread kicks or closes; this class
    // never touches connections beyond SendMessages.
    bool CheckFatal(int slot, const char** reason);

    // Teardown: discard everything queued for the slot. Call where
    // peerConns_[slot] is zeroed. Any thread.
    void FreeSlot(int slot);

    // net-diag: total queued bytes across the slot's lanes (0 = idle). The join gate's queue is not
    // counted: it is held by design and is not a link backlog.
    size_t DepthBytes(int slot);

    // The join gate: MTA's not-yet-joined player (CPlayerManager.cpp:159-172, BroadcastOnlyJoined). From
    // OpenJoinGate to CloseJoinGate, every Default-lane send to the slot is appended to the slot's join
    // queue in send order, except a send made inside a SnapshotSendScope for that slot. CloseJoinGate
    // splices the queue onto the slot's Default lane behind what is already there under the lock (a swap
    // when the lane is empty, an append otherwise), and lifts the gate; a slot that leaves drops its queue
    // with the rest of its backlog (FreeSlot). The queue counts toward the slot's byte cap and its
    // stale-connection stamp, and not toward the no-progress timer, DepthBytes or SlotReliableIdle.
    // OpenJoinGate on an open gate keeps its queue, and counts a hold that CloseJoinGate respects (it
    // closes only at zero) until ReleaseAnnounceHold. OpenJoinGateForSnapshot opens with no hold.
    // DIVERGENCE: MTA skips a not-joined player and sends the state inside the one add; ours defers and
    // replays in send order, since the add spans ticks. MTA's per-element alternative (CPerPlayerEntity.h,
    // a sent-set per player) was rejected: it needs every Default kind's element id at send. The cost: a
    // held row can be older than a later spawn, so the handlers are idempotent.
    // Any thread.
    void OpenJoinGate(int slot);
    void OpenJoinGateForSnapshot(int slot);
    void ReleaseAnnounceHold(int slot);
    void CloseJoinGate(int slot);

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
        // The join gate (OpenJoinGate). joinQ holds the wire packets in send order, and joinStamps is
        // parallel to it: when each was held and which ReliableKind it carries, for the close line.
        struct JoinStamp {
            std::chrono::steady_clock::time_point at;
            uint8_t kind;
        };
        bool joinGate = false;
        // Announces received whose replay has not yet run: a drain finishing in that gap must not
        // close the gate, or the replay's seeds would go out ungated.
        uint32_t announceHolds = 0;
        std::deque<std::vector<uint8_t>> joinQ;
        std::vector<JoinStamp> joinStamps;
        size_t joinBytes = 0;
        std::chrono::steady_clock::time_point joinOpenedAt{};
        bool joinWarned = false;  // the open-too-long warning fires once per open
    };
    // Reset q to a fresh state under mu (caller holds mu). Drops the join gate and its queue too: the
    // slot's leave and a stale connection both come through here.
    void ResetLocked_(SlotQ& s);
    // The gate's opening, shared by both opens (caller holds s.mu).
    void OpenLocked_(SlotQ& s);
    // SendOrQueue's body; the caller holds s.mu.
    SendOutcome SendLocked_(SlotQ& s, int slot, int lane, uint32_t hConn, const uint8_t* wire,
                            int len, SendAdmission& adm);

    SlotQ slots_[coop::players::kMaxPeers];
};

}  // namespace coop::net
