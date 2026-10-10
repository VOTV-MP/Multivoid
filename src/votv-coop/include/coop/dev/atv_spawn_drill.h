// coop/dev/atv_spawn_drill.h -- [dev] an ATV spawned at runtime on the host reaches each client once (L21).
//   HOST   -- spawns an ATV_C in front of its own player: run, once slot 1's world is ready (the spawn's broadcast);
//             join, once slot 1 is seated and still loading (the world-ready replay of every runtime ATV); park, as
//             run. It says when its ATV lane announced the ATV under a synthetic key.
//   CLIENT -- once its world is ready, passes when exactly one runtime ATV mirror stands here with nothing parked,
//             held for a second; run covers either delivery (the broadcast, or the park's hand-back when it lands in
//             a world not yet ready). In park mode atv_park_first_spawn=1 parks the first spawn that reaches a ready
//             world unless the park already holds it, and the pass also needs the park to have held a spawn this
//             session.
// "[ATV-SPAWN-DRILL] FAIL" is the lane failing (--fail-marker), "[ATV-SPAWN-DRILL] ABANDONED" the drill unable to do
// its part (--dead-marker). Run with atv_spawn_drill=run|join|park and --done-marker "[ATV-SPAWN-DRILL] client DONE".

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::atv_spawn_drill {

// Advances this peer's steps; a latched read when off. Game thread.
void Tick(coop::net::Session* s);

// The session ended: the drill starts over.
void OnDisconnect();

}  // namespace coop::dev::atv_spawn_drill
