// coop/dev/hunt_drill.h -- drill: the host's hunters go for the nearest player (ini hunt_drill=off|krampus|follower|gray / env VOTVCOOP_HUNT_DRILL; the HOST runs it, the client only joins and takes what is delivered).
//
// The host first goes to the drill's stage, open ground outside the base, and the client joins there (join_at_host).
// Two players cannot be staged at different distances (a drill may place a player nowhere but one fixed spot, and the client joins at
// the host's), so "the client is nearer" is made by the dev switch hunt_skip_host (VOTVCOOP_HUNT_SKIP_HOST=1), which leaves the host out
// of the hunters' candidates. With it a hunter spawned beside both players must be recorded as hunting slot 1, and the reads of
// "the player" its Blueprint makes must be answered with the client's puppet (served_player's counter rises); without it the creature
// must go for whoever is nearer by the measured distances, and the host when it is. The Grays arm calls the grayEventController's own
// `despawn` on a controller the drill stands, so the punch it makes must reach the client (the host's delivered-hit count rises, and the
// client's log shows the hit applied) or, without the switch, land on the nearer player.
// Every verdict line carries '[HUNT] VERDICT <arm> PASS|FAIL' and the readings; the samples are '[HUNT] sample'. Run: the rig with the
// row set on the host, VOTVCOOP_JOIN_AT_HOST=1, the verdict line as the verdict marker.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hunt_drill {

// Cache the session. Called from the dev install fanout; no-op when off.
void Install(coop::net::Session* session);

// Advance the host's steps. Every pump tick in a world. Game thread.
void Tick();

// The session ended: the steps back to the first.
void OnDisconnect();

}  // namespace coop::dev::hunt_drill
