// coop/net/session.h -- the networking application layer: a host listening on a port plus up to
// kMaxPeers-1 clients over GameNetworkingSockets, driven by one net thread (RunCallbacks and the
// receive loop). Remote state is indexed by peerSlot, the players::Registry index (0 = the host).
// Topology-blind past Session::Start: a LAN dial and an ICE rendezvous are driven the same way.

#pragma once

#include "coop/net/link_kind.h"            // how a player's traffic reaches the session
#include "coop/net/net_stats.h"            // session traffic accounting (the one counter owner)
#include "coop/net/protocol.h"
#include "coop/net/send_backlog.h"         // the reliable-send delivery guarantee
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

enum class Role : uint8_t { Host, Client };

enum class ConnState : uint8_t { Disconnected, Handshaking, Connected };

// Transport topology; only Session::Start branches on it.
enum class Topology : uint8_t {
    LanDirect,  // CreateListenSocketIP / ConnectByIPAddress: a port-forwarded host or
                // one LAN; no signaling.
    P2P,        // CreateListenSocketP2P / ConnectP2PCustomSignaling + ICE through the
                // signaling server: a STUN hole-punch or a TURN relay.
};

struct Config {
    Role role = Role::Host;
    Topology topology = Topology::LanDirect;

    // --- LanDirect ---
    std::string peerIp = "127.0.0.1";   // client: the host's address
    uint16_t port = kDefaultPort;

    // --- P2P ---
    // signalingUrl: "host:port" of the signaling server; both peers connect outbound.
    std::string signalingUrl;
    // The bearer token the signaling server requires in the greeting; a client gets it with the
    // lobby's master entry, the host from its config.
    std::string signalingToken;
    // The host identity the client dials (`gen:<64 hex>`), as the host published it to the master;
    // it must equal the host's peer_identity::LocalIdentityString(). Client-only. This peer's own
    // identity is not a config field: peer_identity::InstallInto sets it process-wide before any
    // socket exists.
    std::string hostIdentity;
    // The lobby password: on a host the secret required (empty = open), on a client the one to
    // present. Never on the wire or in a log; a tag derived from it travels
    // (coop/net/lobby_password.h), and only to a host the client has bound to the identity it was
    // sent to.
    std::string lobbyPassword;
    // ICE candidate sources: STUN (hole-punch) and TURN (a coturn relay with short-lived REST
    // credentials); an empty string disables that path.
    std::string stunList;    // "host:port,host2:port"
    std::string turnList;    // "turn:host:port,..."
    std::string turnUser;    // parallel to turnList
    std::string turnPass;    // parallel to turnList
    // ICE policy: "" or "all" (host, reflexive and relay candidates), "relay" (TURN only),
    // "disable", "default" (GNS's). Mapped to IceEnable in StartP2P.
    std::string iceMode;

    // True when the destination was named locally (a typed address, an ini, an autotest) rather
    // than advertised by the network. peer_admission tells "the player chose this address" from
    // "nobody advertised an identity" by it instead of inferring one from the other.
    bool selfAddressed = false;

    int sendHz = 60;
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

    bool Start(const Config& cfg);
    void Stop();

    bool running() const { return running_.load(); }
    // Aggregate connection state (any peer connected -> Connected).
    ConnState state() const { return state_.load(); }
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

    // Host: the NPC pose batch (one EntityPoseSnapshot per live NPC), copied into localNpcBatch_
    // with its capacity reused, so neither side allocates per tick; an empty batch clears it. Game
    // thread.
    void SetLocalNpcPoseBatch(const std::vector<EntityPoseSnapshot>& batch);

    // Host: the WorldActor pose batch (the non-Character event actors), the same copy-into shape;
    // an empty batch clears it. Game thread.
    void SetLocalWorldActorPoseBatch(const std::vector<WorldActorPoseSnapshot>& batch);

    // Host: the carried-trash-clump batch (host-driven, client-grabbed clumps in carry and in
    // flight); an empty batch clears it. Game thread.
    void SetLocalTrashCarryBatch(const std::vector<TrashClumpPoseSnapshot>& batch);

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

