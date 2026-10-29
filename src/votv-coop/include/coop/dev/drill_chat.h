// coop/dev/drill_chat.h -- the lobby-chat lines two drill peers say to each other. A drill that sequences its peers on
// an edge the other one printed says a line no save can hold and reads the other's: the host says it is ready, the
// client says its start set is recorded. Nothing here ships enabled.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::drill_chat {

// Queues the line. The host refuses a slot's line inside 0.66 s of its last, with no answer (chat_rate.h), so a drill's
// lines leave one at a time, each when the host's own rule would take it: Pump sends them.
void Say(const std::string& line);

// Once per tick, `nowMs` the net clock's milliseconds: sends the oldest queued line when no sent line awaits its echo
// (the host's commit in the chat feed) and chat_rate::Allows it, the budget's last talk being the echo's time. A line
// with no echo in a few seconds is sent again, a few times, then dropped with a WARN: it arrives later or says so.
void Pump(uint64_t nowMs);

// Empties the queue, the wait for an echo and the budget: a session's lines belong to that session.
void Reset();

// True when a LIVE chat row holds `needle`. A row seeded at the join sits in the retained tier and never counts, so a
// peer acts on what the other said after it joined, never on the record of an earlier life.
bool ChatHasLive(const std::string& needle);

// True with the text after `prefix` (trailing spaces cut, never empty) from the LAST live row that starts with `prefix`.
// A retained row never counts, as in ChatHasLive.
bool LiveRestOf(const std::string& prefix, std::string* rest);

// Appends to `rests` the text after `prefix` (trailing spaces cut, never empty) of EVERY live row that starts with it.
// A retained row never counts. For a line said in parts: the reader keeps each part it has seen, so none is lost to the
// feed retiring a row (LiveRestOf sees only the last).
void LiveRestsOf(const std::string& prefix, std::vector<std::string>* rests);

// A feed walk is a few times a second: true on every fifteenth call.
bool PollDue();

}  // namespace coop::dev::drill_chat
