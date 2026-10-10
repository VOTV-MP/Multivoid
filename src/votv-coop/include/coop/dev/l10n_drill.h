// coop/dev/l10n_drill.h -- drill: the interface's language, judged on each peer (ini l10n_drill=
// off|translated|english / env VOTVCOOP_L10N_DRILL).
//
// Once this peer's world is up (the host: the client's), it opens the F1 menu on World > Rules and then
// Network > Stats, each time waiting until the render thread reports it DREW that pane, closes the menu,
// and waits until the chat feed holds the join line naming the other peer, in English or translated.
// Then it judges: the two subs' names as the tree draws them by whether their lookups found a
// translation (l10n::WasFound), the join line by which of its two forms the feed holds. `translated`
// passes when all three are translated; `english` when the interface is English and none is -- so the
// per-peer arm (an English host, a translated client) has a pass on both peers, and `translated` under
// an English language is the red. Lines: "[L10N-DRILL] DONE expect=<e>
// locale=<l> found=<k>/<m> distinct=<n>" or "[L10N-DRILL] FAIL expect=<e> ...". Readiness throughout,
// never a clock: a run that never gets there ends on the rig's budget with no line.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::l10n_drill {

// Advance this peer's steps. Every pump tick in a world; a latched read when off. Game thread.
void Tick(coop::net::Session* session);

// The session ended: the drill starts over and closes a menu it opened.
void OnDisconnect();

}  // namespace coop::dev::l10n_drill
