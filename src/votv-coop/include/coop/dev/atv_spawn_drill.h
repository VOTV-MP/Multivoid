// coop/dev/atv_spawn_drill.h -- [dev] an ATV spawned at runtime on the host reaches each client once (L21).
//   HOST   -- spawns an ATV_C in front of its own player: run, once slot 1's world is ready (the spawn's broadcast);
//             join, once slot 1 is seated and still loading (the world-ready replay of every runtime ATV); park, as
//             run. It says when its ATV lane announced the ATV under a synthetic key.
//   CLIENT -- once its world is ready and no runtime spawn waits in the park, passes when exactly one runtime ATV
//             mirror stands here; in park mode atv_park_first_spawn=1 parks the first spawn as if the world were not
//             ready, and the pass also needs the park to have held it and handed it back.
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
