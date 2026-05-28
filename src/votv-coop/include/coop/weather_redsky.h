// coop/weather_redsky.h -- Phase 5W Inc-fix-2 red-sky discrete-event sync.
//
// Red sky is a story event: AmainGamemode_C::spawnRedSky() instantiates an
// AredSkyEvent_C actor whose .set(bool isred) swaps the world color curves.
// The visual is the unambiguous weather signal chosen for cross-peer
// verification (per the 2026-05-27 user finding that rain particles were
// too subtle to verify cross-peer).
//
// Architecture:
//   HOST observes spawnRedSky POST + redSkyEvent.set POST on the
//   mainGamemode_C / redSkyEvent_C classes, broadcasts RedSkyPayload.
//   CLIENT receives the packet (via event_feed) and replays the same
//   spawn-then-set sequence locally; an echo-suppress flag prevents the
//   client's own re-broadcast.
//
// Resolution is lazy -- redSkyEvent_C is a content BP class that may not
// be loaded until first spawn. The set UFunction can resolve later via the
// spawned actor's runtime class (ResolveSetFn).

#pragma once

namespace coop::net { class Session; struct RedSkyPayload; }

namespace coop::weather_redsky {

// Set the session pointer (atomic; reads in the observer callbacks acquire
// it). Called from weather_sync::Install on every re-entry.
void SetSession(coop::net::Session* session);

// Resolve mainGamemode_C CDO + spawnRedSky UFunction + (best-effort)
// redSkyEvent.set UFunction. Idempotent. Returns true if at least the
// gamemode CDO + spawnRedSky are resolved (set fn may resolve lazily on
// first call via the spawned actor's class).
bool TryResolve();

// HOST-only: register the POST observers on spawnRedSky + redSkyEvent.set
// if not already registered AND TryResolve has succeeded for at least
// spawnRedSky. Safe to call every NetPumpTick.
bool RegisterHostObservers();

// Receiver-side apply: peer (host) reported a red-sky state change.
// Spawns the actor on first ON + calls set(state). Validates
// peerSessionId==0. Game thread only.
void Apply(const coop::net::RedSkyPayload& payload);

// HOST test entrypoint. Forces red-sky on/off via reflection. ON: spawn
// + set(true) if not already spawned; SET(true) thereafter. OFF: set(false)
// if a redSkyEvent actor exists. Returns false if not host, gamemode not
// live, or UFunctions unresolved. Game thread only.
bool DebugForce(bool red);

// Disconnect hook: unregister role-scoped POST observers + clear the
// echo-suppress flag (paranoia -- it should already be false outside an
// Apply call). Called from weather_sync::OnDisconnect.
void OnDisconnect();

}  // namespace coop::weather_redsky
