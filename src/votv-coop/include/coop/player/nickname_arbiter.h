// coop/player/nickname_arbiter.h -- ARC B: the host is the canonical namer.
//
// THE ASK (user, 2026-07-27): "Pelmentor (host), Pelmentor2, Pelmentor3" --
// "просто чтобы у всех был уникальный Nameplate".
//
// AUTHORITY. Uniqueness cannot be decided by the peer that owns the name: two
// clients typing "Pelmentor" each believe they are unique and neither can see
// the other's choice at the moment it is made. So the HOST -- the one peer that
// sees every name at once -- assigns the display name, and every peer including
// the named one adopts what it is handed. This is the same shape as every other
// host-arbitrated field: assigned, never asserted (docs/COOP_SYNCER_MODEL.md).
//
// WHERE IT RUNS -- and why it is NOT at Join receipt. A reconnecting peer can be
// live in a new slot while its OLD row has not yet been reaped, and arbitrating
// against a ghost would hand that human "Pelmentor2" for colliding with itself,
// then "Pelmentor3" the next time: a per-reconnect ratchet. The ledger already
// solves this and we ride its guarantee rather than adding a second one --
// roster_ledger::ReconcileFromSession runs DEATH first and UNCONDITIONALLY
// (roster_ledger.cpp:289, "a generation that no longer matches the row's means
// the previous occupant is gone"), so the occupied set is ghost-free by the time
// a row exists to name. Arbitration therefore keys on the LEDGER'S OCCUPIED
// ROWS, which is also the 42-round pass's R4 finding (key on the nick STORE, not
// on mirror-element existence -- they are not co-timed).
//
// THE HANDBACK rides the existing RosterRow: the nick sits in the row's FIXED
// PREFIX, above the `applyDeclared` gate, so it reaches the described peer's own
// board (player_handshake_roster.cpp:302-307). What arc B adds on the receiving
// side is that a row about ME also updates `g_localNick` -- the single store
// that chat_sync, peer_action_feed, both roster.cpp local-row reads and the Join
// payload all derive from (see player_handshake::AdoptCanonicalNickname).
//
// ALPHABET. FoldKey is ASCII case-folding because SanitizeNickname currently
// strips everything else. Arc D replaces the fold and the cap UNIT together
// (UTF-16 units -> codepoints); a name re-arbitrates on the next join and
// nothing persists across it (bans key on IP, seen-players on GUID), so the
// replacement is cosmetic and self-healing.
#pragma once

#include <string>
#include <vector>

namespace coop::nickname_arbiter {

// The collision key. Two display names collide iff their keys are equal.
// ASCII case-fold today; arc D widens it.
std::wstring FoldKey(const std::wstring& name);

// The policy, as a pure function: the first name in the dense-smallest-free
// sequence (requested, requested2, requested3, ...) whose fold key is not in
// `taken`. Split out from Assign so the selftest exercises the real decision
// without a ledger, a session or a game thread.
std::wstring AssignAgainst(const std::wstring& requested,
                           const std::vector<std::wstring>& taken);

// HOST ONLY, game thread. Return the display name `slot` should carry, given
// the name its owner REQUESTED and the names already assigned to every OTHER
// occupied ledger row. Idempotent -- our own row is excluded, so a retried Join
// lands on the same answer instead of walking the suffix upward.
std::wstring Assign(int slot, const std::wstring& requested);

// Machine-asserted, printed at boot like `link-classify selftest`. Covers what
// no LAN drill can reach: the cap-displacing suffix, the two-20-char-names
// truncation trap, dense reuse after a departure, and fold-key case equality.
bool RunNicknameArbiterSelftest();

}  // namespace coop::nickname_arbiter
