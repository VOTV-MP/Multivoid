// coop/props/prop_throw.h -- the game's own throw, seen at the call and numbered.
//
// Every throw of the player pawn (the hand throw, the grabbed prop's fire press, the host's own) ends in the
// pawn's throwShit(component, velocity), which sets the launch velocity and is called by no other verb. A script-gate
// POST watch on it, filtered to the LOCAL pawn by identity (a puppet is a mainPlayer_C too), mints a throw sequence
// for the thrown actor: a uint16 that is never 0 and travels in the drop intent and the release (0 = not a throw).
// A peer's records of its own recent throws are what the host's grant matches, what the adoption of the thrower's
// own actor yields to, and what the release and the drop intent read their sequence from.
//
// Game thread only. See docs/props.md (a thrown prop's flight).

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PropReleasePayload;
}  // namespace coop::net

namespace coop::prop_throw {

// Per tick, both roles: asks the gate to take and resolve the throwShit watch until it is live, and drops the
// records that waited kGrantTimeoutMs for a grant.
void Tick();

// The sequence of the live record of `actor` (a throw within kGrantTimeoutMs, granted or not), 0 when none.
uint16_t SeqFor(void* actor);

// HOST: a client's release arrived (the finite check passed). Its velocity meets the throw bounds
// (coop/net/throw_bounds.h): clamped in magnitude, direction kept, and counted. A no-op for the host's own release
// and on a client, whose releases the host already judged. Clamps `p` in place.
void JudgeRelease(coop::net::Session& session, int senderSlot, coop::net::PropReleasePayload& p);

// A session ended: the records and counters go.
void OnDisconnect();

}  // namespace coop::prop_throw
