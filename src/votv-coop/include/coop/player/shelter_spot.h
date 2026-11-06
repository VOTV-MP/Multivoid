// coop/player/shelter_spot.h -- where a player is put down in the world by us: a standable floor
// point beside the base's bed, or the KPP landmark when none can be found. Two callers: a revive
// (death_revive, always) and a client's join appearance (net_pump, from day 3 on; in a fresh game,
// day 0 to 2, the joiner is put at the KPP gate and does not ask here). The base is roofed;
// the KPP is open sky, so a revive there during the game's Bad Sun event is killed again by the next
// hit and loops, and the KPP is a 770 m walk from the base.
// Whether the bed's spot is sheltered from that event's sun trace is read off the rig, not assumed.

#pragma once

#include <cstdint>

namespace coop::shelter_spot {

enum class Kind : uint8_t { None, Bed, KppFallback };

// A chosen landing: where the actor's location is to be set, and which rule chose it.
struct Target {
    Kind  kind = Kind::None;
    float x = 0.f, y = 0.f, z = 0.f;
};

const char* KindName(Kind k);

// The bed nearest the KPP: a NavMesh point beside its colliding box, clear of it by the player's
// capsule radius (the NavMesh runs under the bed), its capsule's half-height above the floor so the
// actor's location stands the player on it. Any step that fails yields the KPP
// with kind KppFallback; the first fallback of a session is logged with its reason. `pawn` is the
// world context and the capsule's owner. Cold path: the bed lookup walks the object index once per
// call, and this runs once per death and once per join appearance. Game thread.
Target Choose(void* pawn);

// A drill's switch, VOTVCOOP_DEATH_NO_BED=1: the bed lookup is treated as finding nothing, so the
// fallback is exercised on a world that has a bed. Read once and cached; not a shipped setting.
bool BedLookupDisabled();

// Forget this session's logged fallback. Not on the game thread (the session's own).
void OnSessionStart();

}  // namespace coop::shelter_spot
