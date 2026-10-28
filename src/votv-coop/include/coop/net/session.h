// coop/net/session.h -- the networking application layer: a host listening on a port plus up to
// kMaxPeers-1 clients over GameNetworkingSockets, driven by one net thread (RunCallbacks and the
// receive loop). Remote state is indexed by peerSlot, the players::Registry index (0 = the host).
// Topology-blind past Session::Start: a LAN dial and an ICE rendezvous are driven the same way.
// One owner may be told when a session stops (Session::SetStopListener).

#pragma once

#include "coop/net/apply_pause.h"          // which receive lanes hold the receive loop paused
#include "coop/net/eid_pose_lane.h"        // the trash-carry and driven-prop pose lanes
#include "coop/net/end_reason.h"           // the code a close carries to the peer
#include "coop/net/link_kind.h"            // how a player's traffic reaches the session
#include "coop/net/net_stats.h"            // session traffic accounting (the one counter owner)
#include "coop/net/origin_context.h"       // which occupancy a relayed stream packet belongs to
#include "coop/net/outbound_streams.h"     // what this peer sends on its streams
#include "coop/net/protocol.h"
#include "coop/net/receive_budget.h"       // the receive edge's bound on each client's messages
#include "coop/net/refusal_tally.h"        // the receive edge's refusals, folded
#include "coop/net/remote_streams.h"       // what a receiver keeps of each stream, by owner
#include "coop/net/send_admission.h"       // the send buffer's headroom rule
#include "coop/net/send_backlog.h"         // the reliable-send delivery guarantee
#include "coop/net/send_rate_control.h"    // what each peer's link measures out at
#include "coop/net/session_config.h"      // Role, Topology, Config: what a session starts with
#include "coop/net/stream_refusals.h"      // refused pose packets, said once per streak
#include "coop/player/players_registry.h"  // kMaxPeers (host + 3 clients = 4)

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <thread>

namespace coop::net {

// kMaxPeers (the host + 3 clients) is coop::players::kMaxPeers; the alias sizes this header's
// arrays.
inline constexpr uint8_t kMaxPeers = coop::players::kMaxPeers;

enum class ConnState : uint8_t { Disconnected, Handshaking, Connected };

// Where a client's host link stands, published by the net thread at its own callback edges so the
// join screen can name the stage a joiner is waiting in; the pump reads it each tick. Idle until
// the dial, Admitted once the host's slot assignment finished the link.
enum class LinkStage : uint8_t { Idle, Dialing, FindingRoute, SocketUp, Admitted };

// What the host, or the transport, said when a client's link ended: the code decoded from the
// transport's end reason, and the text that came with it. None until a close is captured.
struct HostClose {
    EndReason   code = EndReason::None;
    std::string text;
};

// SignalingClient (coop/net/signaling_client.h) pulls in the GNS API, so it stays a forward
// declaration here; shared_ptr because per-connection signaling objects co-own it.
class SignalingClient;

class Session {
public:
    // A fixed-size inline payload (no heap allocation per receive). senderPeerSlot is the
    // originating peer's Registry slot (-1 if unknown); drainers route answers by it.
    struct ReliableMessage {
        ReliableKind kind;
        int senderPeerSlot = -1;
        uint16_t payloadLen = 0;
        uint8_t payload[kMaxReliablePayload];
    };

    Session() = default;
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // False when the session did not start; `why` then holds the reason a player can read, which
    // is CouldNotStart with no detail when the failing site names nothing of its own.
    bool Start(const Config& cfg, Refusal* why = nullptr);
    // Returns once the session is stopped: a second caller, on any thread, waits for the first stop
    // to finish rather than returning while it still lingers. Start and Stop share one lock, which no
    // holder keeps while it waits on another thread that would need it.
    void Stop();
    // How many times Stop has stopped a running session, counted as the stop begins, so an end in
    // flight is visible to every thread before its teardown runs (coop/session/net_pump's
    // TearDownSession latches on it). Thread-safe.
    uint64_t StopCount() const { return stopCount_.load(std::memory_order_acquire); }
    // One function, set once at boot, called from inside the stop body the first time it stops a
    // running session -- whoever stops it, on whatever thread. Not called from the destructor, which
    // clears the slot before its own DoStop_(): at process end no other file's statics are reached.
    // A second, different listener is refused with an error line: one owner, by design.
    void SetStopListener(void (*onStopped)());

    bool running() const { return running_.load(); }
    // Aggregate connection state (any peer connected -> Connected).
    ConnState state() const { return state_.load(); }
    // Client: the host link's stage (LinkStage). Any thread.
    LinkStage linkStage() const {
        return static_cast<LinkStage>(linkStage_.load(std::memory_order_acquire));
    }
    bool connected() const { return state_.load() == ConnState::Connected; }
    Role role() const { return cfg_.role; }

    // Game thread: publish the local pose; the net thread fans it out each sendHz tick.
    void SetLocalPose(const PoseSnapshot& pose);
    void SetLocalPropPose(bool set, const PropPoseSnapshot& pose);
    // The local ragdoll's pelvis transform and velocity; `set` true each sendHz frame while
    // ragdolled, false on the recover edge. Sent only while set.
    void SetLocalRagdollPose(bool set, const RagdollPoseSnapshot& pose);
    // The local hand item's view-relative transform; `set` true each tick while holding, false on
    // the hand-empty edge. Sent only while set.
    void SetLocalHandPose(bool set, const HandPoseSnapshot& pose);

    // The local coords-panel cursor; `set` true each pump tick while the desk is claimed and the
    // cursor moved, false on release or still. Sent only while set.
    void SetLocalDeskCursor(bool set, const DeskCursorPoseSnapshot& pose);

    // Host: the NPC pose batch (one EntityPoseSnapshot per live NPC), copied into outbound_.npcBatch
    // with its capacity reused, so neither side allocates per tick; an empty batch clears it. Game
    // thread.
    void SetLocalNpcPoseBatch(const std::vector<EntityPoseSnapshot>& batch);

