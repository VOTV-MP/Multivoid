// coop/dev/pi3_drill.h -- [dev] the PI-3 drill: a creature or prop spawned at a still peer's puppet is not served as
// this machine's player, and the machine-global verbs it would run (a save, a quit, a travel, a camera turn, a 2D
// sound) are not reached. One arm per run, set on BOTH peers by pi3_drill (wisp_w, wisp_o, wisp_p, kelura, orborb,
// orbbirth, grunt, control, client), which makes one peer the spawner and the other the still peer.

// The spawner walks away from the still peer's puppet with the bot director, turns its camera to it, waits for the
// puppet to stand still, then spawns the site AT the puppet with the observer armed (pi3_observer.h) and judges the
// observer's records against the arm's table once the arm's horizon has passed since the first contact. The still
// peer arms the observer before the spawn and reports what it records. Every verb that would save, quit or travel the
// machine is cancelled by the observer in every build. The `control` arm spawns at the spawner's OWN pawn (served, as
// in single player); the `client` arm has the client spawn at the host's puppet, then at its own pawn (a reading).
// The `orbbirth` arm has the host spawn an orb far from both peers and hold it: the client's mirror of it must not
// end the client's game at its birth, and that verdict and DONE are the still peer's.

// Lines, all INFO: [PI-3] observer live / ARMED / STILL / seen / PASS / FAIL / BROKEN / VOID / ABANDONED /
// MP-1 reading / DONE, and the orbbirth arm's SPAWNED / host stamp failed / HOLD lost. The drill runs once per
// process. Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::pi3_drill {

// Register the observer's watches and hooks when the row is on. Idempotent.
void Install(coop::net::Session* session);

// The pump tick's entry: a latched read when the row is off.
void Tick(coop::net::Session* session);

// Session end: a spawner mid-arm says ABANDONED peer-lost (not one holding an orbbirth site), a still peer lets go;
// the observer disarms.
void OnDisconnect();

}  // namespace coop::dev::pi3_drill
