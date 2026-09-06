// coop/world/weather_fog.h -- host-authoritative FOG sync.
//
// Fog is not a flag. enable_fog and enable_superfog are persistent CONFIG gates; the ACTIVE
// fog is an event ACTOR -- an AweatherFogController_C in the cycle's fogEventObject slot for
// rolling fog, a live AsuperFog_C found by class for super fog -- each ramping the
// height-fog density over its own Duration from its own tick, then destroying itself. The
// BASE ambient density is shared, time-of-day driven through an identical unsuppressed tick,
// so host-clear to client-clear needs only actor destruction, never a density float.
//
// So the lane asserts the host's ACTOR PRESENCE (MTA CBlendedWeather::DoPulse): flags2
// carries kFogActive, kSuperFogActive and kPermanentFog, the host stamps them, and the
// client suppresses its own spawnFog and asserts what the host reports. The rolling actor
// self-destructs with NO observable UFunction, so the scheduler observers miss the fog END
// and HostFogStateChanged supplies the edge. Super-fog SPAWN is deferred, leaving this lane
// clear-only for it. docs/events-and-weather.md is the player-facing page.

#pragma once

namespace coop::net { struct WeatherStatePayload; }

namespace coop::weather_fog {

// Resolve spawnFog off daynightCycle_C and, on the CLIENT only, register the
// echo-suppressed spawnFog PRE interceptor (so the client never spawns fog except
// when ApplyFromHost asks). The clear is a plain K2_DestroyActor (no SetFogDensity:
// density settles to the shared ambient via the cycle's own ReceiveTick), so
// spawnFog is the only fog UFunction we resolve. Idempotent; safe every
// NetPumpTick. Returns true once spawnFog is resolved.
bool Install(bool isHost);

// HOST: stamp the active-fog bits into payload.flags2 from the live cycle -- kFogActive
// (fogEventObject is non-null), kSuperFogActive (a live AsuperFog_C), kPermanentFog (the
// gamerule). Called from weather_sync::ReadCycleState. Game thread. Reads
// CountObjectsByClass for the super fog, but only on the occasional broadcast path, never
// per frame.
void ReadHostFogState(void* cycle, coop::net::WeatherStatePayload& out);

// CLIENT receiver: assert the host's fog state on the local cycle (MTA DoPulse,
// no diff-skip). Rolling fog: host clear + client actor live -> destroy + null
// the slot; host fog + client none -> echo-suppressed spawnFog(). Super fog:
// clear-only (destroy stray when host has none). Always mirror enable_fog /
// enable_superfog / permanentFog. Game thread. Called from weather_sync::ApplyFromHost.
void ApplyFromHost(void* cycle, const coop::net::WeatherStatePayload& payload);

// CLIENT per-tick reconcile heartbeat, throttled internally to about 3 s. When the host's
// last-known fog state was CLEAR, destroys any stray rolling-fog actor in the cycle's
// fogEventObject slot: the MTA DoPulse backstop for an actor that leaked the
// pre-suppression connect window, which a clear and static host never re-broadcasts to
// clear. Self-gates to the client and host-clear; no-op otherwise. Game thread. Called from
// weather_sync::TickConnect.
void TickClientReconcile(void* cycle);

// HOST per-tick fog-edge detector (THROTTLED internally to ~3 Hz -- the super-fog
// check walks GUObjectArray). Returns true when the host's (kFogActive,
// kSuperFogActive, kPermanentFog) tuple CHANGED since the last fire, so
// weather_sync re-broadcasts the (now fog-updated) state. This catches the
// rolling actor's silent self-destruct END that the scheduler observers miss.
// Game thread.
bool HostFogStateChanged(void* cycle);

// Disconnect hook: clear the echo-suppress flag + the cached host detector state.
void OnDisconnect();

// True while ApplyFromHost is mid mirror-spawn -- the spawnFog echo window. Read by
// coop/world/weather_event_births so the wire-commanded fog-controller birth passes the
// client birth-catch.
bool MirrorEchoActive();

}  // namespace coop::weather_fog
