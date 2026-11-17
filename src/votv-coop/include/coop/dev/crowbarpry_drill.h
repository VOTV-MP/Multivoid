// coop/dev/crowbarpry_drill.h -- [dev] a client pries a board with a crowbar; the crowbar the pry hands back is judged
// on the host (`[dev] crowbarpry_drill=open|hand` on both peers, the client also prop_birth_key_probe=1).
//
// The HOST spawns the fixture itself: a wall-attachable pryable (prop_baseroomsign_C, else prop_torchHolder_C) in front of the
// client's puppet. The CLIENT waits for its copy, takes a crowbar into the hand, and runs the game's own activatePrying
// on the board, then ends the shown prying crowbar's End as the game's own callers do (opened, or given up).
// open: the board comes free and the crowbar the pry hands back must exist on the host. The host says
//   `rig: VERDICT crowbarpry PASS` when it does and the board is free there; the client says FAIL when its drain ended
//   that crowbar's birth without sending it (the red `crowbarpry_authorship_off=1` on the client).
// hand: the crowbar goes back into the hand and no world crowbar may reach the host. The client says PASS when its
//   crowbar's birth ended without a send and the hand holds a crowbar; the host says FAIL if one appears.
// Every wait that ends on no event says INVALID. A second life (`--rejoin`, `--rehost`) runs the arm again.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::crowbarpry_drill {

// Per frame. A latched read when the drill is off. Game thread.
void Tick(coop::net::Session* session);

// A session's end: the fixture is destroyed and both halves start over.
void OnDisconnect();

}  // namespace coop::dev::crowbarpry_drill
