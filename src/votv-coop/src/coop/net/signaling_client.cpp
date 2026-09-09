// coop/net/signaling_client.cpp -- see signaling_client.h. Ported from GameNetworkingSockets'
// trivial_signaling_client example (BSD-3, Valve): namespaced, raw Winsock only, asserts
// replaced with logging and graceful failure (a malformed signal must never crash the game),
// and a self-contained WSAStartup, so the transport does not depend on GNS having initialised
// Winsock.

// Winsock before any header that may pull in windows.h (steamnetworkingtypes.h does).
#include <winsock2.h>
#include <ws2tcpip.h>

#include "signaling_client.h"

#include "coop/net/peer_identity.h"
#include "ue_wrap/core/log.h"

#include <cstring>
#include <utility>

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#pragma warning(pop)

// The DLL calls Winsock directly, so ws2_32 is linked here regardless of link-dep propagation
// from the static GNS lib.
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

// The inbound TCP stream is attacker-influenceable: the accumulation buffer is capped so a
// server or on-path attacker streaming bytes with no newline cannot grow it without bound. A
// legitimate ICE blob is a few KB; on overflow the connection is dropped and reconnected.
constexpr size_t kMaxInboundBuffer = 64 * 1024;

// The reconnect backoff: without it a down signaling server triggers a connect attempt every
// Poll.
constexpr auto kReconnectBackoff = std::chrono::seconds(5);

// How long to wait for the server's registration challenge after the greeting leaves the
// socket. Generous: it turns a relay that predates the challenge into one named error line
// instead of a silent hang. Matches the server's pre-auth budget.
constexpr auto kChallengeTimeout = std::chrono::seconds(15);

// Must equal REGISTER_TAG in server/src/bin/signaling.rs. The instrument that
// covers the pair is the p2p_smoke scenario, whose two peers sign with this code and register
// against the real relay: if the bytes drift, both fail to register and the verdict goes red.
// The release gate carries its own copy of the tag and never runs this client.
constexpr char kRegisterTag[] = "multivoid-signaling-register-v1";
constexpr char kChallengePrefix[] = "nonce ";
constexpr size_t kNonceHexLen = 64;
// gen: plus 64 hex; the relay accepts exactly this width, which is what makes the un-delimited
// blob unambiguous.
constexpr size_t kIdentityLen = 4 + 64;

const char kHexDigit[] = "0123456789abcdef";

}  // namespace

// The per-connection signaling object handed to GNS: SendSignal hex-encodes the opaque ICE
// blob, prefixes the destination identity and enqueues a line. GNS owns the object and calls
// Release when the connection no longer needs to signal.
struct SignalingClient::ConnectionSignaling : ISteamNetworkingConnectionSignaling {
    // A shared_ptr, so this object keeps the transport alive while GNS still owns it: Stop may run
    // before GNS has released every per-connection object.
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
        for (const uint8_t* p = static_cast<const uint8_t*>(pMsg); cbMsg > 0; --cbMsg, ++p) {
            signal.push_back(kHexDigit[*p >> 4U]);
            signal.push_back(kHexDigit[*p & 0xf]);
        }
        signal.push_back('\n');
        owner_->Enqueue(signal);
        return true;
    }

    void Release() override { delete this; }
};

