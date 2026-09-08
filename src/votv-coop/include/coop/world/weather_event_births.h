// coop/weather_event_births.h -- the weather-event BIRTH seam.
//
// daynightCycle rolls its weather events on the settime NEW-HOUR edge -- red sky at hour 12 on a 1%
// roll, black fog on a 0.05% roll, rolling fog behind its own enable_fog gate on fogProbability --
// and calls gamemode.spawnRedSky, spawnBlackFog and cycle.spawnFog through EX_Context and
// EX_LocalVirtualFunction, which no ProcessEvent hook sees. EVERY peer runs those rolls, so a
// client can sprout its OWN red sky, black fog or rolling fog the host never had. weather_fog's PRE
// interceptor on spawnFog never sees this caller, and red sky had no client suppression at all.
//
// The verbs' BODIES are plain blueprint SpawnActor chains, so they all funnel through
// GameplayStatics::FinishSpawningActor, where our Func-patch POST hook chain already lives. This
// module adds one more consumer: on a CLIENT an UNCOMMANDED birth of redSkyEvent_C,
// weatherFogController_C or blackFog_C is destroyed at birth, the client being the suppressed
// producer, while a wire-commanded mirror birth passes on its lane's echo flag. Host births are
// untouched.

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
