// coop/net/peer_admission.cpp -- see coop/net/peer_admission.h for WHY.

#include "coop/net/peer_admission.h"

#include "coop/net/session.h"
#include "ue_wrap/core/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingtypes.h>
#include <steam/isteamnetworkingsockets.h>
#pragma warning(pop)

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
constexpr char kTag[] = "multivoid-peer-admission-v1";
constexpr size_t kTagLen = sizeof(kTag) - 1;  // 27, terminator excluded
static_assert(kTagLen == 27, "the tag width is part of the blob layout");

constexpr uint8_t kDirHost   = 0x01;  // the host proving itself to the client
constexpr uint8_t kDirClient = 0x02;  // the client proving itself to the host

constexpr size_t kBlobBytes = kTagLen + 1 + 2 + peer_identity::kPubKeyBytes * 2 +
                              kAuthNonceBytes;
static_assert(kBlobBytes == 126, "blob layout changed -- bump the tag, not just the code");

using Blob = uint8_t[kBlobBytes];

void BuildBlob(Blob out, uint8_t dir, const PubKey& hostPub, const PubKey& clientPub,
               const uint8_t nonce[kAuthNonceBytes]) {
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

// --- HOST state -------------------------------------------------------------
// One row per pending band entry. Indexed by the pending index, which the band
// itself allocates and recycles, so HostForgetPending is called on EVERY exit
// path (admit and close alike) -- a recycled index inheriting a live nonce would
// let a new socket answer the previous socket's challenge.
struct HostRow {
    bool     open = false;
    uint32_t hConn = 0;
    uint8_t  nonce[kAuthNonceBytes]{};
    PubKey   remotePub{};
};
constexpr int kMaxHostRows = 8;  // == Session::kMaxPending; asserted in the .cpp below
HostRow g_host[kMaxHostRows];

// --- CLIENT state -----------------------------------------------------------
struct ClientRow {
    bool     open = false;
    bool     proved = false;
    uint32_t hConn = 0;
    uint8_t  nonce[kAuthNonceBytes]{};
    PubKey   hostPub{};
};
ClientRow g_client;

}  // namespace

// ---------------------------------------------------------------------------
// HOST
// ---------------------------------------------------------------------------

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
        if (!peer_identity::RandomBytes(out.nonce, sizeof(out.nonce))) {
            // No randomness means no freshness, and a predictable nonce is worse
            // than no exchange because it LOOKS like one. Refuse loudly.
            UE_LOGE("peer_admission: the OS refused randomness -- cannot challenge");
            r.reason = "host has no randomness";
            return r;
        }
        std::memcpy(row.nonce, out.nonce, sizeof(row.nonce));

        // We prove ourselves FIRST, over the CLIENT's nonce.
        Blob blob;
        BuildBlob(blob, kDirHost, peer_identity::LocalPublicKey(), row.remotePub,
                  hello.nonce);
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
                  row.nonce);
        if (!peer_identity::VerifyBlob(row.remotePub, blob, sizeof(blob), sig)) {
            r.reason = "identity proof did not verify";
            return r;
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
    (void)session;
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
              g_client.nonce);
    Sig hostSig{};
    std::memcpy(hostSig.data(), ch.sig, hostSig.size());
    if (!peer_identity::VerifyBlob(g_client.hostPub, blob, sizeof(blob), hostSig)) {
        *outClose = "the host did not prove the identity it advertised";
        return true;
    }

    // ...then prove ourselves over THEIRS.
    AuthProofPayload out{};
    Blob mine;
    BuildBlob(mine, kDirClient, g_client.hostPub, peer_identity::LocalPublicKey(),
              ch.nonce);
    const Sig sig = peer_identity::SignBlob(mine, sizeof(mine));
    std::memcpy(out.sig, sig.data(), sig.size());
    if (!session.SendRawReliableToConn(hConn, ReliableKind::AuthProof,
                                       &out, sizeof(out))) {
        *outClose = "could not send the identity proof";
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
    BuildBlob(asClient, kDirClient, pkA, mine, nonce1);
    const Sig sigClient = peer_identity::SignBlob(asClient, sizeof(asClient));
    check(peer_identity::VerifyBlob(mine, asClient, sizeof(asClient), sigClient),
          "a well-formed client proof did not verify");

    // NEGATIVE 1 -- REFLECTION. The host's own challenge must not be replayable as
    // the client's proof. Only the direction byte differs, so this is the arm that
    // fails if the direction is ever dropped from the blob.
    Blob asHost;
    BuildBlob(asHost, kDirHost, pkA, mine, nonce1);
    check(!peer_identity::VerifyBlob(mine, asHost, sizeof(asHost), sigClient),
          "a proof verified in the WRONG DIRECTION (the direction byte is not "
          "reaching the blob)");

    // NEGATIVE 2 -- THIRD PARTY. The same signature must not verify inside a blob
    // naming a different counterparty; this is what stops a proof given to host A
    // being relayed into host B's exchange.
    Blob otherHost;
    BuildBlob(otherHost, kDirClient, pkB, mine, nonce1);
    check(!peer_identity::VerifyBlob(mine, otherHost, sizeof(otherHost), sigClient),
          "a proof verified against a DIFFERENT counterparty (both identities are "
          "not reaching the blob)");

    // NEGATIVE 3 -- FRESHNESS. A recorded proof must not answer a new challenge.
    Blob otherNonce;
    BuildBlob(otherNonce, kDirClient, pkA, mine, nonce2);
    check(!peer_identity::VerifyBlob(mine, otherNonce, sizeof(otherNonce), sigClient),
          "a proof verified against a DIFFERENT nonce (the exchange is replayable)");

    // NEGATIVE 4 -- WRONG SIGNER. The decision the whole module exports: a
    // signature made by us must not verify against somebody else's identity.
    check(!peer_identity::VerifyBlob(pkA, asClient, sizeof(asClient), sigClient),
          "a proof verified against SOMEONE ELSE'S identity");

    // The blob is a fixed layout; assert the two facts a future edit could break
    // silently -- a shortened tag, or a field that stopped being copied.
    check(asClient[kTagLen] == kDirClient && asHost[kTagLen] == kDirHost,
          "the direction byte is not where the layout says it is");
    check(std::memcmp(asClient + kBlobBytes - kAuthNonceBytes, nonce1,
                      kAuthNonceBytes) == 0,
          "the nonce is not at the end of the blob");

    if (pass == total) {
        UE_LOGI("peer_admission selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("peer_admission selftest: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace coop::net::peer_admission
