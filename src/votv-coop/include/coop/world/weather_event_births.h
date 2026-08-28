// coop/weather_event_births.h -- the newDay weather-event BIRTH seam (2026-08-29).
//
// daynightCycle's newDay handler rolls per-day chances (measured 1% for red
// sky) and calls gamemode.spawnRedSky / spawnBlackFog / cycle.spawnFog via
// EX_Context + EX_LocalVirtualFunction -- PE-INVISIBLE dispatch (the
// docs/COOP_DISPATCH_VISIBILITY.md class; bytecode-measured in
// research/bp_reflection/daynightCycle.json). EVERY peer runs that roll, so a
// client could sprout its OWN red sky / black fog / rolling fog the host never
// had (the arigalit red-mist field report: one peer blood-red, the other
// clear). The PE PRE interceptor weather_fog registers on spawnFog never sees
// this caller, and red sky had no client suppression at all.
//
// The verbs' BODIES are plain BP SpawnActor chains -> they ALL funnel through
// GameplayStatics::FinishSpawningActor, where our Func-patch POST hook chain
// already lives (host_spawn_watcher, prop_drop_intent). This module adds one
// more consumer: on a CLIENT, an UNCOMMANDED birth of one of the three weather
// event classes (redSkyEvent_C / weatherFogController_C / blackFog_C) is
// destroyed at birth -- the client-side producer is the suppressed side
// (COOP_SYNCER_MODEL par.2b), and the wire-commanded mirror births pass via each
// lane's echo flag. Host births are untouched (the host lanes' field polls
// broadcast them).
//
// Class match is FName-index compare (int compares; no per-spawn allocation);
// the three FNames are minted once on the game thread at install.

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
