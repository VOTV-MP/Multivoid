// coop/net/peer_admission.cpp -- see coop/net/peer_admission.h for WHY.

#include "coop/net/peer_admission.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/lobby_password.h"
#include "coop/net/session.h"
#include "ue_wrap/core/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingtypes.h>
#include <steam/isteamnetworkingsockets.h>
#pragma warning(pop)

#include <windows.h>   // GetTickCount64 -- the guess window's clock

#include <cstring>

namespace coop::net::peer_admission {

namespace {

using peer_identity::PubKey;
using peer_identity::Sig;

// The signed blob. FIXED SIZE on purpose: a length-prefixed or delimited encoding
// would need parsing on the verify side, and a parser is the last thing that
// should sit in front of an unauthenticated peer. Every field is a constant width
// known at compile time, so building it is a memcpy sequence with no branches.
//
//   [0 .. 26]  the tag, 27 bytes, no terminator
//   [27]       direction (kDirHost / kDirClient)
//   [28..29]   kProtocolVersion, little-endian
//   [30..61]   the HOST's public key
//   [62..93]   the CLIENT's public key
//   [94..125]  the VERIFIER's nonce
//   [126]      the challenge FLAGS
//   [127..158] the HOST's own nonce
//
// THE LAST TWO WERE ADDED 2026-08-31 AND THEY ARE NOT COSMETIC. `AuthChallengePayload`
// carries the flag that tells a joiner a password is wanted, and it was OUTSIDE the
// host's signature -- so a relay (the acknowledged P1 residual) could set
// `kAuthFlagPasswordRequired` on an OPEN lobby's challenge, with a nonce of its own
// choosing, and a bound client would derive and emit a real tag for a host that never
// asked. Today P1 buys impersonation; unsigned, it also bought a tag-harvesting oracle
// against a legitimate host. Being inside an existing residual is not a reason to widen
// it. (Post-ship audit.)
//
// Tampering the other way -- clearing the flag, or altering the host's nonce -- only
// denies: the host verifies the client's proof against the nonce IT stored.
constexpr char kTag[] = "multivoid-peer-admission-v2";
constexpr size_t kTagLen = sizeof(kTag) - 1;  // 27, terminator excluded
static_assert(kTagLen == 27, "the tag width is part of the blob layout");

constexpr uint8_t kDirHost   = 0x01;  // the host proving itself to the client
constexpr uint8_t kDirClient = 0x02;  // the client proving itself to the host

constexpr size_t kBlobBytes = kTagLen + 1 + 2 + peer_identity::kPubKeyBytes * 2 +
                              kAuthNonceBytes + 1 + kAuthNonceBytes;
static_assert(kBlobBytes == 159, "blob layout changed -- bump the tag, not just the code");

using Blob = uint8_t[kBlobBytes];

void BuildBlob(Blob out, uint8_t dir, const PubKey& hostPub, const PubKey& clientPub,
               const uint8_t nonce[kAuthNonceBytes], uint8_t flags,
               const uint8_t hostNonce[kAuthNonceBytes]) {
    size_t o = 0;
    std::memcpy(out + o, kTag, kTagLen);                       o += kTagLen;
    out[o++] = dir;
    // The protocol version is IN the blob so a signature cannot be carried across
    // a wire revision. It costs nothing -- the version gate already refuses a
    // cross-build pairing -- and it means a future change to any payload here
    // invalidates old signatures by construction rather than by remembering to.
    out[o++] = static_cast<uint8_t>(kProtocolVersion & 0xFF);
    out[o++] = static_cast<uint8_t>((kProtocolVersion >> 8) & 0xFF);
    std::memcpy(out + o, hostPub.data(), hostPub.size());      o += hostPub.size();
    std::memcpy(out + o, clientPub.data(), clientPub.size());  o += clientPub.size();
    std::memcpy(out + o, nonce, kAuthNonceBytes);              o += kAuthNonceBytes;
    out[o++] = flags;
    std::memcpy(out + o, hostNonce, kAuthNonceBytes);          o += kAuthNonceBytes;
    (void)o;  // == kBlobBytes; asserted by the static_assert above
}

// The remote's 32 identity bytes, read off the CONNECTION -- never off a packet.
// Returns false for any identity that is not exactly a 32-byte GenericBytes one,
// which is the same test as "this peer is not running an identity-bearing build":
// `[V]` a peer that presents nothing falls back to an IP-typed identity
// (`udp.cpp:338-357`), and an IP is not something anyone can sign for.
bool RemoteKeyOf(uint32_t hConn, PubKey& out) {
    auto* sockets = SteamNetworkingSockets();
    if (!sockets) return false;
    SteamNetConnectionInfo_t info{};
    if (!sockets->GetConnectionInfo(static_cast<HSteamNetConnection>(hConn), &info))
        return false;
    const SteamNetworkingIdentity& id = info.m_identityRemote;
    if (id.m_eType != k_ESteamNetworkingIdentityType_GenericBytes) return false;
    if (id.m_cbSize != peer_identity::kPubKeyBytes) return false;
    std::memcpy(out.data(), id.m_genericBytes, peer_identity::kPubKeyBytes);
    return true;
}

// The remote's ADDRESS, as the 16-byte form GNS stores (IPv4 arrives mapped).
// The PORT is deliberately excluded: a retrying attacker gets a fresh source port
// on every connection, so a bucket keyed on the pair would be a bucket of one.
// FALSE WHEN THERE IS NO USABLE ADDRESS, AND THAT IS THE COMMON CASE ON OUR MAIN LANE.
// The vendored header says so in as many words (`steamnetworkingtypes.h:690-692`):
// *"Remote address. Might be all 0's if we don't know it, or if this is N/A. (E.g.
// Basically everything except direct UDP connection.)"* -- and this tree had already
// written that fact down THREE times, including at `session_status.cpp:168-172`, where
// this very arc's own audit REJECTED a per-remote-address cap because *"the P2P
// Connecting edge has an empty remote address, so our main lane has nothing to key on"*.
// I keyed on it anyway. See the bound below for what that cost.
bool RemoteAddrOf(uint32_t hConn, uint8_t out[16]) {
    auto* sockets = SteamNetworkingSockets();
    if (!sockets) return false;
    SteamNetConnectionInfo_t info{};
    if (!sockets->GetConnectionInfo(static_cast<HSteamNetConnection>(hConn), &info))
        return false;
    static const uint8_t kZero[16] = {};
    if (std::memcmp(info.m_addrRemote.m_ipv6, kZero, 16) == 0) return false;
    std::memcpy(out, info.m_addrRemote.m_ipv6, 16);
    return true;
}

// --- HOST password state -----------------------------------------------------
//
// K IS DERIVED ONCE AND CACHED, and that is what makes the guess bound a policy
// rather than an accident of CPU cost. PBKDF2 at 200k rounds is ~100 ms; paying
// it per arriving attempt would mean an attacker could stall the net thread for
// every other peer just by connecting, and it would make the real limit "how fast
// is the host's CPU" instead of a number written down here. Cached, an attempt is
// one HMAC.
//
// Derived LAZILY, on the net thread, at the first attempt that needs it -- so
// there is no cross-thread hand-off to get wrong, and a host whose lobby is open
// never pays it at all. Keyed on the password string so a value that somehow
// changed cannot leave a stale key behind.
struct HostPasswordCache {
    std::string            forPassword;
    std::array<uint8_t, 32> key{};
    bool                   valid = false;
};
HostPasswordCache g_hostPw;

// THE GUESS BOUND. EVERY LANE IS COUNTED, AND NO TWO ATTACKERS EVER SHARE A BUCKET.
//
// IT HAS BEEN WRONG TWICE, IN OPPOSITE DIRECTIONS, AND BOTH ARE WORTH KNOWING.
//
// FIRST it keyed on the remote address and REFUSED when it could not bucket. `[V]`
// `steamnetworkingtypes.h:690-692`: the address "might be all 0's ... basically
// everything except direct UDP connection" -- so on AUTO/P2P, our main lane, every
// joiner shared ONE bucket and ten junk attempts locked the lobby against everybody.
//
// SECOND -- the fix for that -- it stopped bucketing when the address was unusable and
// said the lane's bound was "the master's own RL_JOIN, because every P2P attempt costs a
// /v1/join". **That is false.** `[V]` `/v1/join` returns `signalingToken` from
// `CFG.signaling_token` (`master.rs:301`), a STATIC process-wide value every mod user
// already holds, and `signaling.rs` has no rate limiter of any kind. One /v1/join buys
// unlimited re-dials. `RL_JOIN` bounds DISCOVERY, not attempts -- so the main lane had no
// counter at all, which is worse for the thing this protects than the lockout was.
//
// SO THE KEY IS WHATEVER IDENTIFIES THE ATTEMPTER ON THIS LANE:
//   * a real remote address (direct UDP / LAN), or
//   * the peer's PROVED public key when the address is absent (P2P/ICE, relay).
// The key is only reached AFTER the Ed25519 proof verifies, so it is a key the peer
// demonstrably holds. An attacker can rotate keypairs -- but each rotation costs a fresh
// connection, a full ICE negotiation and a complete admission exchange, and it can never
// take an honest player's bucket with it. **A weak per-attacker bound beats a strong
// shared one, because a shared bucket is a lockout wearing a limiter's clothes.**
//
// AND EXHAUSTION STILL DOES NOT REFUSE. A full table means we stop COUNTING, not that we
// stop CHECKING -- the tag is still verified and a wrong one still refused. Failing closed
// is correct when failing open would ADMIT; here it could only ever DENY
// (`[[lesson-fail-closed-is-right-only-when-failing-open-would-admit]]`). With a
// per-identity key the table is far harder to flood than it was per-address, because every
// row now costs a full handshake.
//
// THE COLLATERAL, priced honestly: peers behind ONE carrier NAT share an address bucket,
// so ten wrong guesses between them locks that address out for a minute. Ten is far above
// what typing costs an honest group and far below a search of the generated password, which
// is 30 bits since 2026-09-01 (it was 50; `host_session_settings.cpp` kPwLen went 10 -> 6 at
// the user's request). The conclusion survives the shortening -- 2^30 against this ceiling
// is still years -- but the number it reasons FROM had to move with it, and this was the
// third site holding it while the other two were updated.
enum class GuessKeyKind : uint8_t { Addr = 1, Ident = 2 };
struct GuessBucket {
    uint8_t      key[16]{};
    GuessKeyKind kind = GuessKeyKind::Addr;
    bool         used = false;
    int          fails = 0;
    uint64_t     windowStartMs = 0;
};
constexpr int      kGuessBuckets  = 32;
constexpr int      kMaxGuesses    = 10;
constexpr uint64_t kGuessWindowMs = 60'000;
GuessBucket g_guess[kGuessBuckets];

// Find (or claim) this address's bucket. Null when the table is full of LIVE
// windows -- and the caller then REFUSES, because a full table is the state a
// flood produces and failing open there would hand an attacker the whole point.
GuessBucket* BucketFor(const uint8_t key[16], GuessKeyKind kind, uint64_t nowMs) {
    GuessBucket* freeRow = nullptr;
    for (auto& b : g_guess) {
        // THE KIND IS PART OF THE KEY. Without it an address could collide with the first
        // 16 bytes of somebody's public key -- vanishingly unlikely, and free to exclude.
        if (b.used && b.kind == kind && std::memcmp(b.key, key, 16) == 0) {
            if (nowMs - b.windowStartMs >= kGuessWindowMs) {
                b.windowStartMs = nowMs;
                b.fails = 0;
            }
            return &b;
        }
        // An EXPIRED row is reusable; a free one is better. Both are collected in
        // one pass so a table full of stale windows never reads as full.
        if (!freeRow && (!b.used || nowMs - b.windowStartMs >= kGuessWindowMs))
            freeRow = &b;
    }
    if (!freeRow) return nullptr;
    *freeRow = GuessBucket{};
    std::memcpy(freeRow->key, key, 16);
    freeRow->kind = kind;
    freeRow->used = true;
    freeRow->windowStartMs = nowMs;
    return freeRow;
}

// --- HOST state -------------------------------------------------------------
// One row per pending band entry. Indexed by the pending index, which the band
// itself allocates and recycles, so HostForgetPending is called on EVERY exit
// path (admit and close alike) -- a recycled index inheriting a live nonce would
// let a new socket answer the previous socket's challenge.
struct HostRow {
    bool     open = false;
    uint32_t hConn = 0;
    uint8_t  nonce[kAuthNonceBytes]{};
    // The flags we SENT. Kept because they are inside both signatures now, so verifying
    // the client's proof means rebuilding the blob we challenged with -- from what we
    // sent, never from anything that comes back.
    uint8_t  flags = 0;
    PubKey   remotePub{};
};
// DERIVED from the band, not asserted against it. The first cut wrote `= 8` with
// a comment claiming the mismatch was "asserted in the .cpp below" -- and no such
// assertion existed, which is the same defect this very arc fixed one level up
// (`pendingSinceMs_` was a bound that lived only in a comment). Raising
// kMaxPending now widens this array by construction; there is nothing left to
// keep in step, so there is nothing to assert. Post-ship audit, 2026-08-29.
constexpr int kMaxHostRows = Session::kMaxPending;
HostRow g_host[kMaxHostRows];

// --- CLIENT state -----------------------------------------------------------
struct ClientRow {
    bool     open = false;
    bool     proved = false;
    // Did the key on the socket MATCH the identity we were sent to dial? False on a
    // lane that advertises no identity (LAN, plain UDP), where there is nothing to
    // match against -- which is a different thing from a mismatch, and the reason
    // this is a fact and not a verdict. See ClientOnConnected.
    bool     bound = false;
    uint32_t hConn = 0;
    uint8_t  nonce[kAuthNonceBytes]{};
    PubKey   hostPub{};
    // The drill knob, resolved ONCE when the link opens. `config::ResolveEnum`
    // opens and line-scans multivoid.ini under a global mutex, and reading it
    // where it is USED would put blocking file I/O on the net thread between
    // "verified the host" and "sent our proof" -- on every join, for a knob that
    // is off by default. Post-ship audit, 2026-08-29.
    std::string drill;
};
ClientRow g_client;

}  // namespace

// ---------------------------------------------------------------------------
// HOST
// ---------------------------------------------------------------------------

bool HostHasOpenExchange(int pendIdx) {
    if (pendIdx < 0 || pendIdx >= kMaxHostRows) return false;
    return g_host[pendIdx].open;
}

void HostForgetPending(int pendIdx) {
    if (pendIdx < 0 || pendIdx >= kMaxHostRows) return;
    g_host[pendIdx] = HostRow{};
}

HostResult HostOnPendingReliable(Session& session, int pendIdx, uint32_t hConn,
                                 ReliableKind kind, const void* payload, int len) {
    HostResult r;
    if (pendIdx < 0 || pendIdx >= kMaxHostRows) {
        r.reason = "pending index out of range";
        return r;
    }
    HostRow& row = g_host[pendIdx];
    // A recycled index: the band handed this slot to a different connection while
    // a row was still open. Start clean rather than answering with the old nonce.
    if (row.open && row.hConn != hConn) row = HostRow{};

    switch (kind) {
    case ReliableKind::AuthHello: {
        if (len != static_cast<int>(sizeof(AuthHelloPayload))) {
            r.reason = "malformed AuthHello";
            return r;
        }
        // ONE hello per connection. A second one would let a peer re-roll the
        // host's nonce after seeing a challenge, which buys nothing today but is
        // the shape of a downgrade, and refusing it costs an honest client
        // nothing (it sends exactly one).
        if (row.open) {
            r.reason = "duplicate AuthHello";
            return r;
        }
        if (!RemoteKeyOf(hConn, row.remotePub)) {
            r.reason = "peer presented no key identity";
            return r;
        }
        AuthHelloPayload hello{};
        std::memcpy(&hello, payload, sizeof(hello));

        AuthChallengePayload out{};
        // TELL THE JOINER WHAT WE REQUIRE. It cannot infer this: a DIRECT or LAN
        // connect has no browser row, and a client that guessed would either
        // withhold a required proof or emit one to a host that never asked -- and
        // the second of those is exactly the emission `lobby_password.h` forbids.
        if (!session.LobbyPassword().empty()) out.flags |= kAuthFlagPasswordRequired;
        if (!peer_identity::RandomBytes(out.nonce, sizeof(out.nonce))) {
            // No randomness means no freshness, and a predictable nonce is worse
            // than no exchange because it LOOKS like one. Refuse loudly.
            UE_LOGE("peer_admission: the OS refused randomness -- cannot challenge");
            r.reason = "host has no randomness";
            return r;
        }
        std::memcpy(row.nonce, out.nonce, sizeof(row.nonce));
        row.flags = out.flags;

        // We prove ourselves FIRST, over the CLIENT's nonce.
        Blob blob;
        BuildBlob(blob, kDirHost, peer_identity::LocalPublicKey(), row.remotePub,
                  hello.nonce, out.flags, out.nonce);
        const Sig sig = peer_identity::SignBlob(blob, sizeof(blob));
        std::memcpy(out.sig, sig.data(), sig.size());

        if (!session.SendRawReliableToConn(hConn, ReliableKind::AuthChallenge,
                                           &out, sizeof(out))) {
            r.reason = "could not send the challenge";
            return r;
        }
        row.open = true;
        row.hConn = hConn;
        r.verdict = Verdict::Continue;
        r.reason = "challenged";
        return r;
    }

    case ReliableKind::AuthProof: {
        if (!row.open) {
            r.reason = "AuthProof before AuthHello";
            return r;
        }
        if (len != static_cast<int>(sizeof(AuthProofPayload))) {
            r.reason = "malformed AuthProof";
            return r;
        }
        // The key is re-read from the connection rather than trusted from the
        // Hello step: it costs one call and it means the decision below rests on
        // what GNS says NOW about the socket in hand.
        PubKey nowPub{};
        if (!RemoteKeyOf(hConn, nowPub) || nowPub != row.remotePub) {
            r.reason = "peer identity changed mid-exchange";
            return r;
        }
        AuthProofPayload proof{};
        std::memcpy(&proof, payload, sizeof(proof));
        Sig sig{};
        std::memcpy(sig.data(), proof.sig, sig.size());

        Blob blob;
        BuildBlob(blob, kDirClient, peer_identity::LocalPublicKey(), row.remotePub,
                  row.nonce, row.flags, row.nonce);
        if (!peer_identity::VerifyBlob(row.remotePub, blob, sizeof(blob), sig)) {
            r.reason = "identity proof did not verify";
            return r;
        }

        // ---- THE LOBBY PASSWORD ------------------------------------------------
        // AFTER the identity, never before: the tag is bound to the peer's public
        // key, so checking it against a key that has not been proved would be
        // checking it against a claim.
        const std::string& want = session.LobbyPassword();
        if (!want.empty()) {
            if (!proof.hasPw) {
                // A DISTINCT REASON, because the client shows this one to a
                // person. "wrong password" for a client that sent none would send
                // them looking for a typo in a box they never filled in.
                r.reason = "this server needs a password";
                return r;
            }
            // THE BOUND, WHERE ONE EXISTS. Checked before the HMAC so a flood costs
            // the comparison and not the crypto -- but a MISSING bucket (no usable
            // address, or a full table) is not a refusal: see the contract above.
            // Both of those used to deny honest joiners while admitting nobody.
            // THE KEY: the real address where there is one, otherwise the key this peer
            // has just PROVED it holds. Never nothing, and never shared -- see the
            // contract above for why both halves of that matter.
            uint8_t key[16]{};
            GuessKeyKind kind = GuessKeyKind::Addr;
            if (!RemoteAddrOf(hConn, key)) {
                std::memcpy(key, row.remotePub.data(), sizeof(key));
                kind = GuessKeyKind::Ident;
            }
            const uint64_t nowMs = ::GetTickCount64();
            GuessBucket* bucket = BucketFor(key, kind, nowMs);
            if (bucket && bucket->fails >= kMaxGuesses) {
                UE_LOGW("peer_admission: password attempts from this address are rate "
                        "limited (%d in the last %llu s) -- refusing without checking",
                        bucket->fails,
                        static_cast<unsigned long long>(kGuessWindowMs / 1000));
                r.reason = "too many password attempts -- try again in a minute";
                return r;
            }

            if (!g_hostPw.valid || g_hostPw.forPassword != want) {
                g_hostPw.valid = lobby_password::DeriveKey(
                    want, peer_identity::LocalPublicKey(), g_hostPw.key);
                g_hostPw.forPassword = want;
                if (!g_hostPw.valid) {
                    // We cannot check, so we cannot admit. Refusing everyone is the
                    // correct failure for a lock whose key we cannot compute -- the
                    // alternative is a lobby that silently stops being locked.
                    UE_LOGE("peer_admission: could not derive the lobby key -- refusing "
                            "every join rather than silently unlocking the session");
                    r.reason = "the host could not check the password";
                    return r;
                }
            }

            lobby_password::Tag expect{};
            if (!lobby_password::ComputeTag(g_hostPw.key, peer_identity::LocalPublicKey(),
                                            row.remotePub, row.nonce, expect)) {
                r.reason = "the host could not check the password";
                return r;
            }
            lobby_password::Tag got{};
            std::memcpy(got.data(), proof.pwTag, got.size());
            if (!lobby_password::TagsEqual(expect, got)) {
                if (bucket) ++bucket->fails;
                UE_LOGW("peer_admission: WRONG PASSWORD from a peer that proved its "
                        "identity (attempt %d of %d from this address%s)",
                        bucket ? bucket->fails : 0, kMaxGuesses,
                        bucket ? (kind == GuessKeyKind::Addr ? "" : "; keyed on the proved "
                                                                   "identity, not an address")
                               : "; the bucket table is FULL -- this attempt was checked "
                                 "but not counted");
                r.reason = "wrong password";
                return r;
            }
        }

        r.verdict = Verdict::Admit;
        r.reason = "identity proved";
        r.provedKey = row.remotePub;
        return r;
    }

    default:
        // Anything else before admission is a protocol violation by this build's
        // own client, which sends AuthHello and then nothing until it is seated.
        // Refusing rather than dropping is deliberate: a peer that is never going
        // to be admitted should learn so, exactly as the protocol-mismatch close
        // beside this one already does, and a silent drop is what made the first
        // admission gate deadlock every honest join.
        r.reason = "spoke before proving its identity";
        return r;
    }
}

// ---------------------------------------------------------------------------
// CLIENT
// ---------------------------------------------------------------------------

void ClientReset() { g_client = ClientRow{}; }

bool ClientProvedHost() { return g_client.open && g_client.proved; }

bool ClientOnConnected(Session& session, uint32_t hConn) {
    g_client = ClientRow{};
    if (peer_identity::LocalIdentityString().empty()) {
        UE_LOGE("peer_admission: no local identity -- cannot open the exchange");
        return false;
    }
    if (!RemoteKeyOf(hConn, g_client.hostPub)) {
        UE_LOGE("peer_admission: the host presented no key identity -- refusing to join "
                "(an older build, or something in the middle)");
        return false;
    }

    // THE BINDING, AND WITHOUT IT THE WHOLE EXCHANGE PROVES NOTHING ON THIS SIDE
    // (security A65, fixed 2026-08-31).
    //
    // The key above comes off the SOCKET. Verifying the host against it -- which is
    // all this module used to do -- asks "does whoever answered hold the key
    // whoever answered presented", and every host on earth passes that. The failure
    // message even said "the host did not prove the identity it ADVERTISED", while
    // nothing advertised reached the verifier: `hostIdentity` appeared nowhere in
    // this file. It is the project's own false-security-comment class, in security
    // code.
    //
    // What we came here for is `cfg.hostIdentity` -- the `gen:<64 hex>` the master
    // returned from /v1/join, or the one a friend put in a direct invite. Compared
    // BYTE-WISE after parsing, never as strings.
    //
    // AN EMPTY ADVERTISED IDENTITY IS NOT A FAILURE and must not become one: the LAN
    // and plain-UDP lanes dial an address, not a name, and there is nothing to bind
    // to. Those lanes are exactly where a password's own binding has to carry the
    // weight instead, which is why this returns a fact rather than a verdict.
    g_client.drill = coop::config::ResolveEnum(coop::config_registry::rows::auth_drill);

    const std::string& advertised = session.AdvertisedHostIdentity();
    peer_identity::PubKey want{};
    bool haveWant = false;
    if (g_client.drill == "mismatch") {
        // SYNTHESIZED, because the drill must be runnable on the lane the rig
        // actually uses. `mp.py authdrill` is a LAN run and LAN advertises no
        // identity at all, so an arm that only fired when one was present would be
        // green on every rig we own -- an instrument blind to the axis it grades.
        // One flipped bit is a key that provably is not the one on this socket.
        want = g_client.hostPub;
        want[0] ^= 0x01;
        haveWant = true;
        UE_LOGW("peer_admission: DRILL 'mismatch' -- pretending we were sent to a "
                "different host than the one that answered. THIS peer must refuse, "
                "before it sends anything at all.");
    } else if (!advertised.empty()) {
        if (!peer_identity::PublicKeyFromIdentityString(advertised, want)) {
            UE_LOGE("peer_admission: the advertised host identity is not a key identity "
                    "(%zu chars) -- refusing to join rather than dialling something this "
                    "build cannot name", advertised.size());
            return false;
        }
        haveWant = true;
    }
    if (haveWant) {
        if (want != g_client.hostPub) {
            UE_LOGE("peer_admission: the host on this socket is NOT the host we were sent "
                    "to. Advertised guid %s, answered %s -- refusing.",
                    peer_identity::GuidForPublicKey(want).c_str(),
                    peer_identity::GuidForPublicKey(g_client.hostPub).c_str());
            return false;
        }
        g_client.bound = true;
    } else {
        // THE SECOND SENTENCE USED TO SAY "Nothing password-derived may be sent to it",
        // and it fired on every direct connect. That stopped being true the moment a
        // self-addressed lane was allowed to carry a password, so it is now stated as the
        // conditional it actually is.
        UE_LOGW("peer_admission: no advertised host identity on this lane -- the exchange "
                "can prove the host holds its own key, but not that it is the host you "
                "meant. A password may be sent only if this destination was named locally.");
    }
    AuthHelloPayload hello{};
    if (!peer_identity::RandomBytes(hello.nonce, sizeof(hello.nonce))) {
        UE_LOGE("peer_admission: the OS refused randomness -- cannot open the exchange");
        return false;
    }
    std::memcpy(g_client.nonce, hello.nonce, sizeof(g_client.nonce));
    if (!session.SendRawReliableToConn(hConn, ReliableKind::AuthHello,
                                       &hello, sizeof(hello))) {
        UE_LOGE("peer_admission: could not send AuthHello");
        return false;
    }
    g_client.open = true;
    g_client.hConn = hConn;
    UE_LOGI("peer_admission: sent AuthHello -- waiting for the host to prove itself");
    return true;
}

bool ClientOnReliable(Session& session, uint32_t hConn, ReliableKind kind,
                      const void* payload, int len, const char** outClose) {
    if (kind != ReliableKind::AuthChallenge) return false;  // not ours
    if (!g_client.open || g_client.hConn != hConn) {
        *outClose = "unexpected AuthChallenge";
        return true;
    }
    if (g_client.proved) {
        // A second challenge would re-open a settled decision on a connection we
        // have already committed to.
        *outClose = "duplicate AuthChallenge";
        return true;
    }
    if (len != static_cast<int>(sizeof(AuthChallengePayload))) {
        *outClose = "malformed AuthChallenge";
        return true;
    }
    AuthChallengePayload ch{};
    std::memcpy(&ch, payload, sizeof(ch));

    // Verify the HOST over OUR nonce, against the identity bytes on this socket.
    Blob blob;
    BuildBlob(blob, kDirHost, g_client.hostPub, peer_identity::LocalPublicKey(),
              g_client.nonce, ch.flags, ch.nonce);
    Sig hostSig{};
    std::memcpy(hostSig.data(), ch.sig, hostSig.size());
    if (!peer_identity::VerifyBlob(g_client.hostPub, blob, sizeof(blob), hostSig)) {
        // TWO REASONS, because they are two different events and telling them apart
        // is the whole of A65. BOUND means we already checked this key IS the one we
        // were sent to, so a bad signature here is a host that cannot back its own
        // advertised name. UNBOUND means nobody advertised anything, so all this
        // could ever have shown is that the answerer holds the key it presented --
        // which is what the old, single message claimed either way.
        *outClose = g_client.bound
                        ? "the host did not prove the identity it advertised"
                        : "the host did not prove the key it presented (no identity was "
                          "advertised on this lane, so there was nothing to bind to)";
        return true;
    }

    // ...then prove ourselves over THEIRS.
    AuthProofPayload out{};
    Blob mine;
    BuildBlob(mine, kDirClient, g_client.hostPub, peer_identity::LocalPublicKey(),
              ch.nonce, ch.flags, ch.nonce);
    const Sig sig = peer_identity::SignBlob(mine, sizeof(mine));
    std::memcpy(out.sig, sig.data(), sig.size());

    // ---- THE LOBBY PASSWORD, IF THIS HOST ASKED FOR ONE ---------------------
    //
    // THE BINDING GATE IS THE SECURITY, NOT THE KDF. A tag is a value derived
    // from a low-entropy secret; hand one to a host we have not established is
    // the host we were SENT to, and that host can grind it offline for as long
    // as it likes, at which point nothing we choose here matters. So an unbound
    // lane does not get a weaker proof or a warning -- it gets nothing, and the
    // join fails with a sentence that says why (`lobby_password.h`; A65 is what
    // makes `bound` mean anything).
    if (ch.flags & kAuthFlagPasswordRequired) {
        // BOUND, **OR** THE PLAYER TYPED THE ADDRESS THEMSELVES (user decision,
        // 2026-09-01: "Вводит адрес и порт... или заполняет поле пароля если он был выдан
        // хостом"). Until then this refused, and a locked host was unjoinable by address
        // from every shipped UI -- which is not a safety property, it is a missing feature
        // wearing one.
        //
        // WHY THIS IS NOT THE GATE COLLAPSING. The rule it amends (the A2 design pass) is
        // that a low-entropy secret must never enter a signature whose other terms the
        // verifier controls -- gated behind binding, or PAKE-shaped, or not a password.
        // Binding answers "is this the host the MASTER sent me to", and it is exactly the
        // right question when a third party named the destination. On a typed address
        // there is no third party: the player IS the authority on where they meant to go,
        // and there is nothing further to bind against.
        //
        // WHO CAN ACTUALLY EXPLOIT IT, measured rather than assumed: only whoever answers
        // at the address this machine named instead of the intended host, which requires a
        // network position this branch is not what stands between them and. The transport's
        // attacker model is recorded in the security register (not in this tree); the part
        // that decides THIS branch is that refusing here does not deny that position
        // anything it does not already reach.
        //
        // What refusing DID buy is narrower, and worth naming because it is a real cost:
        // the TYPO case -- a mistyped address answered by an unrelated host, which then
        // learns a six-character password to a lobby it cannot find. That is the honest
        // price of the feature, and it is stated rather than hidden.
        //
        // NOTHING ELSE RELAXES. The tag is still bound to the key that answered, so it is
        // not replayable to the real host; the host still verifies identity BEFORE looking
        // at the password; and the host's 10-guesses-per-60s bucket still bounds online
        // guessing. What changed is one branch, on one lane, for one reason.
        if (!g_client.bound && !session.DestinationIsSelfAddressed()) {
            *outClose = "this server wants a password, but nothing told us which host we "
                        "were dialling -- refusing to send anything derived from it";
            return true;
        }
        const std::string& pw = session.LobbyPassword();
        if (pw.empty()) {
            // NOT A PROTOCOL ERROR -- a person forgot, or was never given one. The
            // sentence is what the join screen shows them.
            *outClose = "this server needs a password";
            return true;
        }
        std::array<uint8_t, 32> key{};
        lobby_password::Tag tag{};
        if (!lobby_password::DeriveKey(pw, g_client.hostPub, key) ||
            !lobby_password::ComputeTag(key, g_client.hostPub,
                                        peer_identity::LocalPublicKey(), ch.nonce, tag)) {
            *outClose = "could not compute the password proof on this machine";
            return true;
        }
        std::memcpy(out.pwTag, tag.data(), tag.size());
        out.hasPw = 1;
    }

    // THE DRILL, and it lives on this side ONLY. The host's gate has no knob to
    // turn: a bypass there would make the drill's verdict a statement about the
    // bypass. Here it sabotages a real proof travelling the real path.
    const std::string& drill = g_client.drill;  // resolved at ClientOnConnected
    if (drill == "silent") {
        UE_LOGW("peer_admission: DRILL 'silent' -- verified the host and then sending "
                "NO proof. The host must close us on its pending deadline and we must "
                "never take a seat.");
        g_client.proved = false;
        return true;
    }
    if (drill == "corrupt") {
        out.sig[0] ^= 0x01;
        UE_LOGW("peer_admission: DRILL 'corrupt' -- flipping one bit of our proof. The "
                "host must REFUSE us and we must never receive the save.");
    }

    if (!session.SendRawReliableToConn(hConn, ReliableKind::AuthProof,
                                       &out, sizeof(out))) {
        *outClose = "could not send the identity proof";
        return true;
    }
    if (drill == "corrupt") {
        // A corrupted proof must NOT set `proved`: if the host somehow seated us
        // anyway, the client's own AssignPeerSlot gate must be the second thing
        // that refuses -- the drill tests both halves or it tests one.
        return true;
    }
    g_client.proved = true;
    UE_LOGI("peer_admission: host identity VERIFIED (guid %s) -- proof sent, awaiting "
            "the seat",
            peer_identity::GuidForPublicKey(g_client.hostPub).c_str());
    return true;
}

// ---------------------------------------------------------------------------
// SELFTEST
// ---------------------------------------------------------------------------

bool RunSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("peer_admission selftest FAIL: %s", what);
    };

    // Two synthetic COUNTERPARTY identities. They are drawn as raw bytes, not
    // derived from a keypair, and that is correct rather than lazy: the blob
    // embeds 32 identity bytes verbatim and never interprets them, so what the
    // negatives need is two values that DIFFER. Signing is done with the module's
    // own real key, which gives the asymmetry the negatives test.
    //
    // The first cut of this DID declare a keypair and then forget to derive the
    // public halves, leaving pkA and pkB both all-zero -- so the third-party arm
    // compared two byte-identical blobs and failed on both peers, reporting a
    // product defect that did not exist. Two inputs to a difference test being
    // accidentally equal is the failure mode a negative arm is most prone to, so
    // they are asserted distinct below rather than assumed.
    PubKey pkA{}, pkB{};
    if (!peer_identity::RandomBytes(pkA.data(), pkA.size()) ||
        !peer_identity::RandomBytes(pkB.data(), pkB.size())) {
        UE_LOGE("peer_admission selftest: no randomness -- cannot run");
        return false;
    }
    check(pkA != pkB, "the two synthetic counterparties are the SAME -- the "
                      "third-party arm below would prove nothing");
    const PubKey& mine = peer_identity::LocalPublicKey();
    uint8_t nonce1[kAuthNonceBytes]{}, nonce2[kAuthNonceBytes]{};
    check(peer_identity::RandomBytes(nonce1, sizeof(nonce1)) &&
          peer_identity::RandomBytes(nonce2, sizeof(nonce2)),
          "could not draw two nonces");
    check(std::memcmp(nonce1, nonce2, sizeof(nonce1)) != 0,
          "two draws produced the SAME nonce -- the RNG is not one");

    // POSITIVE: we sign as the client, and a host holding our identity accepts.
    Blob asClient;
    // THE TWO NONCE SLOTS GET DIFFERENT VALUES, so the layout assert below can actually
    // see one of them go missing. Every arm used to pass `nonce1` twice, which made a
    // BuildBlob that stopped writing the VERIFIER nonce indistinguishable from a correct
    // one at the byte level (audit, 2026-08-31).
    BuildBlob(asClient, kDirClient, pkA, mine, nonce1, kAuthFlagPasswordRequired, nonce2);
    const Sig sigClient = peer_identity::SignBlob(asClient, sizeof(asClient));
    check(peer_identity::VerifyBlob(mine, asClient, sizeof(asClient), sigClient),
          "a well-formed client proof did not verify");

    // NEGATIVE 1 -- REFLECTION. The host's own challenge must not be replayable as
    // the client's proof. Only the direction byte differs, so this is the arm that
    // fails if the direction is ever dropped from the blob.
    Blob asHost;
    BuildBlob(asHost, kDirHost, pkA, mine, nonce1, kAuthFlagPasswordRequired, nonce2);
    check(!peer_identity::VerifyBlob(mine, asHost, sizeof(asHost), sigClient),
          "a proof verified in the WRONG DIRECTION (the direction byte is not "
          "reaching the blob)");

    // NEGATIVE 2 -- THIRD PARTY. The same signature must not verify inside a blob
    // naming a different counterparty; this is what stops a proof given to host A
    // being relayed into host B's exchange.
    Blob otherHost;
    BuildBlob(otherHost, kDirClient, pkB, mine, nonce1, kAuthFlagPasswordRequired, nonce2);
    check(!peer_identity::VerifyBlob(mine, otherHost, sizeof(otherHost), sigClient),
          "a proof verified against a DIFFERENT counterparty (both identities are "
          "not reaching the blob)");

    // NEGATIVE 3 -- FRESHNESS. A recorded proof must not answer a new challenge.
    Blob otherNonce;
    BuildBlob(otherNonce, kDirClient, pkA, mine, nonce2, kAuthFlagPasswordRequired, nonce2);
    check(!peer_identity::VerifyBlob(mine, otherNonce, sizeof(otherNonce), sigClient),
          "a proof verified against a DIFFERENT nonce (the exchange is replayable)");

    // NEGATIVE 5 -- THE FLAGS ARE SIGNED. This is the arm that fails if the challenge's
    // flag byte ever leaves the blob again. Unsigned, a relay could set "password
    // required" on an OPEN lobby's challenge and a bound client would derive and emit a
    // real tag for a host that never asked for one -- turning an impersonation residual
    // into a tag-harvesting oracle (post-ship audit, 2026-08-31).
    Blob otherFlags;
    BuildBlob(otherFlags, kDirClient, pkA, mine, nonce1, 0, nonce2);
    check(!peer_identity::VerifyBlob(mine, otherFlags, sizeof(otherFlags), sigClient),
          "a proof verified with DIFFERENT challenge flags (the flag byte is not "
          "reaching the blob -- a relay can ask an open host's joiner for a password)");

    // NEGATIVE 6 -- and so is the host's own nonce.
    Blob otherHostNonce;
    BuildBlob(otherHostNonce, kDirClient, pkA, mine, nonce1, kAuthFlagPasswordRequired, nonce1);
    check(!peer_identity::VerifyBlob(mine, otherHostNonce, sizeof(otherHostNonce), sigClient),
          "a proof verified against a DIFFERENT host nonce");

    // NEGATIVE 4 -- WRONG SIGNER. The decision the whole module exports: a
    // signature made by us must not verify against somebody else's identity.
    check(!peer_identity::VerifyBlob(pkA, asClient, sizeof(asClient), sigClient),
          "a proof verified against SOMEONE ELSE'S identity");

    // The blob is a fixed layout; assert the two facts a future edit could break
    // silently -- a shortened tag, or a field that stopped being copied.
    check(asClient[kTagLen] == kDirClient && asHost[kTagLen] == kDirHost,
          "the direction byte is not where the layout says it is");
    // BOTH nonce positions, and they now carry DIFFERENT values -- so a BuildBlob that
    // stopped writing either one is caught here rather than passing because the two slots
    // happened to hold the same bytes.
    check(std::memcmp(asClient + kBlobBytes - kAuthNonceBytes, nonce2,
                      kAuthNonceBytes) == 0,
          "the HOST nonce is not at the end of the blob");
    check(std::memcmp(asClient + kBlobBytes - kAuthNonceBytes - 1 - kAuthNonceBytes,
                      nonce1, kAuthNonceBytes) == 0,
          "the VERIFIER nonce is not where the layout says it is");

    if (pass == total) {
        UE_LOGI("peer_admission selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("peer_admission selftest: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace coop::net::peer_admission
