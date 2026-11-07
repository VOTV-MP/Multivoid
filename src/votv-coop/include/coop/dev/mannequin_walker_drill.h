// coop/dev/mannequin_walker_drill.h -- [dev] the walking mannequin's two-peer drill, the host's walker for every player.
//
// One drill thread per peer, armed by a value of the `mannequin_drill` dev row on BOTH peers (the green is `walker`, the eight
// reds are its `no_*` values, each read in the module it disables). It ends on `rig: VERDICT mannequin PASS|FAIL|INVALID`,
// said by the host over both peers (coop/session/rig_verdict.h). The peers order their steps through harness/autotest/peer_signal
// (the folder is VOTVCOOP_CAN_DIR) and move only with the bot director, never a teleport. The client leaves mid-run and the rig
// restarts it (--rejoin, coop/dev/mannequin_walker_steps_b): the series always runs with the rejoin.
//
// THERE IS NO STEP 3. A keep step (a walker keeps its target while another player stands nearer) cannot be observed by a
// drill. The walker's Blueprint reads its target in three places only: at its move proxy's OnSuccess or OnFail, in the 0.5 s delay
// of its out-of-range loop, and at BeginPlay (prop_wMannequin bytecode, offsets 166-354 and 902). A move toward the target makes no
// read; at OnSuccess the walker stands within 50 cm of the target, so no other player can be nearer; and the out-of-range loop is
// the loss rule, not the keep. Measured on the rig: the client stood nearer for about 12 s and the walker made no read. The rule
// itself is covered by the target module's boot selftest (coop/creatures/walker_target_rule), which needs no walker. The step numbers
// stay as first numbered.

#pragma once

#include <windows.h>

namespace coop::dev::mannequin_walker_drill {

// Whether the `mannequin_drill` row names this drill: `walker` or one of the reds. Resolved once.
bool Armed();

// The drill thread for the harness dispatch (CreateThread): host or client by the role.
DWORD WINAPI Thread(LPVOID arg);

}  // namespace coop::dev::mannequin_walker_drill
