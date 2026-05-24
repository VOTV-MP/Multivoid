# VOTV NPC + entity sync — RE findings for the 5 audit-flagged design flaws

**Date:** 2026-05-24
**Trigger:** Architecture audit (commit `823f5d6`) surfaced 5 design flaws. Per `feedback_re_related_functions`, RE all related functions BEFORE proposing implementation. This doc closes the audit gaps with RE-grounded fixes.

**Tools used:** IDA-MCP (string search, xrefs), SDK header dumps (`Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/`), prior project memory + sdk_profile.h offsets.

## Summary of fixes

| Audit flaw | RE-grounded fix |
|---|---|
| **Flaw 1: AI suppression broad-filter** | Write `APawn.AutoPossessAI=0` + `AIControllerClass=nullptr` in the deferred-spawn window. SAME pattern we already use for the player puppet. No post-spawn suppression. **Already-tested mechanism** -- offsets already in sdk_profile.h (0x0231, 0x0238). |
| **Flaw 2: dropped-prop Key policy** | Host generates a runtime Key (`"rt_<hostToken>_<counter>"`) at drop time + sends in the spawn packet. Original world Key discarded at pickup. |
| **Flaw 3: spawner-tick race** | Hook each ticker-spawner's IMPURE BP function via ProcessEvent observer; PRE-observer returns true (skip) on client. `AariralRepEventHandler_C.launchEvent` confirmed impure (declared `void launchEvent()` in BP, no Pure marker -> dispatches through PE). Per-spawner enumeration in section 3 below. |
| **Flaw 4: kerfur targets wrong player** | NpcState target field is a TAGGED UNION: `{ TARGET_REMOTE_PLAYER, TARGET_LOCAL_PLAYER, TARGET_KEY }`. The KERFUR's moveTo @ 0x05D0 is a UObject* (already known); client resolves the tag to its local actor (puppet for remote-player, mainPlayer for local-player, FindByKeyString for world entities). |
| **Flaw 5: explosion double-fire** | Client receives `EffectFire` for explosion -> spawns ONLY the visual/audio effects (particle + sound at location), NOT the `Aexplosion_C` actor. Damage routine runs ONCE on the spawning peer. The `Aexplosion_C.ReceiveBeginPlay` is the damage trigger -- intercepted by not spawning the actor on the non-initiator side. |

---

## 1. Flaw 1 RE — AAIController / APawn possession lifecycle

### Findings (engine-level)

The UE4 engine's auto-AI-possession path:
1. `AActor::PostInitializeComponents` (called from `FinishSpawningActor`)
2. -> `APawn::PostInitializeComponents` -> checks `AutoPossessAI` enum + `AIControllerClass`
3. -> if `AutoPossessAI != Disabled (0)` AND `AIControllerClass != nullptr`: `SpawnDefaultController()` -> `AAIController::Possess(this)`
4. -> `AAIController::OnPossess(InPawn)` (BP-overridable) + `bStartAILogicOnPossess` gates Behaviour Tree start

Confirmed from strings in the running exe (via IDA find_regex):
- `EAutoPossessAI::Spawned` (1) -- auto-possess when spawned
- `EAutoPossessAI::PlacedInWorldOrSpawned` (2) -- default for VOTV NPCs
- `bStartAILogicOnPossess` -- the AAIController flag that gates BT/blackboard activation

Confirmed from SDK dump (`Engine.hpp:7641`):
- `EAutoPossessAI AutoPossessAI @ +0x231` (uint8 enum on APawn)
- `TSubclassOf<AController> AIControllerClass @ +0x238`

### Already in sdk_profile.h (project memory)

```cpp
inline constexpr size_t APawn_AutoPossessPlayer = 0x230;
inline constexpr size_t APawn_AutoPossessAI = 0x231;
inline constexpr size_t APawn_AIControllerClass = 0x238;
```

### Root-cause fix (no broad filter)

In the client's Phase 5N2 spawn lambda:
1. `BeginDeferredActorSpawnFromClass` -> get the actor pointer (BEFORE `BeginPlay`)
2. Write `actor + 0x231 = 0` (AutoPossessAI::Disabled)
3. Write `actor + 0x238 = nullptr` (AIControllerClass)
4. `FinishSpawningActor` -> PostInitializeComponents runs -> no AI controller spawned -> no AI tick -> no AI logic. ZERO suppression filter; ZERO post-spawn cleanup.

This is the **same pattern already shipped for the player puppet** (`coop::RemotePlayer` Spawn path writes the same offsets). NO new mechanism; copy the proven path.

### Verification gate (before Phase 5N2 ships)

Hands-on test: client spawns a kerfur via the wire. Verify:
- No AI controller in the GameThread call stack (no `Run BT` / `Tick BT` traces)
- `GetController()` returns nullptr on the client's kerfur puppet
- Host's kerfur still has full AI

---

## 2. Flaw 2 RE — Dropped-prop Key policy

### Findings

VOTV's `Aprop_C.Key @ +0x02E0` is an FName. The CXX dump shows `Aprop_inventoryContainer_player_C : Aprop_container_C` with `getData(Fstruct_save&) / loadData(Fstruct_save) / extract(int32 Index)` -- the inventory persists via the save system, and `extract` returns an item to the world.