// Construction and teardown.
std::shared_ptr<SignalingClient> SignalingClient::Create(const std::string& serverAddr,
                                                         const std::string& token,
                                                         ISteamNetworkingSockets* sockets) {
    if (!sockets) {
        UE_LOGE("signaling: Create() with null sockets");
        return nullptr;
    }
    std::string host = serverAddr;
    std::string service;
    // rfind, so a bracketed IPv6 literal's port colon is taken rather than an address colon; a
    // bare IPv6 address is not supported.
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
    // The private constructor is reachable here; the shared_ptr wires enable_shared_from_this.
    auto client = std::shared_ptr<SignalingClient>(
        new SignalingClient(std::move(host), std::move(service), token, sockets));
    // A partially initialised transport is rejected: without Winsock or a resolved address it can
    // never connect, so Start fails cleanly instead of looping on a dead socket.
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

    // The greeting is our own identity, set by ResetIdentity before Create; the server registers us
    // under this exact string, and a peer addresses it identically.
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
        // A whitespace token breaks the greeting framing, the server drops every greeting, and the
        // client would reconnect forever with no diagnostic; fail loudly instead.
        UE_LOGE("signaling: signaling token contains whitespace -- forbidden "
                "(check VOTVCOOP_NET_SIGNALING_TOKEN / net.signaling_token)");
        identityOk_ = false;
    }
    // The greeting: token, space, identity, newline. The server constant-time-compares the token
    // before registering us; an empty token is refused upstream.
    greeting_ = token_;
    greeting_.push_back(' ');
    greeting_.append(selfIdentity_);
    greeting_.push_back('\n');

    // The server address is resolved once here, on the constructing thread; reconnects reuse it,
    // so the blocking getaddrinfo never runs on the net thread.
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

// The socket lifecycle; the caller holds sockMutex_.
void SignalingClient::CloseSocketLocked() {
    if (sock_ != kInvalidSock) {
        closesocket(static_cast<SOCKET>(sock_));
        sock_ = kInvalidSock;
    }
    inBuf_.clear();
    // sendQueue_ is deliberately kept: pending GNS signals survive a reconnect, so a TCP blip
    // mid-handshake does not drop them; ConnectLocked re-inserts the greeting at the front, and
    // the Enqueue cap bounds the queue meanwhile.
}

// getaddrinfo once, on the constructing thread; the result is cached for reconnects.
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

    // A nonblocking connect returns would-block and completes asynchronously; queued lines flush
    // in Poll once writable.
    connect(s, reinterpret_cast<const sockaddr*>(resolvedAddr_), resolvedLen_);
    sock_ = static_cast<std::uintptr_t>(s);

    // The greeting must be the first line on every fresh socket; inserted at the front unless
    // already there, so repeated reconnects do not pile up duplicates ahead of the preserved
    // signals.
    if (sendQueue_.empty() || sendQueue_.front() != greeting_) {
        sendQueue_.push_front(greeting_);
    }

    // A reconnect re-greets, so the server issues a fresh nonce and a fresh proof is owed;
    // carrying the proof-sent state across a drop would skip a challenge about to arrive. The
    // deadline stays unarmed until the greeting leaves the socket: arming it now would time out an
    // unreachable server and blame it for not challenging.
    regState_ = RegState::AwaitingChallenge;
    greetingSent_ = false;
    challengeDeadline_ = std::chrono::steady_clock::time_point{};

    // A proof left over from the previous socket answers a nonce this server never issued, and the
    // relay refuses it with the same words a squat produces, which would make an own goal
    // indistinguishable from an attack in the one log meant to tell them apart. The queue is
    // preserved across a drop, so this is the one line that must not survive.
    for (auto it = sendQueue_.begin(); it != sendQueue_.end();) {
        it = (it->rfind("auth ", 0) == 0) ? sendQueue_.erase(it) : it + 1;
    }
    UE_LOGI("signaling: connecting to %s:%s as '%s'",
            host_.c_str(), service_.c_str(), selfIdentity_.c_str());
}

