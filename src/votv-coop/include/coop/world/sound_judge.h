// coop/world/sound_judge.h -- the host's judgement of a sound a client asks to be played (SoundAt from a client).
//
// A client's sound is an ask, not a state: the host plays it and sends it on to every other client only when it holds.
// A client may ask for what its own player plays (its melee hit, its scrub of a grime decal, its cleaning item's use),
// and only where its body is: the sound's table entry must allow one of those callers (a knock or a bell is the host's
// world's, whatever the sender claims), the sender must have a body on the host that took a pose and be within a melee
// swing's reach of the place named, and it holds a budget of asks (a swing is about one impact). MTA's shape: a
// client's request is checked once where it arrives (a remote event the server registered) and the server alone sends
// sound to the others. The table index and the digest are judged by coop/world/sound_catalog before this. Host, game
// thread.

#pragma once

#include "coop/world/sound_relay.h"

#include <cstdint>

namespace coop::net {
class Session;
struct SoundAtPayload;
}  // namespace coop::net

namespace coop::sound_catalog { struct Entry; }

namespace coop::sound_judge {

// Whether the host may play and send on `slot`'s sound `p`, whose table entry is `entry`; otherwise `why` is the reason.
// Spends one of the slot's budget with each ask that gets past the entry's caller check.
bool Allow(coop::net::Session& session, uint8_t slot, const coop::sound_catalog::Entry& entry,
           const coop::net::SoundAtPayload& p, coop::sound_relay::Refusal& why);

}  // namespace coop::sound_judge
