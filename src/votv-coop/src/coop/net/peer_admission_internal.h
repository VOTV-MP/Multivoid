// coop/net/peer_admission_internal.h -- the signed blob's layout, shared by the two translation
// units that make up peer_admission.
//
// NOT A PUBLIC HEADER: it sits beside the .cpp files rather than under include/, because nothing
// outside these two units may build an admission blob -- doing so elsewhere would mean signing
// something the exchange did not author. `peer_admission.h` stays the module's whole public
// surface.
//
// It exists because the selftest lives in its own translation unit and needs exactly one thing from
// the production one: the BLOB. Its arms assert that a signature over a DIFFERENT blob does not
// verify, so it has to build blobs the way the production code does. Re-declaring the layout in the
// test file is the trap: a selftest carrying its own copy of the structure it verifies passes
// forever after the real one changes. ONE definition, two includers.

#pragma once

#include <cstdint>
#include <cstring>

#include "coop/net/peer_identity.h"
#include "coop/net/protocol.h"

namespace coop::net::peer_admission::internal {

using peer_identity::PubKey;

// THE TAG IS THE VERSION OF THE LAYOUT, and the static_asserts below are the contract: a change to
// any field width must break the build rather than silently invalidate every signature in the
// field.
constexpr char   kTag[]  = "multivoid-peer-admission-v2";
constexpr size_t kTagLen = sizeof(kTag) - 1;  // 27, terminator excluded
static_assert(kTagLen == 27, "the tag width is part of the blob layout");

constexpr uint8_t kDirHost   = 0x01;  // the host proving itself to the client
constexpr uint8_t kDirClient = 0x02;  // the client proving itself to the host

constexpr size_t kBlobBytes = kTagLen + 1 + 2 + peer_identity::kPubKeyBytes * 2 +
                              kAuthNonceBytes + 1 + kAuthNonceBytes;
static_assert(kBlobBytes == 159, "blob layout changed -- bump the tag, not just the code");

using Blob = uint8_t[kBlobBytes];

// The FLAGS and the HOST's nonce are inside the signature on purpose; the reasoning
// (a relay could otherwise set the password-required flag on an open lobby and harvest
// a real tag) stays at the layout comment in peer_admission.cpp, where the exchange
// that depends on it lives.
inline void BuildBlob(Blob out, uint8_t dir, const PubKey& hostPub, const PubKey& clientPub,
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

}  // namespace coop::net::peer_admission::internal
