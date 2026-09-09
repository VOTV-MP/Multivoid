// coop/net/signaling_client.h -- the out-of-band rendezvous channel for P2P (ICE), ported from
// GameNetworkingSockets' trivial_signaling_client example (BSD-3, Valve). A line-oriented TCP
// stream: the first line is "<token> <identity>", every later line "<dest-identity>
// <hex-payload>", routed by the server to the connection registered under that identity.
// Registration is proved: the server sends "nonce <64 hex>" and we answer "auth <128 hex>", an
// Ed25519 signature by the key our identity names; a relay that never challenges is refused
// (a release gate proves the deployed relay speaks the challenge before a release). One
// client per P2P Session: it keeps the connection (auto-reconnect), hands GNS a per-connection
// signaling object whose SendSignal hex-encodes and enqueues, and Poll() drains inbound lines
// into ReceivedP2PCustomSignal. SendSignal may run on any thread and Poll() on the net thread;
// the socket and the queue are under a recursive mutex, and received signals are dispatched
// after it is released, since ReceivedP2PCustomSignal takes a GNS lock another thread may hold
// while calling SendSignal.

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingtypes.h>          // SteamNetworkingIdentity
#include <steam/steamnetworkingcustomsignaling.h>  // ISteamNetworkingConnectionSignaling
#pragma warning(pop)

class ISteamNetworkingSockets;

namespace coop::net {

// enable_shared_from_this: each per-connection ConnectionSignaling co-owns the client, so a
// connection GNS is still tearing down (and may still call SendSignal on) keeps the transport
// alive after Session::Stop() drops its reference.
class SignalingClient : public std::enable_shared_from_this<SignalingClient> {
public:
    // Resolve and begin connecting to `serverAddr` ("host:port", port 10000 by default); `sockets`'
    // identity becomes the greeting and the ReceivedP2PCustomSignal target. nullptr on a bad
    // address or null sockets. Session holds the returned shared_ptr.
    static std::shared_ptr<SignalingClient> Create(const std::string& serverAddr,
                                                    const std::string& token,
                                                    ISteamNetworkingSockets* sockets);
    ~SignalingClient();

    SignalingClient(const SignalingClient&) = delete;
    SignalingClient& operator=(const SignalingClient&) = delete;

    // A per-connection signaling object for ConnectP2PCustomSignaling or an accepted inbound
    // request; GNS owns it and calls Release().
    ISteamNetworkingConnectionSignaling* CreateSignalingForConnection(
        const SteamNetworkingIdentity& peer);

    // Net thread: drain inbound signals, flush the outbound queue, reconnect if dropped. Cheap when
    // idle.
    void Poll();

private:
    struct ConnectionSignaling;  // per-connection ISteamNetworkingConnectionSignaling
    friend struct ConnectionSignaling;

    SignalingClient(std::string host, std::string service, std::string token,
                    ISteamNetworkingSockets* sockets);

    void ResolveServerAddr();   // ctor-time, on the constructing thread (may block on DNS)
    void CloseSocketLocked();   // caller holds sockMutex_
    void ConnectLocked();       // caller holds sockMutex_ (no DNS -- uses the cached addr)
    void Enqueue(const std::string& line);  // thread-safe; line is '\n'-terminated
    // The same, at the front; only the registration proof uses it, since the relay reads the line
    // after the challenge as the proof and a queued ICE signal must not overtake it.
    void EnqueueFront(const std::string& line);

    // Where this connection is in the registration handshake; reset by every ConnectLocked, since a
    // reconnect re-greets and the server issues a fresh nonce.
    enum class RegState { AwaitingChallenge, ProofSent };

    // Answer the server's "nonce <64 hex>"; false if the line is not a well-formed challenge or we
    // cannot sign, and the caller drops the connection. Net thread.
    bool AnswerChallenge(const char* line, size_t len);

    const std::string host_;
    const std::string service_;     // port, as a string for getaddrinfo
    const std::string token_;       // shared bearer token sent in the greeting
    ISteamNetworkingSockets* const sockets_;
    std::string selfIdentity_;      // rendered local identity (no newline)
    std::string greeting_;          // "<token> <identity>\n"
    bool wsaStarted_ = false;       // we successfully called WSAStartup
    bool identityOk_ = true;        // false if our identity is invalid/spaced -> Create() returns nullptr

    // The server address, resolved once in the ctor so reconnects never call the blocking
    // getaddrinfo on the net thread; opaque bytes keep winsock types out of this header.
    bool resolved_ = false;
    int  resolvedFamily_ = 0;
    int  resolvedLen_ = 0;
    unsigned char resolvedAddr_[128] = {};  // >= sizeof(sockaddr_storage) (128 on Win)

    std::recursive_mutex sockMutex_;
    // SOCKET as uintptr_t, so this header needs no winsock include (and its ordering constraint on
    // every includer); ~0 is INVALID_SOCKET.
    std::uintptr_t sock_ = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));
    std::string inBuf_;             // accumulates inbound bytes (net-thread only)
    std::deque<std::string> sendQueue_;  // outbound lines awaiting flush (greeting at front)
    // Reconnect backoff: the earliest time Poll() may retry after a drop, or a down server is
    // dialled at ~200 Hz. Net thread only.
    std::chrono::steady_clock::time_point nextConnectAttempt_{};
    // Registration handshake state (net thread only). The deadline is generous: it turns "an old
    // relay never challenged us" into one named error line, not a latency check.
    RegState regState_ = RegState::AwaitingChallenge;
    // Set when the greeting has left the socket; before it, silence means "not connected", not "old
    // relay", so it gates the fail-closed deadline and the send gate.
    bool greetingSent_ = false;
    std::chrono::steady_clock::time_point challengeDeadline_{};
};

}  // namespace coop::net
