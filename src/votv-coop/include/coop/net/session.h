// coop/net/session.h -- the networking application layer (PR-4 multi-peer edition).
//
// "A session is a host listening on a port + zero..kMaxPeers-1 clients connected."
// Session owns the GameNetworkingSockets connection(s) and runs a dedicated net
// thread that drives RunCallbacks + ReceiveMessagesOnPollGroup (host) /
// ReceiveMessagesOnConnection (client). Game thread <-> net thread bridge is
// pose slots + a reliable inbox.
//
// PR-2 swapped the hand-rolled Winsock UDP for GNS. PR-3 added 3 priority lanes.
// PR-4 (this file) generalizes to kMaxPeers connections via a PollGroup on
// host. Per-peer remote pose storage is indexed by `peerSlot` matching
// coop::players::Registry's indexing (slot 0 = host, slots 1..kMaxPeers-1 =
// clients). On host, slot 0 is unused for remote state (it's local); slots
// 1..3 hold remote-client poses. On client, slot 0 holds the host's pose;
// slots 1..3 unused (host-authoritative model -- clients only talk to host).
//
// Topology-blind: per the forward-compat plan
// (research/findings/votv-gns-p2p-masterserver-plan-2026-05-28.md), every code
// path below is topology-agnostic except Session::Start. P2P + master server
// will add a sibling branch in Start; the multi-peer storage / PollGroup /
// status callback / lanes / receive loop all work unchanged for any
// HSteamNetConnection origin (LAN direct, ICE-signaled P2P, future SDR).

#pragma once

