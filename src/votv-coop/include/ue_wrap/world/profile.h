// ue_wrap/world/profile.h -- the player's own profile save, mainGamemode.save_main (a save_main_C): the
// days its player has lived through and its achievements, which every machine keeps for its own player
// and the day's rollover writes. Engine-wrapper layer (principle 7): no network or gameplay logic.

#pragma once

#include <cstdint>

namespace ue_wrap::profile {

// save_main.stats' days_total on the running world's gamemode. False while there is no gamemode or
// profile, or when a member does not resolve (said once). Game thread.
bool ReadDaysTotal(int32_t& out);

// days_total += n, as the rollover adds one a midnight. Game thread.
bool AddDaysTotal(int32_t n);

// save_main_C::progressAchievement(name, popup, autosave = false), which lib_C's own entry forwards to:
// the achievement progresses on this machine's profile, with its popup. False when it does not resolve.
// Game thread.
bool ProgressAchievement(const wchar_t* name);

}  // namespace ue_wrap::profile
