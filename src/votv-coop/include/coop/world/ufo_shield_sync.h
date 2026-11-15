// coop/world/ufo_shield_sync.h -- the boar war's saucer over the treehouse, host to clients.
//
// The level holds one ufoshieldshader_C, hidden. boarInvasion_C's begin play calls its SetActive(true): the actor
// shows, ticks its spin, starts its hum and registers itself as an event. The invasion ends with flyAway(), a
// timeline that lifts the saucer and ends in SetActive(false). The invasion runs on the host alone, so a client's
// saucer never showed.
//
// The host notes each SetActive on its saucer (a script-gate post watch) and stamps kUfoShieldActive in
// WeatherState.flags2 while the last was true (coop/world/weather_fog computes the bits and detects their edge;
// the 2 s pulse and the connect snapshot answer a late join). A client calls SetActive(true) on its own saucer
// when the bit rises and flyAway() when it falls.

#pragma once

namespace coop::net {
class Session;
struct WeatherStatePayload;
}  // namespace coop::net

namespace coop::ufo_shield_sync {

// Both roles, pre-world: the session, and the watch on ufoshieldshader_C::SetActive (once per process).
void Install(coop::net::Session* s);

// HOST: whether this world's saucer was last set active. A flag read. Game thread.
bool HostActive();

// CLIENT, on every weather state: shows or sends off the client's saucer when the host's bit changed. Game thread.
void ApplyFromHost(const coop::net::WeatherStatePayload& payload);

// Forgets what was applied and noted.
void OnDisconnect();

}  // namespace coop::ufo_shield_sync
