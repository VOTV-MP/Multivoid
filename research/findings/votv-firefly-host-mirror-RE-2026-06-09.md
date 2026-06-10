# VOTV coop -- FIREFLY mirror RE + peer-symmetric design (2026-06-09)

**User ask:** the base FIREFLIES are not mirrored across peers (each peer paints its
own RNG firefly field, or -- with the spawner suppressed -- none). "I want the RNG of
fireflies sent from HOST to peers so they're synced up and mirrored even with tiny
visuals" -> then: "Can we spawn ALL peers' fireflies and mirror all of them to all
peers?" (the host-only design would leave a client far from the host barren).

**Final design: PEER-SYMMETRIC.** Every peer keeps running its OWN firefly spawner (so
each sees fireflies near itself), captures each of its own spawns, and broadcasts the
position; the host relays a client's spawn to the other clients; every peer spawns the
others'. Union = every peer sees fireflies near themselves PLUS near every other peer.

Tooling: `tools/bp_reflect.py` dump (`research/bp_reflection/ticker_fireflySpawner.json`)
+ the project source (game_thread/npc_sync/weather_lightning/reflection) + CXX dump
(`Engine.hpp`). Builds on `votv-ticker-subsystem-RE-2026-06-08.md` (the firefly spawner
disassembly). No IDA needed.

---

## 1. The load-bearing finding: the firefly spawn is NOT observable at the call site

`Aticker_fireflySpawner_C::ReceiveTick` (TickInterval=30 s) -> ubergraph -> on a
successful RNG+grass roll calls `UGameplayStatics::SpawnEmitterAtLocation(self,
eff_fireflies, hit.Location, ...)`. In the bytecode that call is **`EX_CallMath`**
(`ticker_fireflySpawner.json:2748`, `StackNode -38`; params EX_Self, ObjectConst -63 =
eff_fireflies, LocalVar BreakHitResult_Location, zero rotation, unit scale).

`EX_CallMath` (`execCallMathFunction`) invokes the UFunction's native thunk **directly on
the function's class CDO** -- it does NOT route through `UObject::ProcessEvent`. Our
detour (`ue_wrap/game_thread.cpp`) hooks ONLY `ProcessEvent`, so a POST observer / PRE
interceptor on `SpawnEmitterAtLocation` would **never fire** for this call. This is the
same miss as the 2026-06-08 `playerGrabbed` trap (`EX_LocalVirtualFunction` ->
ProcessInternal-direct).

Contrast: npc_sync + weather_lightning successfully hook
`BeginDeferredActorSpawnFromClass` -- proving UGameplayStatics calls CAN be observable --
but that one is emitted as `EX_FinalFunction` (routes through ProcessEvent), whereas the
simpler static `SpawnEmitterAtLocation` got the `EX_CallMath` fast path. So observability
is per-call-opcode, not per-function.

**=> the host cannot observe the spawn directly. Capture it indirectly.**

## 2. Capture: PRE+POST-observe ReceiveTick, diff the ParticleSystemComponent set

