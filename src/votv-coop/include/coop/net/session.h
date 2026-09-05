// coop/net/session.h -- the networking application layer.
//
// A session is a host listening on a port plus zero to kMaxPeers-1 clients connected to it.
// Session owns the GameNetworkingSockets connections and runs a dedicated net thread that drives
// RunCallbacks and the receive loop (ReceiveMessagesOnPollGroup on the host,
// ReceiveMessagesOnConnection on a client). The game thread and the net thread meet at the pose
// slots and the reliable inbox.
//
// Remote state is indexed by `peerSlot`, the coop::players::Registry index (slot 0 = the host,
// slots 1..kMaxPeers-1 = clients). On the host slot 0 is unused for remote state (it is local) and
// slots 1..3 hold the clients; on a client slot 0 holds the host and the rest are unused (clients
// talk only to the host, which relays).
//
// Every path below is topology-blind except Session::Start: once an HSteamNetConnection exists it
// is driven identically whether it came from a LAN dial or an ICE-signaled P2P rendezvous.

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

// kMaxPeers (the host + 3 clients = 4) is defined once, in coop::players::kMaxPeers. The alias
// lets this header's std::array<T, kMaxPeers> sizes resolve without the coop::players:: prefix at
// every use, and a bump there cascades here.
inline constexpr uint8_t kMaxPeers = coop::players::kMaxPeers;

enum class Role : uint8_t { Host, Client };

enum class ConnState : uint8_t { Disconnected, Handshaking, Connected };

// Transport topology. The whole session is topology-blind except Session::Start (see the header
// comment): every HSteamNetConnection, once it exists, is driven identically however it was
// established.
enum class Topology : uint8_t {
    LanDirect,  // rung 0/1: CreateListenSocketIP / ConnectByIPAddress. The host
                // port-forwards (rung 0) or both peers share one LAN (rung 1). The
                // no-signaling path.
    P2P,        // rungs 1-3: CreateListenSocketP2P / ConnectP2PCustomSignaling +
                // ICE. Both peers connect outbound to a signaling server (no host
                // port-forward); ICE then hole-punches a direct path (rung 2, STUN)
                // or relays through TURN (rung 3, coturn).
};

struct Config {
    Role role = Role::Host;
    Topology topology = Topology::LanDirect;

    // --- LanDirect (rung 0/1) -------------------------------------------------
    std::string peerIp = "127.0.0.1";   // client: the host's address
    uint16_t port = kDefaultPort;

    // --- P2P (rungs 1-3) ------------------------------------------------------
    // Signaling rendezvous: "host:port" of the signaling server (the VPS, or a local test server).
    // Both peers connect outbound; no host port-forward.
    std::string signalingUrl;
    // The shared bearer token the signaling server requires in the greeting (it keeps the open
    // internet out of the rendezvous channel). A client gets it with the lobby's master-server
    // entry; the host's own comes from its config.
    std::string signalingToken;
    // This peer's own signaling identity is its durable public key: one value, installed
    // process-wide by peer_identity::InstallInto before any socket exists and read back with
    // peer_identity::LocalIdentityString(). It is not a config field, so nothing can overwrite it.
    //
    // The host's identity the client dials, as the host published it to the master
    // (`gen:<64 hex>`). Client-only. It must equal the host's peer_identity::LocalIdentityString(),
    // a fact about the host's key rather than a convention two config files have to agree on.
    std::string hostIdentity;
    // The lobby password, the same field in both roles: on a host the secret this session requires
    // (empty = open), on a client the one the player was given for the session they are joining.
    // One field because it is one value seen from two ends.
    //
    // It never reaches the wire or a log. What travels is a tag derived from it
    // (`coop/net/lobby_password.h`), and only to a host the client has already bound to the
    // identity it was sent to.
    std::string lobbyPassword;
    // ICE candidate sources. stunList = rung 2 (hole-punch); turn* = rung 3 (a coturn relay with
    // short-lived REST credentials). An empty string disables that rung.
    std::string stunList;    // "host:port,host2:port"
    std::string turnList;    // "turn:host:port,..."
    std::string turnUser;    // parallel to turnList
    std::string turnPass;    // parallel to turnList
    // ICE candidate policy: "" / "all" (default: share host, reflexive and relay candidates),
    // "relay" (force the TURN relay path: privacy, or to validate coturn), "disable" (no ICE),
    // "default" (GNS's default). Mapped to IceEnable in StartP2P.
    std::string iceMode;

    // The destination was named by this machine, not by anything on the network: a typed box (the
    // direct-connect windows) or local configuration (the env client, the reload-churn autotest,
    // the overlay test arm). An ini and a keyboard are both the local operator.
    //
    // It exists so `peer_admission` can tell "the player chose this address" apart from "nobody
    // advertised an identity", two statements that happen to coincide today. Inferring one from the
    // other is how a gate silently widens: the day a lane loses its advertised identity for an
    // unrelated reason, it would inherit this permission without anyone deciding to grant it.
    bool selfAddressed = false;

    int sendHz = 60;
};

// Forward declaration: Session holds a SignalingClient (P2P only). Defined in
// coop/net/signaling_client.h, which pulls in the GNS public API; kept out of this header so the
// broad set of session.h includers never see GNS. Held through shared_ptr: per-connection
// signaling objects co-own the transport, and shared_ptr's type-erased deleter means the
// incomplete-type member needs no special handling in the inline default ctor / out-of-line dtor.
class SignalingClient;

class Session {
public:
    // A fixed-size inline payload (no heap allocation on the net thread per receive).
    // senderPeerSlot is the coop::players::Registry slot of the peer that originated the message
    // (-1 if unknown); drainers route per sender through it (a TeleportClient answer goes back to
    // the requester, say).
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

    // Game thread: publish the local player's pose; the net thread fans it out to every connected
    // peer each sendHz tick.
    void SetLocalPose(const PoseSnapshot& pose);
    void SetLocalPropPose(bool set, const PropPoseSnapshot& pose);
    // Publish the local ragdoll's pelvis physics (transform + velocity) while ragdolling. `set` is
    // true each sendHz frame the local player is ragdolled, false on the recover edge
    // (SetLocalPropPose's held/release gate). The net thread fans it out only while set.
    void SetLocalRagdollPose(bool set, const RagdollPoseSnapshot& pose);
    // Publish the local hand item's live view-relative transform while holding. `set` true each
    // game tick while holding (measured fresh), false on the hand-empty edge, the same held/release
    // gate. The net thread fans a HandPose datagram out at sendHz only while set.
    void SetLocalHandPose(bool set, const HandPoseSnapshot& pose);

    // Publish the local coords-panel live cursor (viewCoordinate). `set` true each pump tick while
    // the desk is claimed and the cursor moved, false on the release or still edge, the same
    // held/release gate. The net thread fans a DeskCursorPose datagram out at sendHz only while
    // set.
    void SetLocalDeskCursor(bool set, const DeskCursorPoseSnapshot& pose);

