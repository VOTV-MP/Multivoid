// coop/net/lobby_proof.h -- the proof a host gives a master that the key it announces is its own.
// Its own file because it is its own contract: the master answers `POST /v1/host/challenge` with a
// 64-hex nonce, and `POST /v1/host` carries `nonce` and `proof`, an Ed25519 signature over
// "multivoid-lobby-announce-v1" || identity || nonce || master name, no separators. Two parties
// build those bytes: this client and `server/src/lobby_proof.rs`; the shared known-answer vector
// `server/tests/vectors/lobby_proof.json` is what keeps them equal.
//
// The name signed is the master's own (the host part of the URL the host dialled): a proof made
// for a rogue master names the rogue, which no honest master accepts, so a rogue cannot relay a
// host's proof to another master.
//
// Nothing here touches a socket or announce state: the caller hands over the identity, the nonce
// and the master's name, and gets the proof to send back.

#pragma once

#include <string>

namespace coop::net::lobby_proof {

// The proof for one create: 128 lower-case hex, the signature of our durable key over the blob
// above. Empty, with the reason logged, when the inputs are not the shapes the master rebuilds the
// blob from (an identity that is not `gen:` plus 64 hex, a nonce that is not 64 lower-case hex, an
// empty master name): a signature over bytes the master cannot rebuild proves nothing, so the
// caller treats empty as "cannot sign" and does not send a proof.
std::string ProofHex(const std::string& identity, const std::string& nonceHex,
                     const std::string& masterName);

// The blob's bytes against the shared vector, the vector's signature against its identity's key,
// a flipped bit refused, a proof for another master name refused, the URL-to-name rows the
// master's tests check too (through http::HostName), and, when our key is loaded, a round trip
// through our own. Once per process (a later call returns the first run's verdict,
// silently); logs an all-pass line or one failure line per failing check. Un-gated, for the
// reason the other identity selftests are: a proof the master rejects does not crash, it unlists
// the host.
bool RunSelftest();

}  // namespace coop::net::lobby_proof
