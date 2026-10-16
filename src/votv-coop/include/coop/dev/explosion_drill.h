// coop/dev/explosion_drill.h -- [dev] the explosion lane's drill (ini explosion_drill=off|dissolve / env
// VOTVCOOP_EXPLOSION_DRILL). The CLIENT acts; the host does nothing for it.
//
// Arm dissolve: a bomb barrel is spawned and broken twice, and its explosion, which the barrel's own Blueprint spawns,
// must be dissolved at its birth. The load half does it inside the client's world load, where the lane's class table
// is still unresolved, so the dissolve must come by class name; the world half does it once the world is ready, where
// it comes by class. Each judges the lane's counters against a watch of its own on the explosion's begin-play, which
// must never be entered, and the world half also that the dissolved actor is gone once the posted destroy ran. A PASS
// is never read off a watch that is not live.
// "[EXPLOSION-DRILL] FAIL" is a step failing (--fail-marker), "[EXPLOSION-DRILL] ABANDONED" the drill unable to do its
// part (--dead-marker), "[EXPLOSION-DRILL] client load PASS by=name" the load half and "[EXPLOSION-DRILL] client DONE
// PASS arm=dissolve" the run's end (--done-marker).

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::explosion_drill {

// The world half. Game thread, once per gameplay pump tick; a latched string compare when off.
void Tick(coop::net::Session* s);

// The load half. Game thread, once per pump tick, the world load included.
void TickLoad(coop::net::Session* s);

// Back to the start: a rejoin's client runs both halves again.
void OnDisconnect();

}  // namespace coop::dev::explosion_drill
