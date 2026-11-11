// coop/dev/offhand_drill.h -- [dev] a client holds an item in its OFF hand and drops it (ini offhand_drill, env
// VOTVCOOP_OFFHAND_DRILL=1; BOTH peers). Lines are tagged [OFFHAND-DRILL].
//
//   CLIENT -- takes a crowbar into the main hand, runs the off-hand key's own handler (InpActEvt_offhand), which moves
//             the selected hotbar item into the off-hand slot, and waits for holdingActor_offhand to name the crowbar.
//             It says so in the lobby chat, drops the off hand's item with dropItem_offhand, says the dropped prop's key,
//             and judges that the key stands in its own world and the off hand is empty.
//   HOST   -- hears both lines and judges that the dropped key resolves to a live prop here. The host's mirror of the
//             client's off hand is the hand_item lane's own `off-hand mirror SPAWNED` line in the same log.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::offhand_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

void OnDisconnect();

}  // namespace coop::dev::offhand_drill