    // Host: publish the current NPC pose batch (one EntityPoseSnapshot per live NPC); the net
    // thread fans one EntityPose datagram out to every peer each sendHz tick. npc_sync calls it
    // every game tick with the current set; an empty batch clears it (no NPCs, nothing sent). Takes
    // the batch by const reference and copies it into localNpcBatch_, reusing its capacity, so the
    // caller can hand over a reused scratch vector with no per-tick heap allocation on either side.
    // Game thread.
    void SetLocalNpcPoseBatch(const std::vector<EntityPoseSnapshot>& batch);

    // Host: publish the current WorldActor pose batch (one WorldActorPoseSnapshot per live
    // non-Character event actor); the net thread fans one WorldActorPose datagram out to every peer
    // each sendHz tick. The same copy-into shape as SetLocalNpcPoseBatch; an empty batch clears it.
    // Game thread.
    void SetLocalWorldActorPoseBatch(const std::vector<WorldActorPoseSnapshot>& batch);

    // Host: publish the carried-trash-clump pose batch (one TrashClumpPoseSnapshot per host-driven,
    // client-grabbed clump, in carry and in throw flight); the net thread fans one TrashCarryPose
    // datagram out each sendHz tick. The same shape; an empty batch clears it. Game thread.
    void SetLocalTrashCarryBatch(const std::vector<TrashClumpPoseSnapshot>& batch);

    // Per-peer accessors. peerSlot is the coop::players::Registry slot (0 = host,
    // 1..kMaxPeers-1 = clients). False if peerSlot is out of range, the slot has no remote pose
    // yet, or the aggregate state is not Connected.
    bool TryGetRemotePose(int peerSlot, PoseSnapshot& out, bool* outIsNew = nullptr);
    bool TryGetRemotePropPose(int peerSlot, PropPoseSnapshot& out, bool* outIsNew = nullptr);
    // Per-peer ragdoll pelvis physics. False unless that slot has a fresh ragdoll pose and the
    // aggregate state is Connected. outIsNew distinguishes a newly arrived packet (apply the
    // velocity) from a re-read of the last one.
    bool TryGetRemoteRagdollPose(int peerSlot, RagdollPoseSnapshot& out, bool* outIsNew = nullptr);
    // Per-peer hand-item view-relative transform (hand_item::TickMirrors consumes; newest wins).
    // The TryGetRemoteRagdollPose contract.
    bool TryGetRemoteHandPose(int peerSlot, HandPoseSnapshot& out, bool* outIsNew = nullptr);
    // Per-peer coords-panel live cursor (desk_cursor_sync consumes; newest wins). The same
    // contract.
    bool TryGetRemoteDeskCursor(int peerSlot, DeskCursorPoseSnapshot& out, bool* outIsNew = nullptr);

    // Host: publish the current world-clock snapshot each game tick (time_sync::Tick); the net
    // thread fans one unreliable ClockPose datagram out to every peer on a ~500 ms throttle,
    // independent of the pose sendHz. `set` false clears it. A client call is a harmless no-op
    // store. Game thread.
    void SetHostClock(bool set, const TimeSyncPayload& clock);
    // Client game thread: move out the latest received host clock and report whether it newly
    // arrived (apply only on isNew; between arrivals the frozen mirror holds). False until the
    // first ClockPose lands (the reliable connect-edge TimeSync seeds the initial value before
    // then). Newest wins by header seq.
    bool TryGetHostClock(TimeSyncPayload& out, bool* outIsNew = nullptr);

    // Host: publish the download-sim output vector each desk_sim_sync::Tick; the net thread fans
    // one unreliable DeskSimPose datagram out on a ~100 ms (10 Hz) throttle. `set` false clears it.
    // A client call is a harmless no-op store. Game thread.
    void SetHostDeskSim(bool set, const DeskSimSnapshot& sim);
    // Client game thread: move out the latest received host download-sim vector and report whether
    // it newly arrived (apply on isNew; interpolate between). False until the first DeskSimPose
    // lands. Newest wins by header seq.
    bool TryGetHostDeskSim(DeskSimSnapshot& out, bool* outIsNew = nullptr);

    // Host: publish one dish-pose row batch (the 4 Hz movers sweep or a settle-tail full-24 sweep;
    // the game-thread sweep owns the cadence); the net thread sends one unreliable DishPose
    // datagram per publish (a dirty one-shot, not interval-driven). A client call is a harmless
    // no-op.
    void SetHostDishPose(const DishPoseBody& body);
    // Client game thread: move out the latest received dish-pose batch and report whether it newly
    // arrived (apply on isNew). Newest wins by header seq.
    bool TryGetHostDishPose(DishPoseBody& out, bool* outIsNew = nullptr);

    // Host: publish the tape-caddy reel corrector (the game-thread sweep owns the ~1 Hz cadence; a
    // dirty one-shot, one unreliable ReelPose datagram per publish). A client call is a harmless
    // no-op.
    void SetHostReelPose(const ReelPosePayload& body);
    // Client game thread: the latest received reel corrector plus isNew (newest wins by header
    // seq).
    bool TryGetHostReelPose(ReelPosePayload& out, bool* outIsNew = nullptr);

    // Client game thread: move out the latest received NPC pose batch and clear the new-data flag
    // (consume once: a tick with no new batch returns false, and the interpolation tick covers
    // between-packet motion). The net thread fills it.
    bool TakeRemoteNpcBatch(std::vector<EntityPoseSnapshot>& out);

    // Client game thread: move out the latest received WorldActor pose batch and clear the new-data
    // flag (consume once, as TakeRemoteNpcBatch). The net thread fills it.
    bool TakeRemoteWorldActorBatch(std::vector<WorldActorPoseSnapshot>& out);

    // Client game thread: move out the latest received trash-clump carry batch and clear the
    // new-data flag (consume once). The net thread fills it.
    bool TakeRemoteTrashCarryBatch(std::vector<TrashClumpPoseSnapshot>& out);

    // Game thread: queue a reliable message. The host fans out to every connected client; a client
    // sends to the host. False on a payload too large or no peer connected.
    bool SendReliable(ReliableKind kind, const void* payload, int len);

    // Single-target reliable send. peerSlot is the coop::players::Registry slot (host = 0, clients
    // 1..kMaxPeers-1). False if peerSlot is out of range, the slot is not connected, or the payload
    // does not fit. AssignPeerSlot (host to one newly connected client) and the per-slot
    // connect-edge replay use it, so each late joiner catches up without a re-broadcast.
    // `senderSlot` is the logical origin stamped into the header: 0 (the default) is the sender's
    // own identity; the late joiner's peer-state replay passes the existing peer's slot so the new
    // client routes the replayed action to that peer's puppet and the eid-range trust check sees
    // the right role.
    // The delivery contract: true means the message will be delivered, from the GNS stream or the
    // send backlog the net thread drains (queued until sent, or fatal to the connection, never
    // silently dropped); false means it never will be (the pre-world gate, a dead or dying slot, an
    // invalid length). The save stream (SaveTransferBegin/Chunk) is the one exemption and goes
    // through TrySendReliableToSlot: its pump owns pacing and retry, and a bypassing chunk in the
    // backlog could overtake a queued Begin in the same lane.
    bool SendReliableToSlot(int peerSlot, ReliableKind kind, const void* payload,
                            int len, uint8_t senderSlot = 0);

