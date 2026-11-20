// ue_wrap/world/physsound_table.h -- the sounds the game plays for a struck material, read from its `list_physSound`
// UDataTable. A melee hit (mainPlayer_C::attack) plays the struck material's row's `impact` sound; this reads that
// column for every row. Engine-wrapper layer (principle 7): no network, no coop state, no policy. The walk is verified
// against the reflected `impact` column, as weapon_catalog's is against `damage`. Game thread only.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::physsound_table {

// How a read ended. NotYet: the table or its reflection is not resolvable yet, and a later read may succeed. Mismatch: a
// verdict about the layout or the data (a member missing, the walk and the engine's own column disagreeing), which no
// retry changes. The same split weapon_catalog makes.
enum class Read : uint8_t { Ok, NotYet, Mismatch };

// The distinct impact sounds of the table, each as ue_wrap::object_path::Of writes it, in no stated order; a row with no
// impact sound adds none. On a failure `out` is empty and `why` names it.
Read ReadImpactSounds(std::vector<std::wstring>& out, std::string& why);

}  // namespace ue_wrap::physsound_table
