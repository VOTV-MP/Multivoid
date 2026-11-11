// coop/items/player_profile.h -- everything about one player that is theirs and not the world's:
// their own save record (their vitals, what they carry, wear and hold), a readable copy of their
// vitals, and where they stood.
//
// The game keeps the player's record among the saved actors of the save object a world is built
// from, and a joiner's world is built from a capture of the HOST's. So it is replaced on the
// joiner's side before the world exists, or the joiner starts as a copy of the host: the host's
// items under the host's keys, the host's hunger, the host's position. coop/items/inventory_wire
// carries this value, coop/items/player_inventory_sync stores it per player on the host and hands
// it back at a join.

#pragma once

#include "ue_wrap/actors/vitals.h"

#include <cstdint>
#include <vector>

namespace coop::player_profile {

// The last place the player was STANDING: on the ground, not ragdolled, not seated on anything
// (the ATV seats its driver through the same field as a chair). A position read while driving or
// falling is somewhere a body cannot be put back. Not valid on a profile that was never streamed
// by its player (a first join), which places the player at the start point instead.
struct Pose {
    float x = 0, y = 0, z = 0, yaw = 0;
    bool  valid = false;
};

struct Profile {
    // The player's own save record as the game wrote it (ue_wrap/actors/player_record.h): the
    // pawn's saved properties, its inventory component's among them. Empty for a player the host
    // has nothing of (a first join), who starts as a new game's player does.
    std::vector<uint8_t>                record;
    // The vitals as read, or none: a pawn whose vitals do not resolve must not stop the record
    // from being stored. Read by the join (a player who left dead starts afresh) and the admin's
    // stat orders; a profile without them is applied with the game's defaults.
    bool                                hasVitals = false;
    ue_wrap::vitals::Snapshot           vitals;
    Pose                                pose;
};

}  // namespace coop::player_profile