    // The pacing lane: one direct GNS attempt, no backlog. False on send-buffer backpressure (GNS's
    // limit-exceeded result), and that false is the caller's pacing signal (retry next tick). Sole
    // intended caller: the save-transfer pump (Begin and Chunk, both success-gated there).
    // Everything else uses SendReliableToSlot's guarantee.
    bool TrySendReliableToSlot(int peerSlot, ReliableKind kind, const void* payload,
                               int len, uint8_t senderSlot = 0);

    // The save-transfer bulk sink: SaveTransferChunk payloads (about 56 KB, far over the fixed
    // inbox slot) are handed to this callback on the net thread instead of entering the
    // ReliableMessage ring. coop/save registers its heap assembler at install; the sink must be
    // thread-safe and never touch the engine. The net core stays feature-agnostic (principle 7).
    using BulkSinkFn = void (*)(int senderPeerSlot, const uint8_t* data, int len);
    void SetBulkSink(BulkSinkFn sink) { bulkSink_.store(sink, std::memory_order_release); }

    // SaveTransferBegin is diverted to the net thread for the same reason as the chunks, and to the
    // same thread as the chunks. Begin fits the inbox, and draining it on the game thread while
    // chunks land on the net thread is a lag that lets bytes accumulate with no announced size.
    // Latching it here puts announce and payload on one in-order lane on one thread, so Begin is
    // always processed before chunk 0 and "chunks with no Begin" is rejected outright instead of
    // needing a guessed pre-Begin buffer cap. The bulk sink's contract: thread-safe, never touches
    // the engine.
    void SetSaveBeginSink(BulkSinkFn sink) { saveBeginSink_.store(sink, std::memory_order_release); }

    // Per-slot world-ready send gate (host side): until event_feed marks a joining slot world-ready
    // (ClientWorldReady), the send paths drop world-mutating kinds to it (the
    // IsPreWorldSendableKind allowlist in session_lanes.h); the world-ready connect replay
    // reconstructs that state by design.
    void MarkSlotWorldReady(int peerSlot, bool ready) {
        if (peerSlot >= 0 && peerSlot < kMaxPeers)
            slotWorldReady_[peerSlot].store(ready, std::memory_order_release);
    }
    bool IsSlotWorldReady(int peerSlot) const {
        return peerSlot >= 0 && peerSlot < kMaxPeers &&
               slotWorldReady_[peerSlot].load(std::memory_order_acquire);
    }
    // The net-thread view of the same event: relay-eligible since the ClientWorldReady receipt for
    // exactly this hConn (relayEligible_).
    bool IsRelayEligible(int peerSlot, uint32_t hConn) const {
        return peerSlot >= 0 && peerSlot < kMaxPeers && hConn != 0 &&
               relayEligible_[peerSlot].load(std::memory_order_acquire) == hConn;
    }

    // Is a remote connection registered in this slot (any state)?
    bool HasPeerConn(int peerSlot) const {
        return peerSlot >= 0 && peerSlot < kMaxPeers && peerConns_[peerSlot].load() != 0;
    }

    // Does any live remote peer pass the world-ready send gate? A fan-out that "failed" with zero
    // eligible receivers is a vacuous success for a save-and-seed-covered lane (absent peers are
    // owed the save and the seed, not the wire); without this test the retry-until-heard idiom
    // re-broadcasts a row a future joiner already has in its save. Game or net thread.
    bool AnyWorldReadyPeer() const {
        for (int i = 0; i < kMaxPeers; ++i)
            if (peerConns_[i].load() != 0 &&
                slotWorldReady_[i].load(std::memory_order_acquire)) return true;
        return false;
    }

    // A receive-side lane parked at its flood bound flips this to pause the client's inbox drain
    // (pause, not drop; see NetThread). Refcounted: each lane edge-latches locally and contributes
    // +1/-1 here, so one lane draining cannot cancel another lane's still-standing escalation.
    // Callers call only on their own local edge (the lanes' g_parkBackpressure latches guarantee
    // that).
    void SetApplyBackpressure(bool on) {
        applyBackpressureCount_.fetch_add(on ? 1 : -1, std::memory_order_acq_rel);
    }

    // Game thread: pop a delivered reliable message. The inbox is shared across peers (a FIFO of
    // arrivals); the kind-typed payload says what happened and senderPeerSlot who sent it.
    bool TryGetReliable(ReliableMessage& out);

    // Voice: send one VoiceFrame datagram (kVoiceFrameHeadBytes + frame.opusLen body bytes) to
    // every world-ready peer (a client: to the host). Game thread (the GNS send API is thread-safe,
    // SendReliable's calling convention). Fire and forget: a per-frame send failure drops silently;
    // the receiver's jitter buffer and PLC cover gaps.
    bool SendVoiceFrame(const VoiceFramePayload& frame);

    // The voice inbox: one received frame. A per-sender FIFO stream, not the newest-wins pose
    // model: every arrival is queued; ordering and loss live in the per-payload voice seq the
    // jitter buffer handles. senderSlot is the relay-rewritten logical origin.
    struct VoiceFrameMsg {
        int8_t senderSlot = -1;
        VoiceFramePayload frame{};
    };
    // Ring depth per sender slot (about 320 ms of one speaker). Public: it sizes the caller's drain
    // buffer (kMaxPeers * kVoiceRingPerSlot covers a full inbox).
    static constexpr int kVoiceRingPerSlot = 16;
    // Drain every queued voice frame in one call (game thread; one lock per tick). Returns the
    // count written to out[0..maxCount).
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

