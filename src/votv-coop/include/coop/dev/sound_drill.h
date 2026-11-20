// coop/dev/sound_drill.h -- [dev] the sound relay's drill (row sound_drill, on the host and two clients, with
// VOTVCOOP_CAN_DIR). Each arm is decided by an event; a bound passing with none is INVALID. The host says PASS
// (`rig: VERDICT sound-<arm> PASS`), any peer FAIL or INVALID.
//   hit       Client 1 swings a crowbar at the floor. FAIL when its hit played here and sent nothing (red sound_send_off);
//             PASS on the host once it and client 2 have played it; FAIL on the host's refusal of the honest hit.
//   knock     The host fires the vent knocker and runs its knock: FAIL when played and not sent (red sound_send_off);
//             PASS when both clients have played it.
//   far       Client 1 sends a sound far beyond its reach: PASS at the host's `reach` refusal.
//   hostworld Client 1 sends the church's bell: PASS at the host's `caller` refusal.
//   index     Client 1 sends an index past the table: PASS at the host's `index` refusal.
// The raw arms FAIL at the first play of the sound on the host or client 2 (reds sound_judge_off; sound_index_check_off).
// Not driven: a client's own church or knocker cancel, and the church's bell end to end. The church is a level actor
// with no cheap trigger, so both are read in coop/world/sound_relay, and the host's knock stands for the host world.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::sound_drill {

// Per frame. A latched read when the drill is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::sound_drill
