// coop/net/signaling_client.h -- out-of-band rendezvous channel for P2P (ICE).
//
// Co-located net-internal header (src tree, not include/). Ported from
// third_party/GameNetworkingSockets/examples/trivial_signaling_client.cpp
// (BSD-3, Valve) into our namespace. The wire is a line-oriented TCP stream:
// the first line is "<token> <identity>" and every subsequent line is
// "<dest-identity> <hex-payload>\n". The server routes a line to the connection
// registered under <dest-identity>, rewriting the leading token to the SENDER's
// identity on the way out.
//
// REGISTRATION IS PROVED (security A59, 2026-08-29). Between the greeting and
// the relay stream the server sends "nonce <64 hex>" and we answer
// "auth <128 hex>" -- an Ed25519 signature, by the key our identity NAMES, over
// a domain-separated blob. Our identity IS that key (b144 made a peer's durable
// public key its rendezvous name), and until this landed the greeting's only
// credential was a static bearer every mod user holds, so anyone who had ever
// played with us could take our permanent name and deny us rendezvous forever.
//
// WE FAIL CLOSED. A relay that never challenges us is older than this build, and
// registering unproved there would silently reopen exactly what the challenge
// closes -- so we refuse and say so by name. The cost is bounded to P2P: the LAN
// and direct-IP lanes never touch a relay. What makes fail-closed SAFE is not
// this code but `tools/sig_gate.py`, which proves the DEPLOYED relay speaks the
// challenge before a build that requires it may be published (docs/RELEASE.md).
//
// One SignalingClient per Session (P2P only). It:
//   - maintains the TCP connection to the signaling server (auto-reconnect),
//   - hands GNS a per-connection ISteamNetworkingConnectionSignaling on demand
//     (CreateSignalingForConnection) whose SendSignal() hex-encodes + enqueues,
//   - drains inbound lines in Poll() and dispatches them via
//     ReceivedP2PCustomSignal so GNS advances the ICE handshake.
//
// Threading: SendSignal may be called from ANY thread (GNS internal threads);
// Poll() runs on the Session net thread. The socket + send queue are guarded by
// a recursive mutex. Poll() does all socket IO under the lock but dispatches
// received signals AFTER releasing it -- ReceivedP2PCustomSignal takes a GNS
// lock that another thread may hold while calling our SendSignal, so holding our
// lock across the dispatch would invert lock order and can deadlock (the trivial
// example documents exactly this).
//
// Transport note: raw TCP today (matches the local test server, proven path).
// The VPS stage swaps this for a WinHTTP WebSocket (wss://:443, firewall-
// friendly) behind the SAME class surface -- only the socket guts change, not
// the GNS-facing API. See the connectivity-ladder design doc s5.3.

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

// enable_shared_from_this: each per-connection ConnectionSignaling holds a
// shared_ptr back to its owning SignalingClient, so a connection that GNS is
// still tearing down (and may still call SendSignal on) keeps the transport
// alive even after Session::Stop() drops its own reference. Without this the
// transport could be deleted while GNS holds a live ConnectionSignaling ->
// use-after-free on the next SendSignal.
class SignalingClient : public std::enable_shared_from_this<SignalingClient> {
public:
    // Resolve + begin connecting to `serverAddr` ("host:port"; defaults to
    // :10000 if no port). `sockets` is the live ISteamNetworkingSockets whose
    // identity (already set via ResetIdentity) becomes our greeting + the
    // ReceivedP2PCustomSignal target. Returns nullptr on a bad address / null
    // sockets. Session holds the returned shared_ptr; per-connection objects
    // co-own it (see enable_shared_from_this note above).
    static std::shared_ptr<SignalingClient> Create(const std::string& serverAddr,
                                                    const std::string& token,
                                                    ISteamNetworkingSockets* sockets);
    ~SignalingClient();

    SignalingClient(const SignalingClient&) = delete;
    SignalingClient& operator=(const SignalingClient&) = delete;

    // Make a per-connection signaling object for ConnectP2PCustomSignaling (or
    // for an accepted inbound request via the recv-context). GNS takes ownership
    // of the returned object and calls Release() on it when done.
    ISteamNetworkingConnectionSignaling* CreateSignalingForConnection(
        const SteamNetworkingIdentity& peer);

    // Net thread: drain inbound signals -> ReceivedP2PCustomSignal, flush the
    // outbound queue, reconnect if the socket dropped. Cheap when idle.
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

    // Where this connection is in the registration handshake. Reset to
    // AwaitingChallenge by EVERY ConnectLocked, because a reconnect re-greets and
    // the server issues a FRESH nonce -- carrying `ProofSent` across a drop would
    // make us skip a challenge we were about to be sent.
    enum class RegState { AwaitingChallenge, ProofSent };

    // Answer the server's "nonce <64 hex>". Returns false when the line is not a
    // well-formed challenge or we cannot sign it, in which case the caller drops
    // the connection: there is nothing to be gained by staying on a relay that
    // will not register us. Net thread; takes sockMutex_ via Enqueue.
    bool AnswerChallenge(const char* line, size_t len);

    const std::string host_;
    const std::string service_;     // port, as a string for getaddrinfo
    const std::string token_;       // shared bearer token sent in the greeting
    ISteamNetworkingSockets* const sockets_;
    std::string selfIdentity_;      // rendered local identity (no newline)
    std::string greeting_;          // "<token> <identity>\n"
    bool wsaStarted_ = false;       // we successfully called WSAStartup
    bool identityOk_ = true;        // false if our identity is invalid/spaced -> Create() returns nullptr

    // Server address resolved ONCE in the ctor (on the constructing thread,
    // before the net thread spawns) so reconnects never call the blocking
    // getaddrinfo on the 200 Hz net thread. Opaque byte storage keeps winsock
    // types out of this header; the .cpp reinterpret_casts to sockaddr.
    bool resolved_ = false;
    int  resolvedFamily_ = 0;
    int  resolvedLen_ = 0;
    unsigned char resolvedAddr_[128] = {};  // >= sizeof(sockaddr_storage) (128 on Win)

    std::recursive_mutex sockMutex_;
    // SOCKET is UINT_PTR on Windows; store as uintptr_t so this header needs no
    // winsock include (which would impose the winsock2-before-windows.h ordering
    // constraint on every includer). The .cpp casts to/from SOCKET. ~0 ==
    // INVALID_SOCKET.
    std::uintptr_t sock_ = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));
    std::string inBuf_;             // accumulates inbound bytes (net-thread only)
    std::deque<std::string> sendQueue_;  // outbound lines awaiting flush (greeting at front)
    // Reconnect backoff: earliest steady_clock time Poll() may retry connecting
    // after a drop (net-thread-only; no lock). Without it a down signaling
    // server triggers a reconnect attempt every Poll (~200 Hz).
    std::chrono::steady_clock::time_point nextConnectAttempt_{};
    // Registration handshake state (net-thread only, like inBuf_). The deadline
    // is generous on purpose: it exists to turn "an old relay never challenged
    // us" into ONE named error line, not to police latency.
    RegState regState_ = RegState::AwaitingChallenge;
    std::chrono::steady_clock::time_point challengeDeadline_{};
};

}  // namespace coop::net
