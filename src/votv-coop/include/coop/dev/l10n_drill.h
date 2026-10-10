// coop/dev/l10n_drill.h -- drill: the interface's language, judged on each peer (ini l10n_drill=
// off|translated|english|atlas / env VOTVCOOP_L10N_DRILL).
//
// Once this peer's world is up (the host: the client's), it opens the F1 menu on World > Rules and then
// Network > Stats, each time waiting until the render thread DREW that pane, closes the menu, and waits
// until the chat feed holds the join line naming the other peer. It judges the two subs' names by
// whether their lookups found a translation (l10n::WasFound) and the join line by which of its forms the
// feed holds: `translated` passes when all three are translated, `english` when the interface is
// English and none is (the per-peer arm has a pass on both peers; `translated` under English is the
// red). `atlas` measures instead: under the Chinese pack it pushes the CJK block into the chat a line
// at a time, with an atlas census after each, until a codepoint fails to pack or the block ends. Each
// prints a DONE or FAIL line tagged [L10N-DRILL]. Readiness throughout, never a clock.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::l10n_drill {

// Advance this peer's steps. Every pump tick in a world; a latched read when off. Game thread.
void Tick(coop::net::Session* session);

// The session ended: the drill starts over and closes a menu it opened.
void OnDisconnect();

}  // namespace coop::dev::l10n_drill
