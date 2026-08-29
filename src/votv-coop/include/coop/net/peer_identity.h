// coop/net/peer_identity.h -- the durable per-install player identity.
//
// WHAT THIS IS. One Ed25519 keypair per install. Its PUBLIC KEY *is* the peer's
// network identity: `[V]` GNS's `k_cbMaxGenericBytes` is exactly 32 bytes
// (`steamnetworkingtypes.h:326`), so the raw key fits a `GenericBytes` identity
// with nothing left over, and the 32-char guid every store already uses becomes
// `hex(SHA-256(pubkey)[0..16])` -- DERIVED from the key, never asserted by the
// peer that wants to be called by it.
//
// WHY IT REPLACED A CA. The design of record until 2026-08-29 was a master-run
// certificate authority (`docs/security/PLAN_01_PEER_AUTH.md`, since retired in
// place). Its enforcement was "set `IP_AllowWithoutAuth = 0`", and `[V]` that
// convar is consulted ONLY by `CSteamNetworkConnectionUDP`
// (`steamnetworkingsockets_udp.cpp:1824-1841`) while the base class returns
// `k_EUnsignedCert_Allow` unconditionally under `STEAMNETWORKINGSOCKETS_OPENSOURCE`
// (`steamnetworkingsockets_connections.cpp:1806-1814`) and the P2P connection --
// our primary transport -- does not override it. The mechanism could never have
// hardened the lane it was written for. Making the identity BE the key needs no
// CA, no minting, no master round trip, and it therefore covers the direct-IP and
// LAN-only lanes too.
//
// WHAT THIS MODULE DOES NOT DO, AND MUST NOT BE READ AS DOING.
// `[V]` A cert's `identity_string` and its `key_data` are two INDEPENDENT
// protobuf fields, and GNS binds NEITHER to the other: `:1452-1458` compares the
// identity to the one the connection already expects, `:1497` verifies the
// session signature against `key_data`. So a peer can present a VICTIM'S public
// key as its identity while signing with its own cert key and pass every check
// the library makes. Installing an identity here is therefore only half the
// story -- `coop/net/peer_admission.h`'s challenge, where a peer must sign with
// the key its IDENTITY names, is what makes the identity mean anything. Delete
// that exchange and this module is decoration.
//
// THREADING. `Load()` runs once at boot (file I/O). `InstallInto()` runs on the
// thread that starts the session, before any connection exists. `Sign` / `Verify`
// are pure and re-entrant: they touch no shared state beyond the immutable key
// loaded at boot, so the net thread may call them freely.
//
// THE KEY FILE. `multivoid_identity.key`, beside `multivoid.ini` but NOT IN IT:
// anyone holding this file can BE you, and players paste their ini into bug
// reports. The old `player_guid=` line is retired with this module (RULE 2) --
// its migration advice ("copy the line to another PC") becomes "copy the key
// file", with the warning the old advice never needed.

#pragma once

#include <array>
#include <cstdint>
#include <string>

class ISteamNetworkingSockets;

namespace coop::net::peer_identity {

inline constexpr int kPubKeyBytes  = 32;  // Ed25519 public key == GNS k_cbMaxGenericBytes
inline constexpr int kPrivKeyBytes = 32;  // the seed; `[V]` CEC25519KeyBase::SetRawData wants 32
inline constexpr int kSigBytes     = 64;  // Ed25519 signature

using PubKey = std::array<uint8_t, kPubKeyBytes>;
using Sig    = std::array<uint8_t, kSigBytes>;

// Load the durable keypair, generating and persisting one on first launch.
// Returns false only when no key could be established at all (in which case the
// session must not start -- an identity-less peer cannot be admitted anywhere).
// A key that could not be PERSISTED still works for this session and says so in
// the log, mirroring how the retired `player_guid` handled an unreadable ini.
bool Load();

// Our own public key / the 32-char lowercase-hex guid derived from it. Both are
// empty/zero before a successful Load().
const PubKey& LocalPublicKey();
const std::string& LocalGuid();

// hex(SHA-256(pubkey)[0..16]) -- the canonical short form of ANY identity, used
// by the host to name a REMOTE peer's stored rows. Pure; 32 lowercase hex chars,
// or empty if `pub` is not a plausible key.
std::string GuidForPublicKey(const PubKey& pub);

// Install our identity into GNS for this process: builds a self-issued, unsigned,
// identity-bearing certificate carrying our private key and calls SetCertificate,
// which then establishes the local identity FROM the cert (`[V]`
// `csteamnetworkingsockets.cpp:779-787` accepts `private_key_data` when we hold
// no key, `:811-816` sets the identity). Must be called after GNS init and before
// any listen/connect. Returns false on any failure -- the caller must NOT start a
// session that would then present a different identity than it signs with.
bool InstallInto(ISteamNetworkingSockets* sockets);

// Sign / verify a domain-separated challenge blob. `Verify` takes the 32 identity
// bytes the caller read off the connection, so the question it answers is exactly
// "does this peer hold the key its identity names".
Sig  SignBlob(const uint8_t* data, size_t len);
bool VerifyBlob(const PubKey& pub, const uint8_t* data, size_t len, const Sig& sig);

// Un-gated arithmetic + crypto selftest, run once per session start. Logs
// `peer_identity selftest: ALL PASS (N checks)` or one FAIL line per failing
// check. Deliberately impossible to switch off, for the same reason the movement
// ledger's is: a wrong verdict here does not crash -- it either locks every
// honest player out or admits anyone, and both read as "working" from outside.
bool RunSelftest();

}  // namespace coop::net::peer_identity
