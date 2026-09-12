// coop/net/peer_identity.h -- the durable player identity: one Ed25519 keypair per install,
// whose public key is the peer's network identity. The transport's generic identity
// holds exactly 32 bytes, so the raw key fits it with nothing left over, and the 32-char guid
// every store uses is derived from the key, never asserted by the peer that wants to be called
// by it; it needs no authority, no minting and no master round trip, so it covers the direct
// and LAN lanes too. What this module does not do: the transport binds a cert's identity string
// and its key to nothing, so a peer can present a victim's public key as its identity while
// signing with its own cert key and pass every check the library makes; the admission challenge
// in coop/net/peer_admission.h, where a peer must sign with the key its identity names, is what
// makes the identity mean anything. Sign and Verify are pure and re-entrant, touching only the
// immutable key loaded at boot. The key file is the install's, beside the ini but never in it
// (inis get pasted into bug reports), under a private access list of its own (this account and
// the system, nothing inherited) re-applied at every load, since anyone holding it can be you;
// an account that cannot own the install's file keeps its own key for that install under its
// profile. A Steam library move carries it, and a tester's two installs are two players.

#pragma once

#include <array>
#include <cstdint>
#include <string>

class ISteamNetworkingSockets;

namespace coop::net::peer_identity {

inline constexpr int kPubKeyBytes  = 32;  // Ed25519 public key == GNS k_cbMaxGenericBytes
inline constexpr int kPrivKeyBytes = 32;  // the seed; the transport's key type takes 32
inline constexpr int kSigBytes     = 64;  // Ed25519 signature

using PubKey = std::array<uint8_t, kPubKeyBytes>;
using Sig    = std::array<uint8_t, kSigBytes>;

// Load the durable keypair, generating and persisting one on first launch; an account that
// cannot read or write the install's file gets its own under its profile. False only when no
// key could be established at all, in which case the session must not start, since an
// identity-less peer cannot be admitted anywhere; a key that could not be persisted still
// works for this session and says so in the log.
bool Load();

// Our own public key, and the 32-char lowercase-hex guid derived from it. Empty or zero
// before a successful load.
const PubKey& LocalPublicKey();

// Our identity as the transport renders it: a prefix plus 64 lowercase hex, 68 chars. The
// string the P2P lane rendezvouses on: the host publishes it to the master and a joiner
// parses it back into the identity it dials, so it is the same value as the public key, not a
// second name for the same peer. Empty before a successful load. The routing name and the
// provable name are one value: as two, the master minted an ephemeral name per session and
// the P2P start installed it, silently overwriting the durable identity installed at start,
// so on the primary transport the key identity never reached the wire; keeping both would be
// two implementations of one concept. The cost, stated: the master and the signaling relay
// see a value stable across sessions where they saw a fresh one each time, and a permanent
// self-asserted name can be squatted, so the relay now proves every registration; the
// residual on a plaintext signaling leg remains.
const std::string& LocalIdentityString();

// The inverse of the above: the rendered form to the 32 key bytes it names. False for
// anything not exactly that form, so a caller cannot end up comparing against a half-parsed
// key. It lives here, beside the renderer, since a second hex codec elsewhere is the drift
// class this project keeps paying for, and its one caller compares the result byte-wise
// against the key on a socket. Never compare the strings: the transport renders an identity
// its own way, the guid is a different value entirely, and a string comparison would silently
// answer not-equal for two spellings of the same key.
bool PublicKeyFromIdentityString(const std::string& identity, PubKey& out);

// Cryptographic random bytes from the OS. Exposed because the admission exchange's nonces
// must come from the same source as the keys, not a second generator picked later. False if
// the OS refused, and a caller that cannot get randomness must fail rather than proceed with
// a weak nonce.
bool RandomBytes(void* out, size_t len);

// The canonical short form of any identity (a hash prefix of the public key, 32 lowercase hex
// chars), used by the host to name a remote peer's stored rows. Pure; empty if the key is not
// plausible.
std::string GuidForPublicKey(const PubKey& pub);

// Install our identity into the transport for this process, as a generic-bytes identity
// carrying the raw public key (the .cpp records why it is not a certificate). Must be called
// after the transport init and before any listen or connect, and nothing may reset the
// identity after it; see LocalIdentityString for the overwrite that made that sentence
// necessary. False on any failure, and the caller must not start a session that would
// present a different identity than it signs with.
bool InstallInto(ISteamNetworkingSockets* sockets);

// Sign or verify a domain-separated challenge blob. Verify takes the 32 identity bytes the
// caller read off the connection, so the question it answers is exactly whether this peer
// holds the key its identity names.
Sig  SignBlob(const uint8_t* data, size_t len);
bool VerifyBlob(const PubKey& pub, const uint8_t* data, size_t len, const Sig& sig);

// The un-gated arithmetic and crypto selftest, run once per session start; logs an all-pass
// line or one failure line per failing check. Deliberately impossible to switch off, for the
// same reason the movement ledger's is: a wrong verdict here does not crash, it either locks
// every honest player out or admits anyone, and both read as working from outside.
bool RunSelftest();

}  // namespace coop::net::peer_identity
