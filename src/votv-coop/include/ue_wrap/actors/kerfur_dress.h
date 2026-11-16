// ue_wrap/actors/kerfur_dress.h -- an Omega's outfit and its pat, as the game keeps and plays them.
//
// kerfurOmega_C keeps what it wears in `drip`, a TArray<FName> of list_kerfurdrip rows; updateDrip destroys the meshes it
// built and builds one per row again. equipItem adds the held item's row, unequipItem(name) spawns the item back and
// removes its row. The radial's "pat" plays the `pat` section of kerfurOmegaV1_Skeleton_Montage and its meow. Game thread.

#pragma once

#include <string>
#include <vector>

namespace ue_wrap::kerfur_dress {

// The rows in `drip`, in order. False when `kerfur` is not an Omega or the field did not resolve.
bool ReadDrip(void* kerfur, std::vector<std::wstring>& out);

// `drip` set to `names` and the outfit rebuilt (updateDrip). False when not an Omega or a step did not run.
bool ApplyDrip(void* kerfur, const std::vector<std::wstring>& names);

// The game's own unequipItem(name): the item spawned at the Omega, its row removed, the outfit rebuilt.
bool Unequip(void* kerfur, const std::wstring& name);

// The Omega's meow (makemeow).
bool Meow(void* kerfur);

// The pat's look and sound alone: the montage's `pat` section on the Omega's mesh, and its meow. None of the pat's
// state (the stop, the meow count and its blast) is touched.
bool PlayPat(void* kerfur);

}  // namespace ue_wrap::kerfur_dress
