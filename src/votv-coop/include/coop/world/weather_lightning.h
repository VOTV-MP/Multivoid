// coop/world/weather_lightning.h -- lightning strike sync.
//
// Discrete-event broadcast of lightningStrike_C spawns. The strike destroys itself (InitialLifeSpan
// = 10 on its blueprint CDO), so there is no teardown wire; the client's own lightning never fires,
// suppressed at the scheduler by weather_sync's timerLightning interceptor. Only the spawn LOCATION
// crosses the wire.
//
// The host observes UGameplayStatics::BeginDeferredActorSpawnFromClass POST, filters by
// ActorClass == lightningStrike_C and broadcasts the SpawnTransform translation. The world event
// dispatcher checks the sender is the host, then posts Apply() to the game thread, which re-spawns
// the actor at the received location through the BeginDeferred + FinishSpawn pair.
//
// Depends only on the UGameplayStatics CDO, the lightning class and the spawn UFunctions' param
// offsets; TryResolve is idempotent and may succeed before the cycle is live.

#pragma once

namespace coop::net { class Session; struct LightningStrikePayload; }

namespace coop::weather_lightning {

// Set the session pointer (atomic; reads in the observer callback acquire
// it). Called from weather_sync::Install on every re-entry so the observer
// always sees the current session. Pass nullptr to clear (e.g. after Stop).
void SetSession(coop::net::Session* session);

// Resolve the UGameplayStatics CDO + the BeginDeferred / FinishSpawn UFunctions + the
// lightningStrike_C class + the ActorClass / SpawnTransform param offsets. Idempotent -- fields are
// cached on first success. Returns true when every dependency is resolved (safe to register the
// observer / call Apply).
bool TryResolve();

// HOST-only: register the POST observer on BeginDeferredActorSpawnFromClass if not already
// registered AND TryResolve() succeeded. Returns true if the observer is now active (whether
// already-registered or newly-registered). Safe to call every net-pump tick.
bool RegisterHostObserver();

// Receiver: spawn lightningStrike_C at the wire-received location via the standard BeginDeferred +
// FinishSpawn UGameplayStatics pair. The host-only sender check belongs to the world event
// dispatcher (senderPeerSlot == 0), not to this function. Game thread only. No-op if TryResolve has
// not succeeded.
void Apply(const coop::net::LightningStrikePayload& payload);

// Disconnect hook: unregister the POST observer if it was registered.
// Mirrors weather_sync::OnDisconnect's role-scoped observer cleanup --
// a reconnect under a different role would otherwise fire the host-only
// observer on the wrong peer. Called from weather_sync::OnDisconnect.
void OnDisconnect();

}  // namespace coop::weather_lightning