    // Host: the WorldActor pose batch (the non-Character event actors), the same copy-into shape;
    // an empty batch clears it. Game thread.
    void SetLocalWorldActorPoseBatch(const std::vector<WorldActorPoseSnapshot>& batch);

    // Host: a trash clump's turn, which a publisher asks before reading its pose, and one clump's
    // pose for the next TrashCarryPose send; `ahead` for a clump a player moved. The queue's rules:
    // coop/net/eid_pose_lane.h. Game thread.
    PoseTurn TrashCarryPoseTurn(uint32_t eid, bool ahead) const;
    void PublishTrashCarryPose(const TrashClumpPoseSnapshot& pose, bool ahead);

    // A driven prop's turn and its pose for the next PropDrivePose send (the host's to every peer, a
    // client's to the host). A pose superseded
    // before a send is the only kind that never goes out, which keeps the publisher's delta baseline
    // consistent. Game thread.
    PoseTurn PropDrivePoseTurn(uint32_t eid) const;
    void PublishPropDrivePose(const PropPoseSnapshot& pose);

    // Per-peer reads; false if peerSlot is out of range, the slot has no pose yet, or the session
    // is not Connected. outIsNew tells a fresh arrival from a re-read of the last one.
    bool TryGetRemotePose(int peerSlot, PoseSnapshot& out, bool* outIsNew = nullptr);
    bool TryGetRemotePropPose(int peerSlot, PropPoseSnapshot& out, bool* outIsNew = nullptr);
    // Per-peer ragdoll pelvis physics; apply the velocity only on outIsNew.
    bool TryGetRemoteRagdollPose(int peerSlot, RagdollPoseSnapshot& out, bool* outIsNew = nullptr);
    // Per-peer hand-item transform (hand_item::TickMirrors consumes; newest wins).
    bool TryGetRemoteHandPose(int peerSlot, HandPoseSnapshot& out, bool* outIsNew = nullptr);
    // Per-peer coords-panel cursor (desk_cursor_sync consumes; newest wins).
    bool TryGetRemoteDeskCursor(int peerSlot, DeskCursorPoseSnapshot& out, bool* outIsNew = nullptr);

    // Host: one world-clock sample to send, unreliably, on the next stream round. The clock lane
    // (time_sync) decides when a sample is due; a client call stores nothing that is read. Game thread.
    void SendHostClock(const TimeSyncPayload& clock);
    // Client: the latest host clock; apply only on isNew (the parked mirror holds between
    // arrivals). False until the first ClockPose, which flows from the connect.
    bool TryGetHostClock(TimeSyncPayload& out, bool* outIsNew = nullptr);

    // Host: the download-sim output vector, sent unreliably on a ~100 ms throttle. `set` false
    // clears it. Game thread.
    void SetHostDeskSim(bool set, const DeskSimSnapshot& sim);
    // Client: the latest download-sim vector; apply on isNew, interpolate between. False until the
    // first DeskSimPose.
    bool TryGetHostDeskSim(DeskSimSnapshot& out, bool* outIsNew = nullptr);

    // Host: one dish-pose row batch per publish, sent as one unreliable DishPose datagram (a dirty
    // one-shot; the game-thread sweep owns the cadence).
    void SetHostDishPose(const DishPoseBody& body);
    // Client: the latest dish-pose batch; apply on isNew. Newest wins by header seq.
    bool TryGetHostDishPose(DishPoseBody& out, bool* outIsNew = nullptr);

    // Host: the tape-caddy reel corrector, one unreliable ReelPose datagram per publish.
    void SetHostReelPose(const ReelPosePayload& body);
    // Client: the latest reel corrector plus isNew.
    bool TryGetHostReelPose(ReelPosePayload& out, bool* outIsNew = nullptr);

    // Client: swap out the latest NPC pose batch, consumed once (false when nothing new arrived);
    // `out` keeps its capacity, and a reader that keeps it allocates nothing.
    bool TakeRemoteNpcBatch(std::vector<EntityPoseSnapshot>& out);

    // Client: swap out the latest WorldActor batch, consumed once, the same way.
    bool TakeRemoteWorldActorBatch(std::vector<WorldActorPoseSnapshot>& out);

    // Client: swap out the trash-clump and driven-prop poses received since the last take, newest
    // per eid; `out` comes in empty and keeps its capacity.
    bool TakeRemoteTrashCarryBatch(std::vector<TrashClumpPoseSnapshot>& out);
    bool TakeRemotePropDriveBatch(std::vector<PropPoseSnapshot>& out);

    // Host: each client slot's driven-prop rows since the last take, into out[slot]; the bits of the slots with rows.
    uint32_t TakeRemotePropDriveBatches(std::vector<PropPoseSnapshot> (&out)[kMaxPeers]);

    // Game thread: queue a reliable message to every connected client (host) or to the host
    // (client). False on an oversize payload or no peer. `skipSlot` skips one slot (-1: none): MTA pSkip.
    bool SendReliable(ReliableKind kind, const void* payload, int len, int skipSlot = -1);

    // `count` equal-sized payloads as one run no join gate splits (SendBacklog::SendOrQueueBatch); the blob chunker's.
    bool SendReliableBatch(ReliableKind kind, const void* payloads, int payloadLen, int count);
    bool SendReliableBatchToSlot(int peerSlot, ReliableKind kind, const void* payloads, int payloadLen, int count);

    // Single-target reliable send; false if peerSlot is out of range, not connected, or the payload
    // does not fit. `senderSlot` is the header's origin (0 = self; the late-join replay passes the
    // replayed peer's slot). True means delivered, from the GNS stream or the backlog the net
    // thread drains; false means never. The save stream alone bypasses this through
    // TrySendReliableToSlot.
    bool SendReliableToSlot(int peerSlot, ReliableKind kind, const void* payload,
                            int len, uint8_t senderSlot = 0);

