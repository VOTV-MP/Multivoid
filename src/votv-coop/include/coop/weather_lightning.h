// coop/weather_lightning.h -- Phase 5W Inc2 lightning strike sync.
//
// Discrete-event broadcast of AlightningStrike_C spawns. The strike is a
// transient actor whose own Timeline self-destructs after a few seconds;
// no teardown wire is needed. The cycle-resolved scheduler suppression
// (timerLightning interceptor in weather_state) prevents the client from
// spawning lightning locally; this module only carries the spawn LOCATION
// from host -> client + materialises it on the client.
//
// Architecture:
//   HOST observes UGameplayStatics::BeginDeferredActorSpawnFromClass POST,
//   filters by ActorClass == AlightningStrike_C, reads SpawnTransform
//   translation, broadcasts a LightningStrikePayload.
//   CLIENT receives the packet (via event_feed) and dispatches Apply(),
//   which re-spawns the actor at the wire-received location via the
//   standard BeginDeferred + FinishSpawn pair.
//
// Decoupled from cycle/state -- depends only on UGameplayStatics CDO,
// the lightning class, and the spawn UFunctions' param offsets. Resolve
// is idempotent and may succeed before the cycle is live.

#pragma once

namespace coop::net { class Session; struct LightningStrikePayload; }

namespace coop::weather_lightning {

// Set the session pointer (atomic; reads in the observer callback acquire
// it). Called from weather_sync::Install on every re-entry so the observer
// always sees the current session. Pass nullptr to clear (e.g. after Stop).
void SetSession(coop::net::Session* session);

// Resolve the UGameplayStatics CDO + BeginDeferred / FinishSpawn UFunctions
// + AlightningStrike_C class + the ActorClass / SpawnTransform param
// offsets. Idempotent -- fields cached on first success. Returns true when
// every dependency is resolved (safe to register the observer / call Apply).
bool TryResolve();

// HOST-only: register the POST observer on BeginDeferredActorSpawnFromClass
// if not already registered AND TryResolve() succeeded. Returns true if the
// observer is now active (whether already-registered or newly-registered).
// Safe to call every NetPumpTick.
bool RegisterHostObserver();

// Receiver: spawn AlightningStrike_C at the wire-received location via the
// standard BeginDeferred + FinishSpawn UGameplayStatics pair. Validates
// peerSessionId==0 (host-only sender). Game thread only. No-op if TryResolve
// has not succeeded.
void Apply(const coop::net::LightningStrikePayload& payload);

// Disconnect hook: unregister the POST observer if it was registered.
// Mirrors weather_sync::OnDisconnect's role-scoped observer cleanup --
// a reconnect under a different role would otherwise fire the host-only
// observer on the wrong peer. Called from weather_sync::OnDisconnect.
void OnDisconnect();

}  // namespace coop::weather_lightning
