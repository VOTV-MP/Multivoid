// coop/props/prop_throw.h -- the game's own throw, seen at the call and numbered.
//
// Every throw of the player pawn (the hand throw, the grabbed prop's fire press, the host's own) ends in the
// pawn's throwShit(component, velocity), which sets the launch velocity and is called by no other verb. A script-gate
// POST watch on it, filtered to the LOCAL pawn by identity (a puppet is a mainPlayer_C too), mints a throw sequence
// for the thrown actor: a uint16 that is never 0 and travels in the drop intent and the release (0 = not a throw).
// A peer's records of its own recent throws are what the host's grant matches, what the adoption of the thrower's
// own actor yields to, and what the release and the drop intent read their sequence from. The host's side: the
// bounds a client's release and drop intent meet, the release's relay to the other clients, and the mark a thrown
// drop intent leaves for the election, which waits for the spawned prop to have its element id.
//
// Game thread only. See docs/props.md (a thrown prop's flight).

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct PropReleasePayload;
struct PropDropIntentPayload;
}  // namespace coop::net

namespace coop::prop_throw {

// Per tick, both roles: asks the gate to take and resolve the throwShit watch until it is live, and drops the
// records and marks that waited kGrantTimeoutMs.
void Tick();

// The sequence of the live record of `actor` (a throw within kGrantTimeoutMs, granted or not), 0 when none.
uint16_t SeqFor(void* actor);

// The host's grant of the throw numbered seq arrived: its record, matched once. The thrown actor and the thrower's
// clock at the throw (active_drive::NowMs) come out through the pointers.
bool TakeForGrant(uint16_t seq, void** actor, uint64_t* startMs);

// HOST: a client's release arrived (the finite check passed). Its velocity meets the throw bounds
// (coop/net/throw_bounds.h): clamped in magnitude, direction kept, and counted. The release then goes to every
// world-ready client but its sender, with the sender's slot kept as the origin, so each mirror clears the holder's
// drive: the relay a client's release had on the net thread, here after the clamp. A no-op for the host's own
// release and on a client. Clamps `p` in place.
void JudgeRelease(coop::net::Session& session, int senderSlot, coop::net::PropReleasePayload& p);

// HOST: a client's drop intent. False when a velocity is not finite (the intent is refused); a finite velocity is
// clamped in place, as a release's. Counted.
bool JudgeDropIntent(coop::net::PropDropIntentPayload& p, uint8_t senderSlot);

// HOST: the spawn of a thrown drop intent leaves a mark under the prop's key (the thrower's slot, the sequence, the
// pose it left from), cleared at its first use or after kGrantTimeoutMs; the election takes it where the spawn watcher
// would open the birth coast, because the spawned prop has no element id before that.
struct Mark {
    uint8_t  slot = 0;
    uint16_t seq = 0;
    float    loc[3] = {};
    uint64_t ms = 0;
};
void NoteMark(const std::wstring& key, uint8_t slot, uint16_t seq, const float loc[3]);
bool HasMarks();   // any mark waiting: the spawn watcher reads a birth's key only then
bool TakeMark(const std::wstring& key, Mark* out);

namespace dev {   // read by the throw arms of the container intent drill
uint32_t Minted();          // throw sequences minted this session
uint32_t PuppetIgnored();   // throwShit calls on a pawn that is not the local one (expected 0)
}  // namespace dev

// A session ended: the records, the marks and the counters go.
void OnDisconnect();

}  // namespace coop::prop_throw