    // Diagnostics. The counters live in coop::net::net_stats (the one owner: bytes and packets,
    // counted at the GNS choke points; the net-stats panel reads the same source). These delegates
    // keep the existing callers.
    uint64_t packetsSent() const { return net_stats::PacketsSent(); }
    uint64_t packetsRecv() const { return net_stats::PacketsRecv(); }
    // Per-slot RTT in ms (the GNS link ping to peer `slot`), or -1 if the slot has no live
    // connection or is not yet sampled. Sampled about once a second on the net thread; 0 is a real
    // value on a LAN.
    // One consumer: roster_ledger::RefreshLinkFacts, which publishes the value on RosterRow so
    // every board reads the host's measurement. A client owns only peerConns_[0] and could only
    // ever measure its own link to the host, a different question from "how is this player
    // connected to the session", so a second reader here would be a second derivation.
    int rttMsForSlot(int slot) const {
        return (slot >= 0 && slot < kMaxPeers) ? rttMsBySlot_[slot].load() : -1;
    }
    // How `peerSlot`'s live connection carries that player's traffic, measured from the connection
    // itself: the GNS relay flag and the remote address. Never derived from cfg_.topology, which is
    // how the connection was established, not how the peer is connected (that assertion labelled a
    // port-forwarded WAN peer "LAN"). LinkKind::Unknown without a live connection.
    // The host calls this and publishes the answer on RosterRow, so every board renders the same
    // value for the same player. Any thread (the GNS API is thread-safe), but GetConnectionInfo
    // takes a GNS lock, so it is not a per-tick call; roster_ledger::RefreshLinkFacts sets the
    // cadence.
    LinkKind LinkKindForSlot(int peerSlot) const;
    // The number of currently connected peers (0..kMaxPeers-1).
    int connectedPeerCount() const;
    // The number of sockets in the pending band: accepted, not yet admitted, mid identity exchange.
    // connectedPeerCount() deliberately excludes them (it counts seats, and an unadmitted socket
    // has none), so a caller asking "is anyone here" rather than "how many seats are taken" needs
    // both; a solo-only dev instrument gated on the seat count alone would fire during a joiner's
    // whole Auth round trip. Any thread (atomics); a snapshot, not a lock.
    int pendingPeerCount() const;
    // True if the slot has an active GNS connection (its handle is set): the harness's per-slot
    // connect/disconnect edge detection. Not a "ready for app traffic" signal; see IsSlotReady.
    bool IsSlotConnected(int peerSlot) const {
        if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
        return peerConns_[peerSlot].load() != 0;
    }

    // True only after the slot's GNS Connected callback ran and ConfigureLanesForPeer succeeded.
    // IsSlotConnected flips true in the earlier Connecting callback (peerConns_[slot] is needed
    // there for the host's AcceptConnection routing); gating app-traffic sends on it would queue
    // messages on the default lane 0, because the per-kind lane mapping is not yet applied to the
    // connection. The snapshot drain and the connect-edge replay gate on IsSlotReady, so the
    // head-of-line-blocking mitigation holds under reconnect races.
    bool IsSlotReady(int peerSlot) const {
        if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
        return peerLanesConfigured_[peerSlot].load(std::memory_order_acquire);
    }

    // The slot's current occupancy generation: a host-minted, never reused, non-zero token naming
    // who occupies `peerSlot` right now; 0 = empty. A change between two reads is a replacement
    // (person X left and person Y took the slot), the transition a connected boolean cannot
    // express: lowest-free slot reuse means X to Y carries no empty moment.
    // Only the host mints (a client's slots 1..3 stay 0; its roster is entirely wire-driven), so
    // this is host-side authority, never a client-side occupancy test. The generation never goes on
    // the wire.
    // A destructive slot-addressed action (kick, ban) validates the token it captured against this
    // live value atomically with resolving the target's address, so a stale capture fails closed
    // instead of landing on the slot's successor. Any thread.
    uint32_t peerGenerationForSlot(int peerSlot) const {
        if (peerSlot < 0 || peerSlot >= kMaxPeers) return 0;
        return peerGenBySlot_[peerSlot].load(std::memory_order_acquire);
    }

    // --- Moderation (host-only admin actions) -------------------------------

    // Accept filter: a predicate the host runs against an incoming connection's remote IP (dotted
    // decimal, no port) before AcceptConnection. True allows, false rejects (the host closes the
    // connection with a "banned" reason). The harness wires it to coop::ban_list::IsBanned. Set
    // once at boot, before Start() spawns the net thread; read on the net thread without a lock
    // (the thread creation in Start() is the happens-before). MTA precedent: the join-time ban
    // check in CGame::Packet_PlayerJoinData.
    using AcceptFilterFn = bool (*)(const char* remoteIp);
    void SetAcceptFilter(AcceptFilterFn fn) { acceptFilter_ = fn; }

    // Host: forcibly disconnect the client at peerSlot (1..kMaxPeers-1). Closes the GNS connection
    // with no linger (an admin kick drops immediately; `reason` reaches the peer's status callback
    // as m_szEndDebug, so it can show why) and runs the same per-slot teardown the ClosedByPeer
    // path does (GNS delivers no status callback for a connection we close). False if the slot is
    // out of range (or 0, the host itself) or not connected. Thread-safe: the GNS API is, and the
    // bookkeeping uses the net thread's own atomics and mutexes. MTA precedent:
    // CGame::QuitPlayer(QUIT_KICK).
    bool Kick(int peerSlot, const char* reason);

    // Kick, but only if `peerSlot` is still occupied by the person whose occupancy generation is
    // `expectedGeneration`; otherwise false, doing nothing.
    // A slot-addressed destructive action captures its target at one moment and executes at
    // another: an admin opens the ban modal, types a reason, and presses Ban seconds later, and
    // slots recycle in between. With a bare slot number a permanent IP ban can land on the
    // successor, a different person banned by the name of a seat they merely inherited.
    // The check compares a value from the lagging mirror (the ledger row, captured when the modal
    // opened) against the live authority (this array), so a stale capture fails closed; comparing
    // the mirror against itself would fail open and merely narrow the window to one tick. The
    // handle CAS makes the claim atomic, and it is sound because of the accept path's store order:
    // the generation is minted before peerConns_, so a successor that got far enough to change the
    // generation also changed the handle, and the CAS fails. Thread-safe.
    bool KickWithToken(int peerSlot, uint32_t expectedGeneration, const char* reason);

    // Resolve `peerSlot`'s remote IP, but only while the slot is still occupied by
    // `expectedGeneration`'s owner. KickWithToken's reasoning: the ban path must not read the
    // successor's address and write it to the permanent ban list.
    bool GetPeerAddressWithToken(int peerSlot, uint32_t expectedGeneration,
                                 char* out, int outLen) const;

  private:
    // Shared teardown for a slot the caller has already claimed (peerConns_ exchanged or CAS'd to
    // 0). Kick claims blind; KickWithToken claims by handle.
    bool KickClaimed(int peerSlot, uint32_t hConn, const char* reason);

  public:

    // The remote IP (dotted decimal, no port) of the connection at peerSlot into `out` (48 bytes or
    // more, SteamNetworkingIPAddr::k_cchMaxString). False, with an empty `out`, if the slot is not
    // connected or GNS has no remote address yet. The ban action captures the IP with it before the
    // kick zeroes the slot.
    bool GetPeerAddress(int peerSlot, char* out, int outLen) const;

    // Client-only: when the host closes our connection (kick, ban, host quit, host crash), GNS
    // reports the human-readable reason the host passed to CloseConnection ("kicked by host",
    // "banned by host"). net_pump reads it once on the client's disconnect edge, to log why before
    // fleeing to the main menu. Returns the reason and clears it (empty if none pending).
    // Thread-safe: set on the net thread, taken on the game thread.
    std::string TakeHostCloseReason();

    // The GNS C-callback adapter; public so the file-local trampoline in session.cpp can forward to
    // it.
    static void OnConnStatusChanged(void* info);

private:
    // Topology dispatch, called by Start() after the common GNS init and the global-callback
    // registration. Each branches on the role internally (host listen / client connect) and returns
    // false on any failure (Start then clears g_session and returns false). Defined in
    // session_start.cpp.
    bool StartLanDirect();  // rung 0/1: CreateListenSocketIP / ConnectByIPAddress
    bool StartP2P();        // rungs 1-3: signaling + CreateListenSocketP2P / Connect

