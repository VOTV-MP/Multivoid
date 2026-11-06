// coop/dev/mannequin_walker_drill.h -- [dev] the walking mannequin's two-peer drill, the host's walker for every player.
//
// One drill thread per peer, armed by a value of the `mannequin_drill` dev row on BOTH peers (the green is `walker`, the nine
// reds are its `no_*` values, each read in the module it disables). It ends on `rig: VERDICT mannequin PASS|FAIL|INVALID`,
// said by the host over both peers (coop/session/rig_verdict.h). The peers order their steps through harness/autotest/peer_signal
// (the folder is VOTVCOOP_CAN_DIR) and move only with the bot director, never a teleport. The client leaves mid-run and the rig
// restarts it (--rejoin, coop/dev/mannequin_walker_steps_b): the series always runs with the rejoin.

#pragma once

#include <windows.h>

namespace coop::dev::mannequin_walker_drill {

// Whether the `mannequin_drill` row names this drill: `walker` or one of the reds. Resolved once.
bool Armed();

// The drill thread for the harness dispatch (CreateThread): host or client by the role.
DWORD WINAPI Thread(LPVOID arg);

}  // namespace coop::dev::mannequin_walker_drill
