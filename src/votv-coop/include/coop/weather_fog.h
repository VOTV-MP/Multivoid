// coop/weather_fog.h -- Phase 5W host-authoritative FOG sync (2026-06-01).
//
// Hands-on bug 2026-06-01: the CLIENT showed STRONG MIST while the HOST was
// CLEAR. RE (4-agent IDA+dump workflow) + a solo runtime probe (mp.py fogprobe)
// established WHY the prior weather_sync fog handling failed and what the fix is:
//
//   - enable_fog / enable_superfog are persistent CONFIG gates ("rolls allowed"),
//     NOT active-state toggles. The ACTIVE fog is an event ACTOR:
//       * rolling fog = AweatherFogController_C, held in cycle->fogEventObject@0x338
//       * super fog   = a live AsuperFog_C (no storage slot; found by class)
//     Each actor self-drives its OWN ReceiveTick, ramping the height-fog density
//     (finalFogDensity@0x418) up over its Duration, then self-destructs.
//   - The base ambient density (~0.04) is SHARED between peers (ToD-driven,
//     identical un-suppressed ReceiveTick). The probe proved a cleared client
//     settles to that same ambient. So host-clear -> client-clear needs ONLY
//     actor destruction; there is NO need to stream a density float.
//   - ApplyFromHost previously only flipped the enable_* bits, so a fog actor the
//     client spawned (pre-connect, or in the Install latch window where spawnFog
//     wasn't even intercepted) rode out its own Duration while the host was clear.
//
// Architecture (assert the host's actor presence -- MTA CBlendedWeather::DoPulse):
//   WIRE: WeatherStatePayload::flags2 carries kFogActive / kSuperFogActive /
//         kPermanentFog (the host's live-actor presence + gamerule).
//   HOST: ReadHostFogState stamps flags2 from the live cycle. The rolling actor
//         self-destructs on Duration with NO observed UFunction firing, so the
//         scheduler POST observers MISS the fog-END; HostFogStateChanged is a
//         throttled per-tick detector that returns true on a fog-bit edge so
//         weather_sync re-broadcasts.
//   CLIENT: suppresses spawnFog (so it can never make uncommanded fog) via an
//         echo-suppressed PRE interceptor, and ApplyFromHost asserts the host's
//         state -- destroy a stray rolling actor on host-clear; echo-suppressed
//         spawnFog to mirror on host-fog (the client's own actor ramps naturally,
//         like the lightning-strike spawn-on-command pattern); destroy any stray
//         super fog on host-clear.
//
// Super-fog SPAWN (host-super -> client-super) is DEFERRED in v1 (clear-only):
// AsuperFog_C owns a UFO encounter (AHoelUfo_C) and its spawn was not reliably
// reproducible in the probe. Documented follow-up.
//
// P7: this module owns the fog engine substrate (offsets + UFunction thunks);
// weather_sync (gameplay/network) drives it. Mirrors weather_lightning/weather_redsky.

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

// HOST: stamp the active-fog bits into payload.flags2 from the live cycle --
// kFogActive (fogEventObject != null), kSuperFogActive (a live AsuperFog_C),
// kPermanentFog (permanentFog@0x42D). Called from weather_sync::ReadCycleState.
// Game thread. (Reads CountObjectsByClass for super fog -- only on the occasional
// broadcast path, never per-frame.)
void ReadHostFogState(void* cycle, coop::net::WeatherStatePayload& out);

// CLIENT receiver: assert the host's fog state on the local cycle (MTA DoPulse,
// no diff-skip). Rolling fog: host clear + client actor live -> destroy + null
// the slot; host fog + client none -> echo-suppressed spawnFog(). Super fog:
// clear-only (destroy stray when host has none). Always mirror enable_fog /
// enable_superfog / permanentFog. Game thread. Called from weather_sync::ApplyFromHost.
void ApplyFromHost(void* cycle, const coop::net::WeatherStatePayload& payload);

// CLIENT per-tick reconcile heartbeat (THROTTLED internally to ~3 s). When the
// host's last-known fog state was CLEAR, destroys any stray rolling-fog actor in
// the cycle's fogEventObject slot -- the MTA DoPulse backstop for a fog actor that
// leaked the pre-suppression connect window and that a clear + static host never
// re-broadcasts to clear (the user's persistent "balls of fog around the client").
// Self-gates to the client + host-clear; no-op otherwise. Game thread. Called from
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

}  // namespace coop::weather_fog
