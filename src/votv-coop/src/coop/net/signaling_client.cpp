// coop/net/signaling_client.cpp -- see signaling_client.h.
//
// Ported from third_party/GameNetworkingSockets/examples/trivial_signaling_client.cpp
// (BSD-3, Valve). Adaptations: namespaced into coop::net; raw Winsock only
// (Windows build); asserts replaced with logging + graceful failure (RULE 1 --
// a malformed signal must never crash the game); self-contained WSAStartup so the
// transport does not depend on GNS having initialized Winsock first.

// Winsock MUST be included before any header that may pull in <windows.h>
// (steamnetworkingtypes.h does). winsock2.h first; windows.h after.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "signaling_client.h"

#include "ue_wrap/log.h"

#include <cstring>
#include <utility>

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#pragma warning(pop)

// Our DLL calls socket/recv/send/WSAStartup directly; ensure ws2_32 is linked
// regardless of CMake link-dep propagation from the static GNS lib.
#pragma comment(lib, "ws2_32.lib")

namespace coop::net {

namespace {

int HexDigitVal(char c) {
    if ('0' <= c && c <= '9') return c - '0';
    if ('a' <= c && c <= 'f') return c - 'a' + 0xa;
    if ('A' <= c && c <= 'F') return c - 'A' + 0xa;
    return -1;
}

inline bool IgnoreSockErr(int e) {
    return e == WSAEWOULDBLOCK || e == WSAENOTCONN;
}

constexpr std::uintptr_t kInvalidSock = static_cast<std::uintptr_t>(INVALID_SOCKET);

// Trust boundary: the inbound TCP stream is remote/attacker-influenceable. Cap
// the accumulation buffer so a server (or on-path attacker) that streams bytes
// with no newline cannot grow inBuf_ unboundedly and OOM the net thread. A
// legitimate ICE rendezvous blob (hex-encoded) is a few KB; 64 KiB is far above
// any real line. On overflow we drop the connection (and reconnect).
constexpr size_t kMaxInboundBuffer = 64 * 1024;

// Reconnect backoff: minimum wall-clock spacing between connect attempts on the
// net thread after a drop. Without it a down signaling server triggers a connect
// attempt every Poll (~200 Hz) -- pointless socket churn.
constexpr auto kReconnectBackoff = std::chrono::seconds(5);

}  // namespace

// ---------------------------------------------------------------------------
// Per-connection signaling object handed to GNS. SendSignal hex-encodes the
// opaque ICE blob, prefixes the destination identity, and enqueues a line.
// GNS owns this object and calls Release() (self-delete) when the connection no
// longer needs to signal.
// ---------------------------------------------------------------------------
struct SignalingClient::ConnectionSignaling : ISteamNetworkingConnectionSignaling {
    // shared_ptr (not raw) so this object keeps the transport alive while GNS
    // still owns us -- prevents a use-after-free if Stop() runs before GNS has
    // Release()d every per-connection object.
    const std::shared_ptr<SignalingClient> owner_;
    const std::string peerIdentity_;  // string-rendered identity of the peer

    ConnectionSignaling(std::shared_ptr<SignalingClient> owner, const char* peer)
        : owner_(std::move(owner)), peerIdentity_(peer) {}

    bool SendSignal(HSteamNetConnection hConn, const SteamNetConnectionInfo_t& info,
                    const void* pMsg, int cbMsg) override {
        (void)hConn;
        (void)info;
        std::string signal;
        signal.reserve(peerIdentity_.size() + static_cast<size_t>(cbMsg) * 2 + 4);
        signal.append(peerIdentity_);
        signal.push_back(' ');
        static const char hexdigit[] = "0123456789abcdef";
        for (const uint8_t* p = static_cast<const uint8_t*>(pMsg); cbMsg > 0; --cbMsg, ++p) {
            signal.push_back(hexdigit[*p >> 4U]);
            signal.push_back(hexdigit[*p & 0xf]);
        }
        signal.push_back('\n');
        owner_->Enqueue(signal);
        return true;
    }

