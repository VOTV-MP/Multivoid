// coop/world/arir_rep_sync.h -- the Arirals' reputation is the host's, and every player's machine holds it.
//
// The reputation is one float in the save slot, `arirReputation`, and lib_C's `addAriralRep` (clamped) is its only
// writer. Its callers are the world's: the treehouse's anti-steal box and its surroundings check, a graffiti's end, a
// fire tank's, a gray boar's and theEvil's deaths, the follower's pickups, the ship and float photos, the player's wake.
// Every peer ran them against its own copy, so each machine drifted to its own value, and what a client read from it
// (a brownie trail's note, the shrimp eaters' tier) was its own. HOST: the value is sent (EventCue 105, x = the value)
// when an addAriralRep returns that moved it, once a second when it moved by any other route, and to a joiner. CLIENT:
// its own addAriralRep is refused, and the host's value is written in; what its own player did (its wake, its swing at
// the follower, its photo of the visit ship) is asked of the host (ArirRepIntent), which makes it as its own, a photo
// once. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct ArirRepIntentPayload;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::arir_rep_sync {

// The EventCue id; the cue registry's ids are append-only.
inline constexpr uint32_t kCueArirRep = 105;

void Install(coop::net::Session* session);

// The watch, retried until registered, and the host's once-a-second read. Game thread, once per pump tick.
void Tick();

// HOST: the value to a joiner at its ready edge.
void SendJoin(int slot);

// A client's receiver: true when the cue was this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

// HOST: a change a client's own player made (ArirRepIntent), judged and made as the host's own.
void OnIntent(coop::net::Session& session, int slot, const coop::net::ArirRepIntentPayload& p);

void OnDisconnect();

}  // namespace coop::arir_rep_sync
