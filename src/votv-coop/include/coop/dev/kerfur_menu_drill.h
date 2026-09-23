// coop/dev/kerfur_menu_drill.h -- drill: a kerfur's two conversions on the host, one by its verb and
// one by the menu event a player's press dispatches (ini kerfur_menu_drill=1 / env
// VOTVCOOP_KERFUR_MENU_DRILL; HOST).
//
// The form assembler resets its per-conversion record when a verb is the outermost of its own verbs.
// A press does not call the verb: the prop's actionOptionIndex does, from its event graph, and other
// lanes name-watch that event, so the verb's body runs nested inside a watched one. This drill makes
// both shapes on one kerfur. Once a client's join is over by its own account (the slot world-ready and
// its bracket closed), it spawns a kerfur, turns it off by calling dropKerfurProp directly, then turns
// the prop back on through actionOptionIndex with the action the prop answers with spawnKerfuro. It
// holds its own watch on actionOptionIndex, so the nesting does not rest on another lane's. Each step
// waits on the object it needs to exist; a refused step ends the drill INVALID with its reason. The
// evidence is the assembler's summary, printed at the end: the order gate's spawn-before-destroy count
// against its destroy-without-spawn count.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::kerfur_menu_drill {

bool IsEnabled();

// Cache the session, for the role. Called from the subsystems Install fanout every pump tick;
// idempotent. No-op when off.
void Install(coop::net::Session* session);

// Advance the host's phase. Every pump tick in a world. Game thread.
void Tick();

// The session ended: back to the first phase.
void OnDisconnect();

}  // namespace coop::dev::kerfur_menu_drill