    // One direct GNS attempt, no backlog: false on send-buffer backpressure, which is the
    // save-transfer pump's pacing signal (retry next tick). Its two callers are that pump and the
    // link probe, which must not be queued either -- a probe delayed behind our own backlog would
    // time that queue instead of the link.
    bool TrySendReliableToSlot(int peerSlot, ReliableKind kind, const void* payload,
                               int len, uint8_t senderSlot = 0);

    // The bulk sink: SaveTransferChunk payloads (~56 KB, over the inbox slot) go to this callback
    // on the net thread instead of the inbox. coop/save registers its assembler at install; the
    // sink is thread-safe and never touches the engine.
    using BulkSinkFn = void (*)(const uint8_t* data, int len);
    void SetBulkSink(BulkSinkFn sink) { bulkSink_.store(sink, std::memory_order_release); }

    // SaveTransferBegin goes to the same net-thread lane as the chunks, so Begin is always
    // processed before chunk 0 and chunks with no Begin are rejected outright. The bulk sink's
    // contract.
    void SetSaveBeginSink(BulkSinkFn sink) { saveBeginSink_.store(sink, std::memory_order_release); }

    // Host: until event_feed marks a joining slot world-ready (ClientWorldReady), the send paths
    // drop world-mutating kinds to it (IsPreWorldSendableKind in session_lanes.h); the world-ready
    // connect replay rebuilds that state.
    void MarkSlotWorldReady(int peerSlot, bool ready) {
        if (peerSlot >= 0 && peerSlot < kMaxPeers)
            slotWorldReady_[peerSlot].store(ready, std::memory_order_release);
    }
    bool IsSlotWorldReady(int peerSlot) const {
        return peerSlot >= 0 && peerSlot < kMaxPeers &&
               slotWorldReady_[peerSlot].load(std::memory_order_acquire);
    }
    // The net-thread view of the same event: relay-eligible since the ClientWorldReady receipt on
    // exactly this hConn.
    bool IsRelayEligible(int peerSlot, uint32_t hConn) const;

    // Is a remote connection registered in this slot (any state)?
    bool HasPeerConn(int peerSlot) const {
        return peerSlot >= 0 && peerSlot < kMaxPeers && peerConns_[peerSlot].load() != 0;
    }

    // Does any live peer pass the world-ready gate? A fan-out with zero eligible receivers is a
    // vacuous success for a save-and-seed-covered lane, not a reason to re-broadcast. Game or net
    // thread. `skipSlot` leaves a slot out, as SendReliable's does: one gate for both.
    bool AnyWorldReadyPeer(int skipSlot = -1) const;

    // A receive lane parked at its flood bound pauses this session's receive loop, keyed by the kind it
    // parks (coop/net/apply_pause.h); true when the call changed it. Set on the game thread.
    bool SetApplyPause(ReliableKind kind, bool on) { return applyPause_.Set(kind, on); }
    bool ApplyPausedBy(ReliableKind kind) const { return applyPause_.Has(kind); }
    bool ApplyPaused() const { return applyPause_.Any(); }
    uint64_t ApplyPauseRaises() const { return applyPause_.Raises(); }

    // Game thread: pop one delivered reliable message (a FIFO of arrivals across peers).
    bool TryGetReliable(ReliableMessage& out);

    // Voice: one VoiceFrame datagram (kVoiceFrameHeadBytes + frame.opusLen) to every world-ready
    // peer (a client: to the host). Fire and forget; the jitter buffer and PLC cover a dropped
    // frame. Game thread.
    bool SendVoiceFrame(const VoiceFramePayload& frame);

    // One received voice frame. A per-sender FIFO, not newest-wins: ordering and loss live in the
    // payload's voice seq. senderSlot is the relay-rewritten origin.
    struct VoiceFrameMsg {
        int8_t senderSlot = -1;
        VoiceFramePayload frame{};
    };
    // Ring depth per sender slot (~320 ms of one speaker); public because it sizes the caller's
    // drain buffer.
    static constexpr int kVoiceRingPerSlot = 16;
    // Drain every queued voice frame in one call (one lock per tick); the count written. Game
    // thread.
    int DrainVoiceFrames(VoiceFrameMsg* out, int maxCount);

    bool SendPropRelease(const PropReleasePayload& payload);
    bool SendPropSpawn(const PropSpawnPayload& payload);
    bool SendPropDestroy(const PropDestroyPayload& payload);
    bool SendEntitySpawn(const EntitySpawnPayload& payload, int skipSlot = -1);  // skipSlot: SendReliable's
    bool SendEntityDestroy(uint32_t elementId);

    // Diagnostics; the counters live in net_stats (bytes and packets at the GNS choke points).
    uint64_t packetsSent() const { return net_stats::PacketsSent(); }
    uint64_t packetsRecv() const { return net_stats::PacketsRecv(); }
    // Per-slot RTT in ms (the GNS ping), -1 without a live connection or a sample; sampled ~1 Hz on
    // the net thread. Its consumers: roster_ledger::RefreshLinkFacts (publishes it on RosterRow) and
    // the explosion request lane's judges (a distance bound widened by one round trip's travel).
    int rttMsForSlot(int slot) const {
        return (slot >= 0 && slot < kMaxPeers) ? rttMsBySlot_[slot].load() : -1;
    }
    // How the peer's connection carries its traffic, read off the connection itself (the relay
    // flag, the remote address), never off cfg_.topology. Unknown without a live connection. Any
    // thread, but it takes a GNS lock, so not per tick; roster_ledger::RefreshLinkFacts sets the
    // cadence.
    LinkKind LinkKindForSlot(int peerSlot) const;
    // The number of currently connected peers (0..kMaxPeers-1).
    int connectedPeerCount() const;
    // Sockets in the pending band (accepted, not yet admitted). connectedPeerCount counts seats and
    // excludes them, so "is anyone here" needs both. Any thread; a snapshot.
    int pendingPeerCount() const;
    // True if the slot has a GNS connection handle: the connect/disconnect edge, not "ready for app
    // traffic" (IsSlotReady).
    bool IsSlotConnected(int peerSlot) const {
        if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
        return peerConns_[peerSlot].load() != 0;
    }