    // Host: the world clock, published each tick (time_sync::Tick) and sent unreliably on a ~500 ms
    // throttle of its own. `set` false clears it; a client call stores nothing that is read. Game
    // thread.
    void SetHostClock(bool set, const TimeSyncPayload& clock);
    // Client: the latest host clock; apply only on isNew (the frozen mirror holds between
    // arrivals). False until the first ClockPose; the connect-edge TimeSync seeds the value before
    // then.
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

    // Client: move out the latest NPC pose batch, consumed once (false when nothing new arrived).
    bool TakeRemoteNpcBatch(std::vector<EntityPoseSnapshot>& out);

    // Client: move out the latest WorldActor batch, consumed once.
    bool TakeRemoteWorldActorBatch(std::vector<WorldActorPoseSnapshot>& out);

    // Client: move out the latest trash-clump carry batch, consumed once.
    bool TakeRemoteTrashCarryBatch(std::vector<TrashClumpPoseSnapshot>& out);

    // Game thread: queue a reliable message to every connected client (host) or to the host
    // (client). False on an oversize payload or no peer.
    bool SendReliable(ReliableKind kind, const void* payload, int len);

    // Single-target reliable send; false if peerSlot is out of range, not connected, or the payload
    // does not fit. `senderSlot` is the header's origin (0 = self; the late-join replay passes the
    // replayed peer's slot). True means delivered, from the GNS stream or the backlog the net
    // thread drains; false means never. The save stream alone bypasses this through
    // TrySendReliableToSlot.
    bool SendReliableToSlot(int peerSlot, ReliableKind kind, const void* payload,
                            int len, uint8_t senderSlot = 0);

    // One direct GNS attempt, no backlog: false on send-buffer backpressure, which is the
    // save-transfer pump's pacing signal (retry next tick). That pump is its only intended caller.
    bool TrySendReliableToSlot(int peerSlot, ReliableKind kind, const void* payload,
                               int len, uint8_t senderSlot = 0);

    // The bulk sink: SaveTransferChunk payloads (~56 KB, over the inbox slot) go to this callback
    // on the net thread instead of the inbox. coop/save registers its assembler at install; the
    // sink is thread-safe and never touches the engine.
    using BulkSinkFn = void (*)(int senderPeerSlot, const uint8_t* data, int len);
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
    bool IsRelayEligible(int peerSlot, uint32_t hConn) const {
        return peerSlot >= 0 && peerSlot < kMaxPeers && hConn != 0 &&
               relayEligible_[peerSlot].load(std::memory_order_acquire) == hConn;
    }

    // Is a remote connection registered in this slot (any state)?
    bool HasPeerConn(int peerSlot) const {
        return peerSlot >= 0 && peerSlot < kMaxPeers && peerConns_[peerSlot].load() != 0;
    }

    // Does any live peer pass the world-ready gate? A fan-out with zero eligible receivers is a
    // vacuous success for a save-and-seed-covered lane, not a reason to re-broadcast. Game or net
    // thread.
    bool AnyWorldReadyPeer() const {
        for (int i = 0; i < kMaxPeers; ++i)
            if (peerConns_[i].load() != 0 &&
                slotWorldReady_[i].load(std::memory_order_acquire)) return true;
        return false;
    }

    // A receive lane parked at its flood bound pauses the client's inbox drain (pause, not drop).
    // Refcounted: each lane contributes +1/-1 on its own edge, so one lane draining cannot cancel
    // another's standing escalation.
    void SetApplyBackpressure(bool on) {
        applyBackpressureCount_.fetch_add(on ? 1 : -1, std::memory_order_acq_rel);
    }

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

    bool SendPropRelease(const WireKey& key,
                         float linVelX, float linVelY, float linVelZ,
                         float angVelX, float angVelY, float angVelZ,
                         uint32_t elementId = 0,  // trash-entity eid (0 = a keyed Aprop, routed by key)
                         uint8_t ctx = 0);        // trash-entity sync-time context (0 = not trash, no enforcement)
    bool SendPropSpawn(const PropSpawnPayload& payload);
    bool SendPropDestroy(const PropDestroyPayload& payload);
    bool SendEntitySpawn(const EntitySpawnPayload& payload);
    bool SendEntityDestroy(uint32_t elementId);

