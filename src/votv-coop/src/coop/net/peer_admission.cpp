// coop/net/peer_admission.cpp -- the admission exchange on a fresh connection: the host proves
// its Ed25519 key over the client's nonce, the client proves its own over the host's, the client
// binds the socket's key to the identity it was sent to, and a locked lobby's password proof is a
// tag bound to those keys. See coop/net/peer_admission.h.

#include "coop/net/peer_admission.h"

#include "peer_admission_internal.h"   // private, beside the .cpp

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

#include <windows.h>   // GetTickCount64, the guess window's clock

#include <cstring>

namespace coop::net::peer_admission {

namespace {

using peer_identity::PubKey;
using peer_identity::Sig;

// The signed blob, fixed size on purpose: a delimited encoding would need a parser in front of an
// unauthenticated peer, and every field here is a constant width, so building it is a memcpy
// sequence with no branches: the 27-byte tag, the direction, the protocol version, the host's
// key, the client's key, the verifier's nonce, the challenge flags and the host's own nonce. The
// flags and the host nonce are inside the signature: outside it, a relay could set the
// password-required flag on an open lobby's challenge with a nonce of its own and harvest a real
// tag from a bound client for a host that never asked. The layout lives in
// peer_admission_internal.h, shared with the selftest, so a negative arm cannot pass against its
// own copy of the structure.
using internal::Blob;
using internal::BuildBlob;
using internal::kBlobBytes;
using internal::kDirClient;
using internal::kDirHost;

// The remote's 32 identity bytes, read off the connection, never off a packet. False for any
// identity that is not exactly a 32-byte GenericBytes one: a peer that presents nothing falls
// back to an IP-typed identity, which nobody can sign for.
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

// The remote's address in the 16-byte form GNS stores (IPv4 arrives mapped), without the port (a
// retrying attacker gets a fresh source port per connection). False when there is no usable
// address, the common case on the P2P lane, where GNS reports all zeros for anything but a direct
// UDP connection.
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

// The host password state. The key is derived once and cached, which is what makes the guess
// bound a policy rather than an accident of CPU cost: the derivation costs about 100 ms, and paid
// per attempt it would let an attacker stall the net thread for every other peer by connecting.
// Derived lazily on the net thread at the first attempt that needs it, so an open lobby never
// pays it; keyed on the password string so a changed value cannot leave a stale key.
struct HostPasswordCache {
    std::string            forPassword;
    std::array<uint8_t, 32> key{};
    bool                   valid = false;
};
HostPasswordCache g_hostPw;

// The guess bound: every lane is counted, and no two attackers share a bucket. Keyed on the
// remote address it refused when it could not bucket, and on the P2P lane the address is all
// zeros, so every joiner shared one bucket and ten junk attempts locked the lobby for everybody;
// not bucketing there left the main lane with no counter at all (the master's join rate limit
// bounds discovery, not attempts, since the signaling token it returns is static and the
// signaling server has no limiter). So the key is whatever identifies the attempter: the real
// address where there is one, else the public key the peer has just proved it holds. A rotated
// keypair costs a fresh connection and a full exchange and can never take an honest player's
// bucket. A full table stops counting, not checking: failing closed is right only when failing
// open would admit, and here it could only deny. The collateral: peers behind one carrier NAT
// share an address bucket, and ten wrong guesses between them lock it for a minute, far above
// what typing costs and far below a search of the 30-bit generated password.
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

// This key's bucket, found or claimed; null when the table is full of live windows.
GuessBucket* BucketFor(const uint8_t key[16], GuessKeyKind kind, uint64_t nowMs) {
    GuessBucket* freeRow = nullptr;
    for (auto& b : g_guess) {
        // The kind is part of the key, so an address cannot collide with the first 16 bytes of a
        // public key.
        if (b.used && b.kind == kind && std::memcmp(b.key, key, 16) == 0) {
            if (nowMs - b.windowStartMs >= kGuessWindowMs) {
                b.windowStartMs = nowMs;
                b.fails = 0;
            }
            return &b;
        }
        // An expired row is reusable and a free one better; both are collected in one pass, so a
        // table of stale windows never reads as full.
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

// The host state: one row per pending band entry, indexed by the pending index the band
// allocates and recycles, so HostForgetPending runs on every exit path; a recycled index
// inheriting a live nonce would let a new socket answer the previous socket's challenge.
struct HostRow {
    bool     open = false;
    uint32_t hConn = 0;
    uint8_t  nonce[kAuthNonceBytes]{};
    // The flags we sent, inside both signatures, so the client's proof is verified against the blob
    // we challenged with, never against anything that comes back.
    uint8_t  flags = 0;
    PubKey   remotePub{};
};
// Derived from the band, not asserted against it: raising kMaxPending widens this array by
// construction.
constexpr int kMaxHostRows = Session::kMaxPending;
HostRow g_host[kMaxHostRows];

// The client state.
struct ClientRow {
    bool     open = false;
    bool     proved = false;
    // Whether the key on the socket matched the identity we were sent to dial; false on a lane that
    // advertises none (LAN, plain UDP), which is not a mismatch, so a fact rather than a verdict.
    bool     bound = false;
    uint32_t hConn = 0;
    uint8_t  nonce[kAuthNonceBytes]{};
    PubKey   hostPub{};
    // The drill knob, resolved once when the link opens: ResolveEnum line-scans the ini under a
    // global mutex, and read where it is used it would put file I/O on the net thread between
    // verifying the host and sending our proof, on every join.
    std::string drill;
};
ClientRow g_client;

}  // namespace

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
        r.code = EndReason::AcceptFailed;
        r.reason = "pending index out of range";
        return r;
    }
    HostRow& row = g_host[pendIdx];
    // A recycled index (the band handed this slot to another connection while a row was open)
    // starts clean rather than answering with the old nonce.
    if (row.open && row.hConn != hConn) row = HostRow{};

