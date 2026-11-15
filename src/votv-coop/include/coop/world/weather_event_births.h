// coop/world/weather_event_births.h -- the weather-event BIRTH seam.
//
// daynightCycle rolls its weather events on the settime NEW-HOUR edge -- black fog on 0.05%, rolling
// fog behind enable_fog on fogProbability -- and calls spawnBlackFog and spawnFog through EX_Context
// and EX_LocalVirtualFunction, which no ProcessEvent hook sees (docs/coop-dispatch-visibility.md).
// EVERY peer rolls, so a client can sprout weather the host never had, and weather_fog's spawnFog
// interceptor never sees it. (The noon's red sky toggle is refused at its verb by weather_redsky.)
//
// The verbs' BODIES are plain blueprint SpawnActor chains, so they all funnel through
// GameplayStatics::FinishSpawningActor, where our Func-patch POST hook chain already lives. This
// module adds one more consumer: on a CLIENT an UNCOMMANDED birth of weatherFogController_C or
// blackFog_C is destroyed at birth, the client being the suppressed producer, while a wire-commanded
// mirror birth passes on its lane's echo flag (weather_fog for the controller, weather_blackfog for
// the black fog). Host births are untouched. That hook fires for EVERY spawn,
// so the class test is an FName-index compare against two names minted once at install: no
// allocation per spawn.

#pragma once

namespace coop::net { class Session; }

namespace coop::weather_event_births {

// Resolve FinishSpawningActor + mint the class FNames + install the POST hook
// (once, process-lifetime -- the hook facade has no remove). Refreshes the
// session pointer + role gate every call. Returns false while resolution is
// incomplete (caller retries next tick; same contract as weather_fog::Install).
bool Install(coop::net::Session* session, bool isHost);

// Session teardown: reset the role gate + counters. The hook stays installed
// (it self-gates on session + role).
void OnDisconnect();

}  // namespace coop::weather_event_births
