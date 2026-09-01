// coop/net/lobby_password.h -- the ONE owner of what a lobby password is worth.
//
// WHAT THIS IS FOR. A host can lock a session (`ui/host_session_settings`), and
// until this module existed `locked` was a badge: announced to the master, drawn
// as a padlock in the browser, and enforced by nothing. This turns knowing the
// password into a value a joiner can PROVE and a host can CHECK, carried inside
// the admission exchange that already runs before a seat is spent.
//
// ---------------------------------------------------------------------------
// THE ONE RULE THIS MODULE EXISTS TO OBEY, and it killed the first design.
//
// A low-entropy secret must NEVER enter a signature the verifying side can
// recompute. The obvious construction -- fold `KDF(password)` into the blob the
// client signs with its Ed25519 key -- is an OFFLINE ORACLE: any host a client
// can be steered to knows every other term (the client's key off its own socket,
// its own key, the nonce IT chose, the tag and direction, and the layout is open
// source), and Ed25519 verification is PUBLIC. So it brute-forces the password
// offline at hardware speed, and a host-side rate limit is irrelevant because the
// attacker never has to ask again. Folding it into the HOST's signature instead
// only swaps which end is robbed. (`/qf` round 2, 2026-08-31;
// `[[lesson-a-low-entropy-secret-must-not-enter-a-signature-blob]]`.)
//
// "A captured proof is useless" is true of REPLAY, and it was quietly doing duty
// as a claim about CONFIDENTIALITY. They are different properties.
//
// WHAT SAVED IT WAS BINDING, NOT ENTROPY -- ON THE LANES THAT HAVE IT. The client
// emits nothing password-derived until it has established that the key on the
// socket IS the key it was sent to dial (`peer_admission`'s `bound`, security
// A65). A rogue host a MASTER steered you to therefore never receives a tag.
//
// THAT IS NO LONGER TRUE OF EVERY LANE, and this paragraph asserted it was for
// ninety minutes after it stopped being so. Since 2026-09-01 a destination the
// local machine named -- a typed address -- may carry a proof unbound
// (`net::Config::selfAddressed`), on the user's explicit instruction, because a
// locked host was otherwise unjoinable by address from any shipped UI. On that
// lane a host that merely ANSWERS receives a grindable tag, and with the generated
// secret at six characters that is on the order of six GPU-hours. The reachable
// case is a MISTYPED address answered by a Multivoid host.
//
// So this module's guarantee is now conditional, and saying so here is the whole
// point of a header that calls itself the ONE owner of what a lobby password is
// worth. The constructions that would restore an unconditional guarantee are a
// longer secret, a per-lane refusal, or a PAKE; none is built.
//
// ---------------------------------------------------------------------------
// THE CONSTRUCTION.
//
//   K   = PBKDF2-HMAC-SHA256(password, salt = hostPub, kIterations, 32 bytes)
//   tag = HMAC-SHA256(K, "MVLP1" || hostPub || clientPub || hostNonce)
//
// The SALT IS THE HOST'S PUBLIC KEY, so a table built against one host is worth
// nothing against another and two hosts who choose the same password derive
// different keys. The TAG covers both identities and the host's nonce, so it is
// fresh per attempt and cannot be replayed toward a third party -- the same
// shape, and the same reasoning, as the identity blob it travels beside.
//
// WHO PAYS THE KDF. The HOST derives K once per session and caches it, so each
// arriving attempt costs one HMAC (microseconds) -- which is what lets the guess
// bound be a policy rather than an accident of CPU cost. The CLIENT derives once
// per join, on the net thread, and that is the deliberate ~100 ms: it is paid
// while connecting, on a link that has nothing else to do.
//
// Everything here is PURE and thread-agnostic: no engine, no globals, no I/O. It
// is called from the net thread today and the selftest calls it from wherever it
// runs.

#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "coop/net/peer_identity.h"

namespace coop::net::lobby_password {

inline constexpr int kTagBytes = 32;   // HMAC-SHA256
using Tag = std::array<uint8_t, kTagBytes>;

// PBKDF2 rounds. Sized for the case where BINDING has already failed -- an
// advertised identity that was itself tampered with on the PLAINTEXT signaling
// leg (security A59's residual). With binding intact no offline oracle exists and
// this number buys nothing; without it, this number is the whole delay. ~100 ms
// on the joining client, once, and nothing on the host, which caches K.
//
// READ THIS WITH `host_session_settings.cpp`'s kPwLen, which the user shortened from ten
// characters to SIX on 2026-09-01. The generated secret is now 30 bits rather than 50, so
// an exposed proof costs on the order of 6 GPU-hours to exhaust instead of 714 GPU-years.
// Raising the count here CANNOT buy that back -- recovering 20 bits would need a million
// times the rounds at ~100 ms each -- so do not reach for this number when what actually
// moved was the length.
//
// AND "Binding is the control" IS NO LONGER TRUE ON EVERY LANE. A lane the local machine
// addressed may now send a password proof unbound (`net::Config::selfAddressed`), so on
// THAT lane there is no binding to be the control and this round count is the whole delay
// -- against a 30-bit secret, which is on the order of six GPU-hours. The reachable case is
// a mistyped address answered by a Multivoid host. Recorded here rather than argued: this
// header is where someone reaching for the round count will look, and the number they
// should be reaching for first is the LENGTH.
inline constexpr uint32_t kIterations = 200000;
// ASSERTED AT COMPILE TIME, because the selftest cannot do it. Its full-cost arm only shows
// the shipped count differs from the cheap one -- which would pass for 2, and for 0 if CNG
// yields a distinct key. A constant this load-bearing owes a check that costs nothing
// (audit, 2026-08-31).
static_assert(kIterations == 200000, "the shipped round count moved -- change it here and "
                                     "in lobby_password.h's rationale together, or not at all");

// K = PBKDF2(password, hostPub). False if CNG refused or the password is empty.
// An empty password must never derive a key: it would give "no password" a
// well-formed tag that a host requiring one would ACCEPT.
bool DeriveKey(const std::string& password, const peer_identity::PubKey& hostPub,
               std::array<uint8_t, 32>& outKey);

// tag = HMAC(K, blob). False only if CNG refused.
bool ComputeTag(const std::array<uint8_t, 32>& key, const peer_identity::PubKey& hostPub,
                const peer_identity::PubKey& clientPub, const uint8_t nonce[32],
                Tag& outTag);

// CONSTANT-TIME EQUALITY, and it is not superstition here. A byte-at-a-time
// compare leaks how many leading bytes matched, and the attacker controls both
// the nonce input and the retry -- which is the textbook setting where a timing
// oracle turns a 2^256 search into 32 searches of 256. `memcmp` short-circuits by
// contract, so it is the wrong tool and is not used.
bool TagsEqual(const Tag& a, const Tag& b);

// UN-GATED selftest, run beside peer_identity's and peer_admission's. It drives
// the REAL derivation and the REAL comparison, and its arms are the ones no LAN
// drill can stage: the same password under two different host keys must NOT
// collide, a one-character difference must not collide, an empty password must
// refuse to derive at all, and a tag must not verify against a different nonce.
// A verifier that accepts everything passes every positive test there is, so the
// negatives are the test.
bool RunSelftest();

}  // namespace coop::net::lobby_password