    void NetThread();
    // Per-peer message dispatch. peerSlot is the peer the message came from: on the host the drain
    // site reads it from the message's m_nConnUserData (a pending tag routes to
    // HandlePendingMessage instead, a seat index here); on a client peerSlot = 0.
    void HandleMessage(int peerSlot, const void* data, int len);
    // Everything a pending (unadmitted) connection sends arrives here instead. Net thread only,
    // from the single drain site.
    void HandlePendingMessage(int pendIdx, uint32_t hConn, const void* data, int len);
    // NPC pose batch send and receive (session_npc.cpp). Serialize builds the body
    // (EntityPoseBatchHeader + entries) of the live local batch into `buf` (kNpcPoseDatagramMax or
    // more), leaving the leading PacketHeader for the per-peer send loop to stamp; returns the
    // datagram length, or 0 if empty (takes localMutex_). Store parses one received datagram and
    // newest-wins-stores it for the game thread (takes remoteMutex_).
    int  SerializeLocalNpcBatch(uint8_t* buf);
    void StoreRemoteNpcBatch(const void* data, int len, uint32_t seq);
    // WorldActor pose batch send and receive (session_worldactor.cpp), the NPC pair's contract:
    // Serialize builds the body after the leading PacketHeader (0 when empty or on a client; only
    // the host populates the local batch); Store parses and newest-wins-stores one received
    // datagram for the game thread. localMutex_ / remoteMutex_ as for NPCs.
    int  SerializeLocalWorldActorBatch(uint8_t* buf);
    void StoreRemoteWorldActorBatch(const void* data, int len, uint32_t seq);
    // Trash-clump carry pose batch send and receive (session_trashcarry.cpp), the same contract:
    // Serialize builds the body after the leading PacketHeader (0 when empty or on a client); Store
    // newest-wins-stores one received datagram.
    int  SerializeLocalTrashCarryBatch(uint8_t* buf);
    void StoreRemoteTrashCarryBatch(const void* data, int len, uint32_t seq);
    // Voice receive-side store (session_voice.cpp): validate one VoiceFrame datagram, queue it on
    // the voice inbox, host-relay it. Net thread.
    void StoreVoiceFrame(int routeSlot, int peerSlot, const void* data, int len);
    // The scalar stream channels' receive store (session_streams.cpp), called from HandleMessage's
    // grouped scalar case labels after the header parse, the epoch latch and the routeSlot
    // derivation. Net thread.
    void StoreStreamPacket(MsgType type, int routeSlot, int peerSlot,
                           const void* data, int len, uint32_t seq);
    // NetThread step 3 (session_streams.cpp): the per-sendHz-tick stream fan-out, the scalar
    // channels plus the npc/worldactor/trashcarry batch stamps (their Serialize* stay in their
    // TUs). `now` is the shell's single per-iteration timestamp (step 4's net-diag shares it); the
    // cadence time_points are NetThread locals advanced here by reference. Net thread.
    void SendStreamsTick(std::chrono::steady_clock::time_point now,
                         std::chrono::milliseconds sendInterval,
                         std::chrono::steady_clock::time_point& nextSend,
                         std::chrono::steady_clock::time_point& nextClockSend,
                         std::chrono::steady_clock::time_point& nextDeskSimSend,
                         uint64_t& sendFails);
    void HandleConnStatusChanged(void* info);
    // Host-only: the lowest empty slot in [1..kMaxPeers-1], or -1 when every client slot is taken
    // (the host is full).
    int FindFreePeerSlotForClient();
    // The peer slot that owns hConn, or -1.
    int FindPeerSlotForConn(uint32_t hConn);
    // Per-peer reset on slot disconnect. The caller holds remoteMutex_.
    void ResetPeerRemoteState(int peerSlot);

    // Host relay: forward an unreliable packet the host just received from `originSlot` to every
    // other connected client. Copies the datagram, rewrites the header's senderEpoch to the host's
    // own (so the receiving client's connection-keyed epoch latch passes) and senderSlot to
    // `originSlot` (so the client routes the pose to the right puppet); seq and body are preserved
    // (per-origin-slot seq monotonicity holds on the receiver). No-op unless role == Host. Net
    // thread only (from HandleMessage). `data`/`len` is the full received datagram (PacketHeader +
    // body).
    void RelayUnreliableToOtherClients(int originSlot, const void* data, int len);

    // Host relay: forward a reliable datagram the host just received from `originSlot` to every
    // other connected client, on the reliable channel and the kind's priority lane, with the same
    // header rewrite as the unreliable relay (epoch to the host's, senderSlot to originSlot). The
    // caller has already verified the kind is client-relayable (peer-originated gameplay);
    // handshake and host-authoritative kinds are never relayed. No-op unless role == Host. Net
    // thread only.
    void RelayReliableToOtherClients(int originSlot, ReliableKind kind,
                                     const void* data, int len);

    Config cfg_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<ConnState> state_{ConnState::Disconnected};

    // GNS handles: uint32_t aliases for HSteamNetConnection / HSteamListenSocket /
    // HSteamNetPollGroup, so this header does not include the GNS public API.
    std::atomic<uint32_t> hListen_{0};     // host only
    std::atomic<uint32_t> hPollGroup_{0};  // host only; receives all client msgs
    // Per-peer connection handles. Host: peerConns_[0] unused (itself), peerConns_[1..kMaxPeers-1]
    // the admitted client connections (the slot assigned by AdmitPending in
    // FindFreePeerSlotForClient order). Client: peerConns_[0] the host's connection, the rest
    // unused.
    std::array<std::atomic<uint32_t>, kMaxPeers> peerConns_{};

    // --- Pending (unadmitted) connections -----------------------------------
    // A connection GNS has accepted but that has not yet proved it may be here holds no player
    // seat. It lives here instead, and its SetConnectionUserData carries `kPendingTag | index`,
    // deliberately outside [1, kMaxPeers), the range the single drain site validates before
    // narrowing to a slot.
    // A separate band rather than a per-slot `admitted_` flag: kMaxPeers is 4, so there are exactly
    // three client seats. If an unadmitted peer held a seat, three sockets that say nothing would
    // hold the entire lobby until a timeout and re-take it instantly, a lockout of the host's real
    // friends that no deadline fixes. Holding no seat makes that impossible rather than
    // time-bounded.
    // What it buys for free: roster_ledger births the roster row (and mints the player number and
    // drives the person fan-out across some twenty subsystems) on `IsSlotReady(slot)` alone, and
    // net_pump reads the same predicate for the puppet edge. A peer with no slot is never ready for
    // any slot, so every IsSlotReady consumer inherits this gate with no edits. That is why this is
    // a band and not a new predicate at twenty call sites.
    static constexpr int64_t  kPendingTag   = 0x7000'0000LL;

public:
    // Public because coop/net/peer_admission.cpp sizes its per-pending exchange rows to it, and a
    // silent mismatch there would drop the last band entry's challenge state rather than fail to
    // build.
    static constexpr int      kMaxPending   = 8;  // sockets, not seats
    // How long an unproved socket may sit before the sweep closes it. Generous on purpose: a
    // pending entry costs one socket (no seat, no lanes, no parser, no roster row), and the real
    // bound on the band is ParkPending's evict-the-oldest. It must clear the slowest legitimate
    // handshake, and on P2P that includes the whole ICE negotiation between the host's park edge
    // (Connecting) and the client's own Connected edge, where the client sends its Hello.
    static constexpr uint64_t kPendingDeadlineMs = 30'000;

private:
    std::array<std::atomic<uint32_t>, kMaxPending> pendingConns_{};
    // The monotonic ms stamp of when each pending entry was accepted; 0 = free. It bounds how long
    // an unproved socket may sit, a resource bound, deliberately not the security control (holding
    // no seat is). SweepPending reads it.
    std::array<std::atomic<uint64_t>, kMaxPending> pendingSinceMs_{};

