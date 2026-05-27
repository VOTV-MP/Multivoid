// coop/weather_sync.h -- Phase 5W host-authoritative weather sync.
//
// VOTV's weather is owned by a single actor: AdaynightCycle_C. It runs the
// scheduler timers (timerRain / timerLightning / fogEvent / superFogEvent /
// permaRain_timer), owns the 12 rain/snow/fog/wind state fields, and exposes
// the mutator UFunctions (causeRain / setRainProperties / setWindParameters /
// intComs_triggerSnow / spawnFog / SetFogDensity).
//
// Architecture (mirrors the mushroomMaster suppression + Phase 5F Inc5
// connect-replay patterns):
//   HOST:
//     - POST observer on the 5 scheduler UFunctions reads the post-mutation
//       state off the cycle and dedups via FNV1a. On state change, broadcasts
//       a WeatherStatePayload over the reliable channel.
//     - On the connect-edge, queues a forced broadcast so a newly-joined
//       client sees the current state (e.g. mid-storm) without waiting for
//       the next scheduler fire.
//     - Does NOT install the client-side interceptor (its own scheduler
//       must run).
//   CLIENT:
//     - Registers PRE-cancel interceptors on the same 5 scheduler UFunctions
//       via the multi-slot interceptor table. The cycle's scheduler timers
//       fire as the engine schedules them, but the BP body is skipped --
//       the client never rolls "rain now" locally.
//     - Receives WeatherStatePayload from event_feed.cpp; applies via the
//       mutator UFunctions on its local AdaynightCycle_C (NOT direct field
//       writes -- intComs_triggerSnow has 53 BP listeners that need the
//       UFunction dispatch fan-out per the RE doc).
//
// See research/findings/votv-weather-DESIGN-2026-05-26.md (synthesis) and
// votv-weather-RE-{mainGamemode,effect-actors,scheduler}-2026-05-26.md
// (evidence).

#pragma once

#include <cstdint>

namespace coop::net { class Session; struct WeatherStatePayload; struct LightningStrikePayload; struct RedSkyPayload; }

namespace coop::weather_sync {

// Idempotent install. Resolves AdaynightCycle_C + the 5 scheduler UFunctions
// + the 6 mutator UFunctions. Role-aware: HOST registers POST observers
// (broadcast on state change), CLIENT registers PRE-cancel interceptors
// (suppress local scheduler). Safe to call every NetPumpTick; retries until
// the cycle class is loaded by the game. Pass the session pointer so the
// host observer can SendReliable; nullptr disables broadcasting.
void Install(coop::net::Session* session);

// Connect-edge sender: snapshot the LOCAL cycle's current state and stash
// it for a retried broadcast on subsequent TickConnect calls. HOST ONLY --
// no-op on client. Mirrors coop::item_activate::QueueConnectBroadcast.
void QueueConnectBroadcast();

// Per-tick worker. Drains the pending broadcast queue (retries SendReliable
// each tick until the reliable channel accepts). Cheap when no pending
// state. Game thread only.
void TickConnect();

// Disconnect hook: clears pending broadcast + dedup state. The stashed
// payload belonged to the dead session.
void OnDisconnect();

// Receiver-side apply: peer (host) reported a weather state change.
// Looks up the local AdaynightCycle_C and invokes the mutator UFunctions
// to apply each delta. Validates peerSessionId==0 (host-only sender).
// Game thread only.
void ApplyFromHost(const coop::net::WeatherStatePayload& payload);

// Phase 5W test entrypoint (host only). Calls setRainProperties on the
// local AdaynightCycle_C via reflection with the supplied (isRaining,
// rainStrength) -- a stronger trigger than causeRain alone because it
// forces both the bool AND the visible rain-strength scalar. The host's
// POST observer on setRainProperties catches the call + broadcasts the
// resulting state to the client. Used by the autonomous LAN test
// (VOTVCOOP_RUN_WEATHER_TEST=1). Returns false if cycle is not yet live
// or the UFunction isn't resolved. Game thread only.
bool DebugForceRain(bool isRaining, float rainStrength);

// Phase 5W hands-on test entrypoint (host only). Calls intComs_triggerSnow(isSnow)
// on the local AdaynightCycle_C. This is the visually-unambiguous weather
// signal (53 BP listeners fan out to snow particles, ground accumulation,
// sky tint shifts) chosen over rain (subtle particles) and red sky (color
// curve changes invisible in some lighting). The host's POST observer on
// intComs_triggerSnow catches the call + broadcasts WeatherState; the
// receiver's ApplyFromHost path then drives the same UFunction locally for
// the BP fan-out. Returns false if cycle isn't live or the UFunction
// hasn't resolved yet. Game thread only.
bool DebugForceSnow(bool isSnow);

// Phase 5W diagnostic: read the local AdaynightCycle_C's isRaining bool
// directly. Used by the autonomous test to verify cross-peer sync on
// both peers WITHOUT relying on log parsing alone. Returns nullopt-like
// behaviour: false if cycle is null; otherwise the current bool value.
// Pass `outFound` to distinguish "false (cycle null)" from "false (not
// raining)". Game thread only.
bool ReadLocalIsRaining(bool* outFound);

// Phase 5W Inc2: receiver-side lightning strike apply. Host's POST observer
// on BeginDeferredActorSpawnFromClass catches AlightningStrike_C spawns
// (the spawn is BP-internal inside AdaynightCycle_C::timerLightning which
// is fully suppressed on the client by Inc1's interceptor; the spawn never
// happens locally on the client). On receive, this spawns AlightningStrike_C
// at the supplied world location via the standard BeginDeferred/FinishSpawn
// pair. The actor's own Timeline self-destructs after a few seconds; no
// teardown wire needed. Game thread only.
void ApplyLightningStrike(const coop::net::LightningStrikePayload& payload);

// Phase 5W Inc-fix-2 (2026-05-27): red sky test entrypoint (host only).
// Forces the red-sky visual on/off via reflection. ON path: if the
// gamemode's redSky pointer @0x0888 is null, call spawnRedSky (which
// instantiates AredSkyEvent_C + stashes the pointer). If non-null,
// call redSky.set(true). OFF path: if non-null, call redSky.set(false).
// Returns false if the gamemode isn't live or UFunctions aren't
// resolved. Used by the autonomous LAN test as the unambiguous visual
// signal (per user 2026-05-27: rain particles were too subtle to
// verify cross-peer; red sky is unmistakable).
bool DebugForceRedSky(bool red);

// Receiver-side apply for the RedSky discrete event packet. Same
// invocation pattern as DebugForceRedSky (spawnRedSky first time +
// redSky.set thereafter). Validates peerSessionId==0 (host-only sender).
void ApplyRedSky(const coop::net::RedSkyPayload& payload);

}  // namespace coop::weather_sync
