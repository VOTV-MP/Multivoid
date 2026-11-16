// coop/dev/atv_riders_join_hold.h -- the rejoined observer's two holds of the ATV riders drill (coop/dev/atv_riders_drill). In the life
// the rig restarts (VOTVCOOP_ATV_RIDERS_REJOIN=1, row on) the driver's puppet is not born and the NPC mirrors are not bound
// until the drill releases them, on the observer's first applied ATV stamp: so the stamp always comes first and the owners'
// own ready edges (the birth, the bind) are what apply it. Off in every other run, and once released for the life.
// Both NPC mirror bind routes wait: the adoption scan (coop/creatures/npc_adoption asks BindHeld) and the fresh spawn
// (coop/creatures/npc_mirror asks ParkSpawn, which keeps the payload and marks its eid in bind_wait, and DropParked at the
// eid's destroy). The release replays the parked payloads through npc_mirror::OnEntitySpawn on the game thread. A payload whose
// bind_wait mark is gone at the replay (a world change or the session's end cleared it) is dropped, not replayed. Read by
// coop/player/puppet_drive, coop/creatures/npc_adoption and coop/creatures/npc_mirror; released by the drill's thread. The
// holds are atomics; the parked payloads are game-thread only.

#pragma once

#include <cstdint>

namespace coop::net { struct EntitySpawnPayload; }

namespace coop::dev::atv_riders_join_hold {

inline constexpr uint8_t kDriverSlot = 2;   // the slot whose puppet is held

// This is the rejoined life of a drill run (the environment's word, read once).
bool RejoinedLife();

// The puppet of `slot` is held unborn.
bool BirthHeld(uint8_t slot);
// A pending NPC mirror bind (the adoption scan's, or a fresh spawn's) is held.
bool BindHeld();

// A fresh-spawn EntitySpawn (no local twin) waits while the bind is held: its payload is parked once per eid and the eid is
// marked in bind_wait. True when parked (the caller returns), false when nothing is held. The spawn is deferred, never
// refused. Game thread.
bool ParkSpawn(const coop::net::EntitySpawnPayload& payload);
// Drops the parked spawn of `eid` (its EntityDestroy arrived while it waited) and gives its bind_wait mark up; true when one
// was parked. Game thread.
bool DropParked(uint32_t eid);

void ReleaseBirth();
// Lets the binds go and posts the replay of the parked spawns to the game thread. Any thread.
void ReleaseBind();

}  // namespace coop::dev::atv_riders_join_hold