    // True when a drained message's m_nConnUserData names a pending connection rather than a player
    // seat. The drain site asks this before its `ud < 1 || ud >= kMaxPeers` drop, so a pending
    // peer's traffic reaches the admission handler and everything else keeps falling into that
    // drop.
    static bool IsPendingUserData(int64_t ud) {
        return (ud & kPendingTag) == kPendingTag &&
               (ud & ~kPendingTag) >= 0 && (ud & ~kPendingTag) < kMaxPending;
    }
    static int  PendingIndexOf(int64_t ud) { return static_cast<int>(ud & ~kPendingTag); }

public:
    // Park a freshly accepted connection in the pending band. Returns the pending index, or -1 when
    // the band is full (the caller closes the connection). Net thread only (both accept edges).
    int  ParkPending(uint32_t hConn);
    // Promote a pending connection to a real player seat: allocates the lowest free slot, re-points
    // the connection's user data at it, mints the occupancy generation and configures the lanes.
    // Returns the slot, or -1 if the lobby is full (the caller closes). Net thread only.
    int  AdmitPending(int pendingIdx, uint32_t hConn);

private:
    // The lanes, the send-buffer mirror, the lanes-configured flag and, on the host, the
    // AssignPeerSlot send. Two callers: the Connected callback for a client's link to the host, and
    // AdmitPending for a host's newly seated client.
    void FinishPeerConnected(int slot, uint32_t hConn);
public:
    // Close every pending entry that has sat longer than kPendingDeadlineMs. Net thread, once per
    // pump pass; eight acquire loads on the idle path (free on x86-64; the ordering pairs with the
    // band's writers).
    void SweepPending();

    // Send one reliable message straight to a connection handle, with no slot, no lane mapping and
    // no world gate: the admission exchange's only send path, and it stays that way. At exchange
    // time the peer holds no seat, so SendReliableToSlot cannot address it, and the pre-world gate
    // it applies is an allowlist keyed on kinds that presuppose a seated peer. Net thread only.
    bool SendRawReliableToConn(uint32_t hConn, ReliableKind kind,
                               const void* payload, int len);

    // Client: the host identity this session was told to dial (`gen:<64 hex>`); empty on a host or
    // when nothing advertised one. It exists for one caller and one question, `peer_admission`
    // asking "is the key on this socket the key I came here for". Without it the client read the
    // host's key off the socket and verified the host against that, so any host that answered
    // passed a challenge it had named itself. Narrow rather than a whole-config accessor: this is
    // the only config field the net-thread exchange has any business reading.
    const std::string& AdvertisedHostIdentity() const { return cfg_.hostIdentity; }
    // Was this destination named by the local player? See `Config::selfAddressed`. A narrow
    // accessor rather than exposing cfg_ whole: exactly one consumer needs exactly this.
    bool DestinationIsSelfAddressed() const { return cfg_.selfAddressed; }

    // The lobby password this session was configured with (see `Config`). `peer_admission` reads it
    // on the net thread in both roles; `cfg_` is written once by `Start` before the net thread
    // exists and never mutated afterwards, which is what makes the unsynchronised read correct.
    const std::string& LobbyPassword() const { return cfg_.lobbyPassword; }

    // Client: the host admitted us (its AssignPeerSlot arrived after we proved ourselves), so
    // finish the link: the lanes, the send-buffer mirror, and the flag every IsSlotReady consumer
    // waits on. Deliberately not done at the Connected edge: an unproved link that is already
    // "ready" would let the joiner ask for the world before it knows who is answering. No-op unless
    // `hConn` is still the connection in slot 0. Net thread only.
    void FinishClientLink(uint32_t hConn);

    // The guid of the peer in `slot`, derived from the public key that peer proved it holds at
    // admission (`hex(SHA-256(pub)[0..16])`). Empty when the slot is free or the occupant never
    // proved one. The game thread reads it into the roster row; the guid is not a field of the Join
    // packet, because a value the peer chooses cannot name whose stored inventory it is.
    void        SetProvedGuidForSlot(int slot, const std::string& guid);
    std::string ProvedGuidForSlot(int slot) const;

    // Refuse a pending connection: retire the band entry first, then close, with `reason` riding to
    // the peer. Retiring first makes a refusal cost O(1) rather than O(packets already dequeued);
    // see HandlePendingMessage's guard. Every refusal path goes through here: a bare
    // CloseConnection would leave the index live and re-log for every message left in the batch.
    // Net thread only.
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
    // Set in the Connected callback after ConfigureLanesForPeer; cleared when peerConns_[slot] is
    // zeroed on disconnect. See IsSlotReady().
    std::array<std::atomic<bool>, kMaxPeers> peerLanesConfigured_{};

    // Per-slot proved identity guid (see SetProvedGuidForSlot). Its own mutex rather than
    // remoteMutex_: that one is taken at 60 Hz by every pose store, and this is written once per
    // admission and read once per Join, so sharing would put a rare write behind the hottest lock
    // in the file.
    mutable std::mutex                     provedGuidMutex_;
    std::array<std::string, kMaxPeers>     provedGuidBySlot_;

    // --- Per-slot occupancy generation --------------------------------------
    // The authoritative "who is in this slot right now" token, minted here (the net layer) because
    // slots recycle: FindFreePeerSlotForClient hands out the lowest free slot, so slot 2 can go
    // from person X to person Y with no empty moment in between, and a polled boolean edge misses
    // that transition entirely. A generation change is a replacement; 0 means the slot is empty.
    // Deliberately not ownEpoch_: that value is minted in the sender's process (Start()) and rides
    // the packet header, so it is peer-declared; a rejoining peer could re-declare its prior epoch
    // and show the host no token change at all. Authority never rests on a value the peer chooses.
    // Ownership: the net layer is the only writer (mint at accept, clear on close); the game-thread
    // ledger only reads it. A game-thread write here would break that single-writer claim.
    std::array<std::atomic<uint32_t>, kMaxPeers> peerGenBySlot_{};
    std::atomic<uint32_t> peerGenCounter_{0};
    // Mint the next non-zero generation. Net thread (the accept paths).
    uint32_t MintPeerGeneration() {
        uint32_t g;
        do { g = peerGenCounter_.fetch_add(1, std::memory_order_relaxed) + 1; } while (g == 0);
        return g;
    }