    // True only after the Connected callback ran and TuneConnection succeeded.
    // IsSlotConnected flips earlier (Connecting), when a send would still queue on lane 0; the
    // snapshot drain and the connect-edge replay gate on this one.
    bool IsSlotReady(int peerSlot) const {
        if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
        return peerLanesConfigured_[peerSlot].load(std::memory_order_acquire);
    }

    // Every reliable byte sent to this slot has been acknowledged: a kick now drops nothing queued
    // for it. False for an out-of-range slot, a slot with no connection, or a failed read. Game
    // thread. Not const: SendBacklog::DepthBytes is not.
    bool SlotReliableIdle(int slot);

    void OpenJoinGateForSnapshot(int slot);  // a re-snapshot, no announce: the slot's Default-lane sends wait behind it (SendBacklog::OpenJoinGateForSnapshot)
    void ReleaseAnnounceHold(int slot);      // the announce's replay has run: its hold on the gate ends (SendBacklog::ReleaseAnnounceHold)
    void CloseJoinGate(int slot);  // the snapshot completed: the held sends join the Default lane (SendBacklog::CloseJoinGate)

    // What the net thread last published about this slot's link: delivered and still-owed reliable
    // bytes, with the sample's time on the net clock. See SendRateControl::Published. Any thread.
    SendRateControl::Published LinkDeliveryOf(int slot) const { return rateControl_.PublishedFor(slot); }

    // The slot's occupancy generation: a host-minted, never reused, non-zero token naming who holds
    // the slot; 0 = empty. A change between reads is a replacement, which lowest-free slot reuse
    // hides from a connected boolean. Never on the wire; kick and ban validate a captured token
    // against it. Any thread.
    uint32_t peerGenerationForSlot(int peerSlot) const {
        if (peerSlot < 0 || peerSlot >= kMaxPeers) return 0;
        return peerGenBySlot_[peerSlot].load(std::memory_order_acquire);
    }
    // The host's number for a slot's occupancy (its roster row); a CLIENT's roster edge steers relays.
    uint8_t originContextForSlot(int peerSlot) const { return originContext_.Stamp(peerSlot); }
    void OnRosterOrigin(int peerSlot, bool left, uint8_t leftCtx, bool live, uint8_t liveCtx);

    // --- Moderation (host-only admin actions) ---

    // The host's ban predicate over a joiner's PROVED player id and its connection's own address
    // (empty when the path is relayed or GNS knows none), asked right after the identity proof and
    // before a seat is given; true refuses the connection as banned, and whatever it wrote into
    // `whyOut` rides the close. Set once before Start spawns the net thread. MTA: the serial and IP
    // checks in CGame::Packet_PlayerJoinData (CGame.cpp:1956, :1973).
    using BanCheckFn = bool (*)(const char* playerId, const char* address, char* whyOut, int whyLen);
    void SetBanCheck(BanCheckFn fn) { banCheck_ = fn; }

    // Host: disconnect the client at peerSlot with no linger; `code` and `reason` reach the peer's
    // status callback, the code as the transport's application end reason. Runs the slot's
    // teardown itself (GNS gives no callback for a connection we close): on the net thread all of
    // it, from any other thread all but freeing the slot (its generation), which the net thread
    // does at the top of its next pass. False if out of range, slot 0, or not connected.
    // Thread-safe. MTA: CGame::QuitPlayer(QUIT_KICK).
    bool Kick(int peerSlot, EndReason code, const char* reason);

    // Kick only while `peerSlot` is still held by `expectedGeneration`'s owner; otherwise false. A
    // ban modal captures its target seconds before it acts and slots recycle in between, so a bare
    // slot number could ban the successor. The handle CAS makes the claim atomic, because the
    // accept path mints the generation before it stores peerConns_. Thread-safe.
    bool KickWithToken(int peerSlot, uint32_t expectedGeneration, EndReason code,
                       const char* reason);

    // Resolve the remote IP only while `expectedGeneration`'s owner still holds the slot, so the
    // ban list never records a successor's address.
    bool GetPeerAddressWithToken(int peerSlot, uint32_t expectedGeneration,
                                 char* out, int outLen) const;

    // The slot's proved id only while it was proved for `expectedGeneration`'s occupancy; empty
    // otherwise, so a ban never records a successor's id. Any thread.
    std::string ProvedGuidForSlotWithToken(int slot, uint32_t expectedGeneration) const;

  private:
    // Shared teardown for a slot the caller has already claimed (peerConns_ exchanged or CAS'd to
    // 0), in session_teardown.cpp. On the net thread it is the whole teardown, the generation clear
    // included, and marks aggregateDue_. From any other thread it does every step but that clear,
    // which it queues for the net thread's RunPendingFrees; the slot is free only after it.
    bool KickClaimed(int peerSlot, uint32_t hConn, EndReason code, const char* reason);
    // Net thread, first in each loop pass (and Stop, after the join): frees every slot an
    // off-thread kick queued -- re-sweeps the slot's remote state for what an in-flight receive
    // wrote after the kick, then clears its generation last -- and marks aggregateDue_.
    void RunPendingFrees();
    // Net thread, right after RunPendingFrees, once per pass: when a close marked aggregateDue_,
    // and the session has no peer left, state_ goes to Disconnected, linkStage_ to Idle and every
    // slot's RTT to -1. The session's aggregate is written by this function, Start, Stop and the
    // net thread's admission, and by nothing else.
    void UpdateAggregateState();

  public:

    // The remote IP (dotted decimal, no port) into `out` (48 bytes or more); false, `out` empty, if
    // the slot is not connected or GNS has no address yet.
    bool GetPeerAddress(int peerSlot, char* out, int outLen) const;

