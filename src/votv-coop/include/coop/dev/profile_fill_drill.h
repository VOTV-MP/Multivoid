// coop/dev/profile_fill_drill.h -- dev-only: the per-player profile of a FULL inventory reaches the host (env
// VOTVCOOP_PROFILE_FILL_DRILL=1, or a prop name such as `crowbar` to fill with that item; off by default).
//
// The profile carries every record the player's pockets hold (coop/items/inventory_wire), and the game's volume rule
// is the only bound on how many there are: maxVolume 50000 over the smallest item the game lists is the realistic
// worst case. The CLIENT waits for the game's volume tables, picks the smallest-volume item whose class loads and
// that the pocket takes (`1`; a name forces that item), pockets one own spawn at a time until the game refuses
// ("Not enough space"), then waits for its profile stream to send the full inventory and for the HOST to say it stored
// the same record (the host deserializes the stored blob and says its hash in the lobby chat). PASS needs a send
// after the fill and the host's stored record to hash like the client's last send; the run's error count is the
// rig's. Lines tagged [PFILL-DRILL]; the client's PASS/FAIL line is the verdict. The host's half only reports what it
// stored. Both peers run it.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::profile_fill_drill {

// Game thread; a single env read when off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::profile_fill_drill