    // P2P only (cfg_.topology == Topology::P2P): the signaling-server transport, the out-of-band
    // channel that carries opaque ICE rendezvous blobs between peers. Created in Start() before the
    // net thread spawns; polled on the net thread each loop; reset in Stop() after the net thread
    // joins and the connections finish lingering (closing a P2P connection may need to send a final
    // signal). nullptr for LanDirect. shared_ptr because per-connection signaling objects co-own it
    // (they may outlive the reset() until GNS releases them). The net thread reads it without a
    // lock: set before the thread spawns, reset only after it joins (ownEpoch_'s discipline).
    std::shared_ptr<SignalingClient> signaling_;

    // The local pose slot (the game thread writes, the net thread reads and fans out).
    std::mutex localMutex_;
    PoseSnapshot localPose_{};
    // The monotonic time at which localPose_ was sampled, not at which it is sent. The net thread
    // stamps this into the header rather than "now": a game-thread hitch would otherwise pair an
    // old position with a fresh stamp, and the receiver's interval would under-report the motion
    // that happened.
    uint32_t     localPoseStateMs_ = 0;
    bool hasLocal_ = false;
    PropPoseSnapshot localPropPose_{};
    bool hasLocalProp_ = false;
    uint32_t lastLocalPropSeq_ = 0;
    // Local ragdoll pelvis physics (the game thread writes while ragdolling, the net thread reads
    // and fans out). localPropPose_'s held/release shape.
    RagdollPoseSnapshot localRagdollPose_{};
    bool hasLocalRagdoll_ = false;
    // Local hand-item view-relative transform (the game thread writes while holding, the net thread
    // reads and fans out). The prop/ragdoll held/release shape.
    HandPoseSnapshot localHandPose_{};
    bool hasLocalHand_ = false;
    // Local coords-panel live cursor (the game thread writes while the desk is claimed and moving,
    // the net thread reads and fans out). localHandPose_'s held/release shape.
    DeskCursorPoseSnapshot localDeskCursor_{};
    bool hasLocalDeskCursor_ = false;
    // Host NPC pose batch (the game thread writes through SetLocalNpcPoseBatch, the net thread
    // reads and fans one EntityPose datagram out). An empty vector is nothing to send.
    std::vector<EntityPoseSnapshot> localNpcBatch_;
    bool hasLocalNpcBatch_ = false;
    // Host WorldActor pose batch (written through SetLocalWorldActorPoseBatch; the net thread fans
    // one WorldActorPose datagram out). An empty vector is nothing to send.
    std::vector<WorldActorPoseSnapshot> localWorldActorBatch_;
    bool hasLocalWorldActorBatch_ = false;
    // Host carried-trash-clump pose batch (written through SetLocalTrashCarryBatch; the net thread
    // fans one TrashCarryPose datagram out). Empty = none.
    std::vector<TrashClumpPoseSnapshot> localTrashCarryBatch_;
    bool hasLocalTrashCarryBatch_ = false;
    // Host world-clock snapshot (written through SetHostClock each tick; the net thread fans one
    // unreliable ClockPose datagram out on its own ~500 ms throttle, independent of the pose
    // sendHz). A single value, host-authoritative, the same to all.
    TimeSyncPayload localHostClock_{};
    bool hasLocalHostClock_ = false;
    // Host download-sim output vector (written through SetHostDeskSim each desk_sim_sync tick; the
    // net thread fans one unreliable DeskSimPose datagram out on its own ~100 ms / 10 Hz throttle).
    // A single value, the same to all.
    DeskSimSnapshot localDeskSim_{};
    bool hasLocalDeskSim_ = false;
    // Host dish-pose row batch (written through SetHostDishPose; the net thread sends one
    // unreliable DishPose datagram per publish: a dirty one-shot, the game-thread sweep owns the
    // cadence).
    DishPoseBody localDishPose_{};
    bool dishPoseDirty_ = false;
    // Host reel-corrector value (written through SetHostReelPose; the net thread sends one
    // unreliable ReelPose datagram per publish: a dirty one-shot).
    ReelPosePayload localReelPose_{};
    bool reelPoseDirty_ = false;

    // Per-peer remote pose slots. The net thread writes (under remoteMutex_) on receive; the game
    // thread reads through TryGetRemotePose(...).
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
    // Per-peer hand-item view-relative transform (the same per-slot shape).
    std::array<HandPoseSnapshot, kMaxPeers> remoteHandPoses_{};
    std::array<bool, kMaxPeers> hasRemoteHand_{};
    std::array<uint32_t, kMaxPeers> lastRemoteHandSeq_{};
    std::array<uint64_t, kMaxPeers> remoteHandStamp_{};
    std::array<uint64_t, kMaxPeers> lastReadHandStamp_{};
    // Per-peer coords-panel live cursor (the same per-slot shape).
    std::array<DeskCursorPoseSnapshot, kMaxPeers> remoteDeskCursors_{};
    std::array<bool, kMaxPeers> hasRemoteDeskCursor_{};
    std::array<uint32_t, kMaxPeers> lastRemoteDeskCursorSeq_{};
    std::array<uint64_t, kMaxPeers> remoteDeskCursorStamp_{};
    std::array<uint64_t, kMaxPeers> lastReadDeskCursorStamp_{};
    // The latest received host world-clock snapshot (host to client; one slot, not per peer: the
    // host is the only sender). The net thread stores under remoteMutex_; the client game thread
    // drains through TryGetHostClock (newest wins by header seq; apply on isNew).
    TimeSyncPayload remoteHostClock_{};
    bool hasRemoteHostClock_ = false;
    uint32_t lastRemoteHostClockSeq_ = 0;
    uint64_t remoteHostClockStamp_ = 0;
    uint64_t lastReadHostClockStamp_ = 0;
    // The latest received host download-sim output vector (host to client; one slot, the host is
    // the only sender). Stored under remoteMutex_; drained through TryGetHostDeskSim (newest wins
    // by header seq; interpolate and overwrite).
    DeskSimSnapshot remoteDeskSim_{};
    bool hasRemoteDeskSim_ = false;
    uint32_t lastRemoteDeskSimSeq_ = 0;
    uint64_t remoteDeskSimStamp_ = 0;
    uint64_t lastReadDeskSimStamp_ = 0;
    // The latest received dish-pose batch (host to client; one slot). Stored under remoteMutex_;
    // drained through TryGetHostDishPose (newest wins by header seq; apply on isNew).
    DishPoseBody remoteDishPose_{};
    bool hasRemoteDishPose_ = false;
    uint32_t lastRemoteDishPoseSeq_ = 0;
    uint64_t remoteDishPoseStamp_ = 0;
    uint64_t lastReadDishPoseStamp_ = 0;
    // The latest received reel corrector (host to client; one slot). Stored under remoteMutex_;
    // drained through TryGetHostReelPose.
    ReelPosePayload remoteReelPose_{};
    bool hasRemoteReelPose_ = false;
    uint32_t lastRemoteReelPoseSeq_ = 0;
    uint64_t remoteReelPoseStamp_ = 0;
    uint64_t lastReadReelPoseStamp_ = 0;
    // The latest received NPC pose batch (host to client; one slot, not per peer: the host is the
    // only sender). Stored under remoteMutex_; the game thread drains through TakeRemoteNpcBatch.
    std::vector<EntityPoseSnapshot> remoteNpcBatch_;
    bool     hasRemoteNpcBatch_ = false;
    uint32_t lastRemoteNpcSeq_  = 0;
    // The latest received WorldActor pose batch (host to client; one slot). Stored under
    // remoteMutex_; drained through TakeRemoteWorldActorBatch.
    std::vector<WorldActorPoseSnapshot> remoteWorldActorBatch_;
    bool     hasRemoteWorldActorBatch_ = false;
    uint32_t lastRemoteWorldActorSeq_  = 0;
    // The latest received trash-clump carry batch (host to client; one slot). Stored under
    // remoteMutex_; drained through TakeRemoteTrashCarryBatch.
    std::vector<TrashClumpPoseSnapshot> remoteTrashCarryBatch_;
    bool     hasRemoteTrashCarryBatch_ = false;
    uint32_t lastRemoteTrashCarrySeq_  = 0;
    // Per-slot expected senderEpoch, latched from the first packet arriving from that slot. Zero =
    // not yet latched; later packets whose header epoch does not match are dropped at HandleMessage
    // entry. Cleared on disconnect in ResetPeerRemoteState so the next connection on the same slot
    // re-latches. Protected by remoteMutex_, updated under the same lock as remoteStamp_ /
    // hasRemote_ on the receive paths.
    std::array<uint32_t, kMaxPeers> expectedEpoch_{};

