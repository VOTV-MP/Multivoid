// ue_wrap/devices/generator.h -- engine access for the base's three transformers (Agenerator_C), in the order of
// gamemode.generators. Principle-7 engine-wrapper layer: no network or coop state; coop/world/power_grid drives
// the grid lane through here. A generator's state is its row: broken, its wear `cycle` (100 new, 0 broken), its
// `upgradeLevel` (0..6) and the saved `cyc`. break() latches, plays the 2D turn-off, zeroes the wear, runs
// update() (the turn-off at the generator, then upd(), which blacks the base out through the panel) and scrambles
// the repair puzzle. damage() is one step of wear: the cycle down by one and the puzzle's sine page scrambled, or
// break() at the last step. A player's repair is the Activate button with the puzzle solved: it mends a broken
// generator in place (isBroken off, the wear full, turnedOn, update(), then its completion trigger) and services
// a whole one (the wear full, the 2D turn-on). fullFix() solves the puzzle and does the mend's writes, turnedOn and
// upd(), but plays nothing and runs no trigger. Verbs resolve on the generator in hand, since a Blueprint class
// can be a new object in a new world.

#pragma once

#include "ue_wrap/core/types.h"  // FVector

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ue_wrap::generator {

struct Row {
    bool    broken = false;
    bool    cyc = false;
    int32_t cycle = 0;
    int32_t upgradeLevel = 0;
};

// Resolve generator_C's fields and verbs and the game mode's list, by name. Idempotent; false until the class
// and the game mode have loaded (retried a second later), or for good when a field is missing (said once). Game
// thread.
bool EnsureResolved();

// gamemode.generators in index order, a null where a slot is empty or its actor dead, so a position names the
// same generator on every peer. 0 before the game mode resolves. Game thread.
size_t ReadGenerators(std::vector<void*>& out);

// `gen`'s place in gamemode.generators, or -1. Game thread.
int32_t IndexOf(void* gen);

bool ReadRow(void* gen, Row& out);

// Raw writes of the row's plain fields: nothing repaints. Game thread.
bool WriteBroken(void* gen, bool broken);
bool WriteCycle(void* gen, int32_t cycle);
bool WriteCyc(void* gen, bool cyc);
bool WriteUpgradeLevel(void* gen, int32_t level);

// The game's own verbs, called as the game calls them. Game thread.
bool CallBreak(void* gen);
bool CallDamage(void* gen);
bool CallUpdUpgrades(void* gen);

// A repair another player made, run on this copy as the Activate route runs it: fullFix() first, since this
// copy's puzzle was scrambled by its own break, then the route's update() (the turn-on at the generator) and its
// completion trigger, `triggerWhenCompleted` run as runTrigger(gen, 0) when one is set (in the tutorial's map
// alone). False when fullFix() or update() did not run. Game thread.
bool Repair(void* gen);

// The Activate button (button_activate), the component a player's look-at trace must strike to repair or
// service; null before the generator resolves. Game thread.
void* ActivateButton(void* gen);

// The upgrade slot (upgradeRoot, the billboard its buttons hang from), where the insert takes a held upgrade; null
// before the generator resolves. Game thread.
void* UpgradeSlot(void* gen);

// Whether `actor` is an upgrade the insert takes (a prop_transformerUpgrade_C, as the insert's cast reads it), and
// one spawned at `at`, which the host's spawn watcher gives every peer. Game thread.
bool IsUpgrade(void* actor);
void* SpawnUpgrade(const FVector& at);

// [dev] the grid drill's repair as a player makes it: the look that marks the Activate button as the one under the
// trace (getActionOptions writes lookAtButton), then actionOptionIndex(player, a hit on the button, 4, the button).
// Game thread.
bool PressActivate(void* gen, void* player);

// [dev] the grid drill's install as a player makes it: the look that marks an upgrade button as the one under the
// trace (getActionOptions writes lookAtUpgrade), then playerUsedOn(player, a hit on the button, the button,
// `upgrade`), whose insert spends the upgrade it is handed. Game thread.
bool InsertUpgrade(void* gen, void* player, void* upgrade);

// [dev] the upgrades within `radius` of `at`, live and not dying, and the nearest of them (null when none): where
// the grid drill finds the upgrade handed to its player, and its refund. Game thread.
int UpgradesNear(const FVector& at, float radius, void** nearest);

// [dev] the upgrade meshes updUpgrades built on `gen` (its upgradeMeshes, the live ones): what a player sees of the
// upgrades installed. -1 while the field or the generator is unread. Game thread.
int UpgradeMeshCount(void* gen);

// [dev] the grid drill's shortcut through the repair puzzle: the generator's panel reads all three pages solved,
// as a player's solving leaves them. Game thread.
bool WritePuzzleSolved(void* gen);

// [dev] the cue update() last set on the generator's own sound (its `turnon` component): true for the turn-on,
// false for the turn-off. False when the component or its sound is unread. Game thread.
bool ReadLastCue(void* gen, bool& turnOn);

}  // namespace ue_wrap::generator