    // Client: what the host or the transport said when the link ended (the code and its text),
    // taken once and cleared; net_pump shows it on the disconnect edge. Thread-safe.
    HostClose TakeHostCloseReason();

    // The GNS C-callback adapter; public for session.cpp's file-local trampoline.
    static void OnConnStatusChanged(void* info);

private:
    // Topology dispatch from Start(), each branching on the role; false on any failure
    // (session_start.cpp).
    bool StartLanDirect();  // rung 0/1: CreateListenSocketIP / ConnectByIPAddress
    bool StartP2P(bool relayOnly);  // rungs 1-3: signaling + CreateListenSocketP2P / Connect

    void NetThread();
    // Per-peer message dispatch; peerSlot is the sender (from m_nConnUserData on the host, 0 on a
    // client). A pending tag routes to HandlePendingMessage instead. `hConn` is the connection the
    // message arrived on: the reliable inbox takes it only while the slot still holds that handle.
    void HandleMessage(int peerSlot, uint32_t hConn, const void* data, int len);
    // Everything a pending (unadmitted) connection sends. Net thread, from the single drain site.
    void HandlePendingMessage(int pendIdx, uint32_t hConn, const void* data, int len);
    // NPC pose batch (session_npc.cpp): Serialize builds the body after the PacketHeader into `buf`
    // (kNpcPoseDatagramMax or more) and returns the length, 0 if empty (localMutex_); Store
    // newest-wins-stores one received datagram for the game thread (remoteMutex_).
    int  SerializeLocalNpcBatch(uint8_t* buf);
    void StoreRemoteNpcBatch(const void* data, int len, uint32_t seq);
    // WorldActor pose batch (session_worldactor.cpp), the NPC pair's contract; only the host
    // populates the local batch.
    int  SerializeLocalWorldActorBatch(uint8_t* buf);
    void StoreRemoteWorldActorBatch(const void* data, int len, uint32_t seq);
    // Trash-clump carry and driven-prop batches (session_trashcarry.cpp, session_propdrive.cpp):
    // Serialize drains up to a datagram's worth of the waiting poses; Store validates and merges.
    int  SerializeLocalTrashCarryBatch(uint8_t* buf);
    void StoreRemoteTrashCarryBatch(const void* data, int len, uint32_t seq);
    int  SerializeLocalPropDriveBatch(uint8_t* buf);
    void StoreRemotePropDriveBatch(int originSlot, const void* data, int len, uint32_t seq);
    // Voice receive store (session_voice.cpp): validate one VoiceFrame, queue it, host-relay it.
    // Net thread.
    void StoreVoiceFrame(int routeSlot, int peerSlot, const void* data, int len);
    // The scalar stream channels' receive store (session_streams.cpp), after HandleMessage's header
    // parse, epoch latch and routeSlot derivation. Net thread.
    void StoreStreamPacket(MsgType type, int routeSlot, int peerSlot,
                           const void* data, int len, uint32_t seq);
    // The per-sendHz-tick stream fan-out (session_streams.cpp): the scalar channels plus the batch
    // stamps. `now` is the loop's one timestamp; the cadence time_points are NetThread locals
    // advanced here. Net thread.
    void SendStreamsTick(std::chrono::steady_clock::time_point now,
                         std::chrono::milliseconds sendInterval,
                         std::chrono::steady_clock::time_point& nextSend,
                         std::chrono::steady_clock::time_point& nextDeskSimSend,
                         uint64_t& sendFails);
    // One measurement pass over every live connection (net thread, 10 Hz): the send-rate sample and
    // the round-trip probe each slot owes. See coop/net/send_rate_control.h.
    void SampleLinkRates(uint64_t nowMs);
    void HandleConnStatusChanged(void* info);
    // Host: the lowest empty slot in [1..kMaxPeers-1], or -1 when full.
    int FindFreePeerSlotForClient();
    // The peer slot that owns hConn, or -1.
    int FindPeerSlotForConn(uint32_t hConn);
    // Per-peer reset, at the slot's disconnect and at session stop; slot 0's covers the host's own
    // streams too. The caller holds remoteMutex_.
    void ResetPeerRemoteState(int peerSlot);
    void ResetOriginStreams(int peerSlot);  // the relayed streams only; remoteMutex_ held
    // Everything this peer has to send, back to empty under the locks its writers take. Start's, before
    // the net thread spawns: Start can run off the game thread.
    void ResetOutboundStreams();

    // A game-thread read or take of one received stream: nothing unless connected, the slot in
    // range. Takes remoteMutex_.
    template <class T>
    bool ReadOrigin(int slot, StreamSlot<T> OriginStreams::*stream, T& out, bool* isNew) {
        if (!connected() || slot < 0 || slot >= kMaxPeers) return false;
        std::lock_guard<std::mutex> lk(remoteMutex_);
        return (origin_[slot].*stream).Read(out, isNew);
    }
    template <class T>
    bool ReadHost(StreamSlot<T> HostStreams::*stream, T& out, bool* isNew) {
        if (!connected()) return false;
        std::lock_guard<std::mutex> lk(remoteMutex_);
        return (host_.*stream).Read(out, isNew);
    }
    template <class S, class T>
    bool TakeHost(S HostStreams::*stream, T& out) {
        if (!connected()) return false;
        std::lock_guard<std::mutex> lk(remoteMutex_);
        return (host_.*stream).Take(out);
    }
    template <class S, class T>
    uint32_t TakeOrigins(S OriginStreams::*stream, T (&out)[kMaxPeers]) {   // slots 1.., one lock; the slots with data
        uint32_t got = 0; std::lock_guard<std::mutex> lk(remoteMutex_);
        for (int slot = 1; connected() && slot < kMaxPeers; ++slot)
            if ((origin_[slot].*stream).Take(out[slot])) got |= 1u << slot;
        return got;
    }

