// coop/dev/explosion_drill.h -- [dev] the explosion lane's drill (ini explosion_drill=off|dissolve|apply|coast|
// spawn_malformed / env VOTVCOOP_EXPLOSION_DRILL). Each arm runs the roles named here, and only those.
// dissolve (CLIENT acts): a bomb barrel is spawned and broken in the world load (the dissolve comes by class name)
// and in the loaded world (by class); its explosion must never reach begin-play, and the world half also that the
// dissolved actor is gone. apply, coast, spawn_malformed (HOST and CLIENT; nobody walks or is teleported): at a blast
// point B near the client, apply blasts the three classes (each applied once, narrowed, rune check refused,
// nothing dropped), coast blasts among four papers (each client copy flies over 100 cm and rests again) and
// spawn_malformed sends one valid ExplosionSpawn and four with one broken field (each dropped by its reason).
// Every wait is a counter's rise, a rest (under 5 cm/s for 10 Ticks) or a registry state, and has a failure bound:
// 60 s per phase on either role, the client's join and its verdicts 120 s from its own world-ready edge (the wait
// for that edge is the rig's).
// "[EXPLOSION-DRILL] FAIL" is a step failing (--fail-marker), "[EXPLOSION-DRILL] ABANDONED" the drill unable to do
// its part (--dead-marker), "[EXPLOSION-DRILL] client load PASS by=name" the dissolve's load half and
// "[EXPLOSION-DRILL] client DONE PASS arm=dissolve", and the same for each arm, the run's end (--done-marker).

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::explosion_drill {

// The world half of dissolve, or either role of the other arms. Game thread, once per gameplay pump tick; a latched
// string compare when off.
void Tick(coop::net::Session* s);

// The load half of dissolve. Game thread, once per pump tick, the world load included.
void TickLoad(coop::net::Session* s);

// Back to the start: a rejoin's peers run their halves again; the host first destroys the papers its coast spawned.
void OnDisconnect();

}  // namespace coop::dev::explosion_drill