#include "coop/net/protocol.h"
#include "coop/players_registry.h"  // kMaxPeers (host + 3 clients = 4)

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace coop::net {

// kMaxPeers (host + 3 clients = 4) is canonically defined in
// coop::players::kMaxPeers. Alias it into coop::net so this header's
// std::array<T, kMaxPeers> sizes resolve without coop::players:: prefixes
// at every use site, and so any future bump to kMaxPeers cascades here
// automatically.
inline constexpr uint8_t kMaxPeers = coop::players::kMaxPeers;

enum class Role : uint8_t { Host, Client };

enum class ConnState : uint8_t { Disconnected, Handshaking, Connected };

struct Config {
    Role role = Role::Host;
    std::string peerIp = "127.0.0.1";   // the host's address (LAN direct topology)
    uint16_t port = kDefaultPort;
    int sendHz = 60;
    // Future: enum Topology { LanDirect, P2P } topology; std::string peerIdentity;
    // std::string signalingUrl; etc. -- see the P2P plan doc. Current shape is
    // LAN-direct only.
};

class Session {
public:
    // Fixed-size inline payload (no heap alloc on net thread per receive).
    // senderPeerSlot is the coop::players::Registry slot of the peer that
    // originated this message (-1 if unknown). Drainers route per-sender
    // (e.g. respond TeleportClient back to the requester) via this field.
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

    // Game thread: publish our local player's pose -- net thread fan-outs to all
    // connected peers each sendHz tick.
    void SetLocalPose(const PoseSnapshot& pose);
    void SetLocalPropPose(bool set, const PropPoseSnapshot& pose);
    // v22: publish our local ragdoll's pelvis physics (transform + velocity) while
    // we are ragdolling. `set` is true each sendHz frame the local player is
    // ragdolled, false on the recover edge (mirrors SetLocalPropPose's held/release
    // gate). Net thread fan-outs to all peers only while set.
    void SetLocalRagdollPose(bool set, const RagdollPoseSnapshot& pose);

    // Per-peer accessors. peerSlot is the coop::players::Registry slot
    // (0 = host, 1..kMaxPeers-1 = clients). Returns false if peerSlot is
    // out of range, that slot has no remote pose yet, or aggregate state
    // is not Connected.
    bool TryGetRemotePose(int peerSlot, PoseSnapshot& out, bool* outIsNew = nullptr);
    bool TryGetRemotePropPose(int peerSlot, PropPoseSnapshot& out, bool* outIsNew = nullptr);
    // v22: per-peer ragdoll pelvis physics. Returns false unless that slot has a
    // fresh ragdoll pose AND aggregate state is Connected. outIsNew distinguishes a
    // newly-arrived packet (apply the velocity) from a re-read of the last one.
    bool TryGetRemoteRagdollPose(int peerSlot, RagdollPoseSnapshot& out, bool* outIsNew = nullptr);

    // Game thread: queue a reliable message. Host fan-outs to all connected
    // clients; client sends to host. Returns false on payload-too-large or
    // when no peers are connected.
    bool SendReliable(ReliableKind kind, const void* payload, int len);

    // Single-target reliable send. peerSlot is the coop::players::
    // Registry slot index (host=0, clients 1..kMaxPeers-1). Returns
    // false if peerSlot is out of range, the slot is not connected, or
    // the payload doesn't fit. Used by AssignPeerSlot (host -> specific
    // newly-connected client) and per-slot connect-edge replay (each
    // late-joiner gets caught up without re-broadcasting to peers that
    // already have the state).
    //
    // `senderSlot` (v18 host-relay): the LOGICAL origin slot stamped into
    // the header. Defaults to 0 (the sender's own identity -- correct for
    // host->client AssignPeerSlot/Join/own-state replay). The late-joiner
    // PEER-state replay (T2-4) passes the existing peer's slot so the new
    // client routes the replayed action to that peer's puppet (and the
    // receiver's eid-range trust check sees the right role).
    bool SendReliableToSlot(int peerSlot, ReliableKind kind, const void* payload,
                            int len, uint8_t senderSlot = 0);

    // Game thread: pop a delivered reliable message. Inbox is shared across
    // peers (FIFO of arrivals); the kind-typed payload tells the drainer what
    // happened, not who sent it. Future work could add a sender peerSlot to
    // ReliableMessage if per-sender routing is needed.
    bool TryGetReliable(ReliableMessage& out);

    bool SendPropRelease(const WireKey& key,
                         float linVelX, float linVelY, float linVelZ,
                         float angVelX, float angVelY, float angVelZ);
    bool SendPropSpawn(const PropSpawnPayload& payload);
    bool SendPropDestroy(const PropDestroyPayload& payload);
    bool SendEntitySpawn(const EntitySpawnPayload& payload);
    bool SendEntityDestroy(uint32_t elementId);

    // Diagnostics.
    uint64_t packetsSent() const { return sent_.load(); }
    uint64_t packetsRecv() const { return recv_.load(); }
    int lastRttMs() const { return lastRttMs_.load(); }
    // Count of currently-connected peers (0..kMaxPeers-1).
    int connectedPeerCount() const;
    // True if the given slot has an active GNS connection (handle set).
    // Used by the harness for per-slot connect/disconnect edge detection.
    // NOT a "ready for app traffic" signal -- see IsSlotReady below.
    bool IsSlotConnected(int peerSlot) const {
        if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
        return peerConns_[peerSlot].load() != 0;
    }

    // True only after the slot's GNS Connected callback ran AND
    // ConfigureLanesForPeer succeeded. IsSlotConnected flips true in
    // the earlier Connecting callback (peerConns_[slot] is needed there
    // for host AcceptConnection routing) -- using it as the gate for
    // app-traffic sends would queue messages on the default lane 0
    // because the per-kind lane mapping hasn't been applied to the
    // connection yet. Snapshot drain and connect-edge replay must
    // gate on IsSlotReady, not IsSlotConnected, so PR-3's HOL-block
    // mitigation actually applies under reconnect races.
    bool IsSlotReady(int peerSlot) const {
        if (peerSlot < 0 || peerSlot >= kMaxPeers) return false;
        return peerLanesConfigured_[peerSlot].load(std::memory_order_acquire);
    }

    // --- Phase 2 moderation (host-only host-admin actions) ------------------

    // Accept filter: a predicate the host runs against an incoming connection's
    // remote IP (dotted-decimal, port excluded) BEFORE AcceptConnection. Return
    // true to allow, false to reject (the host closes the connection with a
    // "banned" reason). Wired by the harness to coop::ban_list::IsBanned. Set
    // ONCE at boot, before Start() spawns the net thread (read on the net thread
    // without a lock -- the thread-creation in Start() is the happens-before).
    // MTA precedent: the join-time ban check in CGame::Packet_PlayerJoinData
    // (reference/mtasa-blue/.../CGame.cpp:1973).
    using AcceptFilterFn = bool (*)(const char* remoteIp);
    void SetAcceptFilter(AcceptFilterFn fn) { acceptFilter_ = fn; }

    // Host: forcibly disconnect the client at peerSlot (1..kMaxPeers-1). Closes
    // the GNS connection (no linger -- an admin kick drops immediately; `reason`
    // is delivered to the peer's status callback m_szEndDebug so it can show
    // WHY) and runs the SAME per-slot teardown the ClosedByPeer path does (GNS
    // delivers no status callback for a connection WE close). Returns false if
    // the slot is out of range (or 0, the host self) or not connected. Thread-
    // safe (GNS API calls are; the bookkeeping uses the same atomics/mutexes the
    // net thread does). MTA precedent: CGame::QuitPlayer(QUIT_KICK).
    bool Kick(int peerSlot, const char* reason);

    // Query the remote IP (dotted-decimal, port excluded) of the connection at
    // peerSlot into `out` (recommend >= 48 bytes, SteamNetworkingIPAddr::
    // k_cchMaxString). Returns false (and out[0]='\0') if the slot isn't
    // connected or GNS has no remote address yet. Used by the BAN action to
    // capture the IP BEFORE the kick zeroes the slot.
    bool GetPeerAddress(int peerSlot, char* out, int outLen) const;

    // Client-only: when the HOST closes our connection (kick / ban / host quit /
    // host crash), GNS reports a human-readable reason -- the m_szEndDebug string
    // the host passed to CloseConnection (e.g. "kicked by host" / "banned by
    // host"). net_pump reads this ONCE on the client's disconnect edge to log WHY
    // before fleeing to the main menu. Returns the reason and clears it (empty if
    // none pending). Thread-safe (set on the net thread, taken on the game thread).
    std::string TakeHostCloseReason();

    // GNS C-callback adapter -- public so the file-local trampoline in
    // session.cpp can forward to it.
    static void OnConnStatusChanged(void* info);

private:
    void NetThread();
    // Per-peer message dispatch. peerSlot identifies which peer the message
    // came from; on host that's read from msg->m_nConnUserData (set via
    // SetConnectionUserData at AcceptConnection time); on client peerSlot=0.
    void HandleMessage(int peerSlot, const void* data, int len);
    void HandleConnStatusChanged(void* info);
    // Host-only: find the lowest empty slot in [1..kMaxPeers-1]. Returns -1
    // if all client slots are taken (host is full).
    int FindFreePeerSlotForClient();
    // Find which peer slot owns hConn. Returns -1 if not found.
    int FindPeerSlotForConn(uint32_t hConn);
    // Per-peer reset (on slot disconnect). Caller holds remoteMutex_.
    void ResetPeerRemoteState(int peerSlot);

    // Host-relay topology (PR-FOUNDATION Tier 2 T2-2): forward an unreliable
    // packet the host just received from `originSlot` to every OTHER connected
    // client. Copies the datagram, rewrites the header's senderEpoch to the
    // host's own epoch (so the receiving client's connection-keyed epoch latch
    // passes) and senderSlot to `originSlot` (so the client routes the pose to
    // the right puppet). seq + body are preserved (per-origin-slot seq
    // monotonicity holds on the receiver). No-op unless role == Host. Net
    // thread only (called from HandleMessage). `data`/`len` is the full
    // received datagram (PacketHeader + body).
    void RelayUnreliableToOtherClients(int originSlot, const void* data, int len);

    // Host-relay topology (PR-FOUNDATION Tier 2 T2-3): forward a RELIABLE
    // datagram the host just received from `originSlot` to every OTHER
    // connected client, on the reliable channel + the kind's priority lane.
    // Same header rewrite as the unreliable relay (epoch -> host's,
    // senderSlot -> originSlot). Caller must have already verified the kind
    // is client-relayable (peer-originated gameplay) -- handshake +
    // host-authoritative kinds are NOT relayed. No-op unless role == Host.
    // Net thread only.
    void RelayReliableToOtherClients(int originSlot, ReliableKind kind,
                                     const void* data, int len);

    Config cfg_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<ConnState> state_{ConnState::Disconnected};

    // GNS handles. uint32_t aliases for HSteamNetConnection / HSteamListenSocket /
    // HSteamNetPollGroup so this header doesn't include the GNS public API.
    std::atomic<uint32_t> hListen_{0};     // host only
    std::atomic<uint32_t> hPollGroup_{0};  // host only; receives all client msgs
    // Per-peer connection handles. On host: peerConns_[0] = unused (host self),
    // peerConns_[1..kMaxPeers-1] = accepted client connections (slot assigned
    // by AcceptConnection in FindFreePeerSlotForClient order). On client:
    // peerConns_[0] = the host's connection, rest unused.
    std::array<std::atomic<uint32_t>, kMaxPeers> peerConns_{};
    // Set in the Connected callback after ConfigureLanesForPeer; cleared
    // when peerConns_[slot] is zeroed on disconnect. See IsSlotReady().
    std::array<std::atomic<bool>, kMaxPeers> peerLanesConfigured_{};

    // Local pose slot (game thread writes, net thread reads + fan-outs).
    std::mutex localMutex_;
    PoseSnapshot localPose_{};
    bool hasLocal_ = false;
    PropPoseSnapshot localPropPose_{};
    bool hasLocalProp_ = false;
    uint32_t lastLocalPropSeq_ = 0;
    // v22: local ragdoll pelvis physics (game thread writes while ragdolling, net
    // thread reads + fan-outs). Same held/release shape as localPropPose_.
    RagdollPoseSnapshot localRagdollPose_{};
    bool hasLocalRagdoll_ = false;

    // Per-peer remote pose slots. Net thread writes (under remoteMutex_) on
    // receive; game thread reads via TryGetRemotePose(...).
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
    // v22: per-peer ragdoll pelvis physics (same per-slot stamp/seq shape as prop).
    std::array<RagdollPoseSnapshot, kMaxPeers> remoteRagdollPoses_{};
    std::array<bool, kMaxPeers> hasRemoteRagdoll_{};
    std::array<uint32_t, kMaxPeers> lastRemoteRagdollSeq_{};
    std::array<uint64_t, kMaxPeers> remoteRagdollStamp_{};
    std::array<uint64_t, kMaxPeers> lastReadRagdollStamp_{};
    // Per-slot expected senderEpoch latched from the first packet arriving
    // from that slot (PR-FOUNDATION-1b v16). Zero == not yet latched;
    // subsequent packets whose header epoch doesn't match are dropped at
    // HandleMessage entry. Cleared on disconnect in ResetPeerRemoteState
    // so the next connection on the same slot re-latches. Protected by
    // remoteMutex_ -- updated under the same lock as remoteStamp_ /
    // hasRemote_ on receive paths.
    std::array<uint32_t, kMaxPeers> expectedEpoch_{};

    // Reliable inbox (shared across peers; FIFO of arrival order).
    std::mutex reliableInboxMutex_;
    std::deque<ReliableMessage> reliableInbox_;

    std::atomic<uint32_t> sendSeq_{0};
    std::atomic<uint64_t> sent_{0};
    std::atomic<uint64_t> recv_{0};
    std::atomic<int> lastRttMs_{0};

    // Phase 2: the host's accept predicate (ban filter). nullptr = accept all.
    // Set before Start() spawns the net thread; read on the net thread.
    AcceptFilterFn acceptFilter_ = nullptr;

    // Client-only: the reason GNS reported when the host closed our connection
    // (m_szEndDebug). Written on the net thread in the slot-0 ClosedByPeer/
    // ProblemDetectedLocally branch; taken (move + clear) on the game thread by
    // net_pump via TakeHostCloseReason. Tiny dedicated mutex (rare path, no lock-
    // order entanglement with remoteMutex_ / reliableInboxMutex_).
    std::mutex  hostCloseMutex_;
    std::string hostCloseReason_;

    // This peer's own per-process session epoch (PR-FOUNDATION-1b v16).
    // Minted non-zero via std::random_device at Start(); stamped on the
    // header of every outbound packet (WriteHeader's senderEpoch arg).
    // Read on the net thread without a lock -- Start() completes before
    // the net thread is spawned and the value never changes afterward,
    // so it's effectively const for the session's lifetime.
    uint32_t ownEpoch_ = 0;
};

}  // namespace coop::net