    // Host relay of an unreliable datagram from `originSlot` to every other client: the header's
    // senderEpoch rewritten to the host's (the receiver's epoch latch is per connection) and
    // senderSlot to `originSlot`; seq and body kept. Net thread, from HandleMessage.
    void RelayUnreliableToOtherClients(int originSlot, const void* data, int len);

    // The same relay on the reliable channel and the kind's lane. The caller has checked the kind
    // is client-relayable; handshake and host-authoritative kinds never relay. Net thread.
    void RelayReliableToOtherClients(int originSlot, ReliableKind kind,
                                     const void* data, int len);

    // The stop body, without the lifecycle mutex. Stop() takes the mutex then calls this; the
    // destructor calls it directly, because at process exit no Start or Stop runs concurrently (the
    // slot is cleared first) and blocking there on a mutex a dying thread may hold would hang the
    // DLL unload. The caller guarantees exclusive access to running_, either way.
    void DoStop_();

    Config cfg_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    // Start's and Stop's lock (Stop above), and the count of the stops of a running session.
    std::mutex lifecycleMutex_;
    std::atomic<uint64_t> stopCount_{0};
    // SetStopListener's one slot; a plain function pointer, the transport knows nothing of its owner.
    std::atomic<void (*)()> stopListener_{nullptr};
    std::atomic<ConnState> state_{ConnState::Disconnected};
    // The net thread's id, stored first thing in NetThread: KickClaimed compares it to tell the net
    // thread (frees the slot inline) from any other (queues the free).
    std::atomic<std::thread::id> netThreadId_{};
    // Slots an off-thread kick claimed and tore down but could not free: the net thread drains
    // them in RunPendingFrees. teardownMutex_ is a leaf lock, held for a push, a pop or a clear.
    struct PendingFree { int slot; };
    std::mutex teardownMutex_;
    std::vector<PendingFree> pendingFrees_;
    // Set by every close (an inline teardown, a ClosedByPeer, RunPendingFrees); UpdateAggregateState
    // consumes it. Cleared by Start.
    std::atomic<bool> aggregateDue_{false};

    // GNS handles as uint32_t, so this header does not include the GNS API.
    std::atomic<uint32_t> hListen_{0};     // host only
    std::atomic<uint32_t> hPollGroup_{0};  // host only; receives all client msgs
    // Per-peer connection handles: on the host [1..kMaxPeers-1] are the admitted clients (assigned
    // by AdmitPending), on a client [0] is the host.
    std::array<std::atomic<uint32_t>, kMaxPeers> peerConns_{};

    // --- Pending (unadmitted) connections ---
    // A connection GNS accepted but that has not yet proved itself holds no player seat: it lives
    // here, its user data `kPendingTag | index`, outside the [1, kMaxPeers) range the drain site
    // validates. So three silent sockets cannot lock the three seats, and every IsSlotReady
    // consumer inherits the gate unchanged.
    static constexpr int64_t  kPendingTag   = 0x7000'0000LL;

public:
    // Public: peer_admission.cpp sizes its per-pending exchange rows to it.
    static constexpr int      kMaxPending   = 8;  // sockets, not seats
    // How long an unproved socket may sit before SweepPending closes it. Generous: a pending entry
    // costs one socket, and on P2P the whole ICE negotiation runs between the host's park edge and
    // the client's Hello.
    static constexpr uint64_t kPendingDeadlineMs = 30'000;

private:
    std::array<std::atomic<uint32_t>, kMaxPending> pendingConns_{};
    // The monotonic ms stamp of each pending entry's accept; 0 = free. A resource bound, not the
    // security control (holding no seat is).
    std::array<std::atomic<uint64_t>, kMaxPending> pendingSinceMs_{};

    // True when a drained message's m_nConnUserData names a pending connection rather than a seat;
    // asked before the drain site's out-of-range drop.
    static bool IsPendingUserData(int64_t ud) {
        return (ud & kPendingTag) == kPendingTag &&
               (ud & ~kPendingTag) >= 0 && (ud & ~kPendingTag) < kMaxPending;
    }
    static int  PendingIndexOf(int64_t ud) { return static_cast<int>(ud & ~kPendingTag); }

public:
    // Park a freshly accepted connection; the pending index. Never fails: a full band evicts its
    // oldest unproved entry (the .cpp says why an arrival outranks it). Net thread.
    int  ParkPending(uint32_t hConn);
    // Promote a pending connection to a seat: the lowest free slot, the user data re-pointed, the
    // generation minted, the lanes configured. -1 if the lobby is full (the caller closes). Net
    // thread.
    int  AdmitPending(int pendingIdx, uint32_t hConn);

private:
    // The lanes, the send-buffer mirror, the lanes-configured flag and (host) the AssignPeerSlot
    // send; called from the client's Connected callback and from AdmitPending.
    void FinishPeerConnected(int slot, uint32_t hConn);
public:
    // Close every pending entry older than kPendingDeadlineMs. Net thread, once per pump pass.
    void SweepPending();

    // One reliable message straight to a connection handle, with no slot, lane or world gate: the
    // admission exchange's only send path (the peer holds no seat yet). Net thread.
    bool SendRawReliableToConn(uint32_t hConn, ReliableKind kind,
                               const void* payload, int len);

    // Client: the host identity this session was told to dial; empty on a host or when nothing
    // advertised one. peer_admission checks the key on the socket against it, so a host cannot pass
    // a challenge it named itself. The one config field the exchange reads.
    const std::string& AdvertisedHostIdentity() const { return cfg_.hostIdentity; }
    // Was the destination named by the local player? (Config::selfAddressed; one consumer.)
    bool DestinationIsSelfAddressed() const { return cfg_.selfAddressed; }

    // The configured lobby password, read by peer_admission on the net thread in both roles; cfg_
    // is written once by Start and never mutated, which makes the unsynchronised read correct.
    const std::string& LobbyPassword() const { return cfg_.lobbyPassword; }