    // Diagnostics; the counters live in net_stats (bytes and packets at the GNS choke points).
    uint64_t packetsSent() const { return net_stats::PacketsSent(); }
    uint64_t packetsRecv() const { return net_stats::PacketsRecv(); }
    // Per-slot RTT in ms (the GNS ping), -1 without a live connection or a sample; sampled ~1 Hz on
    // the net thread. Its one consumer, roster_ledger::RefreshLinkFacts, publishes the host's
    // measurement on RosterRow.
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

    // True only after the Connected callback ran and ConfigureLanesForPeer succeeded.
    // IsSlotConnected flips earlier (Connecting), when a send would still queue on lane 0; the
    // snapshot drain and the connect-edge replay gate on this one.
    bool IsSlotReady(int peerSlot) const {
        if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
        return peerLanesConfigured_[peerSlot].load(std::memory_order_acquire);
    }

    // The slot's occupancy generation: a host-minted, never reused, non-zero token naming who holds
    // the slot; 0 = empty. A change between reads is a replacement, which lowest-free slot reuse
    // hides from a connected boolean. Never on the wire; kick and ban validate a captured token
    // against it. Any thread.
    uint32_t peerGenerationForSlot(int peerSlot) const {
        if (peerSlot < 0 || peerSlot >= kMaxPeers) return 0;
        return peerGenBySlot_[peerSlot].load(std::memory_order_acquire);
    }

    // --- Moderation (host-only admin actions) ---

    // The host's accept predicate over an incoming connection's remote IP (dotted decimal); false
    // closes it with a "banned" reason. Set once before Start spawns the net thread (the harness
    // wires coop::ban_list::IsBanned). MTA: the join-time ban check in
    // CGame::Packet_PlayerJoinData.
    using AcceptFilterFn = bool (*)(const char* remoteIp);
    void SetAcceptFilter(AcceptFilterFn fn) { acceptFilter_ = fn; }

    // Host: disconnect the client at peerSlot with no linger; `reason` reaches the peer's status
    // callback. Runs the ClosedByPeer teardown itself (GNS gives no callback for a connection we
    // close). False if out of range, slot 0, or not connected. Thread-safe. MTA:
    // CGame::QuitPlayer(QUIT_KICK).
    bool Kick(int peerSlot, const char* reason);

    // Kick only while `peerSlot` is still held by `expectedGeneration`'s owner; otherwise false. A
    // ban modal captures its target seconds before it acts and slots recycle in between, so a bare
    // slot number could ban the successor. The handle CAS makes the claim atomic, because the
    // accept path mints the generation before it stores peerConns_. Thread-safe.
    bool KickWithToken(int peerSlot, uint32_t expectedGeneration, const char* reason);

    // Resolve the remote IP only while `expectedGeneration`'s owner still holds the slot, so the
    // ban list never records a successor's address.
    bool GetPeerAddressWithToken(int peerSlot, uint32_t expectedGeneration,
                                 char* out, int outLen) const;

  private:
    // Shared teardown for a slot the caller has already claimed (peerConns_ exchanged or CAS'd to
    // 0).
    bool KickClaimed(int peerSlot, uint32_t hConn, const char* reason);

  public:

    // The remote IP (dotted decimal, no port) into `out` (48 bytes or more); false, `out` empty, if
    // the slot is not connected or GNS has no address yet.
    bool GetPeerAddress(int peerSlot, char* out, int outLen) const;

    // Client: the reason the host passed to CloseConnection ("kicked by host", ...), taken once and
    // cleared; net_pump logs it on the disconnect edge. Thread-safe.
    std::string TakeHostCloseReason();

    // The GNS C-callback adapter; public for session.cpp's file-local trampoline.
    static void OnConnStatusChanged(void* info);

private:
    // Topology dispatch from Start(), each branching on the role; false on any failure
    // (session_start.cpp).
    bool StartLanDirect();  // rung 0/1: CreateListenSocketIP / ConnectByIPAddress
    bool StartP2P();        // rungs 1-3: signaling + CreateListenSocketP2P / Connect

