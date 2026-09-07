// coop/weather_rain.h -- the rain and snow half of the day-night cycle's weather lane.
//
// The module owns the rain and snow engine substrate on AdaynightCycle_C: its own five mutator
// UFunction resolves (causeRain, setRainProperties, setWindParameters, intComs_triggerSnow,
// setRainParticles), its own install latch, the causeRain echo-suppress flag and its PRE
// interceptor, and the rain-side read/apply/debug bodies. weather_sync, the orchestrator, keeps the
// scheduler observers and interceptors, the WeatherState broadcast and its dedup signature, the
// connect seed, TickConnect, and the composition ReadCycleState / ApplyFromHost that call into this
// module the same way they call weather_fog and directionalwind.
//
// Principle 7: the substrate (offsets and UFunction thunks) lives here and weather_sync drives it,
// the same shape as weather_fog, weather_lightning and weather_redsky.

#pragma once

namespace coop::net { class Session; struct WeatherStatePayload; }

namespace coop::weather_rain {

// Session pointer for the Debug* host-role checks (the weather_redsky /
// weather_lightning SetSession shape). Set from weather_sync::Install every
// re-entry; atomic inside.
void SetSession(coop::net::Session* session);

// Idempotent install. Resolves the 5 mutator UFunctions off daynightCycle_C
// (own once-latch; UFunction ptrs are UClass-stable across cycle recreation)
// and registers the causeRain echo-suppress PRE interceptor (both roles;
// pass-through unless the echo flag is set around the receiver's apply).
// Returns true once all 5 mutators are resolved. Safe every NetPumpTick.
bool Install();

// HOST read half: stamp the cycle's config/rain/snow FLAG bits (incl. the
// enable_fog/enable_superfog CONFIG gates -- their actor-driven APPLY lives
// in weather_fog) + the 5 rain scalars into the wire payload. The fog ACTOR
// bits (flags2) and the wind fields are stamped by weather_fog /
// ue_wrap::directionalwind from the orchestrator's composition. Resets
// out.flags; call FIRST in the composition. Game thread.
void ReadState(void* cycle, coop::net::WeatherStatePayload& out);

// Receiver apply half: the cycle-side delta apply (enable-bit direct writes,
// setRainProperties, rain ease-target pin, echo-bracketed causeRain,
// setRainParticles, setWindParameters, intComs_triggerSnow). `cur` is the
// pre-apply composite state the ORCHESTRATOR read (the delta-compare
// baseline); `outcome` reports what transitioned so the orchestrator emits
// the fused "weather: applied" log line unchanged. Game thread.
struct ApplyOutcome {
    bool rainTx = false;
    bool snowTx = false;
    bool scalarsChanged = false;
};
void ApplyFromHost(void* cycle, const coop::net::WeatherStatePayload& payload,
                   const coop::net::WeatherStatePayload& cur, ApplyOutcome& outcome);

// Test entrypoint (host only): force rain the way the game does it -- an enable_rain write, then
// setRainProperties, causeRain and setWindParameters. The host's own POST observers, registered by
// weather_sync on its own resolves of the same UFunctions, catch the calls and broadcast. False if
// the cycle is not live or the mutators are unresolved. Game thread only.
bool DebugForceRain(bool isRaining, float rainStrength);

// Test entrypoint (host only): intComs_triggerSnow(isSnow), the visually unambiguous signal, which
// is why it is the one a person can check by looking. Game thread only.
bool DebugForceSnow(bool isSnow);

// Diagnostic: read the local cycle's isRaining bool. `outFound` distinguishes
// "false (cycle null)" from "false (not raining)". Game thread only.
bool ReadLocalIsRaining(bool* outFound);

// The module's validated cycle pointer (may be null while loading). For
// diagnostic consumers (coop/dev/weather_probe) that gate their own cadence;
// the console_desk::Instance() precedent. Game thread only.
void* Cycle();

// Disconnect hook: clears the session ptr, the echo flag (defensive), and
// the module's own cycle cache (may dangle across level transitions).
void OnDisconnect();

}  // namespace coop::weather_rain
