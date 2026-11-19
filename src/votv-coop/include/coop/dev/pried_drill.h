// coop/dev/pried_drill.h -- [dev] a crowbar's pry of a plank, a radiator and a security TV, judged from the host
// (ini pried_drill=off|host|tv|client|far|twice|join|midjoin / env VOTVCOOP_PRIED_DRILL; BOTH peers).
//
// The host spawns the fixture, static as the game places it, ahead of the client's puppet; the client finds its copy by the
// key the host says in the lobby chat. Every wait ends on an event a peer printed; a bound says INVALID, never a verdict.
//   host, tv   the host pries a radiator, a security TV: the client's broken joints (BreakConstraint's native post hook is
//              the witness: IsBroken reads 0 after a break) equal the host's, and the copies rest within 1 cm and 1 deg.
//   client     the client pries a plank by activatePrying and End: its crowbarOpen is asked, not run, the host grants it.
//   far        a raw ask for a plank 3000 uu away: refused as out of reach, the plank stays stuck.
//   twice      the host pries a plank twice: the second crowbarOpen is cancelled, its body ran once.
//   join       the client's life 1 prints `pried-drill: REJOIN-NOW`; life 2 (--rejoin-env VOTVCOOP_PRIED_LIFE=2) takes
//              its joints from the host's prop row. midjoin: the host pries as the joiner's snapshot rows drain.
//   rig: VERDICT pry-<arm>  PASS | FAIL | INVALID. Reds (each a [dev] row): pry_judge_off (far), pry_ask_off (client),
//   pry_coast_off (host, tv), pry_row_off (join), pry_again_off (twice).

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::dev::pried_drill {

bool IsEnabled();

// The drill's pass on either peer. A single read when off. Game thread.
void Tick(coop::net::Session* session);

// Back to the first step. Net disconnect (the host keeps its fixture across the client's departure in the join arms).
void OnDisconnect();

}  // namespace coop::dev::pried_drill
