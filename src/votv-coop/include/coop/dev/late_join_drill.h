// coop/dev/late_join_drill.h -- [dev] what the bind wait held, replayed and dropped for a joiner, per message kind,
// and whether the host's staged world stands on the joiner. Set late_join_drill=1 on both peers. It tests the
// modules' own hold and replay, where each module holds its message for a prop that has no actor yet and replays it
// at the bind. One life, no rejoin. The HOST, once its own world is up and before any client joins, stages one
// subject of each lane in front of its player (an open portable PC, a floppy box, a Kerfus switched on, a drive rack
// with two rows, a drive seated in a slot) and says `[LATE-JOIN] host STAGED ...`. The CLIENT, the first time the
// join's phase returns to idle after it had left idle, checks the staged world is in its world (exactly one open PC
// and one floppy box, a Kerfus, a rack, the staged drive: a fresh world holds no box and no open PC of its own, so
// the drill is valid on a --host-fresh world) and prints one tally line per kind (held, coalesced, replayed,
// dropped, given up, never bound) and a totals line, tagged at=idle; ten seconds later the same again, tagged
// at=idle+10s, then its verdict, then `[LATE-JOIN] client DONE`, once.
// The verdict is read by the joiner's own bind wait row: `hold` (bind_wait_mark_unknown on, the host's
// join_gate_red=1) wants every seed held and replayed, none lost; `clean` (it off) wants nothing never-bound. It
// prints PASS and the number of checks, or one FAIL line per failing check, each line led by the arm's name. A
// staged world missing from the join prints one FAIL line, `the staged world was not in this join`, and no verdict.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::late_join_drill {

// Game thread, once per pump tick; a latched bool read when off.
void Tick(coop::net::Session* session);

// The session ended: a later session's client starts over.
void OnDisconnect();

}  // namespace coop::dev::late_join_drill
