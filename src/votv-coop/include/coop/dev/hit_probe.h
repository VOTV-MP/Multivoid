// coop/dev/hit_probe.h -- [dev] the sound relay's drill (row sound_drill, on all three peers: the host and two clients).
//
// Every arm is decided by an event, and the point after which the outcome cannot change; a bound passing with none is
// INVALID. The host says PASS (`rig: VERDICT sound-<arm> PASS`), any peer FAIL or INVALID. The peers order their steps
// on VOTVCOOP_CAN_DIR marks; the host says `go` once both clients are in and it has client 1's body.
//   hit       Client 1 takes a crowbar and swings at the floor through its own attack. It FAILs the moment a catalogued
//             sound was played there and not sent (red sound_send_off). The host and client 2 each play it: PASS on the
//             host when it has played and client 2 has said so, FAIL on the host's first refusal of the honest hit.
//   knock     The host fires the vent knocker event and runs its knock. FAIL the moment the sound was played and not
//             sent (red sound_send_off); PASS when both clients have played it.
//   far       Client 1 sends the host a sound of the table at a place far beyond its reach: PASS at the host's `reach`
//             refusal. The raw arms FAIL at the first play of the sound on the host or client 2 (red sound_judge_off).
//   hostworld Client 1 sends the church's bell near its body: PASS at the host's `caller` refusal (red sound_judge_off).
//   index     Client 1 sends an index past the table: PASS at the host's `index` refusal (red sound_index_check_off,
//             which plays the sound the index wraps to).
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hit_probe {

// Per frame. A latched read when the drill is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::hit_probe
