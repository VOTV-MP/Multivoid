// coop/net/link_kind.h -- how one player's traffic reaches the session.
//
// Gameplay/network layer (principle 7). Its own header because BOTH the net
// layer (which MEASURES it) and the roster ledger (which CARRIES and publishes
// it) need the type, and the ledger must not pull in all of session.h.
//
// THE ONE QUESTION. Every player-list row answers exactly one thing: "how is
// THIS PLAYER connected to the session?" Before v131 the answer was computed
// from "what can I measure about you", which is a different question per viewer
// -- so a client's board showed the transport of the peers it happened to hold
// a connection to and the ROUTE ("VIA HOST") for the rest, two axes in one
// column (user, 2026-07-27: "why it says via host on 2 clients and lan on one
// client. It should be all the same, no special treatment"). Now the HOST
// measures every link and publishes the answer on RosterRow, so every board
// renders the same value for the same player.
//
// EVERY KIND IS MEASURED FROM THE CONNECTION, never asserted from config. The
// pre-v131 code returned "LAN" whenever cfg_.topology was LanDirect -- a config
// assertion, so a port-forwarded WAN peer was labelled LAN. A value nobody
// measured is the same defect as "VIA HOST" wearing a truer-looking word.
//
// The kinds deliberately do NOT distinguish LanDirect from P2P: that describes
// how a connection was ESTABLISHED (our config), not how the player is
// CONNECTED. A hole-punched P2P route and a port-forwarded direct route are the
// same thing from a player's seat.

#pragma once

#include <cstdint>

namespace coop::net {

enum class LinkKind : uint8_t {
    Unknown = 0,  // no live connection yet, or the transport could not be read
    Local   = 1,  // this player IS the session host -- their traffic never crosses a socket
    Lan     = 2,  // loopback or an RFC1918 private address
    Direct  = 3,  // a public address, no relay in the path
    Relayed = 4,  // GNS reports the path is relayed (TURN / SDR)
};

// Wire-safe narrowing: an unknown byte from the wire becomes Unknown rather
// than an out-of-range enum. The receiver renders Unknown as "no answer yet",
// which is exactly what an unrecognized kind means.
inline LinkKind LinkKindFromWire(uint8_t v) {
    return (v <= static_cast<uint8_t>(LinkKind::Relayed)) ? static_cast<LinkKind>(v)
                                                          : LinkKind::Unknown;
}

// Machine-assert the address classifier over SYNTHETIC addresses, once at boot.
//
// WHY IT EXISTS: two of the four kinds are UNREACHABLE by any drill we can run.
// Every peer in a LAN smoke -- and in a same-box P2P run -- resolves to
// loopback, so `Direct` never happens; `Relayed` needs a real TURN path. Without
// this, shipping the classifier would mean shipping two branches nothing ever
// executes, plus the edges that are easy to get wrong: GetIPv4() returns HOST
// byte order, an IPv4-mapped-IPv6 address must still classify, and a real IPv6
// address yields 0 there and must NOT be read as 0.0.0.0.
//
// It COUNTS (cases passed / total) rather than confirming, and it carries known
// NEGATIVES (public addresses that must NOT read as Lan), so a classifier that
// answered one value for everything would fail it. Declared here -- with no GNS
// types in the signature -- so the header stays free of the Steam headers;
// implemented beside the classifier in session_status.cpp.
bool RunLinkClassifySelftest();

}  // namespace coop::net
