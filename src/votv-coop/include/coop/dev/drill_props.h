// coop/dev/drill_props.h -- [dev] the prop helpers the drills share: a floor found by a trace, a prop or the kerfur
// NPC put on it, a kerfur NPC turned into its prop, and the test of a minted key. Game thread.

#pragma once

#include <cstdint>

namespace coop::dev::drill_props {

// Why FloorAt found no floor.
enum class FloorMiss : uint8_t { None, TraceFailed, NoFloor, InsideGeometry };

// The floor under the column at (x, y): the first static or dynamic thing a trace meets going down `downCm` from
// `fromZ`, every player body ignored. False when none is met, or when the column began less than `minColumnCm` above
// it (inside geometry); `miss`, when given, says which. `ctx` is any live actor, the trace's world context.
bool FloorAt(void* ctx, float x, float y, float fromZ, float downCm, float minColumnCm, float* floorZ,
             FloorMiss* miss = nullptr);

enum class Drop : uint8_t { Spawned, NoFloor, SpawnFailed };

// FloorAt under (x, y), then a spawn `riseCm` above the floor found: the actor of class `cls`, or, when `cls` is null,
// the NPC `npc` through the host's announced lane. `out` gets the actor and `floorZ` (when given) the floor on Spawned.
Drop DropOnFloor(void* ctx, void* cls, const wchar_t* npc, float x, float y, float fromZ, float downCm,
                 float minColumnCm, float riseCm, void** out, float* floorZ = nullptr);

// dropKerfurProp on a kerfur NPC, the call the game's own turn-off makes. False when it could not dispatch.
bool TurnOffKerfur(void* npc);

// Does this prop carry a minted key (neither empty nor "None")?
bool HasKey(void* actor);

}  // namespace coop::dev::drill_props
