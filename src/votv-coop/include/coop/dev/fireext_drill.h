// coop/dev/fireext_drill.h -- drill: a fire extinguisher taken off its wall mount, and its foam (ini
// fireext_drill=off|carry|short|client|join|runaway|spray|spray_join / env VOTVCOOP_FIREEXT_DRILL; BOTH peers).
//
// One peer acts, both watch. The actor walks to the nearest mounted extinguisher by the bot director and grabs it through the
// use key's chain. carry: the host carries it and lets go; the client's setPropProps must not unseat the drive. short: the host
// lets go at once and re-sends the closed hold's pose, which receivers drop. client: the client carries. join: the host takes
// it off while a joiner loads. runaway: the host's damaged extinguisher runs away and the client's copy shows it and never
// runs it; a fresh one over the client takes the client's damage, which asks the host: rig: VERDICT fireext-runaway (reds
// fireext_claim_off, fireext_cancel_off, fireext_ask_off, fireext_thrust_say_off). spray and spray_join take no extinguisher
// off a mount (fireext_drill_foam.cpp): a peer's spray puts out the other peer's copy of a fire (rig: VERDICT fireext-spray,
// red fireext_mirror_ray_off), and a joiner is told a spray and a runaway that began before it joined, the client launched
// by mp.py's --client-after (rig: VERDICT fireext-spray-join, red fireext_snapshot_off). Each step waits on a state the actor
// reads; the last prints `ACTOR DONE`, a refused one `INVALID`. The watch prints every extinguisher and each change.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::fireext_drill {

// True unless the row is `off`.
bool IsEnabled();

// Advance this peer's steps and its watch. Every pump tick in a world. Game thread.
void Tick(coop::net::Session* session);

// The session ended: back to the first step, the watch emptied.
void OnDisconnect();

}  // namespace coop::dev::fireext_drill