    void NetThread();
    // Per-peer message dispatch; peerSlot is the sender (from m_nConnUserData on the host, 0 on a
    // client). A pending tag routes to HandlePendingMessage instead.
    void HandleMessage(int peerSlot, const void* data, int len);
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
    // Trash-clump carry batch (session_trashcarry.cpp), the same contract.
    int  SerializeLocalTrashCarryBatch(uint8_t* buf);
    void StoreRemoteTrashCarryBatch(const void* data, int len, uint32_t seq);
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
                         std::chrono::steady_clock::time_point& nextClockSend,
                         std::chrono::steady_clock::time_point& nextDeskSimSend,
                         uint64_t& sendFails);
    void HandleConnStatusChanged(void* info);
    // Host: the lowest empty slot in [1..kMaxPeers-1], or -1 when full.
    int FindFreePeerSlotForClient();
    // The peer slot that owns hConn, or -1.
    int FindPeerSlotForConn(uint32_t hConn);
    // Per-peer reset on slot disconnect. The caller holds remoteMutex_.
    void ResetPeerRemoteState(int peerSlot);

    // Host relay of an unreliable datagram from `originSlot` to every other client: the header's
    // senderEpoch rewritten to the host's (the receiver's epoch latch is per connection) and
    // senderSlot to `originSlot`; seq and body kept. Net thread, from HandleMessage.
    void RelayUnreliableToOtherClients(int originSlot, const void* data, int len);

    // The same relay on the reliable channel and the kind's lane. The caller has checked the kind
    // is client-relayable; handshake and host-authoritative kinds never relay. Net thread.
    void RelayReliableToOtherClients(int originSlot, ReliableKind kind,
                                     const void* data, int len);

    Config cfg_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<ConnState> state_{ConnState::Disconnected};

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
    // Park a freshly accepted connection; the pending index, or -1 when the band is full (the
    // caller closes). Net thread.
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
    void        SetProvedGuidForSlot(int slot, const std::string& guid);
    std::string ProvedGuidForSlot(int slot) const;

    // Refuse a pending connection: retire the band entry, then close with `reason`. Retiring first
    // keeps a refusal O(1); a bare CloseConnection would re-log for every message left in the
    // batch. Net thread.
    void RetirePending(int pendIdx, uint32_t hConn, const char* reason);

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
    // Set after ConfigureLanesForPeer in the Connected callback; cleared when the slot's handle is
    // zeroed (IsSlotReady).
    std::array<std::atomic<bool>, kMaxPeers> peerLanesConfigured_{};

    // Per-slot proved guid, under its own mutex: remoteMutex_ is taken at 60 Hz by every pose
    // store, and this is written once per admission.
    mutable std::mutex                     provedGuidMutex_;
    std::array<std::string, kMaxPeers>     provedGuidBySlot_;

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

    // The local pose slot (the game thread writes, the net thread reads and fans out).
    std::mutex localMutex_;
    PoseSnapshot localPose_{};
    // When localPose_ was sampled, stamped into the header instead of "now": a game-thread hitch
    // would otherwise pair an old position with a fresh stamp.
    uint32_t     localPoseStateMs_ = 0;
    bool hasLocal_ = false;
    PropPoseSnapshot localPropPose_{};
    bool hasLocalProp_ = false;
    uint32_t lastLocalPropSeq_ = 0;
    // Local ragdoll pelvis physics, localPropPose_'s held/release shape.
    RagdollPoseSnapshot localRagdollPose_{};
    bool hasLocalRagdoll_ = false;
    // Local hand-item transform, the same held/release shape.
    HandPoseSnapshot localHandPose_{};
    bool hasLocalHand_ = false;
    // Local coords-panel cursor, the same held/release shape.
    DeskCursorPoseSnapshot localDeskCursor_{};
    bool hasLocalDeskCursor_ = false;
    // Host NPC pose batch (SetLocalNpcPoseBatch); empty = nothing to send.
    std::vector<EntityPoseSnapshot> localNpcBatch_;
    bool hasLocalNpcBatch_ = false;
    // Host WorldActor pose batch (SetLocalWorldActorPoseBatch); empty = nothing to send.
    std::vector<WorldActorPoseSnapshot> localWorldActorBatch_;
    bool hasLocalWorldActorBatch_ = false;
    // Host carried-trash-clump batch (SetLocalTrashCarryBatch); empty = none.
    std::vector<TrashClumpPoseSnapshot> localTrashCarryBatch_;
    bool hasLocalTrashCarryBatch_ = false;
    // Host world clock (SetHostClock), fanned out on its own ~500 ms throttle.
    TimeSyncPayload localHostClock_{};
    bool hasLocalHostClock_ = false;
    // Host download-sim vector (SetHostDeskSim), fanned out on its own ~100 ms throttle.
    DeskSimSnapshot localDeskSim_{};
    bool hasLocalDeskSim_ = false;
    // Host dish-pose batch (SetHostDishPose), one datagram per publish.
    DishPoseBody localDishPose_{};
    bool dishPoseDirty_ = false;
    // Host reel corrector (SetHostReelPose), one datagram per publish.
    ReelPosePayload localReelPose_{};
    bool reelPoseDirty_ = false;

    // Per-peer remote pose slots: the net thread writes under remoteMutex_, the game thread reads
    // through TryGetRemotePose.
    std::mutex remoteMutex_;
    std::array<PoseSnapshot, kMaxPeers> remotePoses_{};
    std::array<bool, kMaxPeers> hasRemote_{};
    std::array<uint32_t, kMaxPeers> lastRemoteSeq_{};
    std::array<uint64_t, kMaxPeers> remoteStamp_{};
    std::array<uint64_t, kMaxPeers> lastReadStamp_{};
    std::array<PropPoseSnapshot, kMaxPeers> remotePropPoses_{};
    std::array<bool, kMaxPeers> hasRemoteProp_{};
    std::array<uint32_t, kMaxPeers> lastRemotePropSeq_{};
    std::array<uint64_t, kMaxPeers> remotePropStamp_{};
    std::array<uint64_t, kMaxPeers> lastReadPropStamp_{};
    // Per-peer ragdoll pelvis physics (the prop's per-slot stamp/seq shape).
    std::array<RagdollPoseSnapshot, kMaxPeers> remoteRagdollPoses_{};
    std::array<bool, kMaxPeers> hasRemoteRagdoll_{};
    std::array<uint32_t, kMaxPeers> lastRemoteRagdollSeq_{};
    std::array<uint64_t, kMaxPeers> remoteRagdollStamp_{};
    std::array<uint64_t, kMaxPeers> lastReadRagdollStamp_{};
    // Per-peer hand-item transform (the same per-slot shape).
    std::array<HandPoseSnapshot, kMaxPeers> remoteHandPoses_{};
    std::array<bool, kMaxPeers> hasRemoteHand_{};
    std::array<uint32_t, kMaxPeers> lastRemoteHandSeq_{};
    std::array<uint64_t, kMaxPeers> remoteHandStamp_{};
    std::array<uint64_t, kMaxPeers> lastReadHandStamp_{};
    // Per-peer coords-panel cursor (the same per-slot shape).
    std::array<DeskCursorPoseSnapshot, kMaxPeers> remoteDeskCursors_{};
    std::array<bool, kMaxPeers> hasRemoteDeskCursor_{};
    std::array<uint32_t, kMaxPeers> lastRemoteDeskCursorSeq_{};
    std::array<uint64_t, kMaxPeers> remoteDeskCursorStamp_{};
    std::array<uint64_t, kMaxPeers> lastReadDeskCursorStamp_{};
    // The latest host clock (one slot: the host is the only sender); drained by TryGetHostClock,
    // newest wins by seq.
    TimeSyncPayload remoteHostClock_{};
    bool hasRemoteHostClock_ = false;
    uint32_t lastRemoteHostClockSeq_ = 0;
    uint64_t remoteHostClockStamp_ = 0;
    uint64_t lastReadHostClockStamp_ = 0;
    // The latest host download-sim vector; drained by TryGetHostDeskSim.
    DeskSimSnapshot remoteDeskSim_{};
    bool hasRemoteDeskSim_ = false;
    uint32_t lastRemoteDeskSimSeq_ = 0;
    uint64_t remoteDeskSimStamp_ = 0;
    uint64_t lastReadDeskSimStamp_ = 0;
    // The latest dish-pose batch; drained by TryGetHostDishPose.
    DishPoseBody remoteDishPose_{};
    bool hasRemoteDishPose_ = false;
    uint32_t lastRemoteDishPoseSeq_ = 0;
    uint64_t remoteDishPoseStamp_ = 0;
    uint64_t lastReadDishPoseStamp_ = 0;
    // The latest reel corrector; drained by TryGetHostReelPose.
    ReelPosePayload remoteReelPose_{};
    bool hasRemoteReelPose_ = false;
    uint32_t lastRemoteReelPoseSeq_ = 0;
    uint64_t remoteReelPoseStamp_ = 0;
    uint64_t lastReadReelPoseStamp_ = 0;
    // The latest NPC pose batch; drained by TakeRemoteNpcBatch.
    std::vector<EntityPoseSnapshot> remoteNpcBatch_;
    bool     hasRemoteNpcBatch_ = false;
    uint32_t lastRemoteNpcSeq_  = 0;
    // The latest WorldActor batch; drained by TakeRemoteWorldActorBatch.
    std::vector<WorldActorPoseSnapshot> remoteWorldActorBatch_;
    bool     hasRemoteWorldActorBatch_ = false;
    uint32_t lastRemoteWorldActorSeq_  = 0;
    // The latest trash-clump carry batch; drained by TakeRemoteTrashCarryBatch.
    std::vector<TrashClumpPoseSnapshot> remoteTrashCarryBatch_;
    bool     hasRemoteTrashCarryBatch_ = false;
    uint32_t lastRemoteTrashCarrySeq_  = 0;
    // Per-slot expected senderEpoch, latched from the slot's first packet (0 = not yet); a
    // mismatching packet is dropped at HandleMessage entry. Cleared in ResetPeerRemoteState so the
    // next occupant re-latches. Under remoteMutex_.
    std::array<uint32_t, kMaxPeers> expectedEpoch_{};

    // The reliable inbox (shared across peers; a FIFO of arrival order).
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
    // The receive-side apply-backpressure refcount (see SetApplyBackpressure).
    std::atomic<int> applyBackpressureCount_{0};
    // Per-slot relay eligibility: the hConn that announced ClientWorldReady, stamped on the net
    // thread at receipt. The game-thread world-ready flag lags by one drain, and the relay consults
    // this so a reliable received in that gap is neither lost nor doubled; hConn-stamped so a
    // recycled slot never inherits it.
    std::array<std::atomic<uint32_t>, kMaxPeers> relayEligible_{};

    std::atomic<uint32_t> sendSeq_{0};

    // Per-(slot, lane) send backlogs (send_backlog.h), drained each net-thread pass and freed
    // wherever peerConns_ is zeroed. sendBufBytes_ mirrors the configured per-connection
    // SendBufferSize; the drain's reserve gate is computed against it.
    SendBacklog backlog_;
    int sendBufBytes_ = 512 * 1024;
    // The fatal-backlog close: Kick(slot) on the host; on a client, claim plus the KickClaimed
    // teardown of the host connection. Net thread.
    void FatalCloseSlot(int slot, const char* reason);
    // The client's only way to end its host link: records `why`, claims slot 0 and runs the
    // KickClaimed teardown, which drives state_ to Disconnected, the edge net_pump needs to show
    // the player a reason. Net thread.
    void LeaveHost(const char* why);
    // Per-slot RTT in ms from GNS's m_nPing, sampled ~1 Hz on the net thread; -1 without a live
    // connection. event_feed fans it to each puppet.
    std::array<std::atomic<int>, kMaxPeers> rttMsBySlot_{};

    // The host's accept predicate (the ban filter); nullptr = accept all. Set before Start spawns
    // the net thread.
    AcceptFilterFn acceptFilter_ = nullptr;

    // Client: the reason GNS reported when the host closed the connection (m_szEndDebug); written
    // on the net thread, taken on the game thread by TakeHostCloseReason. Its own small mutex: a
    // rare path with no lock order against the hot mutexes.
    std::mutex  hostCloseMutex_;
    std::string hostCloseReason_;

    // This peer's per-process session epoch, minted non-zero at Start() and stamped on every
    // outbound header (senderEpoch). Read on the net thread without a lock: set before the thread
    // spawns, never changed.
    uint32_t ownEpoch_ = 0;
};

}  // namespace coop::net
