// coop/net/session.h -- the networking application layer (PR-2 GNS edition).
//
// "A session is a host listening on a port + zero or one clients connected."
// Session owns the GameNetworkingSockets connection (or the listen socket for
// the host) and runs a dedicated net thread that drives GNS's RunCallbacks +
// ReceiveMessagesOnConnection. The game thread <-> net thread bridge stays the
// same as pre-PR-2: pose slots (mutex-guarded snapshots) + a reliable inbox.
//
// PR-2 rewrite (2026-05-28): the hand-rolled Winsock UDP transport, the
// Hello/HelloAck handshake + session-token anti-spoof, the Ping/Pong RTT,
// and the stop-and-wait reliable ARQ are all gone. GNS owns connection
// lifetime + auth (ECDH + AES-GCM) + ordering + reliability + RTT. We carry
// our existing packet structs (protocol.h) as opaque payloads inside
// SendMessageToConnection.
//
//   game thread  --SetLocalPose-->  [local slot]  --net thread--> SendMessage(hConn, Unreliable+NoNagle+NoDelay)
//   game thread <--TryGetRemote--   [remote slot] <--net thread-- ReceiveMessages -> HandleMessage(payload)
//
// All public API signatures are preserved -- the 28 caller files
// (harness, prop_lifecycle, weather_sync, etc.) compile unchanged.

#pragma once

#include "coop/net/protocol.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace coop::net {

enum class Role : uint8_t { Host, Client };

enum class ConnState : uint8_t { Disconnected, Handshaking, Connected };

struct Config {
    Role role = Role::Host;
    std::string peerIp = "127.0.0.1";   // the OTHER machine (or self, for loopback)
    uint16_t port = kDefaultPort;       // host binds this; client targets it on peerIp
    int sendHz = 60;                    // pose send rate; 60 Hz matches a 60 fps client
    // PR-2 dropped the `initiate` flag: GNS connection is always client-driven
    // (CreateListenSocketIP on host + ConnectByIPAddress on client). Loopback
    // self-test still works by running host + client in two processes.
};

class Session {
public:
    // Reliable message delivered to the game thread. Replaces the pre-PR-2
    // ReliableChannel::Message (the standalone ReliableChannel was deleted in
    // PR-2 -- GNS provides reliable+ordered delivery natively).
    struct ReliableMessage {
        ReliableKind kind;
        std::vector<uint8_t> payload;
    };