    // Client: the host admitted us (AssignPeerSlot arrived), so finish the link: the lanes, the
    // send-buffer mirror, the ready flag. Not at the Connected edge, so an unproved link never asks
    // for the world. No-op unless `hConn` is still slot 0's. Net thread.
    void FinishClientLink(uint32_t hConn);

    // The guid of the peer in `slot`, hex(SHA-256(pub)[0..16]) of the key it proved at admission;
    // empty if the slot is free or unproved. Not a Join-packet field: a value the peer chooses
    // cannot name whose stored inventory it is.
    // The id is stored WITH the generation it was proved for (ProvedGuidForSlotWithToken reads it
    // back against a captured token); the teardowns clear it with generation 0.
    void        SetProvedGuidForSlot(int slot, uint32_t generation, const std::string& guid);
    std::string ProvedGuidForSlot(int slot) const;

    // Refuse a pending connection: retire the band entry, then close with `code` and `reason`.
    // Retiring first keeps a refusal O(1); a bare CloseConnection would re-log for every message
    // left in the batch. Net thread.
    void RetirePending(int pendIdx, uint32_t hConn, EndReason code, const char* reason);

    // The close of a socket the host ends without ever seating it, for a reason the host decides: a
    // refusal at the proof, the proof deadline, the band's eviction, an accept-edge refusal, a Stop.
    // One line per end, so a reader of the log sees how every join the HOST ended without a seat
    // ended (a socket the client or the transport ends prints its own line or none). Net thread, or
    // Stop once the net thread has joined.
    void EndPendingSocket(uint32_t hConn, EndReason code, const char* reason);

    // Drop a pending entry (its connection closed, or it was refused).
    void ReleasePending(uint32_t hConn);
    // Is this connection already parked? Net thread only.
    bool IsPendingConn(uint32_t hConn) const {
        if (hConn == 0) return false;
        for (int i = 0; i < kMaxPending; ++i)
            if (pendingConns_[i].load(std::memory_order_acquire) == hConn) return true;
        return false;
    }
    // The pending index for a connection, or -1. Net thread only.
    int PendingIndexForConn(uint32_t hConn) const {
        if (hConn == 0) return -1;
        for (int i = 0; i < kMaxPending; ++i)
            if (pendingConns_[i].load(std::memory_order_acquire) == hConn) return i;
        return -1;
    }

private:
    // Set after TuneConnection in the Connected callback; cleared when the slot's handle is
    // zeroed (IsSlotReady).
    std::array<std::atomic<bool>, kMaxPeers> peerLanesConfigured_{};

    // Per-slot proved guid, under its own mutex: remoteMutex_ is taken at 60 Hz by every pose
    // store, and this is written once per admission.
    mutable std::mutex                     provedGuidMutex_;
    struct ProvedId {
        uint32_t    generation = 0;
        std::string guid;
    };
    std::array<ProvedId, kMaxPeers>        provedGuidBySlot_;

    // --- Per-slot occupancy generation ---
    // Minted here because slots recycle: lowest-free reuse can replace person X with Y with no
    // empty moment, which a polled boolean misses. Not ownEpoch_, which the peer declares in its
    // packet header and a rejoiner could repeat. The net layer is the only writer (mint at accept,
    // clear on close).
    std::array<std::atomic<uint32_t>, kMaxPeers> peerGenBySlot_{};
    std::atomic<uint32_t> peerGenCounter_{0};
    // Mint the next non-zero generation. Net thread (the accept paths).
    uint32_t MintPeerGeneration() {
        uint32_t g;
        do { g = peerGenCounter_.fetch_add(1, std::memory_order_relaxed) + 1; } while (g == 0);
        return g;
    }

    // P2P only: the signaling transport carrying the ICE rendezvous blobs. Created in Start()
    // before the net thread spawns, polled on it, reset in Stop() after the thread joins and the
    // connections finish lingering. shared_ptr: per-connection signaling objects co-own it. nullptr
    // for LanDirect.
    std::shared_ptr<SignalingClient> signaling_;

    // What this peer sends on its streams (coop/net/outbound_streams.h): the game thread writes, the
    // net thread's send round copies the samples and serializes the batches, both under localMutex_.
    std::mutex localMutex_;
    OutboundStreams outbound_;
    // The trash-carry queue (host-originated) and the driven-prop queue (the host's rows, or on a client the
    // rows of a prop it syncs, sent to the host alone), each with its own mutex (what a client receives of
    // them is in host_; what the host receives of a client's driven-prop rows is in origin_).
    EidPoseQueue<TrashClumpPoseSnapshot> trashCarryPoses_;
    EidPoseQueue<PropPoseSnapshot>       propDrivePoses_;
    // The batch kinds only the host originates: a host refuses each from a client, said once per
    // kind and per session (the net thread sets, Stop clears).
    enum class HostBatch : uint8_t { Npc, WorldActor, TrashCarry, Count };
    std::array<std::atomic<bool>, static_cast<size_t>(HostBatch::Count)> saidClientBatch_{};
    // HOST: a client sent `kind`, which only a host originates; dropped, said once. Net thread.
    void RefuseClientBatch(HostBatch kind, const char* tag, const char* msgName);

    // What this peer keeps of the streams it receives (coop/net/remote_streams.h): the net thread
    // stores, the game thread reads or takes, both under remoteMutex_. Each origin slot's streams,
    // and on a client the host's own.
    std::mutex remoteMutex_;
    OriginContext originContext_;    // a client's relayed-stream latches, under remoteMutex_
    StreamRefusals streamRefusals_;
    std::array<OriginStreams, kMaxPeers> origin_{};
    HostStreams host_;
    // Per-slot expected senderEpoch, latched from the slot's first packet (0 = not yet); a
    // mismatching packet is dropped at HandleMessage entry. Cleared in ResetPeerRemoteState so the
    // next occupant re-latches. Under remoteMutex_.
    std::array<uint32_t, kMaxPeers> expectedEpoch_{};

