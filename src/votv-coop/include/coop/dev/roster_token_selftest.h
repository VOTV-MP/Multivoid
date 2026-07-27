// coop/dev/roster_token_selftest.h -- the successor-ban drill, in-process
// (`[dev] roster_token_selftest=1`, HOST only).
//
// WHY IT EXISTS. Arc A's most destructive claim is the one an idle smoke cannot
// touch: a moderation token captured from the person who WAS in a slot must be
// refused once someone else holds that seat. The failure it guards against is a
// permanent IP ban landing on a stranger who merely inherited the seat -- and the
// only honest way to observe it is to let a slot actually change hands and then
// fire the real action with the real stale token.
//
// The manual form of this drill is "open the ban modal, let the target leave, let
// a new peer take the slot, press Ban". Everything in that sentence except the
// two clicks is what this instrument reproduces: it captures a token when a slot
// is first occupied (the modal opening), HOLDS it across the departure (the
// admin typing a reason), and fires `moderation::BanPlayer` with it once a
// different person occupies the slot. The click itself is the only part not
// exercised; the code path from the token onward is the production one, not a
// re-implementation of its checks.
//
// WHAT IT ASSERTS, per hand-over:
//   NEGATIVE  Session::GetPeerAddressWithToken with the STALE generation must be
//             REFUSED. This is the guard that also covers the accept-ordering
//             case, because it validates against the LIVE net-layer authority
//             rather than the ledger mirror -- a successor accepted before the
//             game thread has reconciled is already rejected here.
//   POSITIVE  the same call with the successor's LIVE generation must be
//             ACCEPTED. Without it the negative proves nothing: a token check
//             that refuses everything would pass a negative-only drill
//             ([[feedback-probe-must-count-not-confirm]]).
//   REAL PATH `moderation::BanPlayer(stale)` must log its ABORT and write no ban
//             row. A drill that asserted the two primitives and skipped the real
//             call would be testing this file's copy of the rule, not the rule.
//
// The positive control is deliberately a READ (the address), not a ban that is
// meant to succeed: this instrument must never be able to ban anyone, on any
// path, however it fails.
//
// Dev instrument; RULE 2 does not retire probes
// ([[feedback-rule2-exempts-probes-diagnostics-tools]]). It never runs with the
// flag off, and it costs one latched bool read at Install when it is off.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::roster_token_selftest {

// Cache the session and subscribe to the ledger's hand-over fanout. Call once at
// boot (subsystems Install). No-op with the flag off or off-host.
void Install(coop::net::Session* session);

}  // namespace coop::dev::roster_token_selftest
