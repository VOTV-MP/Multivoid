// coop/dev/grid_drill_upgrade.h -- [dev] the grid drill's upgrade arms, beside grid_drill.cpp. The client walks
// beside the drill's generator (grid_drill_walk.h). An install op with no insert behind it must be refused, the host
// refunding nothing for it; then the client installs the upgrades the host hands it: the first must be taken, the
// second, while the host refuses installs as if another player's had filled the generator first, must roll back
// and come back as a refund beside it. upgradered is the control, a host that judges an install as if its spend
// were recorded, which takes the bare op.
// upgraderepair: the client installs the one upgrade the host hands it, the host breaks the generator, the client
// solves its puzzle and presses Activate (grid_drill_solve.h), the host takes the repair; 20 s on, both peers read
// the generator's upgrades and the meshes they built, which must still be the install's (a client's repair must not
// take its upgrades); the host judges its own generator the same way. upgraderepairred is the control with the install
// skipped, which must end on level 0. upgraderepairlossred is the control whose host takes the upgrades off its own
// generator after the repair (the level written back, updUpgrades run), so both verdicts must FAIL. Src-local.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dev::grid_drill {

enum class UpgradeFlavour : uint8_t { Plain, Repair, RepairRed, RepairLossRed };

// Either peer's legs, per frame once the grid resolved. Game thread.
void UpgradeTick(coop::net::Session* session, UpgradeFlavour flavour);

// The legs start again. Session end.
void UpgradeOnDisconnect();

}  // namespace coop::dev::grid_drill
