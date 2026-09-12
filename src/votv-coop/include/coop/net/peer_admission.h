// coop/net/peer_admission.h -- the mutual challenge a peer must pass before it is a peer at
// all. peer_identity gives every install a durable Ed25519 keypair whose public key is its
// network identity, a name; this module is what makes the name mean anything: before a host
// spends a player seat, the two ends sign each other's nonce with the key their own identity
// names, and each verifies against the identity bytes GNS handed it for the connection in
// hand. Load-bearing, not ceremony: a GNS certificate's identity and its key data are
// independent fields the library never compares, so a peer can present a victim's public key
// as its identity and pass every check GNS makes; this exchange refuses it, and without it the
// storage guid derived from the identity is a bearer token. Not closed: a pure relay forwards
// the nonce and the signature, and GNS exposes no key exporter to bind the proof to the
// channel, so nothing here is anti-MITM. Three messages, the host proving itself first: hello
// {clientNonce}; challenge {hostNonce, flags, sigHost}; proof {sigClient, hasPw, pwTag}; then
// the host's existing slot assignment is the admission signal, since it is sent only from the
// admit path. The lobby password rides the proof as a separate field, deliberately outside the
// signed blob (lobby_password.h has the rule); hasPw is 0 unless the client bound the host.

#pragma once

#include <cstdint>

#include "coop/net/end_reason.h"
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
    EndReason   code    = EndReason::None;  // on Refuse: the code the peer is told
    // Valid only on Admit: the key the peer just proved it holds. The caller derives the storage
    // guid from this and from nothing on the wire.
    peer_identity::PubKey provedKey{};
};

// Host: a parked connection sent a reliable message. `pendIdx` is its slot in the pending
// band; `kind`, `payload` and `len` are the already-parsed reliable body. The signed blob is
// domain-separated and names both ends: a fixed tag, a direction byte, the protocol version,
// both public keys, the verifier's nonce, the challenge flags and the host's own nonce. Both
// identities, so a harvested signature cannot be replayed toward a third party; the direction
// byte, so the host's challenge cannot be reflected back as the client's proof; the flags and
// the host nonce, so a relay cannot set the password-wanted flag on an open lobby's challenge
// and harvest a real tag from a bound client. Each side takes the counterparty's key from its
// own connection, never from the message.
HostResult HostOnPendingReliable(Session& session, int pendIdx, uint32_t hConn,
                                 ReliableKind kind, const void* payload, int len);

// Host: has this pending index got an exchange in progress, a well-formed hello we answered?
// The band's eviction policy asks, so a socket that has said nothing is evicted before one
// that is mid-proof; without it, evict-the-oldest is a policy an attacker times, since an
// honest joiner's entry becomes the oldest as soon as enough silent sockets arrive after it.
// Net thread only.
bool HostHasOpenExchange(int pendIdx);

// Host: drop a pending index's exchange state. Called on admit and on close, so a recycled
// index can never inherit a previous peer's nonce.
void HostForgetPending(int pendIdx);

// Host: the accept edge says whether it counted this pending entry against the connection cap,
// which it can only where the transport knew an address there; the proof counts an entry the
// edge could not. Called right after the entry is parked, net thread.
void HostMarkCountedAtEdge(int pendIdx, bool counted);

// Client: our link to the host reached connected. Opens the exchange. False when we cannot
// even start (no identity, or the host presented something that is not a 32-byte key
// identity); the caller closes the connection.
bool ClientOnConnected(Session& session, uint32_t hConn);

// Client: an inbound reliable arrived on the host link. True when this module consumed the
// message (part of the exchange; it must not reach the game thread). `outClose` is set to a
// reason, and `outCode` to its code, when the exchange failed and the caller must close the
// connection.
bool ClientOnReliable(Session& session, uint32_t hConn, ReliableKind kind,
                      const void* payload, int len, const char** outClose,
                      EndReason* outCode);

// Client: has the host proved possession of the key its identity names? The admission signal
// is refused unless this is true, else a host that skipped the challenge could seat us.
bool ClientProvedHost();

// Client: forget the exchange (session stop or link closed). Everything in this module runs on
// the net thread with no lock: both park edges, the pending drain and the client's status
// callback; it touches no engine object, so it runs while a joining client has no world. A
// caller must be on the net thread or run after it is joined, which is why the session stop
// calls this after joining the net thread.
void ClientReset();

// The selftest of the blob construction and the verify decision, run once per session start
// beside the identity one. It drives the real blob builder and the real verifier, and its
// arms are the ones no LAN drill can stage: a proof replayed in the wrong direction, a proof
// aimed at a third party, and a proof for a different nonce. A verifier that accepts
// everything passes every positive test, so the negatives are the test.
bool RunSelftest();

}  // namespace coop::net::peer_admission
