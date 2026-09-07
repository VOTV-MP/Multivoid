// coop/weather_redsky.h -- red-sky discrete-event sync.
//
// Red sky is a story event: AmainGamemode_C::spawnRedSky() instantiates an AredSkyEvent_C actor
// whose .set(bool isred) swaps the world colour curves. The ORGANIC trigger is in daynightCycle: on
// a new-hour edge, when the hour is 12, a 1% roll calls gamemode.spawnRedSky through EX_Context and
// EX_LocalVirtualFunction -- invisible to the ProcessEvent detour
// (docs/COOP_DISPATCH_VISIBILITY.md), which is why a POST observer on spawnRedSky or set cannot
// see a native red sky at all, only our own reflected calls.
//
// The HOST POLLS the state field-level instead (the weather_fog shape; MTA's
// CBlendedWeather::DoPulse): gamemode.redSky actor liveness and its `isred` bool, with an edge
// broadcasting a RedSkyPayload -- robust whichever dispatch path, and whichever caller, flipped it.
// The CLIENT receives through event_feed and replays spawn and set locally behind an echo-suppress
// flag; its OWN roll is killed at birth by coop/world/weather_event_births, the FinishSpawningActor
// class-catch.

#pragma once

namespace coop::net { class Session; struct RedSkyPayload; }

namespace coop::weather_redsky {

// Set the session pointer (atomic; read in the poll + Apply). Called from
// weather_sync::Install on every re-entry.
void SetSession(coop::net::Session* session);

// Resolve mainGamemode_C's CDO, the spawnRedSky UFunction and, best-effort, redSkyEvent.set.
// Idempotent. True once the CDO and spawnRedSky are resolved; `set` may resolve later, because
// redSkyEvent_C is a content BP class that need not be loaded before the first spawn, and
// ResolveSetFn takes it off the spawned actor's runtime class.
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