    // The reliable inbox (shared across peers; a FIFO of arrival order).
    std::mutex reliableInboxMutex_;
    std::deque<ReliableMessage> reliableInbox_;
    // The exact high-water of reliableInbox_ depth since the last ~1 Hz net-diag sample, stamped at
    // the enqueue site (under reliableInboxMutex_, so it sees every push; sampling in the drain
    // loop would only observe the 200 Hz pre-receive value and miss a spike the game thread drained
    // between iterations). Reported and reset by the net-diag block. Without the depth that armed
    // it, the inbox-depth pause would be invisible: "no pause fired" could not be told from "the
    // depth never got there". Net thread writes, net thread reads.
    std::atomic<uint32_t> reliableInboxPeak_{0};

    // The voice inbox: per-slot fixed rings, zero net-thread heap allocation and per-sender
    // fairness (a spammer overwrites only their own oldest audio). Drop-oldest per slot on
    // overflow; the jitter buffer treats the loss as an ordinary gap. head/tail are monotonic
    // (tail - head <= kVoiceRingPerSlot).
    struct VoiceSlotRing {
        VoiceFrameMsg ring[kVoiceRingPerSlot];
        uint32_t head = 0, tail = 0;
    };
    std::mutex voiceInboxMutex_;
    VoiceSlotRing voiceRings_[kMaxPeers];

    // The bulk-chunk diversion target (see SetBulkSink). Written once at install (game thread),
    // read per message on the net thread.
    std::atomic<BulkSinkFn> bulkSink_{nullptr};
    // The SaveTransferBegin diversion target (see SetSaveBeginSink); bulkSink_'s
    // write-once-at-install lifetime.
    std::atomic<BulkSinkFn> saveBeginSink_{nullptr};

    // Per-slot world-ready flags (see MarkSlotWorldReady).
    std::array<std::atomic<bool>, kMaxPeers> slotWorldReady_{};
    // The receive-side apply-backpressure refcount (see SetApplyBackpressure).
    std::atomic<int> applyBackpressureCount_{0};
    // Per-slot relay-eligibility stamp: the hConn that announced ClientWorldReady, set on the net
    // thread at receipt. The game-thread world-ready flag lags by one drain, and the relay consults
    // this so a reliable received in that gap is neither skipped and lost nor double-delivered (the
    // session.cpp receive path and session_relay.cpp). hConn-stamped so a recycled slot can never
    // inherit eligibility. Cleared beside backlog_.FreeSlot.
    std::array<std::atomic<uint32_t>, kMaxPeers> relayEligible_{};

    std::atomic<uint32_t> sendSeq_{0};

    // The reliable-send delivery guarantee: per-(slot, lane) backlogs (send_backlog.h), drained on
    // the net thread each loop pass and freed at every peerConns_ zeroing site. sendBufBytes_
    // mirrors the per-connection SendBufferSize actually configured (knob or default); the drain's
    // reserve gate is computed against it.
    SendBacklog backlog_;
    int sendBufBytes_ = 512 * 1024;
    // The fatal-backlog close: host -> Kick(slot); client -> claim plus the same KickClaimed
    // teardown on its host connection (GNS delivers no callback for a connection we close
    // ourselves). Net thread.
    void FatalCloseSlot(int slot, const char* reason);
    // The client's only way to end its own host link. Records `why`, claims slot 0 and runs the
    // full KickClaimed teardown, which is what drives `state_` to Disconnected, the gate net_pump's
    // connect-fail edge needs before it can show the player a reason. A bare CloseConnection here
    // would be a mute refusal: correct on the wire, invisible on screen. Net thread.
    void LeaveHost(const char* why);
    // Per-slot RTT (ms), sampled about once a second on the net thread from GNS's m_nPing.
    // 0-initialised; the sampler writes -1 for a slot with no live connection and the real ping for
    // a connected one. event_feed fans the per-peer ping to each puppet.
    std::array<std::atomic<int>, kMaxPeers> rttMsBySlot_{};

    // The host's accept predicate (the ban filter); nullptr = accept all. Set before Start() spawns
    // the net thread; read on the net thread.
    AcceptFilterFn acceptFilter_ = nullptr;

    // Client-only: the reason GNS reported when the host closed our connection (m_szEndDebug).
    // Written on the net thread in the slot-0 ClosedByPeer / ProblemDetectedLocally branch; taken
    // (moved and cleared) on the game thread by net_pump through TakeHostCloseReason. A tiny
    // dedicated mutex (a rare path, no lock-order entanglement with remoteMutex_ /
    // reliableInboxMutex_).
    std::mutex  hostCloseMutex_;
    std::string hostCloseReason_;

    // This peer's own per-process session epoch, minted non-zero from std::random_device at Start()
    // and stamped on the header of every outbound packet (WriteHeader's senderEpoch). Read on the
    // net thread without a lock: Start() completes before the net thread spawns and the value never
    // changes afterwards, so it is effectively const for the session's lifetime.
    uint32_t ownEpoch_ = 0;
};

}  // namespace coop::net
