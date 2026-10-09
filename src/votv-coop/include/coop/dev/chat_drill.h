// coop/dev/chat_drill.h -- drill: the chat's checks, judged in memory (ini chat_drill=
// off|history|seed|i18n|span / env VOTVCOOP_CHAT_DRILL; every peer in the arm).
//
// Sends through the chat's own submit call and reads the feed's rows (chat_feed::ForEachRow); no
// keystroke and no log text is judged. Every line begins "[chat-drill] <role> " (host, c1, c2, c3),
// so any peer rebuilds another's line from a role and a number. history: host + c1, the HOST
// judges the retained tier and the open history. seed: host + c1, then c2, which JUDGES the
// joiner's seed and the lines said while it loaded. i18n: all four, EVERY peer judges the others'
// lines, in four scripts, and nicknames; its wait restarts its budget at each new line and its
// ABORT names the lines still missing. span: every peer pushes a peer-action line with its nick
// mid-line and one whose nick the 255-byte cut reaches, and judges what the feed hands back. Lines tagged [CHAT-DRILL]: "<role> PASS", "<role> FAIL:
// <why>" (roles, indices, counts; never chat text), "<role> ABORT: <why>" (not a measurement: a
// wait ran out or the case could not be exercised), and the seed arm's rig cue "host HAS c1 4".
// Reds: chat_no_retain (history), chat_seed_suppress (seed), chat_drill_bad_nick (i18n, on c1),
// chat_span_prefix (span).

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::chat_drill {

// Advance this peer's phases. Every pump tick in a world; a latched read when off. Game thread.
void Tick(coop::net::Session* session);

// The session ended: the drill starts over, and a chat the drill opened is closed.
void OnDisconnect();

}  // namespace coop::dev::chat_drill
