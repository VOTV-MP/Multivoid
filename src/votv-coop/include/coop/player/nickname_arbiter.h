// coop/player/nickname_arbiter.h -- the host is the canonical namer.
//
// AUTHORITY. Uniqueness cannot be decided by the peer that owns the name: two clients typing
// the same name each believe they are unique and neither can see the other's choice at the
// moment it is made. So the HOST -- the one peer that sees every name at once -- assigns the
// display name, and every peer including the named one adopts what it is handed.
//
// WHERE IT RUNS, and why it is NOT at Join receipt. A reconnecting peer can be live in a new
// slot while its OLD row has not yet been reaped, and arbitrating against a ghost would hand
// that human "name2" for colliding with itself, then "name3" the next time: a per-reconnect
// ratchet. roster_ledger::ReconcileFromSession runs DEATH first and unconditionally, so the
// occupied set is ghost-free by the time a row exists to name. Arbitration therefore keys on
// the LEDGER'S OCCUPIED ROWS, not on mirror-element existence -- the two are not co-timed.
#pragma once

#include <string>
#include <vector>

namespace coop::nickname_arbiter {

// What every un-drawable codepoint folds to. PUBLIC because a second site has to
// agree with it: the persist split in AdoptCanonicalNickname asks "did my request
// contain anything that folds to the sentinel?", and U+FFFD is IN the repertoire
// (it must be baked -- it is the fallback glyph) yet folds to itself, so a
// repertoire test alone would answer NO for a name whose key is pure sentinel.
// It is U+FFFD precisely because that is what those codepoints DRAW as.
inline constexpr wchar_t kAbsentSentinel = 0xFFFD;

// The collision key. Two display names collide iff their keys are equal -- which means "iff
// they could look the same", not merely "iff they are the same string".
//
// FoldKey folds in CODEPOINTS and case-folds through coop::text::CaseFold, a generated table.
// It maps every codepoint OUTSIDE coop::text::InRepertoire to one sentinel, because ImGui
// draws every absent codepoint as the SAME fallback glyph: two names with no codepoint in
// common would otherwise be two distinct keys and one identical nameplate. Sentinelling them
// makes them collide and one takes the suffix, so the guarantee holds on the SCREEN and holds
// independently of which fonts we embed. See coop/text/repertoire.h.
std::wstring FoldKey(const std::wstring& name);

// The policy, as a pure function: the first name in the dense-smallest-free
// sequence (requested, requested2, requested3, ...) whose fold key is not in
// `taken`. Split out from Assign so the selftest exercises the real decision
// without a ledger, a session or a game thread.
std::wstring AssignAgainst(const std::wstring& requested,
                           const std::vector<std::wstring>& taken);

// HOST ONLY, game thread. Return the display name `slot` should carry, given the name its
// owner REQUESTED and the names already assigned to every OTHER occupied ledger row.
// Idempotent -- our own row is excluded, so a retried Join lands on the same answer instead
// of walking the suffix upward.
//
// THE HANDBACK rides the existing RosterRow: the nick sits in the row's FIXED PREFIX, above the
// `applyDeclared` gate, so it reaches the described peer's own board. A row about ME also
// updates `g_localNick`, the single store chat_sync, peer_action_feed, roster.cpp's local-row
// reads and the Join payload all derive from (player_handshake::AdoptCanonicalNickname).
//
// AND IT IS NOT COSMETIC, which is why this file carries a selftest: the assigned name is
// written to multivoid.ini, so a suffix earned here follows a human into every future session.
// That is also why the ADOPT side splits -- a suffix earned against a REPERTOIRE collision is an
// artifact of this build's font set and must not rename anybody permanently
// (player_handshake_nick.cpp).
std::wstring Assign(int slot, const std::wstring& requested);

// Machine-asserted, printed at boot like `link-classify selftest`. Covers what
// no LAN drill can reach: the cap-displacing suffix, the two-20-char-names
// truncation trap, dense reuse after a departure, and fold-key case equality.
bool RunNicknameArbiterSelftest();

}  // namespace coop::nickname_arbiter