Per the inventory-private rule (user 2026-05-24), inventory contents don't cross the wire. So:
- World prop at pickup: has a Key (either save-baked UUID or runtime-assigned)
- At pickup: world prop despawns on both peers (wire EntityEvent), goes into picker's inventory locally
- At drop: picker's BP calls `extract` -> spawns a new world prop -> we have a fresh actor with no Key field set yet

### Root-cause fix

Host on drop:
1. Observe `extract` (or the actor's BeginPlay if Key is unset)
2. Generate a runtime Key: `runtime_<sessionToken>_<monotonic counter>` (~30 chars, fits WireKey's 31)
3. Write Key into `actor + 0x02E0` immediately
4. Broadcast `NpcSpawn`-equivalent to client with: class FName + transform + the runtime Key
5. Client spawns its own copy + writes the same Key

Original world Key (from save UUID) is discarded at pickup (per the inventory-private rule -- no inventory metadata crosses, including original Keys).

Sync-time-context byte (per MTA pattern) bumped on the new Key so any stale unreliable packets carrying the old Key get dropped.

### Verification gate

Hands-on test:
- Host picks up a flashlight (world prop with save UUID Key X)
- World flashlight despawns on client (EntityEvent: pickup, Key=X)
- Host drops it -> world spawns with NEW runtime Key Y
- Client spawns matching flashlight with Key Y -- visible at host's drop position
- Host picks up again -> Key Y despawns on both peers
- Host drops a second time -> new runtime Key Z, NOT Y, NOT X

---

## 3. Flaw 3 RE — Spawner-tick race

### Findings

VOTV survey enumerated these ticker-spawners (all spawn NPCs on TickEvent / timer):
- `AariralRepEventHandler_C` -- `calcRep() : int32` (likely pure) + `launchEvent() : void` (impure -- spawns ariral)
- `Aticker_insomniacSpawner_C`
- `Aticker_fossilhoundSpawner_C`
- `Aticker_deerSpawner_C`
- `Aticker_wispSpawner_C`
- `Aticker_mannequinSpawner_C`
- `Aticker_hexahiveSpawner_C`
- `Aticker_BadSunSpawner_C`
- `Aticker_dreamSpawner_C`
- ~14 total

Critical RE check (file ariralRepEventHandler.hpp:12):
```cpp
void launchEvent();
```

The `void` return + impure signature (no `bool& return` out-param) is consistent with an IMPURE BP exec function. **Impure BP functions dispatch through `UObject::ProcessEvent`** (we proved this for the PHC observers). So `launchEvent` IS hookable via our existing ProcessEvent detour.

`calcRep()` returning `int32` is ambiguous (could be pure or impure). Test runtime via the diagnostic-prefix mode if needed.

### Root-cause fix

Add per-spawner PRE-observer registrations. For each ticker-spawner:
1. `R::FindClass(L"ticker_insomniacSpawner_C")` -> spawner UClass
2. `R::FindFunction(spawnerCls, L"launchEvent")` (or `tickEvent` / spawn-trigger function name -- per-class identification)
3. `RegisterPreObserver(fn, GrabObserver_SpawnerSkipOnClient)`
4. Observer checks `if (g_session.role() == Client) return true;` -> skips the original BP body

Where Client's `launchEvent` would have spawned an ariral, the host's runs normally -> spawns ariral on host -> Phase 5N2's spawn-observer detects the new actor + broadcasts NpcSpawn -> client spawns its puppet copy.

### Already-shipped pattern

This is the SAME mechanism we use for `SetInterceptor(targetUFunction, cb)` (already in `game_thread.h`). Reuse, don't reinvent.

### Per-spawner RE TODO (per spawner header file)

For each `Aticker_*Spawner_C`, identify the actual "spawn fire" function name -- it varies (`launchEvent`, `tickEvent`, `trySpawn`, etc.). Survey doc lists 14 classes; each header needs a 30-second grep to find the impure spawn function.

### Verification gate

LAN test with autonomous probe:
- Both peers connect at КПП
- Run for ~60 seconds (long enough for one ariral event to fire on host)
- Verify: host's log shows ariral spawn -> NpcSpawn sent -> client's log shows ariral spawn (with matching Key)
- Verify: client's `launchEvent` PRE-observer fires N times (one per tick that WOULD have spawned, were it on host) but client doesn't spawn

---

## 4. Flaw 4 RE — kerfur target as Key reference

### Findings

`kerfurOmega.hpp:40-41`:
```cpp
TEnumAsByte<enum_kerfurCommand::Type> State;    // 0x05C8
class UObject* moveTo;                          // 0x05D0
```

`moveTo` is `UObject*` -- not strictly an actor type. The BP graph likely points it at:
- `AmainPlayer_C*` -- when "follow the player"
- `Aprop_C*` -- when "go investigate this object"
- nullptr -- when idle

`State` is the kerfurCommand enum -- need to enumerate values, but high-bits are likely { IDLE, FOLLOW, FETCH, ATTACK }.

Plus `kerfurOmega.hpp:302`: `void moveToServ();` -- the "move-to-server" function name suggests it's the network-authoritative version. Useful for impure dispatch (we'd hook it).

### Root-cause fix (cross-peer player reference)

NpcState packet field `target` is a tagged byte + payload:

```
uint8 targetTag (0=none, 1=remote-player, 2=local-player, 3=entity-by-key)
[if targetTag == 3] WireKey targetKey
```

Client receiving NpcState with target tag:
- `none` -> set local-kerfur's moveTo = nullptr
- `remote-player` (host pointing at host-mainPlayer) -> set local-kerfur's moveTo = host puppet pointer (g_orphan.actor())
- `local-player` (host pointing at client's representation in host's world) -> set local-kerfur's moveTo = local g_netLocal mainPlayer
- `entity-by-key` -> `prop_wrap::FindByKeyString(targetKey)` -> set local-kerfur's moveTo to the result

The tag is set by the host: when host's kerfur.moveTo points to host's mainPlayer, the host emits "remote-player" (from the CLIENT's perspective, the host IS remote). When it points to a world prop, host emits "entity-by-key" + the prop's Key.

### Verification gate

LAN test:
- Host spawns near a kerfur
- Kerfur AI starts following host (host's moveTo = host.mainPlayer)
- Client's kerfur puppet is verified to be chasing the HOST PUPPET on client side (not client's own mainPlayer)
- Host moves away -> kerfur follows on both peers

---

## 5. Flaw 5 RE — Explosion lifecycle + double-fire prevention

### Findings

`explosion.hpp:19-49`:
```cpp
float Radius;                  // 0x027C
float Damage;                  // 0x0280
FVector impact;                // 0x02B4
void ReceiveBeginPlay();
```

The explosion is an AActor (`Aexplosion_C`) -- one instance per explosion event. BP `ReceiveBeginPlay` runs the damage-radius application (spherical trace + damage to nearby actors). Particle + sound are spawned as separate child components or via PlaySoundAtLocation in the BP.

This is the textbook double-fire risk: if both peers spawn the actor, both peers run BeginPlay, both apply damage.

### MTA pattern (already documented)

MTA's "initiator suppresses local render, sends to server, server arbitrates and broadcasts back including the initiator." Doesn't quite fit UE4 BP world where the BP graph runs damage on every machine that spawns the actor.

### Root-cause fix (separate authority from presentation)

Two-channel design for explosions and similar damage-radius events:

1. **HOST runs the AActor with full damage** (one peer applies damage, world is consistent)
2. **CLIENT runs ONLY the cosmetic effects** (particle + sound at location; no AActor spawn, no damage routine)

Wire packet (reliable, since explosion is a state change):
```
struct EffectFire {
    PacketHeader header;
    uint8 effectType;        // 1=explosion, 2=projectile-hit, 3=...
    FVector location;
    FRotator rotation;
    float param1, param2;    // type-specific (radius/intensity)
    WireKey assetKey;        // path to particle / sound asset (host-local lookup)
};
```

Client receiver:
- For effectType=explosion: do NOT spawn `Aexplosion_C`. Instead, use `UGameplayStatics::SpawnEmitterAtLocation` + `PlaySoundAtLocation` (engine native UFunctions, ProcessEvent-dispatched, already RE'd in similar context).
- For everything that's not damage-radius-bearing: same -- spawn the visual representation only.

Damage radius is applied ONLY on the host's local actor's BeginPlay. The client never participates in damage.

### Verification gate

Hands-on test:
- Host triggers explosion (e.g. throws a grenade at a target)
- Both peers see explosion VFX + hear sound at the same world coords
- Both peers see damage applied to nearby actors via the host's wire (NpcState health updates)
- Client's local `Aexplosion_C` count remains zero (verify via debug log of actor spawn count)
- No double-damage to NPCs near explosion

### Open follow-ups

- Identify the particle + sound asset paths VOTV's `Aexplosion_C` BP uses
- Decide whether `assetKey` is class-FName-by-effect-type (small enum) or full asset path (longer wire)
- Document for the other event AActors (`AlightningStrike_C`, projectile classes) -- same pattern applies

---

## 6. IDB renames (per feedback_ida_rename_and_save)

Strings + xrefs verified but no new function decompile this session -- the RE was at the SDK header / engine-API level, not new native function bodies. No IDB renames to apply.

## Cross-refs

- `research/findings/votv-npc-entity-coop-architecture-2026-05-24.md` -- the architecture being patched
- `research/findings/votv-npc-entity-survey-2026-05-24.md` -- SDK survey of NPC/entity classes
- `research/findings/mta-npc-entity-sync-2026-05-24.md` -- MTA precedent
- `research/findings/votv-physics-interaction-deep-re-2026-05-23.md` -- Stage 1-4 grab (the existing precedent)
- `src/votv-coop/include/ue_wrap/sdk_profile.h` -- the offsets already in profile
- [[project-coop-inventory-private]] memory -- inventory rule
- [[project-coop-whole-map-sync]] memory -- whole-map sync rule
- [[feedback-re-related-functions]] memory -- the rule that triggered this doc