    Session() = default;
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Open the GNS endpoint + start the net thread. Idempotent-safe (returns
    // false if already running). Host calls CreateListenSocketIP(cfg.port);
    // client calls ConnectByIPAddress(cfg.peerIp:cfg.port). Returns false on
    // GNS init failure or socket bind failure.
    bool Start(const Config& cfg);

    // Close the connection, join the net thread. GNS's library state stays
    // initialized across Start/Stop cycles so a reconnect is fast.
    void Stop();

    bool running() const { return running_.load(); }
    ConnState state() const { return state_.load(); }
    bool connected() const { return state_.load() == ConnState::Connected; }
    Role role() const { return cfg_.role; }

    // Game thread: publish our local player's pose for the net thread to send.
    void SetLocalPose(const PoseSnapshot& pose);

    // Game thread: fetch the most recent remote pose. Returns true and fills
    // `out` only when connected AND at least one snapshot has arrived.
    // `outIsNew` reports whether it is newer than the previous TryGet.
    bool TryGetRemotePose(PoseSnapshot& out, bool* outIsNew = nullptr);

    // Game thread: queue a reliable message (chat / system event). PR-2: this
    // now ALWAYS succeeds at queue time -- GNS handles retransmit + ordering
    // internally. Returns false only on payload-too-large or no active
    // connection.
    bool SendReliable(ReliableKind kind, const void* payload, int len);

    // Game thread: pop a delivered reliable message, if a new one arrived.
    bool TryGetReliable(ReliableMessage& out);

    // v4: publish the held-prop world transform for the net thread to send each
    // tick. While `set==true`, the net thread emits one PropPosePacket per
    // sendHz interval.
    void SetLocalPropPose(bool set, const PropPoseSnapshot& pose);

    // v4: fetch the most recent remote PropPose.
    bool TryGetRemotePropPose(PropPoseSnapshot& out, bool* outIsNew = nullptr);

    // v5: signal the peer that we released the prop (one-shot, reliable).
    bool SendPropRelease(const WireKey& key,
                         float linVelX, float linVelY, float linVelZ,
                         float angVelX, float angVelY, float angVelZ);

    // v5: signal a NEW prop spawn from inventory drop or world generation.
    bool SendPropSpawn(const PropSpawnPayload& payload);

    // v5 Inc2: signal a prop destruction (eaten food, container break, etc.).
    bool SendPropDestroy(const WireKey& key);

    // Phase 5N1 Inc2: NPC spawn broadcast.
    bool SendEntitySpawn(const EntitySpawnPayload& payload);

    // Phase 5N1 Inc2: NPC destruction broadcast.
    bool SendEntityDestroy(uint32_t sessionId);

    // Diagnostics.
    uint64_t packetsSent() const { return sent_.load(); }
    uint64_t packetsRecv() const { return recv_.load(); }

    // Round-trip time in milliseconds, sampled from GNS's
    // GetConnectionRealTimeStatus. 0 until the first sample post-Connected.
    int lastRttMs() const { return lastRttMs_.load(); }

    // GNS C-callback adapter. Public so the file-local trampoline in
    // session.cpp (which has the exact FnSteamNetConnectionStatusChanged
    // signature GNS expects) can forward to it. The instance is found via
    // the singleton pointer set in Start(). `info` is a
    // SteamNetConnectionStatusChangedCallback_t*; void* hides the GNS type
    // from this header.
    static void OnConnStatusChanged(void* info);

private:
    void NetThread();
    void HandleMessage(const void* data, int len);
    void HandleConnStatusChanged(void* info);

    Config cfg_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<ConnState> state_{ConnState::Disconnected};

    // GNS handles. Stored as uint32_t (opaque GNS HSteamNetConnection /
    // HSteamListenSocket types) so this header doesn't include the GNS
    // public API. Real types are projected in session.cpp via a cast.
    std::atomic<uint32_t> hConn_{0};      // 0 == invalid; host: accepted client; client: dialed host
    std::atomic<uint32_t> hListen_{0};    // host only

    // Local pose slot (game thread writes, net thread reads).
    std::mutex localMutex_;
    PoseSnapshot localPose_{};
    bool hasLocal_ = false;
    PropPoseSnapshot localPropPose_{};
    bool hasLocalProp_ = false;
    uint32_t lastLocalPropSeq_ = 0;

    // Remote pose slot (net thread writes, game thread reads).
    std::mutex remoteMutex_;
    PoseSnapshot remotePose_{};
    bool hasRemote_ = false;
    uint32_t lastRemoteSeq_ = 0;
    uint64_t remoteStamp_ = 0;
    uint64_t lastReadStamp_ = 0;
    PropPoseSnapshot remotePropPose_{};
    bool hasRemoteProp_ = false;
    uint32_t lastRemotePropSeq_ = 0;
    uint64_t remotePropStamp_ = 0;
    uint64_t lastReadPropStamp_ = 0;

    // Reliable inbox: net thread enqueues delivered reliable messages; game
    // thread pops via TryGetReliable. GNS provides FIFO ordering, so a queue
    // is the right shape (vs the pre-PR-2 single-message inbox + dedup).
    std::mutex reliableInboxMutex_;
    std::deque<ReliableMessage> reliableInbox_;

    std::atomic<uint32_t> sendSeq_{0};
    std::atomic<uint64_t> sent_{0};
    std::atomic<uint64_t> recv_{0};
    std::atomic<int> lastRttMs_{0};
};

}  // namespace coop::net