void SignalingClient::Enqueue(const std::string& line) {
    std::lock_guard<std::recursive_mutex> lk(sockMutex_);
    // Best-effort delivery: a backed-up queue drops the oldest signals, which are the most stale;
    // GNS retries current ones.
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

void SignalingClient::EnqueueFront(const std::string& line) {
    std::lock_guard<std::recursive_mutex> lk(sockMutex_);
    // No cap trim: the only caller is the registration proof, one line per socket, and dropping it
    // for an ICE signal would be backwards, since without the proof no signal is deliverable.
    sendQueue_.push_front(line);
}

// The per-connection signaling factory.
ISteamNetworkingConnectionSignaling* SignalingClient::CreateSignalingForConnection(
    const SteamNetworkingIdentity& peer) {
    SteamNetworkingIdentityRender peerRender(peer);
    UE_LOGI("signaling: creating signaling session for peer '%s'", peerRender.c_str());
    // shared_from_this co-owns the transport from the per-connection object; valid because the
    // object is always managed by the shared_ptr from Create.
    return new ConnectionSignaling(shared_from_this(), peerRender.c_str());
}

// The registration proof: sign the server's nonce with the key our identity names. See the
// header for why it is load-bearing.
bool SignalingClient::AnswerChallenge(const char* line, size_t len) {
    // A trailing CR is tolerated, so a relay behind a line-ending-normalising proxy is not a
    // protocol violation.
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) --len;

    constexpr size_t kPrefixLen = sizeof(kChallengePrefix) - 1;
    if (len != kPrefixLen + kNonceHexLen ||
        std::memcmp(line, kChallengePrefix, kPrefixLen) != 0) {
        UE_LOGE("signaling: expected a registration challenge and got a %zu-byte "
                "line that is not one -- this relay does not speak the "
                "nonce/auth exchange. REFUSING to register unproved.", len);
        return false;
    }
    const char* nonce = line + kPrefixLen;
    for (size_t i = 0; i < kNonceHexLen; ++i) {
        const char c = nonce[i];
        // Lowercase only, matching the server's alphabet: a proof must not be laxer about its
        // inputs than the name it proves.
        if (!(('0' <= c && c <= '9') || ('a' <= c && c <= 'f'))) {
            UE_LOGE("signaling: the registration challenge is not 64 lowercase "
                    "hex digits -- refusing to sign it");
            return false;
        }
    }

    // The blob is tag, identity, nonce with no separators: every field is fixed width, so the
    // concatenation is unambiguous. The identity's width is asserted, not assumed: nothing
    // upstream checks it (Create only rejects a spaced one), and signing an off-width identity
    // would produce a blob the relay cannot rebuild, so the honest failure is here.
    if (selfIdentity_.size() != kIdentityLen) {
        UE_LOGE("signaling: our identity is %zu chars, not %zu -- refusing to sign "
                "a blob the relay cannot rebuild", selfIdentity_.size(), kIdentityLen);
        return false;
    }
    std::string blob;
    blob.reserve(sizeof(kRegisterTag) - 1 + selfIdentity_.size() + kNonceHexLen);
    blob.append(kRegisterTag, sizeof(kRegisterTag) - 1);
    blob.append(selfIdentity_);
    blob.append(nonce, kNonceHexLen);

    const peer_identity::Sig sig = peer_identity::SignBlob(
        reinterpret_cast<const uint8_t*>(blob.data()), blob.size());

    std::string out;
    out.reserve(5 + sig.size() * 2 + 1);
    out.append("auth ");
    for (uint8_t b : sig) {
        out.push_back(kHexDigit[b >> 4U]);
        out.push_back(kHexDigit[b & 0xf]);
    }
    out.push_back('\n');
    // At the front: the relay reads the line after its challenge as the proof, and the queue may
    // already hold ICE signals GNS produced meanwhile.
    EnqueueFront(out);
    regState_ = RegState::ProofSent;
    // Answered, not accepted: the relay's verdict is not observable here. A rejected proof closes
    // the socket, which arrives as the ordinary closed-connection path; the reason lives in the
    // relay's log.
    UE_LOGI("signaling: answered the relay's registration challenge as '%s'",
            selfIdentity_.c_str());
    return true;
}