    void Release() override { delete this; }
};

// ---------------------------------------------------------------------------
// Construction / teardown
// ---------------------------------------------------------------------------
std::shared_ptr<SignalingClient> SignalingClient::Create(const std::string& serverAddr,
                                                         const std::string& token,
                                                         ISteamNetworkingSockets* sockets) {
    if (!sockets) {
        UE_LOGE("signaling: Create() with null sockets");
        return nullptr;
    }
    std::string host = serverAddr;
    std::string service;
    // rfind(':') so a bracketed IPv6 literal's port colon is taken, not an
    // address colon. Bare-IPv6 signaling URLs are not supported (use host:port).
    const size_t colon = host.rfind(':');
    if (colon == std::string::npos) {
        service = "10000";  // default trivial-signaling port
    } else {
        service = host.substr(colon + 1);
        host.erase(colon);
    }
    if (host.empty() || service.empty()) {
        UE_LOGE("signaling: bad server address '%s'", serverAddr.c_str());
        return nullptr;
    }
    // Private ctor reachable here (static member). shared_ptr wraps the raw
    // pointer, wiring up enable_shared_from_this's weak ref so later
    // shared_from_this() calls are valid.
    auto client = std::shared_ptr<SignalingClient>(
        new SignalingClient(std::move(host), std::move(service), token, sockets));
    // Reject a partially-initialized transport: without Winsock or a resolved
    // address it can never connect, so fail Start() cleanly rather than hand back
    // an object that loops forever on a dead socket.
    if (!client->wsaStarted_) {
        UE_LOGE("signaling: WSAStartup failed -- P2P transport unavailable");
        return nullptr;
    }
    if (!client->identityOk_) {
        UE_LOGE("signaling: refusing to connect with an invalid/spaced identity");
        return nullptr;
    }
    if (!client->resolved_) {
        UE_LOGE("signaling: could not resolve signaling server '%s'", serverAddr.c_str());
        return nullptr;
    }
    return client;
}

SignalingClient::SignalingClient(std::string host, std::string service, std::string token,
                                 ISteamNetworkingSockets* sockets)
    : host_(std::move(host)), service_(std::move(service)), token_(std::move(token)),
      sockets_(sockets) {
    WSADATA wsa{};
    wsaStarted_ = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    if (!wsaStarted_) {
        UE_LOGW("signaling: WSAStartup failed (%d)", WSAGetLastError());
        return;  // Create() sees wsaStarted_==false and returns nullptr
    }

    // Greeting = our own identity (set via ResetIdentity before Create()). The
    // server registers us under this exact string; the peer must address it
    // identically (both derive it from the same SetGenericString value).
    SteamNetworkingIdentity self;
    self.Clear();
    sockets_->GetIdentity(&self);
    if (self.IsInvalid() || self.IsLocalHost()) {
        UE_LOGE("signaling: local identity is invalid/localhost -- P2P needs a "
                "concrete identity (ResetIdentity must run before Create)");
        identityOk_ = false;  // Create() returns nullptr -- do not connect with a broken identity
    }
    SteamNetworkingIdentityRender render(self);
    selfIdentity_ = render.c_str();
    if (selfIdentity_.find(' ') != std::string::npos) {
        UE_LOGE("signaling: identity '%s' contains a space -- the wire protocol "
                "is space-delimited and forbids it", selfIdentity_.c_str());
        identityOk_ = false;  // a spaced identity silently corrupts the wire protocol -> fail
    }
    if (token_.find_first_of(" \t") != std::string::npos) {
        // A whitespace token breaks the "<token> <identity>" greeting framing ->
        // the server drops every greeting -> the client would reconnect forever
        // with no clear diagnostic. Fail loudly instead (Create returns nullptr).
        UE_LOGE("signaling: signaling token contains whitespace -- forbidden "
                "(check VOTVCOOP_NET_SIGNALING_TOKEN / net.signaling_token)");
        identityOk_ = false;
    }
    // Greeting = "<token> <identity>\n". The server constant-time-compares the
    // token before registering us; an empty token is rejected (StartP2P refuses
    // to create us without one).
    greeting_ = token_;
    greeting_.push_back(' ');
    greeting_.append(selfIdentity_);
    greeting_.push_back('\n');

    // Resolve the server address ONCE here (constructing thread, before the net
    // thread spawns). Reconnects in Poll() reuse the cached address so the
    // blocking getaddrinfo never runs on the 200 Hz net thread.
    ResolveServerAddr();
    if (!resolved_) return;  // Create() sees resolved_==false and returns nullptr

    std::lock_guard<std::recursive_mutex> lk(sockMutex_);
    ConnectLocked();
}

SignalingClient::~SignalingClient() {
    {
        std::lock_guard<std::recursive_mutex> lk(sockMutex_);
        CloseSocketLocked();
    }
    if (wsaStarted_) WSACleanup();
}

// ---------------------------------------------------------------------------
// Socket lifecycle (caller holds sockMutex_)
// ---------------------------------------------------------------------------
void SignalingClient::CloseSocketLocked() {
    if (sock_ != kInvalidSock) {
        closesocket(static_cast<SOCKET>(sock_));
        sock_ = kInvalidSock;
    }
    inBuf_.clear();
    // sendQueue_ is deliberately NOT cleared. Pending GNS signals (which
    // SendSignal already reported as best-effort delivered) are preserved across
    // a reconnect so a transient TCP blip mid-ICE-handshake doesn't silently drop
    // them; ConnectLocked re-inserts the greeting at the front so identity
    // re-registers first. The Enqueue() cap bounds the queue meanwhile.
}

// getaddrinfo ONCE, on the constructing thread (may block on DNS). The result is
// cached so reconnects never resolve on the net thread.
void SignalingClient::ResolveServerAddr() {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    const int gai = getaddrinfo(host_.c_str(), service_.c_str(), &hints, &res);
    if (gai != 0 || !res) {
        UE_LOGW("signaling: getaddrinfo('%s:%s') failed (%d)",
                host_.c_str(), service_.c_str(), gai);
        if (res) freeaddrinfo(res);
        resolved_ = false;
        return;
    }
    resolvedFamily_ = res->ai_family;
    resolvedLen_ = static_cast<int>(res->ai_addrlen);
    const size_t n = res->ai_addrlen < sizeof(resolvedAddr_) ? res->ai_addrlen
                                                             : sizeof(resolvedAddr_);
    std::memcpy(resolvedAddr_, res->ai_addr, n);
    resolved_ = true;
    freeaddrinfo(res);
    UE_LOGI("signaling: resolved %s:%s (family=%d)", host_.c_str(), service_.c_str(),
            resolvedFamily_);
}

void SignalingClient::ConnectLocked() {
    CloseSocketLocked();
    if (!resolved_) {
        UE_LOGW("signaling: ConnectLocked with unresolved address");
        return;
    }

    const SOCKET s = socket(resolvedFamily_, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        UE_LOGW("signaling: socket() failed (%d)", WSAGetLastError());
        return;
    }
    u_long nonblock = 1;
    if (ioctlsocket(s, FIONBIO, &nonblock) != 0) {
        UE_LOGW("signaling: ioctlsocket(FIONBIO) failed (%d)", WSAGetLastError());
        closesocket(s);
        return;
    }

    // Nonblocking connect returns WSAEWOULDBLOCK and completes asynchronously;
    // queued lines flush in Poll() once writable.
    connect(s, reinterpret_cast<const sockaddr*>(resolvedAddr_), resolvedLen_);
    sock_ = static_cast<std::uintptr_t>(s);

    // The greeting (identity registration) must be the FIRST line on every fresh
    // socket. Insert at the front unless it's already there -- across repeated
    // reconnects this avoids piling up duplicate greetings ahead of the pending
    // signals CloseSocketLocked preserved.
    if (sendQueue_.empty() || sendQueue_.front() != greeting_) {
        sendQueue_.push_front(greeting_);
    }
    UE_LOGI("signaling: connecting to %s:%s as '%s'",
            host_.c_str(), service_.c_str(), selfIdentity_.c_str());
}

void SignalingClient::Enqueue(const std::string& line) {
    std::lock_guard<std::recursive_mutex> lk(sockMutex_);
    // Best-effort delivery: if the queue backs up (server unreachable), drop the
    // OLDEST signals -- they are the most stale and GNS will retry current ones.
    bool dropped = false;
    while (sendQueue_.size() > 32) {
        sendQueue_.pop_front();
        dropped = true;
    }
    if (dropped) {
        UE_LOGW("signaling: send queue backed up -- discarding oldest signals");
    }
    sendQueue_.push_back(line);
}

// ---------------------------------------------------------------------------
// Per-connection signaling factory
// ---------------------------------------------------------------------------
ISteamNetworkingConnectionSignaling* SignalingClient::CreateSignalingForConnection(
    const SteamNetworkingIdentity& peer) {
    SteamNetworkingIdentityRender peerRender(peer);
    UE_LOGI("signaling: creating signaling session for peer '%s'", peerRender.c_str());
    // shared_from_this() co-owns the transport from the per-connection object
    // (valid: the object is always managed by the shared_ptr returned by Create).
    return new ConnectionSignaling(shared_from_this(), peerRender.c_str());
}

// ---------------------------------------------------------------------------
// Poll (net thread): drain inbound -> dispatch, flush outbound, reconnect.
// ---------------------------------------------------------------------------
void SignalingClient::Poll() {
    {
        std::lock_guard<std::recursive_mutex> lk(sockMutex_);

        if (sock_ == kInvalidSock) {
            // Reconnect, backoff-gated. ConnectLocked does no DNS (cached addr),
            // so this is just socket()+connect() at most once per kReconnectBackoff.
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextConnectAttempt_) {
                ConnectLocked();
                nextConnectAttempt_ = now + kReconnectBackoff;
            }
        } else {
            const SOCKET s = static_cast<SOCKET>(sock_);
            for (;;) {
                char buf[512];
                const int r = recv(s, buf, sizeof(buf), 0);
                if (r == 0) {
                    UE_LOGW("signaling: server closed connection -- will reconnect");
                    CloseSocketLocked();
                    break;
                }
                if (r < 0) {
                    const int e = WSAGetLastError();
                    if (!IgnoreSockErr(e)) {
                        UE_LOGW("signaling: recv error %d -- will reconnect", e);
                        CloseSocketLocked();
                    }
                    break;
                }
                inBuf_.append(buf, static_cast<size_t>(r));
                if (inBuf_.size() > kMaxInboundBuffer) {
                    UE_LOGW("signaling: inbound buffer exceeded %zu bytes with no "
                            "complete line -- dropping connection", kMaxInboundBuffer);
                    CloseSocketLocked();
                    break;
                }
            }
        }

        // Flush the send queue (nonblocking; stop on would-block, retry next Poll).
        if (sock_ != kInvalidSock) {
            const SOCKET s = static_cast<SOCKET>(sock_);
            while (!sendQueue_.empty()) {
                const std::string& line = sendQueue_.front();
                const int l = static_cast<int>(line.size());
                const int r = ::send(s, line.c_str(), l, 0);
                if (r < 0 && IgnoreSockErr(WSAGetLastError())) break;  // would block
                if (r == l) {
                    sendQueue_.pop_front();
                } else {
                    UE_LOGW("signaling: send failed (r=%d/%d err=%d) -- reconnecting",
                            r, l, WSAGetLastError());
                    CloseSocketLocked();
                    break;
                }
            }
        }
    }  // release sockMutex_ BEFORE dispatch -- ReceivedP2PCustomSignal takes a GNS
       // lock that a GNS thread may hold while calling our SendSignal; holding
       // sockMutex_ across it would invert lock order and can deadlock.

