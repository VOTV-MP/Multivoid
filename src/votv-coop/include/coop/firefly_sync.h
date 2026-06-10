// coop/firefly_sync.h -- PEER-SYMMETRIC ambient FIREFLY mirror (v51).
//
// VOTV's Aticker_fireflySpawner_C rolls per-peer RNG every 30 s and
// SpawnEmitterAtLocation's the eff_fireflies particle in a ring around the LOCAL
// camera over grass. Fireflies are camera-relative, so there is no single shared
// position -- the right model is SYMMETRIC: every peer keeps running its OWN spawner
// (so each sees fireflies near itself) AND shares each spawn so every other peer sees
// it too. The union = every peer sees fireflies near themselves PLUS near every other
// peer. (A host-authoritative-only design would leave a client far from the host
// barren -- the fireflies would all be over by the host.)
//
// The catch (RE: votv-firefly-host-mirror-RE-2026-06-09.md): the spawn is an
// EX_CallMath native call -- it invokes the GameplayStatics CDO thunk directly and
// BYPASSES our ProcessEvent detour, so we can NOT observe it at the call site (the same
// miss as playerGrabbed). So each peer captures its OWN spawn indirectly: PRE+POST-observe
// the spawner's ReceiveTick (which IS ProcessEvent-dispatched, every 30 s) and diff the
// live ParticleSystemComponent set across that one SYNCHRONOUS tick -- the lone new
// component is the firefly. The peer broadcasts its world position (SendReliable; a client
// -> host, the host fans out / RELAYS a client's spawn to the OTHER clients via
// IsClientRelayableReliableKind). On receive, a peer spawns eff_fireflies there via a
// reflected SpawnEmitterAtLocation. No suppression (every peer runs its own spawner); no
// echo (the origin never receives its own send -- the host doesn't self-receive, the relay
// excludes the origin client). Fireflies are transient -> no connect-snapshot.

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
