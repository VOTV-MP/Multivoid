// coop/net/peer_admission_selftest.cpp -- the admission exchange's NEGATIVE arms.
//
// Extracted 2026-09-01 from peer_admission.cpp, which crossed the 800-LOC soft cap that
// day. The body below is MOVED VERBATIM; the only edits are the includes and the blob
// helpers now coming from `peer_admission_internal.h` instead of the anonymous namespace
// they used to share.
//
// WHY THIS IS THE CUT BOTH AUDITS PICKED. `RunSelftest` has no production caller -- it
// runs once at boot beside peer_identity's and lobby_password's -- so lifting it moves
// ~115 lines without touching a single seam the exchange depends on. Nothing else in
// that file could be removed without designing an interface first.
//
// WHAT THESE ARMS ARE FOR, kept with them because it is the reason they exist: a
// verifier that accepts everything passes every POSITIVE test there is. The happy path
// is proven by every smoke and by `mp.py authdrill --control`; only the negatives can
// show the gate is a gate. They are also the arms no LAN drill can stage -- a signature
// over a different direction byte, a different protocol version, a third party's key --
// because staging them would mean building a peer that lies in a specific way.
//
// UN-GATED on purpose: it runs on every launch, not behind a dev flag. A selftest that
// only runs when someone remembers to arm it is a selftest nobody has run.

#include "coop/net/peer_admission.h"

#include "peer_admission_internal.h"   // private: beside the .cpp, never under include/
#include "coop/net/peer_identity.h"
#include "coop/net/protocol.h"
#include "ue_wrap/core/log.h"

#include <cstdint>
#include <cstring>

namespace coop::net::peer_admission {
namespace {

// The blob layout, from the header the production TU also includes -- NOT a local copy.
// A selftest that re-declares the structure it verifies passes forever after the real
// one changes, which is the whole reason `peer_admission_internal.h` exists.
using internal::Blob;
using internal::BuildBlob;
using internal::kBlobBytes;
using internal::kDirClient;
using internal::kDirHost;
using internal::kTagLen;

using peer_identity::PubKey;
using peer_identity::Sig;

}  // namespace

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
