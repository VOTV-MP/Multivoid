// ue_wrap/desk/coord_tower.h -- the three coordinate towers (coordRadarDish_C), read-only.
//
// The towers triangulate the coordinates panel's cursor. Each can break: eight fuses and a
// lights-out puzzle, both saved with the actor, and a broken tower blocks the ping and the
// movement keys. A tower that loads broken scrambles itself again inside its own loadData, on
// every machine that loads the save, which is what an instrument has to be able to see.
// Instances come from the object index, never from a walk. Principle 7: no network or gameplay
// logic. Game thread.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::coord_tower {

// One tower's saved repair state. `fuses` holds one character per fuse, its byte value as a digit
// (the game keeps a byte per fuse, 0..2); `puzzleLights` one '0' or '1' per light.
struct State {
    int32_t      id = -1;
    bool         isBroken = false;
    bool         opened = false;
    std::wstring fuses;
    std::wstring puzzleLights;
};

// Resolve the class and its five members. A missing class is looked for again at most every 2 s,
// and so is one that has gone with its map; a member missing from the loaded class latches the
// wrapper off with one warning naming it, since it will not appear later. True while resolved.
bool EnsureResolved();

// Every live tower, sorted by id. Returns how many were written, or -1 while the wrapper is not
// resolved, so a caller never prints an unresolved read as "no towers".
int32_t ReadAll(State* out, int32_t cap);

// The id of `tower`, or -1 when it is not a tower or a member is missing. An unresolved wrapper
// resolves from the tower's own class: a name compare and that class's property chain, never a
// walk of the object array. So it is safe inside a script-gate callback, and it names the tower
// even inside the first world load, before any tick could have resolved the wrapper.
int32_t IdOf(void* tower);

}  // namespace ue_wrap::coord_tower
