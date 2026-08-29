// coop/net/peer_admission.h -- the mutual challenge a peer must pass before it
// is a peer at all.
//
// WHAT THIS DECIDES. `coop/net/peer_identity.h` gives every install a durable
// Ed25519 keypair whose PUBLIC KEY is its network identity. That is a NAME. This
// module is the only thing that makes the name mean anything: before a host
// spends a player seat, the two ends sign each other's nonce with the key their
// own identity names, and each verifies against the 32 identity bytes GNS handed
// it for the connection in hand.
//
// WHY IT IS LOAD-BEARING AND NOT A CEREMONY. `[V]` A GNS certificate's
// `identity_string` and its `key_data` are independent protobuf fields and the
// library binds neither to the other: `connections.cpp:1452-1458` compares the
// identity to the one the connection already expects, `:1497` verifies the
// session signature against `key_data`, and NOTHING compares the two. So a peer
// can present a victim's public key as its identity and pass every check GNS
// makes. What refuses it is this exchange. Delete it and the identity is
// forgeable again, and the storage guid derived from it goes back to being a
// bearer token (security A15).
//
// WHAT IT DOES NOT CLOSE. A pure RELAY (security P1) defeats any challenge: it
// forwards the nonce and forwards the signature, and `[V]` GNS exposes no key
// exporter (`isteamnetworkingsockets.h:384`), so there is no channel value to
// bind the proof to. Closing that needs the ~6-line fork of the vendored library
// in `PLAN_01_PEER_AUTH.md` s6, which is a decision about owning a repo, not a
// thing this module can do. Nothing here should be read as anti-MITM.
//
// SHAPE OF THE EXCHANGE (three messages; the host proves itself FIRST).
//   C->H  AuthHello     { clientNonce }
//   H->C  AuthChallenge { hostNonce, sigHost(blob[HOST, hostPub, clientPub, clientNonce]) }
//   C->H  AuthProof     { sigClient(blob[CLIENT, hostPub, clientPub, hostNonce]) }
// then the host admits, and its EXISTING `AssignPeerSlot` is the client's signal
// that it may begin. There is deliberately no fourth message: `[V]`
// FinishPeerConnected sends AssignPeerSlot only on a host, and on a host it runs
// only from AdmitPending, so that packet already means "you were admitted" -- and
// making the client wait for it removes a race that a self-declared "I am done"
// would have had, since the client's first real message (SaveTransferRequest)
// rides a DIFFERENT lane than the proof and could overtake it.
//
// THE BLOB IS DOMAIN-SEPARATED AND NAMES BOTH ENDS. A signature carries a fixed
// tag, a direction byte, the protocol version, both public keys and the
// verifier's nonce. Both identities are in it so a signature harvested from one
// session cannot be replayed toward a THIRD party, and the direction byte is
// what stops the host's own challenge being reflected back as the client's proof.
// Each side takes the counterparty's key from ITS OWN connection and never from
// the message -- a blob built from an attacker-supplied key would verify against
// that key and prove nothing.
//
// THREADING. Everything here runs on the NET thread: both hosts' park edges, the
// pending drain, and the client's status callback. There is no lock, because
// there is no other thread. It touches no engine object, which is why it can run
// while a joining client has no world at all.
//
// THAT CLAIM IS A CONSTRAINT ON CALLERS, and it was briefly false: `Session::Stop`
// runs on the GAME thread and called `ClientReset()` BEFORE joining the net
// thread, which left a ~5-10 ms window racing `ClientOnReliable`. Fixed
// 2026-08-29 (post-ship audit) by moving the reset after the join. **A new caller
// must be on the net thread, or after it is joined -- there is no third option
// here, and nothing in the type system says so.**

#pragma once

#include <cstdint>

#include "coop/net/peer_identity.h"
#include "coop/net/protocol.h"

namespace coop::net {
class Session;
}

namespace coop::net::peer_admission {

// What the host should do with a pending connection after this message.
enum class Verdict {
    Continue,  // the exchange is progressing; keep the connection parked
    Admit,     // proved -- spend the seat
    Refuse,    // close it, with `reason` naming why (never silent: a joiner that
               // is refused must be able to tell that from a hang)
};

struct HostResult {
    Verdict     verdict = Verdict::Refuse;
    const char* reason  = "admission failed";
    // Valid only on Admit: the key the peer just PROVED it holds. The caller
    // derives the storage guid from this and from nothing on the wire.
    peer_identity::PubKey provedKey{};
};

// HOST: a parked connection sent a reliable message. `pendIdx` is its slot in the
// pending band; `kind`/`payload`/`len` are the already-parsed reliable body.
HostResult HostOnPendingReliable(Session& session, int pendIdx, uint32_t hConn,
                                 ReliableKind kind, const void* payload, int len);

// HOST: has this pending index got an exchange IN PROGRESS -- i.e. did the peer
// send a well-formed AuthHello that we answered? The band's eviction policy asks
// this so a socket that has said NOTHING is evicted before one that is mid-proof:
// without it, "evict the oldest" is a policy an attacker times, because an honest
// joiner's entry becomes the oldest as soon as enough silent sockets arrive after
// it. NET THREAD ONLY.
bool HostHasOpenExchange(int pendIdx);

// HOST: drop a pending index's exchange state. Called on admit AND on close, so
// a recycled index can never inherit a previous peer's nonce.
void HostForgetPending(int pendIdx);

// CLIENT: our link to the host reached Connected. Opens the exchange. Returns
// false when we cannot even start (no identity, or the host presented something
// that is not a 32-byte key identity) -- the caller closes the connection.
bool ClientOnConnected(Session& session, uint32_t hConn);

// CLIENT: an inbound reliable arrived on the host link. Returns true when this
// module CONSUMED the message (it was part of the exchange and must not reach the
// game thread). `outClose` is set to a reason when the exchange failed and the
// caller must close the connection.
bool ClientOnReliable(Session& session, uint32_t hConn, ReliableKind kind,
                      const void* payload, int len, const char** outClose);

// CLIENT: has the host proved possession of the key its identity names? The
// admission signal (AssignPeerSlot) is refused unless this is true -- otherwise a
// host that skipped the challenge could seat us anyway.
bool ClientProvedHost();

// CLIENT: forget the exchange (session stop / link closed).
void ClientReset();

// Un-gated selftest of the blob construction and the verify decision, run once
// per session start beside peer_identity's. It drives the REAL blob builder and
// the REAL verifier, and its arms are the ones no LAN drill can stage: a proof
// replayed in the wrong direction, a proof aimed at a third party, and a proof
// for a different nonce. A verifier that accepts everything passes every positive
// test there is, so the negatives are the test.
bool RunSelftest();

}  // namespace coop::net::peer_admission
