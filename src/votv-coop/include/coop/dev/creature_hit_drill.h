// coop/dev/creature_hit_drill.h -- drill: a parked creature's hit reaches a client once, and a client's hit on the
// creature is judged by the host (ini creature_hit_drill=off|zombie|fossilhound / env VOTVCOOP_CREATURE_HIT_DRILL;
// BOTH peers, one run, no rejoin).
//
// The host spawns the arm's creature 300 cm in front of the client's puppet once the client's join is over; the
// creature walks to the player, and no player is moved. INBOUND: the creature's hits on the puppet are delivered to
// the client (three applied, and none made by the mirror's own copy of the hit). OUTBOUND: with an empty hand the
// client's call of the mirror's addDamage is refused by the host; with an axe in its hand the client calls it every
// 600 ms, as a player's swings, until the mirror is destroyed, and the host's creature must have died of judged
// hits. The fossilhound arm adds its per-viewer perception. Every verdict line carries the tag CHD in brackets, then
// PASS or FAIL, the step and the role, and a FAIL names its reason; a step's expiry is a FAIL line, never a silence.
// The step list is in coop/dev/creature_hit_drill.cpp. Run: the rig's smoke with the row set on both peers,
// --host-fresh, the client's DONE line as the done marker and the FAIL lines as the fail marker.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::creature_hit_drill {

// True unless the row is `off`.
bool IsEnabled();

// Cache the session. Called from the dev install fanout; no-op when off.
void Install(coop::net::Session* session);

// Advance this peer's steps. Every pump tick in a world. Game thread.
void Tick();

// The session ended: the client's steps back to the first. The host's spawn and creature stay with its process.
void OnDisconnect();

}  // namespace coop::dev::creature_hit_drill
