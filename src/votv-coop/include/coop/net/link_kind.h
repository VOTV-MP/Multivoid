// coop/net/link_kind.h -- how one player's traffic reaches the session.
//
// Gameplay/network layer, in its own header because the net layer that MEASURES the kind and the
// roster ledger that publishes it both need the type, and the ledger must not pull in all of
// session.h.
//
// THE ONE QUESTION every player-list row answers is "how is THIS PLAYER connected to the session?"
// It has one answer, not one per viewer, so the HOST measures every link and publishes it on
// RosterRow and every board renders the same value. Measuring locally puts two axes in one column:
// the transport of the peers a viewer happens to hold a connection to, and a route word for the
// rest. Every kind is MEASURED FROM THE CONNECTION, never asserted from config -- reading "LAN" off
// a LanDirect topology setting labels a port-forwarded WAN peer LAN. For the same reason the kinds
// do not separate LanDirect from P2P: that is how a connection was ESTABLISHED, not how the player
// is CONNECTED, and a hole-punched route and a port-forwarded one are one thing from a player's
// seat.

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
// implemented beside the classifier in link_kind.cpp.
bool RunLinkClassifySelftest();

}  // namespace coop::net
