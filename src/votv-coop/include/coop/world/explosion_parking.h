// coop/world/explosion_parking.h -- the explosion copy on a client writes nothing in the world but its own
// player's damage and the scorch grime: the four interface verbs it calls on anything but this machine's own
// player are refused, its rune check is refused, and its radial force is narrowed to pawns. The watches are
// installed from the load watchers; every refusal acts only on an active client session.
#pragma once

#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::explosion_parking {

// Store the session and install the six watches (the four verbs, the rune check, the begin-play narrowing) and the
// FireImpulse post-hook. Idempotent. A watch that is refused is retried on a one-second throttle, at most 30 tries each, then said once
// and left out for the process. Game thread, every pump tick.
void Install(coop::net::Session* s);

// The six watches are installed and their names resolved, so the gate really intercepts. A copy of an explosion
// must not begin before this holds.
bool Live();

// Host, game thread, every gameplay tick: for each explosion whose radial force fired since the last tick, the
// simulating, enrolled props within its radius are handed to the driven-prop channel to coast, so every client
// sees them fly. A cheap return when nothing fired. The FireImpulse hook this reads is not part of Live(): a
// missing coast costs smoothness and never writes a client's world.
void Tick(coop::net::Session* s);

// The counters are cleared, and so are the explosions waiting for Tick; a summary line first when anything was
// counted.
void OnDisconnect();

// Per verb (addDamage, exploded, reachedByExplosion, ignite): the calls the watch saw on a client session and the
// ones it refused. runeRefused: the rune checks refused. narrowed: the explosion instances whose force was narrowed.
struct Counters {
    uint32_t verbCalls[4], verbRefused[4], runeRefused, narrowed;
};
Counters GetCounters();

}  // namespace coop::explosion_parking