    // The reliable inbox (shared across peers; a FIFO of arrival order). The receive path enqueues
    // only while peerConns_[slot] still holds the arrival's handle, read under this mutex; an
    // off-thread kick zeroes that handle before it takes the mutex to erase the slot's entries, so
    // a message is either erased by the kick or never enqueued.
    std::mutex reliableInboxMutex_;
    std::deque<ReliableMessage> reliableInbox_;
    // The high-water of reliableInbox_ since the last ~1 Hz net-diag sample, stamped at the enqueue
    // site (the drain loop would miss a spike drained between iterations). Reported and reset by
    // the net-diag block; net thread only.
    std::atomic<uint32_t> reliableInboxPeak_{0};

    // The voice inbox: per-slot fixed rings (no net-thread allocation, per-sender fairness),
    // drop-oldest on overflow; head/tail are monotonic, tail - head <= kVoiceRingPerSlot.
    struct VoiceSlotRing {
        VoiceFrameMsg ring[kVoiceRingPerSlot];
        uint32_t head = 0, tail = 0;
    };
    std::mutex voiceInboxMutex_;
    VoiceSlotRing voiceRings_[kMaxPeers];

    // The bulk-chunk sink (SetBulkSink): written once at install, read per message on the net
    // thread.
    std::atomic<BulkSinkFn> bulkSink_{nullptr};
    // The SaveTransferBegin sink (SetSaveBeginSink), the same lifetime.
    std::atomic<BulkSinkFn> saveBeginSink_{nullptr};

    // Per-slot world-ready flags (see MarkSlotWorldReady).
    std::array<std::atomic<bool>, kMaxPeers> slotWorldReady_{};
    // The receive lanes holding the receive loop paused (see SetApplyPause); reset by Start.
    ApplyPauseSet applyPause_;
    // The receive edge's refusals of a kind no client may author (client_authorship.h): the first of each value
    // in a session said with its number, the rest summed. Net thread; reset by the stop, after the join.
    std::array<bool, 256> saidHostKind_{};
    RefusalTally hostKindRefusals_{"net"};
    // The epoch latch's drops (HandleMessage), each cause folded on its own: a zero epoch is a sender's bug, an
    // earlier connection's epoch a benign reconnect race. Net thread; cleared with the one above.
    RefusalTally zeroEpochRefusals_{"net"};
    RefusalTally staleEpochRefusals_{"net"};
    // The receive budget's stream drops, folded the same way. Net thread; cleared with the two above.
    RefusalTally streamRateRefusals_{"net"};
    // The host's bound on what each client sends, every packet (receive_budget.h). Net thread only.
    ReceiveBudget receiveBudget_;
    // The receive budget's answer to one packet (receive_budget.h): true to go on. A stream past its rate is dropped;
    // a client that empties its message bucket is recorded in the flood history and its link ended. Net thread.
    bool TakeBudget(int peerSlot, uint32_t hConn, MsgType type, const void* data, int len);
    // The flood history's record of a slot's occupant before its link is ended for flooding: its proved identity and
    // its address where the transport has one, refused from now (connect_history::Floods). Net thread.
    void RecordFlood(int slot, uint32_t hConn);
    // Per-slot relay eligibility: the hConn that announced ClientWorldReady, stamped on the net
    // thread at receipt. The game-thread world-ready flag lags by one drain, and the relay consults
    // this so a reliable received in that gap is neither lost nor doubled; hConn-stamped so a
    // recycled slot never inherits it.
    std::array<std::atomic<uint32_t>, kMaxPeers> relayEligible_{};

    std::atomic<uint32_t> sendSeq_{0};

    // Per-(slot, lane) send backlogs (send_backlog.h), drained each net-thread pass and freed
    // wherever peerConns_ is zeroed.
    SendBacklog backlog_;
    // The headroom rule every reliable path obeys, and the send-buffer size it measures against:
    // re-anchored per slot at each link sample, fed by every byte handed to a connection.
    SendAdmission admission_;
    // The link measurement: goodput per slot from our own queued-byte count against GNS's pending
    // totals, and a round trip timed with the LinkProbe pair.
    SendRateControl rateControl_;
    // The fatal-backlog close: Kick(slot) on the host; on a client, claim plus the KickClaimed
    // teardown of the host connection. Net thread.
    void FatalCloseSlot(int slot, const char* reason);
    // The client's only way to end its host link: records `code` and `why`, claims slot 0 and runs
    // the KickClaimed teardown, which marks aggregateDue_; UpdateAggregateState, at the top of the
    // next pass, drives state_ to Disconnected, the edge net_pump needs to show the player a
    // reason. Net thread.
    void LeaveHost(EndReason code, const char* why);
    // Per-slot RTT in ms from GNS's m_nPing, sampled ~1 Hz on the net thread; -1 without a live
    // connection. event_feed fans it to each puppet.
    std::array<std::atomic<int>, kMaxPeers> rttMsBySlot_{};

    // The host's ban predicate (SetBanCheck); nullptr = nobody is banned. Set before Start spawns
    // the net thread.
    BanCheckFn banCheck_ = nullptr;

    // Client: what the host or the transport said when the link ended (the end reason decoded, and
    // its text); written on the net thread, taken on the game thread by TakeHostCloseReason. Its
    // own small mutex: a rare path with no lock order against the hot mutexes.
    std::mutex hostCloseMutex_;
    HostClose  hostClose_;
    // The host link's stage for the join screen (LinkStage); written at the callback edges.
    std::atomic<uint8_t> linkStage_{0};

    // This peer's per-process session epoch, minted non-zero at Start() and stamped on every
    // outbound header (senderEpoch). Read on the net thread without a lock: set before the thread
    // spawns, never changed.
    uint32_t ownEpoch_ = 0;
};

}  // namespace coop::net
