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

#include "coop/net/peer_identity.h"
#include "ue_wrap/core/log.h"

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

// How long we wait for the server's registration challenge after our greeting
// leaves the socket. Generous on purpose: this deadline is not policing latency,
// it exists to turn "this relay predates the challenge" into ONE named error
// line instead of a silent hang. Matches the server's own pre-auth budget.
constexpr auto kChallengeTimeout = std::chrono::seconds(15);

// MUST equal `REGISTER_TAG` in tools/coop-server-rs/src/bin/signaling.rs.
//
// An earlier version of this comment said `tools/sig_gate.py` keeps the two
// honest. It does NOT, and a post-ship audit was right to call that a false
// reassurance: sig_gate carries its own THIRD copy of the tag and never runs this
// client, so changing the constant here leaves it at 14/14. The instrument that
// actually covers this leg is `p2p_smoke`, whose two peers sign with this code and
// register against the real relay -- if these bytes drift, both peers fail to
// register and the smoke's verdict goes red.
constexpr char kRegisterTag[] = "multivoid-signaling-register-v1";
constexpr char kChallengePrefix[] = "nonce ";
constexpr size_t kNonceHexLen = 64;
// `gen:` + 64 hex. The relay's `identity_shape_ok` accepts exactly this width, and
// it is what makes the un-delimited blob unambiguous.
constexpr size_t kIdentityLen = 4 + 64;

const char kHexDigit[] = "0123456789abcdef";

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

    // A reconnect re-greets, so the server will issue a FRESH nonce and we owe a
    // fresh proof: carrying ProofSent across a drop would make us skip a
    // challenge we are about to be sent. The deadline is left UNARMED here and
    // armed only once the greeting actually leaves the socket -- arming it now
    // would time out an unreachable server and blame it for "not challenging us",
    // which is a different fault with a different fix.
    regState_ = RegState::AwaitingChallenge;
    greetingSent_ = false;
    challengeDeadline_ = std::chrono::steady_clock::time_point{};

    // Drop any proof left over from the PREVIOUS socket. It answers a nonce this
    // server never issued, so the relay refuses it -- with the same words a SQUAT
    // produces ("does not hold the key this identity names"), which would make an
    // own-goal indistinguishable from an attack in the one log that is supposed to
    // tell them apart. The queue is deliberately preserved across a drop (see
    // CloseSocketLocked), so this is the one line that must NOT survive.
    for (auto it = sendQueue_.begin(); it != sendQueue_.end();) {
        it = (it->rfind("auth ", 0) == 0) ? sendQueue_.erase(it) : it + 1;
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

void SignalingClient::EnqueueFront(const std::string& line) {
    std::lock_guard<std::recursive_mutex> lk(sockMutex_);
    // No cap trim here: the only caller is the registration proof, exactly one
    // line per socket, and dropping IT to make room for an ICE signal would be
    // the wrong way round -- without the proof no signal is deliverable at all.
    sendQueue_.push_front(line);
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
// Registration proof (security A59): sign the server's nonce with the key our
// identity NAMES. See the header for why this is load-bearing rather than a
// ceremony.
// ---------------------------------------------------------------------------
bool SignalingClient::AnswerChallenge(const char* line, size_t len) {
    // Tolerate a trailing CR so a relay behind a line-ending-normalising proxy
    // does not read as a protocol violation.
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
        // Lowercase-only, matching the server's own alphabet: a proof must not be
        // laxer about its inputs than the name it proves.
        if (!(('0' <= c && c <= '9') || ('a' <= c && c <= 'f'))) {
            UE_LOGE("signaling: the registration challenge is not 64 lowercase "
                    "hex digits -- refusing to sign it");
            return false;
        }
    }

    // blob = tag || identity || nonce, no separators: every field is fixed width
    // by construction (the tag is a literal, our identity is `gen:` + 64 hex, the
    // nonce is 64 and checked above), so the concatenation cannot be ambiguous.
    //
    // ASSERTED, not assumed. Nothing upstream checks our own identity's WIDTH --
    // `Create()` only rejects a spaced one -- so "fixed width by construction" was
    // being enforced solely at the far end (post-ship audit). If it is ever not 68
    // characters, signing would produce a blob the relay cannot reconstruct, and
    // the honest failure is here rather than an unexplained refusal there.
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
    // FRONT, not back: the relay reads the line after its challenge as the proof,
    // and by now the queue may already hold ICE signals GNS produced while we were
    // waiting. Appending would put one of them where the proof belongs.
    EnqueueFront(out);
    regState_ = RegState::ProofSent;
    // "answered", not "accepted": the relay's verdict is not observable from
    // here. A rejected proof simply closes the socket, which arrives as the
    // ordinary "server closed connection" path -- the REASON lives in the relay's
    // log, which is where a refusal decision belongs.
    UE_LOGI("signaling: answered the relay's registration challenge as '%s'",
            selfIdentity_.c_str());
    return true;
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
                // THE PROOF MUST BE THE SECOND LINE ON THE WIRE. Once the greeting
                // is out we send NOTHING until the challenge is answered, because
                // the relay reads whatever comes next as the `auth` line: a queued
                // ICE signal overtaking it is read as a malformed proof and the
                // connection is REFUSED. GNS can enqueue one before the nonce
                // round-trips -- `Create()` and `ConnectP2PCustomSignaling` run
                // back to back on the same thread -- and on loopback the nonce
                // always wins that race, which is why every smoke here is blind to
                // it and only a real-RTT relay would show it. (post-ship audit H1)
                if (regState_ == RegState::AwaitingChallenge && greetingSent_) break;
                const std::string& line = sendQueue_.front();
                const int l = static_cast<int>(line.size());
                const int r = ::send(s, line.c_str(), l, 0);
                if (r < 0 && IgnoreSockErr(WSAGetLastError())) break;  // would block
                if (r == l) {
                    sendQueue_.pop_front();
                    // The greeting is always the first line on a fresh socket, so
                    // the first successful send IS "our greeting reached the
                    // server" -- the moment from which a missing challenge means
                    // the RELAY is old, rather than that we never got through.
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

        // Fail CLOSED on a relay that never challenges us. Registering unproved
        // would reopen exactly what the challenge closes (A59), so we drop the
        // socket instead -- the backoff retries, and each attempt prints the one
        // line an operator needs. Only P2P is affected; LAN and direct-IP never
        // reach a relay.
        if (sock_ != kInvalidSock && regState_ == RegState::AwaitingChallenge &&
            challengeDeadline_ != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() > challengeDeadline_) {
            UE_LOGE("signaling: the relay at %s:%s never sent a registration "
                    "challenge -- it is older than this build and cannot verify "
                    "who registers a name. REFUSING to register unproved. Update "
                    "the signaling server (see docs/RELEASE.md). P2P is "
                    "unavailable; LAN and direct-IP are unaffected.",
                    host_.c_str(), service_.c_str());
            CloseSocketLocked();
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

        // BEFORE REGISTRATION the only line the server may send is its challenge,
        // and no peer line can arrive at all: the relay routes by looking us up in
        // its map, and we are not in it until the proof lands. So this branch
        // needs no disambiguation against a relayed line -- a peer cannot forge a
        // challenge here because a peer cannot reach us here.
        if (regState_ == RegState::AwaitingChallenge) {
            if (!AnswerChallenge(inBuf_.data() + cursor, nl - cursor)) {
                std::lock_guard<std::recursive_mutex> lk(sockMutex_);
                CloseSocketLocked();  // clears inBuf_; nothing left to consume
                return;
            }
            cursor = nl + 1;
            continue;
        }

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
