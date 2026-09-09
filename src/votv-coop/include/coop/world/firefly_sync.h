// coop/world/firefly_sync.h -- PEER-SYMMETRIC ambient FIREFLY mirror.
//
// Aticker_fireflySpawner_C rolls per-peer RNG every 30 s and SpawnEmitterAtLocation's eff_fireflies
// in a ring around the LOCAL camera. They are camera-relative, so there is no shared position: the
// model is symmetric, every peer running its OWN spawner and sharing each spawn, so each sees
// fireflies near itself and near the others. Host authority alone would leave a distant client
// barren.
//
// The catch: the spawn is an EX_CallMath native call, invoking the GameplayStatics CDO thunk
// directly and BYPASSING our ProcessEvent detour, so the call site cannot be observed. Each peer
// captures its own spawn indirectly: PRE and POST observe the spawner's ReceiveTick, which IS
// ProcessEvent-dispatched, and diff the live ParticleSystemComponent set across that one
// SYNCHRONOUS tick; the new component is the firefly. Its position goes out reliably, the host
// relaying a client's spawn to the others, and a receiver spawns eff_fireflies there. Nothing is
// suppressed and nothing echoes. Fireflies are transient: no connect snapshot.

#pragma once

namespace coop::net { class Session; struct FireflySpawnPayload; }

namespace coop::firefly_sync {

// Idempotent per-NetPumpTick install. Resolves the firefly class + its ReceiveTick and
// registers the PRE/POST capture observers (every peer captures its own spawns), and
// resolves the reflected spawn path. Safe to call every tick; retries until the firefly
// BP class is loaded by the game.
void Install(coop::net::Session* session);

// Receive: spawn eff_fireflies at another peer's broadcast position. Game thread.
void OnReliable(const coop::net::FireflySpawnPayload& payload);

// Teardown: clear the capture snapshot + session pointer. Observers stay registered
// (they self-gate on a connected session), like the weather observers.
void OnDisconnect();

}  // namespace coop::firefly_sync
