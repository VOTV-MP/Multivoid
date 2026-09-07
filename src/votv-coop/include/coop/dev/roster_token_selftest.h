// coop/dev/roster_token_selftest.h -- the successor-ban drill, in-process (`[dev]
// roster_token_selftest=1`, HOST only).
//
// It covers the claim an idle smoke cannot reach: a moderation token captured from the person who
// WAS in a slot must be refused once someone else holds that seat, or a permanent IP ban lands on a
// stranger who merely inherited it. The only honest way to observe that is to let a slot actually
// change hands and then fire the real action with the real stale token.
//
// Manually the drill is "open the ban modal, let the target leave, let a new peer take the slot,
// press Ban". Everything there except the two clicks is what this reproduces, and from the token
// onward the path is the production one rather than a re-implementation of its checks. The positive
// control is deliberately a READ, the peer address, never a ban meant to succeed: this instrument
// must not be able to ban anyone, on any path, however it fails. Dev instrument, exempt from
// retirement, never runs with the flag off, one latched bool read at Install when it is.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::roster_token_selftest {

// Cache the session and subscribe to the ledger's hand-over fanout. Call once at boot (subsystems
// Install); a no-op with the flag off or off-host.
//
// On each hand-over it asserts three things. NEGATIVE: Session::GetPeerAddressWithToken with the
// STALE generation must be REFUSED -- this also covers the accept-ordering case, because it
// validates against the LIVE net-layer authority rather than the ledger mirror, so a successor
// accepted before the game thread has reconciled is already rejected. POSITIVE: the same call with
// the successor's LIVE generation must be ACCEPTED, without which the negative proves nothing,
// since a check that refuses everything would pass a negative-only drill. REAL PATH:
// moderation::BanPlayer(stale) must log its ABORT and write no ban row -- asserting the two
// primitives alone would test this file's copy of the rule rather than the rule.
void Install(coop::net::Session* session);

}  // namespace coop::dev::roster_token_selftest
