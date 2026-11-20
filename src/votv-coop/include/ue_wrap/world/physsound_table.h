// ue_wrap/world/physsound_table.h -- the sounds the game plays for a struck material, read from its `list_physSound`
// UDataTable. A melee hit (mainPlayer_C::attack) plays the struck material's row's `impact` sound; this reads that
// column for every row. Engine-wrapper layer (principle 7): no network, no coop state, no policy. The walk is verified
// against the reflected `impact` column, as weapon_catalog's is against `damage`. Game thread only.

#pragma once

#include <string>
#include <vector>

namespace ue_wrap::physsound_table {

// The distinct impact sounds of the table, each as ue_wrap::object_path::Of writes it, in no stated order; a row with no
// impact sound adds none. False, `out` empty and `why` naming the failure, when the table is not loaded yet, its layout
// does not resolve, or the walk and the engine's own column read disagree: the caller may try again later.
bool ReadImpactSounds(std::vector<std::wstring>& out, std::string& why);

}  // namespace ue_wrap::physsound_table