    // Dispatch complete lines directly from inBuf_, OUTSIDE the lock. inBuf_ is
    // touched only on this (net) thread and Poll() is not re-entrant, so reading
    // it unlocked is safe; SendSignal (from GNS threads) touches sendQueue_, not
    // inBuf_. No scratch buffer -> no per-Poll heap allocation on the idle path.
    size_t cursor = 0;
    for (;;) {
        const size_t nl = inBuf_.find('\n', cursor);
        if (nl == std::string::npos) break;

        // Line is [cursor, nl). Format: "<from-identity> <hexpayload>".
        const size_t spc = inBuf_.find(' ', cursor);
        if (spc != std::string::npos && spc < nl) {
            const size_t hexLen = nl - (spc + 1);
            if ((hexLen & 1u) != 0) {
                UE_LOGW("signaling: odd-length hex payload -- dropping line");
            } else {
                std::string data;
                data.reserve(hexLen / 2);
                bool ok = true;
                for (size_t i = spc + 1; i + 2 <= nl; i += 2) {
                    const int dh = HexDigitVal(inBuf_[i]);
                    const int dl = HexDigitVal(inBuf_[i + 1]);
                    if ((dh | dl) & ~0xf) {
                        // Malformed hex from the signaling server: drop this line
                        // (do NOT crash -- the trivial example asserted here).
                        UE_LOGW("signaling: bad hex in signal -- dropping line");
                        ok = false;
                        break;
                    }
                    data.push_back(static_cast<char>((dh << 4) | dl));
                }
                if (ok && !data.empty()) {
                    // Recv context: an inbound connect request is handled through
                    // the normal listen-socket state machine
                    // (CreateSignalingForConnection returns the reply channel).
                    // Rejections are silently ignored (returning failure lets an
                    // attacker scrape who is online).
                    struct Context : ISteamNetworkingSignalingRecvContext {
                        SignalingClient* owner = nullptr;
                        ISteamNetworkingConnectionSignaling* OnConnectRequest(
                            HSteamNetConnection hConn, const SteamNetworkingIdentity& peer,
                            int nLocalVirtualPort) override {
                            (void)hConn;
                            (void)nLocalVirtualPort;
                            return owner->CreateSignalingForConnection(peer);
                        }
                        void SendRejectionSignal(const SteamNetworkingIdentity& peer,
                                                 const void* pMsg, int cbMsg) override {
                            (void)peer;
                            (void)pMsg;
                            (void)cbMsg;
                        }
                    };
                    Context ctx;
                    ctx.owner = this;
                    sockets_->ReceivedP2PCustomSignal(
                        data.c_str(), static_cast<int>(data.size()), &ctx);
                }
            }
        }
        cursor = nl + 1;
    }

    // Drop consumed lines; keep any trailing partial line for the next Poll.
    // inBuf_ is net-thread-only, so no lock needed.
    if (cursor > 0) inBuf_.erase(0, cursor);
}

}  // namespace coop::net
