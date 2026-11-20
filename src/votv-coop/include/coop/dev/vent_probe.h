// coop/dev/vent_probe.h -- drill: a crouched player's steps come twice as often on the other peer
// (row vent_crawl_drill / env VOTVCOOP_VENT_CRAWL_DRILL; BOTH peers), for Ghezra/Multivoid#41.
//
// The host crouches and says so; the client notes how many steps the host's puppet has taken and says it is armed; the host
// then walks a NavMesh route of 20 to 50 m through the bot director, crouched, and says the distance it travelled and where
// it stopped. The client waits until the puppet stands there with its step count unchanged, divides the distance by the steps
// counted, and judges the spacing: within 20% of the crouched stride (75 cm) is PASS, within 20% of the standing one
// (150 cm) is FAIL, any other spacing or fewer than 8 steps is INVALID. The host says the verdict the client reports.
//   rig: VERDICT vent-crawl PASS | FAIL | INVALID      [VENT-DRILL] [H|C] DONE
// Every wait ends on the other peer's line or on a state it reads; its bound (120 s, and for the client's wait on the walk
// the walk's deadline and a minute) says INVALID only. Red: crouch_bit_off on the host (the pose never carries the crouched
// bit), which must say FAIL. The walk is on open ground: a duct needs a teleport onto a place, and the stride does not
// depend on where it is walked.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::vent_probe {

// Advance this peer's steps. Every pump tick in a world. Game thread.
void Tick(coop::net::Session* session);

// The session ended: back to the first step.
void OnDisconnect();

}  // namespace coop::dev::vent_probe