`ReceiveTick` IS ProcessEvent-dispatched (the engine ticks the actor via
`Actor->ProcessEvent(ReceiveTickFn)`), every 30 s. Register a PRE + a POST observer on
the firefly class's OWN `ReceiveTick` UFunction (`FindFunction(fireflyClass,
"ReceiveTick")` -- a per-class UFunction, so the observers fire ONLY for the firefly,
no self-filter needed). Between PRE (before body) and POST (after body) only the firefly
tick body runs -- synchronously, one ProcessEvent dispatch -- and the ONLY new
`ParticleSystemComponent` it creates is the firefly emitter (SpawnEmitterAtLocation).
So:
- PRE: snapshot `FindObjectsByClass("ParticleSystemComponent")` into a set.
- POST: the PSC in the new set but not the PRE set = the firefly. Read its world position.
No `Template` offset needed (the scoping to one tick identifies it); no slot-reuse hazard
(the tick only spawns, never destroys, in that window).

Spawn position = the new PSC's `RelativeLocation` (@USceneComponent +0x011C, raw read).
SpawnEmitterAtLocation's AtLocation variant creates an UNATTACHED component at the world
location, so RelativeLocation == world spawn point. Raw read avoids a nested UFunction
call from inside the observer. *(UNVERIFIED hands-on: if the PSC were ever attached,
read ComponentToWorld.Translation instead.)*

Perf: `FindObjectsByClass` walks the ~190k GUObjectArray + builds a wstring/object, twice
per firefly tick -- but the tick is 30 s, so ~per-30 s, NOT per-frame. A defensive ~1 Hz
cap (`g_lastCaptureMs`) bounds it if a recook ever made ReceiveTick hot. The 2 extra
registered observers are bloom-filtered/count-bounded in the detour -> ~free on
non-firefly dispatches.

## 3. Spawn-on-receive (every peer)

`SpawnEmitterAtLocation(WorldContextObject, EmitterTemplate, Location, Rotation, Scale,
bAutoDestroy, PoolingMethod, bAutoActivateSystem)` (Engine.hpp:11306). Called via reflection
on the GameplayStatics CDO (the weather_lightning precedent: `FindClassDefaultObject` ->
`ParamFrame` set-by-name -> `Call(cdo, frame)`):
- WorldContextObject = `players::Registry::Get().Local()` (local pawn -- a valid world ctx).
- EmitterTemplate = `FindObject("eff_fireflies", "ParticleSystem")` (each peer resolves its
  own process-local pointer; the asset is always loaded).
- Location = received pos; Rotation = 0; Scale = (1,1,1); bAutoDestroy=true; PoolingMethod=0;
  bAutoActivateSystem=true.
This is NOT called from inside an observer (only from event_feed's posted task) -> no
ProcessEvent re-entrancy.

## 4. Peer-symmetric transport (no suppression, no echo)

- Each peer runs its own spawner (NOT suppressed) -> sees its own fireflies locally.
- Each peer broadcasts its captured spawn via `SendReliable(FireflySpawn)`: a client -> host;
  the host fans out to all clients.
- The host ALSO relays a client's FireflySpawn to the OTHER clients +
  processes it locally -- `FireflySpawn` added to `IsClientRelayableReliableKind`
  (`session_lanes.h`).
- On receive, `firefly_sync::OnReliable` spawns the emitter. OnReliable NEVER re-sends ->
  no relay loop.
- No echo: the origin never receives its own send (the host doesn't self-receive; the relay
  excludes the origin client). The origin already has the local emitter from its own spawner.

Wire: `ReliableKind::FireflySpawn=40`, `FireflySpawnPayload{float x,y,z}` (12 B), protocol
v50->v51. Module `coop/firefly_sync.{h,cpp}` (206 LOC). Reflection helper
`R::FindObjectsByClass(className) -> vector<void*>` added. NO interceptor (so the
kMaxInterceptors=16 table is untouched; the v50 wind changeWindOrigin interceptor is
net-zero vs the removed ticker_sync slot).

## 5. RULE 2 / history

The earlier same-session `coop/ticker_sync` module (which SUPPRESSED the client firefly
spawner -> the "client has none" bug) was DELETED -- the peer-symmetric model supersedes
it (every peer runs its spawner). The brief "host-only mirror" intermediate (host captures
+ broadcasts, client suppresses) was also discarded before shipping per the user's "all
peers' fireflies to all peers" correction; this doc's filename keeps "host-mirror" only
because protocol.h + the module cite it.

## 6. UNVERIFIED (hands-on)

- The new-PSC RelativeLocation == world spawn point (assumes unattached; true for the
  AtLocation variant). If positions look off, switch to ComponentToWorld.Translation.
- The PRE/POST diff captures exactly one firefly PSC per successful roll (no nested PSC
  create/destroy by the tick's EX_FinalFunction sub-calls). Believed sound (the tick only
  spawns the one emitter) but confirm in the smoke log (one "broadcast own spawn" per
  visible firefly).
- The reflected SpawnEmitterAtLocation renders on the receiver (the particle is visible).
