// coop/dev/drive_drill.h -- [dev] a data drive's row is the host's, a client's eraser press runs on the host and shows
// on the client, and every drive a client brings into the world sends its row to the host, which takes no other.
//   HOST   -- gives a fixture drive the row "drill-fixture" (run: a drive it spawns once a client is in its world;
//             join: a drive of its world in no slot, written after the joiner's world was taken and before it is
//             ready, so only the lane's world-ready seed carries it) and fails if its copy takes the client's
//             deviation or its forgery. After its wipe it turns the born drive's physics on (a slot's pull-out) and
//             counts the client's rows it takes and refuses; its DONE line, the run's end, comes at three taken and
//             one refused, and the rig's grace covers the client's hold.
//   CLIENT -- the put-back ("drill-deviation" written and upd() run must read "drill-fixture" at once); a director walk
//             to a standpoint beside the eraser, the fixture seated, its slot line out, the delete pressed: the wipe
//             within 10 s, the host's start and done shown by its eraser; a drive spawned with "drill-newborn"; a
//             born drive pocketed once it rests gatherable, taken back out, held and thrown; the wiped fixture forged
//             and claimed.
// "[DRIVE-DRILL] FAIL" is the lane failing a step (--fail-marker), "[DRIVE-DRILL] ABANDONED" the drill unable to do
// its part (--dead-marker). Run with drive_drill=run|join and --done-marker "[DRIVE-DRILL] host DONE".

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::drive_drill {

// Game thread, once per pump tick; two short string compares when off.
void Tick(coop::net::Session* session);

// The per-session half: a rejoin's peers run their legs again.
void OnDisconnect();

}  // namespace coop::dev::drive_drill