// Poll, on the net thread: drain inbound and dispatch, flush outbound, reconnect.
void SignalingClient::Poll() {
    {
        std::lock_guard<std::recursive_mutex> lk(sockMutex_);

        if (sock_ == kInvalidSock) {
            // Reconnect, backoff-gated; ConnectLocked does no DNS.
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

        // Flush the send queue: nonblocking, stop on would-block, retry next Poll.
        if (sock_ != kInvalidSock) {
            const SOCKET s = static_cast<SOCKET>(sock_);
            while (!sendQueue_.empty()) {
                // The proof must be the second line on the wire. Once the greeting is out nothing
                // is sent until the challenge is answered: the relay reads whatever comes next as
                // the proof, and a queued ICE signal overtaking it is a malformed proof and a
                // refused connection. GNS can enqueue one before the nonce round-trips (Create and
                // the P2P connect run back to back on one thread), and on loopback the nonce always
                // wins that race, so only a real-RTT relay shows it.
                if (regState_ == RegState::AwaitingChallenge && greetingSent_) break;
                const std::string& line = sendQueue_.front();
                const int l = static_cast<int>(line.size());
                const int r = ::send(s, line.c_str(), l, 0);
                if (r < 0 && IgnoreSockErr(WSAGetLastError())) break;  // would block
                if (r == l) {
                    sendQueue_.pop_front();
                    // The greeting is always the first line on a fresh socket, so the first
                    // successful send is the moment our greeting reached the server, from which a
                    // missing challenge means the relay is old rather than that we never got
                    // through.
                    if (!greetingSent_) {
                        greetingSent_ = true;
                        challengeDeadline_ = std::chrono::steady_clock::now() + kChallengeTimeout;
                    }
                } else {
                    UE_LOGW("signaling: send failed (r=%d/%d err=%d) -- reconnecting",
                            r, l, WSAGetLastError());
                    CloseSocketLocked();
                    break;
                }
            }
        }

        // Fail closed on a relay that never challenges: registering unproved would reopen what the
        // challenge closes, so the socket is dropped, the backoff retries, and each attempt prints
        // the one line an operator needs. Only P2P is affected.
        if (sock_ != kInvalidSock && regState_ == RegState::AwaitingChallenge &&
            challengeDeadline_ != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() > challengeDeadline_) {
            UE_LOGE("signaling: the relay at %s:%s never sent a registration "
                    "challenge -- it is older than this build and cannot verify "
                    "who registers a name. REFUSING to register unproved. Update "
                    "the signaling server (see docs/release.md). P2P is "
                    "unavailable; LAN and direct-IP are unaffected.",
                    host_.c_str(), service_.c_str());
            CloseSocketLocked();
        }
    }  // released before dispatch: ReceivedP2PCustomSignal takes a GNS lock a GNS thread may hold while calling SendSignal

    // Complete lines are dispatched from inBuf_ outside the lock: it is touched only on this
    // thread and Poll is not re-entrant, while SendSignal on GNS threads touches sendQueue_ only.
    // No scratch buffer, so the idle path allocates nothing.
    size_t cursor = 0;
    for (;;) {
        const size_t nl = inBuf_.find('\n', cursor);
        if (nl == std::string::npos) break;

        // Before registration the only line the server may send is its challenge, and no peer line
        // can arrive: the relay routes by looking us up in its map, which we are not in until the
        // proof lands, so a peer cannot forge a challenge here.
        if (regState_ == RegState::AwaitingChallenge) {
            if (!AnswerChallenge(inBuf_.data() + cursor, nl - cursor)) {
                std::lock_guard<std::recursive_mutex> lk(sockMutex_);
                CloseSocketLocked();  // clears inBuf_; nothing left to consume
                return;
            }
            cursor = nl + 1;
            continue;
        }

        // The line is [cursor, nl): from-identity, space, hex payload.
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
                        // Malformed hex from the server: drop the line, never crash.
                        UE_LOGW("signaling: bad hex in signal -- dropping line");
                        ok = false;
                        break;
                    }
                    data.push_back(static_cast<char>((dh << 4) | dl));
                }
                if (ok && !data.empty()) {
                    // The receive context: an inbound connect request goes through the normal
                    // listen-socket state machine, with CreateSignalingForConnection as the reply
                    // channel. Rejections are silently ignored, since returning a failure lets an
                    // attacker scrape who is online.
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

    // Consumed lines are dropped and a trailing partial line kept for the next Poll; inBuf_ is
    // net-thread-only.
    if (cursor > 0) inBuf_.erase(0, cursor);
}

}  // namespace coop::net
