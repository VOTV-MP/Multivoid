// coop/net/lobby_password.h -- the one owner of what a lobby password is worth: knowing it
// becomes a value a joiner can prove and a host can check, inside the admission exchange that
// already runs before a seat is spent. The rule: a low-entropy secret never enters a signature
// the verifying side can recompute. Folding a password-derived key into the blob the client
// signs would be an offline oracle, since Ed25519 verification is public and any host a client
// can be steered to knows every other term; so the tag is a separate field, and binding
// protects it: the client emits nothing password-derived until the key on the socket is the
// key it was sent to dial. One lane is unbound by design, a destination the local machine
// typed (the self-addressed config), so there a host that merely answers receives a grindable
// tag against a 30-bit generated secret. The construction: K = PBKDF2-HMAC-SHA256(password,
// salt = hostPub, kIterations); tag = HMAC-SHA256(K, "MVLP1" || hostPub || clientPub ||
// hostNonce). The salt makes a table built against one host worthless against another; the
// tag covers both identities and the host's nonce, so it is fresh per attempt and cannot be
// replayed toward a third party. The host derives K once per session and caches it (an
// attempt then costs one HMAC); the client derives once per join, on the net thread.

#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "coop/net/peer_identity.h"

namespace coop::net::lobby_password {

inline constexpr int kTagBytes = 32;   // HMAC-SHA256
using Tag = std::array<uint8_t, kTagBytes>;

// PBKDF2 rounds. Sized for the case where binding has already failed (an advertised identity
// tampered with on the plaintext signaling leg): with binding intact no offline oracle exists
// and this number buys nothing; without it, this number is the whole delay. About 100 ms on
// the joining client, once, and nothing on the host, which caches K. Read this with the
// generated password length in ui/host_session_settings.cpp: six characters from a 32-symbol
// alphabet is 30 bits, so an exposed proof is hours of GPU time to exhaust, and raising the
// round count cannot buy that back (recovering 20 bits would need a million times the rounds
// at 100 ms each); the number to reach for first is the length. On the self-addressed lane
// there is no binding to be the control, and this count is the whole delay.
inline constexpr uint32_t kIterations = 200000;
// Asserted at compile time, because the selftest cannot: its full-cost arm only shows that the
// shipped count differs from the cheap one, which would pass for 2.
static_assert(kIterations == 200000, "the shipped round count moved -- change it here and "
                                     "in lobby_password.h's rationale together, or not at all");

// K = PBKDF2(password, hostPub). False if CNG refused or the password is empty: an empty
// password must never derive a key, since it would give no-password a well-formed tag that a
// host requiring one would accept.
bool DeriveKey(const std::string& password, const peer_identity::PubKey& hostPub,
               std::array<uint8_t, 32>& outKey);

// tag = HMAC(K, blob). False only if CNG refused.
bool ComputeTag(const std::array<uint8_t, 32>& key, const peer_identity::PubKey& hostPub,
                const peer_identity::PubKey& clientPub, const uint8_t nonce[32],
                Tag& outTag);

// Constant-time equality: a byte-at-a-time compare leaks how many leading bytes matched, and
// the attacker controls both the nonce and the retry, the setting where a timing oracle turns
// a 2^256 search into 32 searches of 256. memcmp short-circuits by contract, so it is not
// used.
bool TagsEqual(const Tag& a, const Tag& b);

// The selftest, run beside the identity and admission ones. It drives the real derivation and
// the real comparison, and its arms are the ones no LAN drill can stage: the same password
// under two host keys must not collide, a one-character difference must not collide, an empty
// password must refuse to derive, and a tag must not verify against a different nonce. A
// verifier that accepts everything passes every positive test, so the negatives are the test.
bool RunSelftest();

}  // namespace coop::net::lobby_password