    switch (kind) {
    case ReliableKind::AuthHello: {
        if (len != static_cast<int>(sizeof(AuthHelloPayload))) {
            r.code = EndReason::MalformedHello;
            r.reason = "malformed AuthHello";
            return r;
        }
        // One hello per connection: a second would let a peer re-roll the host's nonce after seeing
        // a challenge, the shape of a downgrade, and an honest client sends exactly one.
        if (row.open) {
            r.code = EndReason::DuplicateHello;
            r.reason = "duplicate AuthHello";
            return r;
        }
        if (!RemoteKeyOf(hConn, row.remotePub)) {
            r.code = EndReason::NoKeyIdentity;
            r.reason = "peer presented no key identity";
            return r;
        }
        AuthHelloPayload hello{};
        std::memcpy(&hello, payload, sizeof(hello));

        AuthChallengePayload out{};
        // The joiner is told what is required: a direct or LAN connect has no browser row, and a
        // guess would either withhold a required proof or emit one to a host that never asked.
        if (!session.LobbyPassword().empty()) out.flags |= kAuthFlagPasswordRequired;
        if (!peer_identity::RandomBytes(out.nonce, sizeof(out.nonce))) {
            // No randomness means no freshness, and a predictable nonce is worse than no exchange,
            // since it looks like one.
            UE_LOGE("peer_admission: the OS refused randomness -- cannot challenge");
            r.code = EndReason::HostNoRandomness;
            r.reason = "host has no randomness";
            return r;
        }
        std::memcpy(row.nonce, out.nonce, sizeof(row.nonce));
        row.flags = out.flags;

        // The host proves itself first, over the client's nonce.
        Blob blob;
        BuildBlob(blob, kDirHost, peer_identity::LocalPublicKey(), row.remotePub,
                  hello.nonce, out.flags, out.nonce);
        const Sig sig = peer_identity::SignBlob(blob, sizeof(blob));
        std::memcpy(out.sig, sig.data(), sig.size());

        if (!session.SendRawReliableToConn(hConn, ReliableKind::AuthChallenge,
                                           &out, sizeof(out))) {
            r.code = EndReason::CouldNotSendChallenge;
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
            r.code = EndReason::ProofBeforeHello;
            r.reason = "AuthProof before AuthHello";
            return r;
        }
        if (len != static_cast<int>(sizeof(AuthProofPayload))) {
            r.code = EndReason::MalformedProof;
            r.reason = "malformed AuthProof";
            return r;
        }
        // The key is re-read from the connection rather than trusted from the hello, so the
        // decision rests on what GNS says now about the socket in hand.
        PubKey nowPub{};
        if (!RemoteKeyOf(hConn, nowPub) || nowPub != row.remotePub) {
            r.code = EndReason::IdentityChanged;
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
            r.code = EndReason::IdentityNotProved;
            r.reason = "identity proof did not verify";
            return r;
        }

        // The lobby password, after the identity and never before: the tag is bound to the peer's
        // public key, and checked against an unproved key it would be checked against a claim.
        const std::string& want = session.LobbyPassword();
        if (!want.empty()) {
            if (!proof.hasPw) {
                // A distinct reason, shown to a person: "wrong password" for a client that sent
                // none would send them looking for a typo in a box they never filled in.
                r.code = EndReason::PasswordRequired;
                r.reason = "this server needs a password";
                return r;
            }
            // The bound, checked before the HMAC so a flood costs the comparison and not the
            // crypto; a missing bucket (no usable address, a full table) is not a refusal. The key
            // is the real address where there is one, otherwise the key this peer just proved.
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
                r.code = EndReason::TooManyPasswordAttempts;
                r.reason = "too many password attempts -- try again in a minute";
                return r;
            }

            if (!g_hostPw.valid || g_hostPw.forPassword != want) {
                g_hostPw.valid = lobby_password::DeriveKey(
                    want, peer_identity::LocalPublicKey(), g_hostPw.key);
                g_hostPw.forPassword = want;
                if (!g_hostPw.valid) {
                    // Unable to check, unable to admit: the alternative is a lobby that silently
                    // stops being locked.
                    UE_LOGE("peer_admission: could not derive the lobby key -- refusing "
                            "every join rather than silently unlocking the session");
                    r.code = EndReason::HostCannotCheckPassword;
                    r.reason = "the host could not check the password";
                    return r;
                }
            }

            lobby_password::Tag expect{};
            if (!lobby_password::ComputeTag(g_hostPw.key, peer_identity::LocalPublicKey(),
                                            row.remotePub, row.nonce, expect)) {
                r.code = EndReason::HostCannotCheckPassword;
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
                r.code = EndReason::WrongPassword;
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
        // Anything else before admission is a protocol violation (this build's client sends the
        // hello and then nothing until seated). Refused rather than dropped, so a peer that will
        // never be admitted learns so; a silent drop once deadlocked every honest join.
        r.code = EndReason::SpokeBeforeProving;
        r.reason = "spoke before proving its identity";
        return r;
    }
}

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

    // The binding, without which the exchange proves nothing on this side. The key above comes off
    // the socket, and verifying the host against it asks only whether whoever answered holds the
    // key whoever answered presented, which every host passes. What this is for is the advertised
    // identity (the `gen:<64 hex>` the master returned, or the one in a direct invite), compared
    // byte-wise after parsing. An empty advertised identity is not a failure: the LAN and plain-UDP
    // lanes dial an address, and there is nothing to bind to, which is where the password's own
    // binding carries the weight instead.
    g_client.drill = coop::config::ResolveEnum(coop::config_registry::rows::auth_drill);

    const std::string& advertised = session.AdvertisedHostIdentity();
    peer_identity::PubKey want{};
    bool haveWant = false;
    if (g_client.drill == "mismatch") {
        // Synthesised: the drill must run on the LAN lane the rig uses, which advertises no
        // identity, and an arm that fired only with one present would be green on every rig. One
        // flipped bit is a key that provably is not the one on this socket.
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
        // Stated as the conditional it is: a self-addressed lane may carry a password.
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
                      const void* payload, int len, const char** outClose,
                      EndReason* outCode) {
    if (kind != ReliableKind::AuthChallenge) return false;  // not ours
    if (!g_client.open || g_client.hConn != hConn) {
        *outCode = EndReason::BadChallenge;
        *outClose = "unexpected AuthChallenge";
        return true;
    }
    if (g_client.proved) {
        // A second challenge would re-open a settled decision on a committed connection.
        *outCode = EndReason::BadChallenge;
        *outClose = "duplicate AuthChallenge";
        return true;
    }
    if (len != static_cast<int>(sizeof(AuthChallengePayload))) {
        *outCode = EndReason::BadChallenge;
        *outClose = "malformed AuthChallenge";
        return true;
    }
    AuthChallengePayload ch{};
    std::memcpy(&ch, payload, sizeof(ch));

    // The host verified over our nonce, against the identity bytes on this socket.
    Blob blob;
    BuildBlob(blob, kDirHost, g_client.hostPub, peer_identity::LocalPublicKey(),
              g_client.nonce, ch.flags, ch.nonce);
    Sig hostSig{};
    std::memcpy(hostSig.data(), ch.sig, hostSig.size());
    if (!peer_identity::VerifyBlob(g_client.hostPub, blob, sizeof(blob), hostSig)) {
        // Two reasons, two events: bound means this key was already checked to be the one we were
        // sent to, so a bad signature is a host that cannot back its advertised name; unbound means
        // nobody advertised anything, and all this could show is that the answerer holds the key it
        // presented.
        *outCode = EndReason::HostNotProved;
        *outClose = g_client.bound
                        ? "the host did not prove the identity it advertised"
                        : "the host did not prove the key it presented (no identity was "
                          "advertised on this lane, so there was nothing to bind to)";
        return true;
    }

    // Then our proof over theirs.
    AuthProofPayload out{};
    Blob mine;
    BuildBlob(mine, kDirClient, g_client.hostPub, peer_identity::LocalPublicKey(),
              ch.nonce, ch.flags, ch.nonce);
    const Sig sig = peer_identity::SignBlob(mine, sizeof(mine));
    std::memcpy(out.sig, sig.data(), sig.size());

    // The lobby password, if this host asked for one. The binding gate is the security, not the
    // derivation: a tag is derived from a low-entropy secret, and handed to a host not established
    // as the one we were sent to it can be ground offline, so an unbound lane gets nothing and the
    // join fails with a sentence that says why.
    if (ch.flags & kAuthFlagPasswordRequired) {
        // Bound, or the player typed the address themselves. Binding answers "is this the host the
        // master sent me to", the right question when a third party named the destination; on a
        // typed address the player is the authority on where they meant to go, and there is nothing
        // further to bind against. The cost is the typo case: a mistyped address answered by an
        // unrelated host, which learns a six-character password to a lobby it cannot find. Nothing
        // else relaxes: the tag is bound to the key that answered, so it cannot be replayed to the
        // real host; the host verifies identity before the password; and its guess bucket still
        // bounds online guessing.
        if (!g_client.bound && !session.DestinationIsSelfAddressed()) {
            *outCode = EndReason::PasswordUnbound;
            *outClose = "this server wants a password, but nothing told us which host we "
                        "were dialling -- refusing to send anything derived from it";
            return true;
        }
        const std::string& pw = session.LobbyPassword();
        if (pw.empty()) {
            // Not a protocol error: a person forgot, or was never given one. The join screen shows
            // this line.
            *outCode = EndReason::PasswordMissing;
            *outClose = "this server needs a password";
            return true;
        }
        std::array<uint8_t, 32> key{};
        lobby_password::Tag tag{};
        if (!lobby_password::DeriveKey(pw, g_client.hostPub, key) ||
            !lobby_password::ComputeTag(key, g_client.hostPub,
                                        peer_identity::LocalPublicKey(), ch.nonce, tag)) {
            *outCode = EndReason::PasswordProof;
            *outClose = "could not compute the password proof on this machine";
            return true;
        }
        std::memcpy(out.pwTag, tag.data(), tag.size());
        out.hasPw = 1;
    }

    // The drill, on this side only: a knob on the host's gate would make the verdict a statement
    // about the bypass. Here it sabotages a real proof on the real path.
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
        *outCode = EndReason::CouldNotSendProof;
        *outClose = "could not send the identity proof";
        return true;
    }
    if (drill == "corrupt") {
        // A corrupted proof must not set `proved`: were the host to seat us anyway, the client's
        // own AssignPeerSlot gate must be the second refusal.
        return true;
    }
    g_client.proved = true;
    UE_LOGI("peer_admission: host identity VERIFIED (guid %s) -- proof sent, awaiting "
            "the seat",
            peer_identity::GuidForPublicKey(g_client.hostPub).c_str());
    return true;
}

}  // namespace coop::net::peer_admission
