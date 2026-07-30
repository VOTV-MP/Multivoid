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
// ALPHABET (arc D2, built 2026-07-28). FoldKey folds in CODEPOINTS, case-folds
// via coop::text::CaseFold -- a GENERATED table, since 2026-07-30; the hand-
// written one covered ASCII, Latin-1 and Cyrillic while claiming to cover
// "exactly the cased scripts the repertoire draws", and was 73% self-folding
// by the time anyone counted (Greek 146, Armenian 38, Georgian 38, Latin-Ext
// 374). It maps
// every codepoint OUTSIDE coop::text::InRepertoire to one sentinel -- because
// ImGui draws every absent codepoint as the SAME fallback glyph, so two names
// with no codepoint in common can be two distinct keys and one identical
// nameplate. Sentinelling them makes them collide and one takes the suffix, so
// the guarantee holds on the SCREEN and holds independently of which fonts we
// embed. See coop/text/repertoire.h for why the layer moved.
//
// AND IT IS NOT COSMETIC. An earlier draft of this comment said a name
// "re-arbitrates on the next join and nothing persists across it" -- that was
// FALSE when written and is worth naming, because it is the reason this file
// gets a selftest rather than a shrug: player_handshake.cpp:290 writes the
// assigned name to multivoid.ini (the user's 2026-07-28 keep-the-name decision),
// so a suffix earned here follows a human into every future session. That is
// also why the ADOPT side splits: a suffix earned against a REPERTOIRE
// collision is a rendering artifact of this build's font set, and widening the
// repertoire later must not have renamed anybody permanently.
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

// The collision key. Two display names collide iff their keys are equal --
// which now means "iff they could look the same", not merely "iff they are the
// same string".
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
