// coop/world/weather_blackfog.h -- the black fog event, host to clients.
//
// mainGamemode_C::spawnBlackFog spawns a blackFog_C at the origin and holds it in the gamemode's
// `blackfog` member (it returns at once while one is held). The actor ramps a post-process darkness
// in, holds it 180 s, fades it out in 1 s and destroys itself; once dark it spawns an eyer_C ghost
// every 40 s from its own timer, ghosts that watch the LOCAL camera and bite the local player. The
// ghosts are per viewer and ride coop/creatures/owner_entity_sync, so each peer runs its own copy of
// the fog and the ghosts come with it.
//
// The host stamps kBlackFogActive in WeatherState.flags2 while its gamemode holds a live blackFog_C
// (weather_fog's bits and edge detector carry it, the pulse and the connect snapshot answer a late
// join). A client runs the game's own spawnBlackFog once per host episode, inside an echo bracket the
// birth catch honours (coop/world/weather_event_births), and ends its copy, ghosts included, when
// the bit clears. docs/events-and-weather.md is the player-facing page.

#pragma once

namespace coop::net { struct WeatherStatePayload; }

namespace coop::weather_blackfog {

// Refresh the role gate; call every install tick (cheap). The module resolves nothing at install:
// the gamemode's members and spawnBlackFog resolve on first use.
void Install(bool isHost);

// HOST: whether the gamemode holds a live blackFog_C. Constant time (a member read), so the fog-edge
// detector may call it every sample. False when the gamemode or the member does not resolve. Game thread.
bool HostHasBlackFog();

// CLIENT receiver: assert the host's black fog. Host fog and no copy here, and none run yet this
// episode in this world: spawnBlackFog under the echo bracket. Host clear and a live copy: destroy it
// and its ghosts. Called from weather_sync::ApplyFromHost on every state, so the host's 2 s pulse is
// the heartbeat. Game thread.
void ApplyFromHost(const coop::net::WeatherStatePayload& payload);

// True while ApplyFromHost is inside its spawnBlackFog call. Read by weather_event_births so the
// wire-commanded birth passes the client birth catch.
bool MirrorEchoActive();

// Disconnect hook: drop the echo flag and the episode latch.
void OnDisconnect();

}  // namespace coop::weather_blackfog
