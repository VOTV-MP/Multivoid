// coop/weather_sync.h -- host-authoritative weather sync.
//
// VOTV's weather is owned by a single actor, AdaynightCycle_C. It runs the scheduler timers
// (timerRain, timerLightning, fogEvent, superFogEvent, permaRain_timer), owns the twelve
// rain/snow/fog/wind state fields, and exposes the mutator UFunctions (causeRain,
// setRainProperties, setWindParameters, intComs_triggerSnow, spawnFog, SetFogDensity).
//
// The HOST puts a POST observer on the five scheduler UFunctions, reads the post-mutation state off
// the cycle, dedups it with FNV-1a, and on a change broadcasts a WeatherStatePayload over the
// reliable channel. It installs no client-side interceptor, since its own scheduler must run.
//
// A CLIENT rolls no weather of its own: it registers PRE-cancel interceptors on those same five
// through the multi-slot interceptor table, so the cycle's timers still fire as the engine
// schedules them while the Blueprint body is skipped.

#pragma once

#include <cstdint>

namespace coop::net { class Session; struct WeatherStatePayload; }

namespace coop::weather_sync {

// Idempotent install. Resolves AdaynightCycle_C + the 5 scheduler UFunctions
// + the 6 mutator UFunctions. Role-aware: HOST registers POST observers
// (broadcast on state change), CLIENT registers PRE-cancel interceptors
// (suppress local scheduler). Safe to call every NetPumpTick; retries until
// the cycle class is loaded by the game. Pass the session pointer so the
// host observer can SendReliable; nullptr disables broadcasting.
void Install(coop::net::Session* session);

// Per-slot connect-edge sender. Snapshots the LOCAL cycle's current state and queues a forced
// broadcast, so a newly joined client sees the state it arrives into -- mid-storm, say --
// instead of waiting for the next scheduler fire.
void QueueConnectBroadcastForSlot(int peerSlot);

// Per-tick worker. Drains the pending broadcast queue (retries SendReliable
// each tick until the reliable channel accepts). Cheap when no pending
// state. Game thread only.
void TickConnect();

// Disconnect hook: clears pending broadcast + dedup state. The stashed
// payload belonged to the dead session.
void OnDisconnect();

// Receiver-side apply: the host reported a weather state change. Applied through the mutator
// UFunctions on the local AdaynightCycle_C rather than by writing the fields, because
// intComs_triggerSnow has 53 Blueprint listeners that need the dispatch fan-out.
void ApplyFromHost(const coop::net::WeatherStatePayload& payload);

// Read-only accessors for the changeWindOrigin roll interceptor counters
// (the counters stay with their writer -- the interceptor in this TU).
// Consumed by coop/dev/weather_probe's [probe wind] line.
uint32_t WindRollFired();
uint32_t WindRollSuppressed();

// DebugForceRain, DebugForceSnow and ReadLocalIsRaining live in coop/world/weather_rain.h, the
// rain-and-snow cycle-side sub-lane. For a red sky or a lightning strike, call
// coop::weather_redsky or coop::weather_lightning directly.

}  // namespace coop::weather_sync
