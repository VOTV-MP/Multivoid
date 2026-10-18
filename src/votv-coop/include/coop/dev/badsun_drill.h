// coop/dev/badsun_drill.h -- drill: the Bad Sun on a joined pair (ini badsun_drill=off|clock|midnight|spawn|cheat|
// join|end|absent_stale / env VOTVCOOP_BADSUN_DRILL; BOTH peers, the same arm). Every phase waits on a state its peer
// can read, never on a clock; a phase that does not come ends FAIL after a bound. Each verdict is one line: the tag,
// the arm, the role, PASS, FAIL or INCONCLUSIVE, then the reason (a client's names its live count and each copy's
// state); every other line starts with the drill key and a colon, so a run's done marker meets the verdict alone.
// Nobody walks. The lane's own counters (coop/world/weather_badsun) are the receiver evidence, never its log. What
// each arm sets up and judges is on its enumerator in coop/dev/badsun_drill_shared.h. Run: the rig's smoke with
// badsun_drill=<arm> on both peers, the client's verdict line as its done marker; the clock, join, end arms on the
// rig's save, the others on a fresh host world.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::badsun_drill {

// True unless the row is `off`.
bool IsEnabled();

// Cache the session. Called from the dev install fanout; no-op when off.
void Install(coop::net::Session* session);

// Advance this peer's phase. Every pump tick in a world. Game thread.
void Tick();

// The session ended: back to the first phase.
void OnDisconnect();

}  // namespace coop::dev::badsun_drill
