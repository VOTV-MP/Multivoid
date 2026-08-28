// coop/weather_redsky.h -- Phase 5W red-sky discrete-event sync.
//
// Red sky is a story event: AmainGamemode_C::spawnRedSky() instantiates an
// AredSkyEvent_C actor whose .set(bool isred) swaps the world color curves.
//
// Architecture (REROOTED 2026-08-29, the arigalit red-mist field report):
//   The ORGANIC trigger is a 1% roll in daynightCycle's newDay handler that
//   calls gamemode.spawnRedSky via EX_Context + EX_LocalVirtualFunction --
//   PE-INVISIBLE (measured in research/bp_reflection/daynightCycle.json; the
//   docs/COOP_DISPATCH_VISIBILITY.md class). The original POST observers on
//   spawnRedSky/set therefore NEVER fired for a native red sky (zero
//   "host broadcast RedSky" lines across every log on disk) -- they observed
//   only OUR OWN reflected Calls. Retired whole (RULE 2).
//
//   HOST now POLLS the state field-level (the proven weather_fog shape, MTA
//   CBlendedWeather::DoPulse): gamemode.redSky actor liveness + its `isred`
//   bool, edge -> broadcast RedSkyPayload. Robust regardless of which
//   dispatch path (or which caller) flipped the state.
//   CLIENT receives (via event_feed) and replays spawn + set locally, with
//   an echo-suppress flag; its OWN organic 1% roll is killed at birth by
//   coop/weather_event_births (the FinishSpawningActor class-catch).
//
// Resolution is lazy -- redSkyEvent_C is a content BP class that may not
// be loaded until first spawn. The set UFunction can resolve later via the
// spawned actor's runtime class (ResolveSetFn).

#pragma once

namespace coop::net { class Session; struct RedSkyPayload; }

namespace coop::weather_redsky {

// Set the session pointer (atomic; read in the poll + Apply). Called from
// weather_sync::Install on every re-entry.
void SetSession(coop::net::Session* session);

// Resolve mainGamemode_C CDO + spawnRedSky UFunction + (best-effort)
// redSkyEvent.set UFunction. Idempotent. Returns true if at least the
// gamemode CDO + spawnRedSky are resolved (set fn may resolve lazily on
// first call via the spawned actor's class).
bool TryResolve();

// HOST poll: read the live gamemode's redSky state (actor live && isred),
// broadcast RedSkyPayload on an EDGE. Internally throttled (~500 ms);
// safe to call every NetPumpTick. No-op on a client / no session.
void HostPollEdge();

// True iff the local world currently has an ACTIVE red sky (live
// AredSkyEvent_C with isred). Used by the host's per-joiner weather seed
// (a late joiner must enter an already-red world red -- principle 8).
// Game thread.
bool LocalRedSkyActive();

// True while Apply() is mid spawn/set (the echo window). Read by
// coop/weather_event_births to let the wire-commanded mirror birth pass
// the client birth-catch.
bool ApplyEchoActive();

// Receiver-side apply: peer (host) reported a red-sky state change.
// ON: spawn (if absent) + set(true). OFF: set(false) + destroy the local
// actor (full mirror of "no red sky"). Game thread only.
void Apply(const coop::net::RedSkyPayload& payload);

// HOST test entrypoint. Forces red-sky on/off via reflection. ON: spawn
// + set(true) if not already spawned; SET(true) thereafter. OFF: set(false)
// if a redSkyEvent actor exists. Returns false if not host, gamemode not
// live, or UFunctions unresolved. Game thread only.
bool DebugForce(bool red);

// Disconnect hook: clear the echo-suppress flag + poll edge memory.
// Called from weather_sync::OnDisconnect.
void OnDisconnect();

}  // namespace coop::weather_redsky
