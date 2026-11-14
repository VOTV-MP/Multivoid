// coop/dev/grid_drill_upgrade.h -- [dev] the grid drill's upgrade arms, beside grid_drill.cpp. The client walks beside the
// drill's generator (grid_drill_walk.h). A bare install op with no insert behind it must be refused, nothing refunded
// (the row hand_spend_drill=hand_spend_unchecked is its control); then the client installs the upgrades the host hands it:
// the first taken, the second, while the host refuses installs as if another player's had filled the generator, rolled
// back and refunded beside it. With the row grid_upgrade_hand each install is of an upgrade in the hold slot.
// upgradedouble: one install from the hold slot, then a second op of that hold, which the host must refuse
// (hand_spend_no_latch is its control). upgradesametick: an upgrade taken into the hand and installed in the same call,
// which the host must take (hand_announce_off is its control). upgraderepair: the client installs the one upgrade, the
// host breaks the generator, the client solves its puzzle and presses Activate, the host takes the repair, and 20 s on both
// peers read the install and its meshes; upgraderepairred installs nothing and must end on level 0; upgraderepairlossred has
// the host take the upgrades off its own generator after the repair, so both verdicts must FAIL. Src-local.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dev::grid_drill {

enum class UpgradeFlavour : uint8_t { Plain, Repair, RepairRed, RepairLossRed, Double, SameTick };

// Either peer's legs, per frame once the grid resolved. Game thread.
void UpgradeTick(coop::net::Session* session, UpgradeFlavour flavour);

// The legs start again. Session end.
void UpgradeOnDisconnect();

}  // namespace coop::dev::grid_drill
