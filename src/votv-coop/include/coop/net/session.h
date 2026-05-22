// coop/net/session.h -- the networking application layer (Phase 3, methodology 3.2/3.5).
//
// "A session is a host listening on a port + zero or one clients connected."
// Session owns the Transport (pure I/O) and the protocol (serialization), runs a
// dedicated net thread, and bridges to the game thread via two small pose slots:
//
//   game thread  --SetLocalPose-->  [local slot]  --net thread--> sendto(peer)
//   game thread <--TryGetRemote--   [remote slot] <--net thread-- recvfrom(peer)
//
// The net thread NEVER touches the engine; the game thread NEVER touches the
// socket. All the engine work (read local pose, Drive the puppet) stays on the
// game thread in the harness/coop integration, which calls Set/TryGet here.
//
// Host-authoritative (methodology 3.1): the host owns world truth. For Phase 3
// (position-only) the path is symmetric -- each side sends its own player's pose
// and renders the other's -- but the role still decides who binds the port and
// who initiates the handshake, and it is the hook for Phase 4 authority.

#pragma once

#include "coop/net/protocol.h"
#include "coop/net/transport.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace coop::net {

enum class Role : uint8_t { Host, Client };

enum class ConnState : uint8_t { Disconnected, Handshaking, Connected };

struct Config {
    Role role = Role::Host;
    std::string peerIp = "127.0.0.1";  // the OTHER machine (or self, for loopback)
    uint16_t port = kDefaultPort;       // host binds this; client targets it on peerIp
    int sendHz = 30;                    // pose send rate (methodology 3.4: ~13-30 Hz)
    // Who opens the handshake. A client always initiates (Hello -> peerIp:port). A
    // real host waits and learns its peer from the first client Hello (peer unknown
    // up front). `initiate` forces the host to also target peerIp -- used for the
    // single-process loopback self-test (peerIp == self), never for a real host.
    bool initiate = false;
};

class Session {
public:
    Session() = default;
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Open the socket + start the net thread. Idempotent-safe (returns false if
    // already running). Host binds cfg.port; client binds an ephemeral port and
    // targets cfg.peerIp:cfg.port. Returns false if the socket can't open.
    bool Start(const Config& cfg);

    // Stop the net thread, send a Bye, close the socket.
    void Stop();

    bool running() const { return running_.load(); }
    ConnState state() const { return state_.load(); }
    bool connected() const { return state_.load() == ConnState::Connected; }

    // Game thread: publish our local player's pose for the net thread to send.
    void SetLocalPose(const PoseSnapshot& pose);

    // Game thread: fetch the most recent remote pose. Returns true and fills
    // `out` only when connected AND at least one snapshot has arrived. Always
    // returns the latest (Drive is idempotent); `outIsNew` reports whether it is
    // newer than the previous TryGet (for skip-if-unchanged callers).
    bool TryGetRemotePose(PoseSnapshot& out, bool* outIsNew = nullptr);

    // Diagnostics / validation (methodology 5.2: packets sent/received counts).
    uint64_t packetsSent() const { return sent_.load(); }
    uint64_t packetsRecv() const { return recv_.load(); }

private:
    void NetThread();
    void HandleDatagram(const void* data, int len, const Endpoint& from);

    Config cfg_;
    Transport transport_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<ConnState> state_{ConnState::Disconnected};

    // Peer address. For the host it is learned from the first Hello; for the
    // client it is the configured peerIp:port. Once locked, a Hello from a
    // DIFFERENT address is ignored (no hijack). Guarded by peerMutex_.
    std::mutex peerMutex_;
    Endpoint peer_;
    bool peerLocked_ = false;

    // Session nonce. The host mints a random one at Start; the client learns it
    // from the host's Hello. Every packet must carry it once known, or it is
    // dropped (anti-spoof / anti-replay). Guarded by peerMutex_ (set rarely).
    uint64_t sessionToken_ = 0;

    // Local pose slot (game thread writes, net thread reads).
    std::mutex localMutex_;
    PoseSnapshot localPose_{};
    bool hasLocal_ = false;

    // Remote pose slot (net thread writes, game thread reads).
    std::mutex remoteMutex_;
    PoseSnapshot remotePose_{};
    bool hasRemote_ = false;
    uint32_t lastRemoteSeq_ = 0;  // highest seq applied (stale-drop reordered UDP)
    uint64_t remoteStamp_ = 0;    // bumped on each accepted pose (for outIsNew)
    uint64_t lastReadStamp_ = 0;  // last stamp returned by TryGetRemotePose

    std::atomic<uint32_t> sendSeq_{0};
    std::atomic<uint64_t> sent_{0};
    std::atomic<uint64_t> recv_{0};
};

}  // namespace coop::net
