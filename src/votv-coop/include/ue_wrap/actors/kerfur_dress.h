// ue_wrap/actors/kerfur_dress.h -- an Omega's outfit and its pat, as the game keeps and plays them.
//
// kerfurOmega_C keeps what it wears in `drip`, a TArray<FName> of list_kerfurDrip rows; updateDrip destroys the meshes it
// built and builds one per row again, and fills `dripBones` with the bone of each row that has a mesh or a type-0 accessory.
// equipItem adds the player's held row, unequipItem(name) spawns the item back and removes its row. The radial's "pat"
// plays the `pat` section of kerfurOmegaV1_Skeleton_Montage and its meow, unless the Omega's state is 3, 4 or 5. The
// equipment screen is the gamemode's one objectViewer, whose genList rebuilds the list of the actor it captures. Game thread.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::kerfur_dress {

// The rows in `drip`, in order. False when `kerfur` is not an Omega or the field did not resolve.
bool ReadDrip(void* kerfur, std::vector<std::wstring>& out);

// The bones in `dripBones`, in order: the bones the worn rows hold. False as ReadDrip.
bool ReadDripBones(void* kerfur, std::vector<std::wstring>& out);

// `drip` set to `names` and the outfit rebuilt (updateDrip). False when not an Omega or a step did not run.
bool ApplyDrip(void* kerfur, const std::vector<std::wstring>& names);

// The game's own unequipItem(name): the item spawned at the Omega, its row removed, the outfit rebuilt.
bool Unequip(void* kerfur, const std::wstring& name);

// The player's use of the held item on the Omega, the call the use key makes: playerUsedOn with the player, its hand actor
// and its held row. The game's equipItem reads the row from the player and destroys the hand actor when it equips.
bool PutOn(void* kerfur, void* player, void* holdObject, const std::wstring& row);

// What list_kerfurDrip says of a row: its `type` (0 is an accessory on a bone, 1 a mesh with no bone check; the game's
// equipItem equips no other) and the bone (`accessoryName`).
struct RowFacts {
    bool found = false;
    uint8_t type = 0;
    std::wstring bone;
};
// The row named `row` (compared without case). False when the table or its members did not resolve; true with `found`
// false when the table has no such row.
bool ReadRow(const std::wstring& row, RowFacts& out);

// The pat's look alone: the montage's `pat` section on the Omega's mesh. None of the pat's state (the stop, the meow count
// and its blast) is touched, and no meow plays.
bool PlayPat(void* kerfur);

// Whether the pat montage is playing on the Omega's mesh now (the anim instance's Montage_IsPlaying).
bool IsPatPlaying(void* kerfur);

// Whether the game's own pat branch plays for this Omega: its state is not 3, 4 or 5.
bool CanPat(void* kerfur);

// The equipment screen's list rebuilt (genList), when the screen is open on this Omega: its viewer's `capturingACtor` is
// the Omega. True when it ran.
bool RefreshViewer(void* kerfur);

}  // namespace ue_wrap::kerfur_dress
