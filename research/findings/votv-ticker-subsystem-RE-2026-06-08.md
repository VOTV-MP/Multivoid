# VOTV ticker subsystem — RE + coop-sync design (2026-06-08)

Gating RE for the **client-side firefly desync** bug: a CLIENT that joins a
CLEAR-weather host starts spawning fireflies (plus wind/fog, handled by
another agent) the host doesn't have. Root cause is architectural, not
firefly-specific: VOTV's `ticker_*` ambient-spawner actors run
**independently on every peer with independent RNG**, so the two worlds
diverge. This doc disassembles the firefly ticker byte-for-byte, proves how
tickers are instantiated, categorizes all ~28 tickers, and recommends a
host-authoritative suppression mechanism with exact UFunction names.

Tools used (attributed per finding): **bp_reflect.py** (repak +
kismet-analyzer kismet disassembly) for all BP logic; CXX header dump for
signatures/offsets; existing mod source for the suppression precedent. IDA
was **not** needed — bp_reflect resolved every load-bearing claim.

---

## TL;DR

1. **The firefly ticker is a pure ambient particle spawner.** Every 30 s
   (`TickInterval = 30.0`) it rolls `RandomBoolWithWeight` with a weight that
   **rises as the sun sets** (reads `daynightCycle.sun_height`), picks a random
   point in a 3000–10000 cm ring around the **local player camera**, traces
   down, and **only if the ground physmat is `grass`** spawns the
   `/Game/particles/eff_fireflies` particle system via
   **`UGameplayStatics::SpawnEmitterAtLocation`**. (bp_reflect:
   `ticker_fireflySpawner.json`.)

2. **It spawns a PARTICLE, not an actor** → it goes through **NO** UFunction
   our mod hooks. `npc_sync` intercepts `BeginDeferredActorSpawnFromClass`;
   `SpawnEmitterAtLocation` is never observed by anything in the mod (grep:
   zero hits). So the client runs the full firefly spawner unsuppressed. This
   is the firefly desync.

3. **Tickers are spawned by the gamemode, not level-placed.**
   `ExecuteUbergraph_mainGamemode` (stmt 1612-1625+) spawns the entire ticker
   set — firefly, surveilanceMaster, tick, … — in one deferred
   `BeginDeferredActorSpawnFromClass` → `FinishSpawningActor` block at gamemode
   BeginPlay. The ticker ACTOR classes are **not** in `kNpcAllowlist`, so
   `npc_sync` lets them spawn on BOTH peers → every peer owns a full set of
   independently-ticking ticker actors.

4. **Most other ambient spawners spawn ACTORS via
   `BeginDeferredActorSpawnFromClass`** (deer, wisp, yellowWisp, roach,
   mannequin, insomniac, fossilhound, hexahive, bp7, beehive, susHole, tree,
   bush, eyers, tick). Two of those spawned classes (`insomniac_C`,
   `fossilhound_C`) ARE in the NPC allowlist and are therefore **already
   suppressed + mirrored** on the client. The rest (deer, wisps, trees, holes,
   bees, plants, eyers, mannequin-as-prop, …) are **NOT** allowlisted → also
   running twice, same class of bug as fireflies but for actors.

5. **Recommended fix (RULE 1, root cause):** make the client a **slave** for
   ambient world spawners by PRE-intercepting each ambient ticker's
   **`ReceiveTick`** UFunction on the client and returning `true` (cancel the
   body), exactly like `weather_sync` cancels the 5 weather schedulers and
   `garbage_sync` cancels `prop_openContainer_C::ReceiveTick`. Per-player-state
   tickers (food/sleep/pause/tolerance) keep running locally. NPC-spawning
   tickers whose payload is allowlisted are already covered by `npc_sync` and
   should be LEFT to it (do not double-suppress). Scope now: firefly + the 3
   cheap particle/ambient ones; queue the rest.

---

## 1. The firefly ticker, disassembled

Source: `bp_reflect.py ticker_fireflySpawner` →
`research/bp_reflection/ticker_fireflySpawner.json` (4243 lines).
Header: `CXXHeaderDump/ticker_fireflySpawner.hpp`.

### 1.1 Class shape + cadence

`Aticker_fireflySpawner_C : public Aticker_base_C` has only `ReceiveTick(float)`
+ `ExecuteUbergraph_ticker_fireflySpawner(int32)` (header, lines 4-10). The CDO
(`Default__ticker_fireflySpawner_C`) sets `PrimaryActorTick`:

- `bCanEverTick = true` (json:3166)
- **`TickInterval = 30.0`** (json:3176-3177)

So `ReceiveTick` fires **once every 30 seconds**, not every frame.

`ReceiveTick` body (json:2912-2957) is trivial — it just stores `DeltaSeconds`
into the uber-frame and calls `ExecuteUbergraph_ticker_fireflySpawner(EntryPoint=10)`
(`EX_LocalFinalFunction StackNode 1`, IntConst 10, json:2940-2947), then
`EX_Return`. **All real logic is in the ubergraph.** Important for the fix: a
PRE-interceptor on `ReceiveTick` that returns true cancels the entire spawn —
there is no other work in `ReceiveTick` the client needs.

### 1.2 Ubergraph flow (EntryPoint 10)

The ubergraph (json:1172-2810) is straight-line with three `EX_JumpIfNot`
guards, all jumping to the same `CodeOffset 1369` (the function's
`EX_Return`/end). I.e. the structure is:

```
sun = gamemode.daynightCycle.sun_height                         # json:1228-1283
clampDark = FClamp(sun * 2.0, 0.0, 1.0)                         # json:1287-1343 (StackNode FClamp)
inv       = 1.0 - clampDark                                     # json:1372-1394 (Subtract_FloatFloat)
weight    = inv * 4.0                                           # json:1421-1443 (MultiplyMultiply_FloatFloat, *4)
roll      = RandomBoolWithWeight(weight)                        # json:1446-1478 (StackNode -45)
if (!roll) return;                                              # GATE 1  json:1481-1496

cam   = GetPlayerCameraManager(self, playerIndex=0)            # json:1498-1524 (StackNode -37)
base  = cam.K2_GetActorLocation()                              # json:1526-1584 (StackNode -34)
ang   = RandomFloatInRange(0, 360)                             # json:1586-1623 (StackNode -46)
dist  = RandomFloatInRange(3000, 10000)                        # json:1626-1663 (StackNode -46)
dir   = RotateAngleAxis( MakeVector(dist,0,0), ang, (0,0,1) )  # json:1665-1783
center= base + dir                                             # json:1785-1841 (Add_VectorVector)
traceStart = center + (0,0, 2000)                              # json:1936-1988
traceEnd   = center - (0,0, 2000)                              # json:1882-1934 (Subtract_VectorVector)
objTypes   = obj_static(self, ...)                             # json:1990-2028 (lib_obj_C helper -> EObjectTypeQuery[])
hit?  = LineTraceSingleForObjects(traceStart, traceEnd, objTypes, ...)   # json:2030-2178 (StackNode -50)
if (!hit) return;                                              # GATE 2  json:2180-2195
BreakHitResult(hit) -> PhysMat                                 # json:2196-2435 (StackNode -36)
isGrass = (PhysMat == grass)                                   # json:2437-2473 (EqualEqual, ObjectConst -66 = grass)
if (!isGrass) return;                                          # GATE 3  json:2476-2490
BreakHitResult(hit) -> Location                                # json:2492-2731
SpawnEmitterAtLocation(self, eff_fireflies, hit.Location, rot=0, scale=1, autoDestroy=true)   # json:2732-2800 (StackNode -38)
return;                                                        # json:2803-2806
```

### 1.3 The four answers the task asked for

- **What it spawns / how:** the **`eff_fireflies` `UParticleSystem`** at
  `/Game/particles/eff_fireflies` (Imports json:4011-4019; the `ObjectConst -63`
  passed to the spawn at json:2755-2757). Spawned via
  **`UGameplayStatics::SpawnEmitterAtLocation`** (Imports json:3786-3793; the
  `EX_CallMath StackNode -38` at json:2748-2749). It is a fire-and-forget
  particle emitter (`autoDestroy = true`, json:2797) — **no Actor, no
  pooling**. There is no spawned `_C` actor class at all.

- **What gates a spawn:**
  1. **Time of day** — weight `= (1 - FClamp(sun_height·2, 0, 1)) · 4`. With
     `sun_height` high (daytime) the clamp saturates to 1 → weight 0 → never
     spawns. As the sun drops the weight climbs to a max of 4. `sun_height` is
     read off `daynightCycle` (json:1228-1283), so **YES it reads
     `AdaynightCycle_C`** (via `GameMode.daynightCycle`). It does **not** read
     `isRaining`/`enable_rain` — weather is NOT a firefly gate (only darkness).
  2. **Per-tick RNG roll** — `RandomBoolWithWeight(weight)` (GATE 1). With the
     **engine's global RNG**, unseeded/unsynced. This is the divergence source.
  3. **Ground type** — a downward `LineTraceSingleForObjects` must hit (GATE 2)
     AND the hit's `PhysicalMaterial` must be **`grass`** (GATE 3,
     `ObjectConst -66` = `/Game/materials/phys/grass`, Imports json:4040-4045).
     So fireflies only appear over grassy terrain.
  4. **Player proximity** — position is sampled in a **3000–10000 cm ring
     around the local `PlayerCameraManager`** (player index 0). On a client,
     "player 0" is the client's own pawn, so even the candidate positions
     differ from the host's.

- **Rate / lifetime:** one ROLL per **30 s**; on success exactly one emitter is
  spawned. Lifetime is the particle system's own duration (auto-destroyed). No
  cap/registry — purely transient cosmetic.

- **Does it read `AdaynightCycle_C` / `AmainGamemode_C`?** Both. It reaches
  `daynightCycle` **through** `GameMode` (`Aticker_base_C::GameMode`,
  header `ticker_base.hpp:8`, `AmainGamemode_C*` @ 0x0230), then reads
  `sun_height` off the cycle.

**Why it desyncs (proven):** the spawn decision is `RandomBoolWithWeight` on
the **engine global RNG** + positions around the **local** camera. The two
peers roll independently and sample around different cameras, so each peer
paints its own firefly field. Even if you synced `sun_height` perfectly, the
RNG + per-camera positioning still diverge. The fix is not "sync the RNG" — it
is "the client must not run the spawner at all" (host authority).

---

## 2. How tickers are instantiated

Source: `bp_reflect.py mainGamemode` → `research/bp_reflection/mainGamemode.json`
(197 functions). Header: `CXXHeaderDump/mainGamemode.hpp`.

- The gamemode imports **every** ticker class
  (`mainGamemode.json` NameMap 323-342: `ticker_bp7Spawner`, `…deerSpawner`,
  `…fireflySpawner`, `…flickerer`, … through `…tick`).
- `ticker_fireflySpawner_C` (import ref `-542`) is referenced by exactly one
  function: **`ExecuteUbergraph_mainGamemode`** (verified by scanning every
  function export's bytecode).
- In that ubergraph, the firefly is spawned at **statement 1613**:
  `EX_LetObj StackNode = BeginDeferredActorSpawnFromClass`, class const =
  `ticker_fireflySpawner_C`, immediately followed by stmt 1615
  `FinishSpawningActor`. Statements 1612-1625+ are a back-to-back
  `EX_PushExecutionFlow`-guarded sequence that spawns
  **firefly → surveilanceMaster → tick → …** — i.e. the **whole ticker set is
  created in one block at gamemode BeginPlay**, unconditionally (no per-ticker
  gate).

**Conclusions for coop:**
- Tickers are **gamemode-spawned actors** (not level-placed `.umap` actors).
  There is typically **one instance of each** per world, persistent for the
  session.
- They are spawned via **`BeginDeferredActorSpawnFromClass`** — the SAME
  UFunction `npc_sync` intercepts. But the ticker classes are **not in
  `kNpcAllowlist`** (`sdk_profile.h:688-701`: zombie, kerfurOmega, krampus,
  funguy, goreSlither, insomniac, fossilhound, antibreather, orborb, 3×ariral).
  So `npc_sync`'s interceptor sees them, finds no allowlist match, and **lets
  them spawn on the client too**. That is correct for the actors (we WANT the
  ticker actor to exist so per-player tickers run) — the problem is purely
  what those actors DO each tick.

---

## 3. Full ticker categorization (all 28)

Per-ticker data from `bp_reflect.py` on each `ticker_*.uasset`
(`research/bp_reflection/ticker_*.json`): `TickInterval` from the CDO, spawn
UFunctions present in the NameMap, the non-engine `_C` classes it can spawn,
and the gate primitives present. **"npc_sync covers?"** = does the spawned
actor class fall under `kNpcAllowlist` (so the client already suppresses +
mirrors it)?

### (a) WORLD-AMBIENT SPAWNERS — should be HOST-AUTHORITATIVE

| ticker | tick (s) | spawns | via | gate | npc_sync covers? | notes |
|---|---|---|---|---|---|---|
| **ticker_fireflySpawner** | 30 | `eff_fireflies` particle | **SpawnEmitterAtLocation** | sun_height + RandomBoolWithWeight + grass line-trace | **NO** (particle, not actor) | **the reported bug**; cosmetic only |
| ticker_wispSpawner | 600 | `wisp_C`, `wisp_b_C` (+ `eff_wisp`) | BeginDeferredActorSpawnFromClass | RandomBool + RandomFloatInRange | NO | ambient wisp creatures |
| ticker_yellowWispSpawner | 240 | `killerwisp_C` (+ gib props) | BeginDeferredActorSpawnFromClass | LineTrace + RandomFloatInRange | NO | hostile wisp |
| ticker_deerSpawner | 300 | `deer_C` | BeginDeferredActorSpawnFromClass | RandomBool(+Weight) + RandomFloatInRange | NO | ambient deer |
| ticker_roachSummoner | 1 | (roaches via gamemode call) | — (calls into `mainGamemode_C`) | — | indirect | summons roaches through gamemode; needs deeper RE (see §3c) |
| ticker_mannequinSpawner | 5400 | `prop_C` mannequin (+comp_*) | BeginDeferredActorSpawnFromClass (+K2_DestroyActor) | RandomBoolWithWeight + RandomFloatInRange | NO (spawns `prop_C`, not an NPC class) | rare mannequin event |
| ticker_insomniacSpawner | 5 | `insomniac_C`, `insomniac_Child_C` | BeginDeferredActorSpawnFromClass | RandomBoolWithWeight | **YES** (`insomniac_C` allowlisted) | already suppressed+mirrored by npc_sync |
| ticker_fossilhoundSpawner | 600 | `fossilhound_C` | BeginDeferredActorSpawnFromClass | RandomBool(+Weight)+RandomFloatInRange | **YES** (`fossilhound_C`) | already covered |
| ticker_hexahiveSpawner | 3600 | `hexahiveSPawner_C` | BeginDeferredActorSpawnFromClass | RandomBoolWithWeight + RandomFloatInRange | NO | hive structure |
| ticker_beehiveSpawner | 1 | `beehiveBranch_C` (on foliage) | BeginDeferredActorSpawnFromClass | RandomFloatInRange | NO | also reads `InstancedFoliageActor`/HISM (header) |
| ticker_bp7Spawner | 600 | `NewBlueprint7_C` | BeginDeferredActorSpawnFromClass | — | NO | scripted/anomaly spawn |
| ticker_susHoleSpawner | 300 | `dirthole_C`, `susDirtHole_C` | BeginDeferredActorSpawnFromClass | LineTrace + RandomFloatInRange | NO | ground holes |
| ticker_treeSpawner | 600 | `walkingTree_C` | BeginDeferredActorSpawnFromClass | RandomFloatInRange | NO | walking-tree anomaly |
| ticker_bushSpawning | 300 | `growingPlant_C` | BeginDeferredActorSpawnFromClass | LineTrace + RandomFloatInRange | NO | also has `spawnBush()`/`ReceiveBeginPlay` |
| ticker_eyers | 7200 | `eyer_C` | BeginDeferredActorSpawnFromClass | RandomBoolWithWeight | NO | eye anomaly (rare) |
| ticker_tick | 60 | `NewBlueprint17_C` | BeginDeferredActorSpawnFromClass | — | NO | "tick" creature/anomaly |

### (b) PER-PLAYER / LOCAL-STATE tickers — MUST keep running locally

| ticker | reads | role | leave running on client? |
|---|---|---|---|
| ticker_foodTolerance | `saveSlot_C`, food | mutates the player's food tolerance stat | **YES** — local player state |
| ticker_foodSleeper | `mainPlayer`, food, RandomBoolWithWeight | sleep/food effect on the local player | **YES** |
| ticker_foodHall | food | food upkeep | **YES** |
| ticker_paused | `ui_menu_C`, pause | pause-state housekeeping | **YES** — purely local UI/pause |
| ticker_surveilanceMaster | `GetPlayerCameraManager`, camera/tv props | drives the player's camera/CCTV view; `intComs_settingsApplied` | **YES** — local rendering; also the only ticker the gamemode keeps a handle to (`mainGamemode.hpp:340 ticker_securityMaster`) |
| ticker_widgetRender | `WidgetComponent`, `analogDScreenTest_C`, `changeFramerate` | throttles widget/screen re-render | **YES** — local render perf |

### (c) UNCLEAR — needs more RE before touching

| ticker | why unclear |
|---|---|
| ticker_flickerer | reads `generator_C`/`transformerMGPanel_C`/`svtarget_C` + RandomBoolWithWeight — power-grid light flicker. Likely shared WORLD state (base power), but it may only toggle local light visibility. If it changes generator/breaker STATE it must be host-auth; if cosmetic-only it can run locally. RE its ubergraph before deciding. |
| ticker_serverBreaker | reads `daynightCycle_C`/`serverBox_C`/`svtarget_C` + RandomBoolWithWeight — randomly breaks a server box (shared world object). **Probably host-authoritative** (it damages a synced prop), but confirm what it writes. |
| ticker_disher / ticker_dishUncalib | read `dish_C`/`door_C`/`lightswitch_C`/`ceilingLamp_C` — the satellite-dish signal-reception loop + un-calibration. Mixed: signal RECEPTION (gameplay/score, host-auth candidate) vs local lamp/ambience. Needs RE; out of scope for the firefly fix. |
| ticker_kerf | 0 functions disassembled (no ReceiveTick/ubergraph in the dump) — inert or fully data-driven. Investigate separately. |
| ticker_roachSummoner | tick=1, no direct spawn fn; calls into `mainGamemode_C`. The actual roach spawn happens in the gamemode (likely `BeginDeferredActorSpawnFromClass` there). Roaches aren't allowlisted, so they'd double-spawn — but the suppression point is the gamemode path, not this ticker. Trace `mainGamemode` roach spawn before fixing. |

---

## 4. Recommended suppression mechanism

### 4.1 Principle (RULE 1 — root cause, not a firefly filter)

The desync class is "the client independently runs shared-world ambient
spawners." The correct architecture is the one the mod already uses for
weather: **the host is authoritative for ambient world content; the client is
a slave and does not run the spawners locally.** For fireflies specifically,
the visual is a transient cosmetic particle with no gameplay state — so the
**minimum-viable, correct fix is: don't run the firefly spawner on the
client** (do NOT try to mirror individual firefly emitters over the wire; that
is gold-plating a cosmetic). If we later want fireflies to appear on the client
at all, the host can broadcast spawn events — but parity ("neither side has
host-only fireflies") is achieved purely by suppression. Suppressing it on the
client makes both worlds match the host's authoritative ambient state.

### 4.2 The exact hook — PRE-intercept `ReceiveTick` on the client

Use the existing `ue_wrap::game_thread::RegisterInterceptor(targetUFunction,
cb)` (game_thread.h:105). The interceptor returns `true` to cancel the BP body.
This is **identical** to two shipping precedents:

- `weather_sync.cpp:252-259` `OnSchedulerPreSuppress` returns `true` to cancel
  the 5 weather scheduler UFunctions on the client.
- `garbage_sync.cpp:236-243` resolves `R::FindFunction(cls, L"ReceiveTick")`
  and registers a PRE-interceptor on a BP class's `ReceiveTick` — the exact
  call we need.

**For the firefly fix, hook this single UFunction on the CLIENT only:**

| ticker class | UFunction to intercept | resolve via | cancel by |
|---|---|---|---|
| `Aticker_fireflySpawner_C` | **`ReceiveTick`** | `R::FindFunction(R::FindClass(L"ticker_fireflySpawner_C"), L"ReceiveTick")` | interceptor returns `true` |

Why `ReceiveTick` and not the inner `SpawnEmitterAtLocation`:
- `ReceiveTick`'s body is *only* the ubergraph call (json:2912-2957) — cancelling
  it has **no collateral**; the firefly ticker does nothing else.
- `SpawnEmitterAtLocation` is a shared `UGameplayStatics` method used by many
  systems; intercepting it would need a `self`/params filter and risks
  suppressing unrelated emitters. Intercepting the **ticker's own
  `ReceiveTick`** is surgical (one class, one method) and matches the
  `garbage_sync` precedent. **Recommended: `ReceiveTick`.**

Host: register **nothing** (the host runs the spawner normally and is
authoritative). This is role-gated exactly like `weather_sync` (host registers
observers; client registers interceptors).

### 4.3 Extend to the other ambient spawners (scope-aware, RULE 5)

For the **non-allowlisted actor spawners** in §3a (deer, wisps, trees, holes,
bees, plants, hexahive, bp7, eyers, tick, mannequin, …), the SAME
`ReceiveTick` PRE-interceptor pattern applies per class — one interceptor slot
each, returning `true` on the client. UFunction is `ReceiveTick` for all of
them except the few that spawn from a named method:

| ticker class | UFunction to intercept on client | note |
|---|---|---|
| `Aticker_wispSpawner_C` | `ReceiveTick` | |
| `Aticker_yellowWispSpawner_C` | `ReceiveTick` | |
| `Aticker_deerSpawner_C` | `ReceiveTick` | |
| `Aticker_mannequinSpawner_C` | `ReceiveTick` | also calls K2_DestroyActor — fine to cancel whole tick |
| `Aticker_hexahiveSpawner_C` | `ReceiveTick` | |
| `Aticker_bp7Spawner_C` | `ReceiveTick` | |
| `Aticker_susHoleSpawner_C` | `ReceiveTick` | |
| `Aticker_treeSpawner_C` | `ReceiveTick` | |
| `Aticker_eyers_C` | `ReceiveTick` | |
| `Aticker_tick_C` | `ReceiveTick` | |
| `Aticker_beehiveSpawner_C` | **`Spawn`** (header has `Spawn()` + `ReceiveBeginPlay`, **no `ReceiveTick`**) | spawns from `Spawn()`; intercept that |
| `Aticker_bushSpawning_C` | **`spawnBush`** (header has `spawnBush()` + `ReceiveTick` + `ReceiveBeginPlay`) | safer to intercept `spawnBush` than the whole tick if the tick does other work; otherwise `ReceiveTick` |

**IMPORTANT — do NOT double-suppress the allowlisted ones.**
`Aticker_insomniacSpawner_C` (`insomniac_C`) and
`Aticker_fossilhoundSpawner_C` (`fossilhound_C`) spawn allowlisted NPC classes.
`npc_sync` ALREADY suppresses those spawns on the client at the
`BeginDeferredActorSpawnFromClass` interceptor AND mirrors the host's spawn
back. **Leave these two tickers running** and let `npc_sync` own them — adding
a `ReceiveTick` interceptor would stop the client from receiving the mirror set
up by npc_sync's bypass slot. (If you DID suppress the ticker, you'd also have
to suppress the host-side broadcast — net loss.) The clean rule: **a ticker is
suppressed by THIS mechanism only if its spawned class is not already handled
by npc_sync.**

### 4.4 Risks / caveats (VERIFY before extending past fireflies)

- **`ReceiveTick` may carry non-spawn work.** The firefly ticker does NOT
  (proven). For the others, confirm per-ticker that the tick has no
  client-needed side effect before cancelling the whole tick (e.g. a spawner
  that also updates a local HUD count). Where the spawn is isolated in a named
  method (`Spawn`, `spawnBush`), prefer intercepting THAT method. This is the
  per-site discipline RULE 4 demands — don't blanket-cancel ticks you haven't
  read.
- **Mannequin spawner also calls `K2_DestroyActor`** — on the client you want
  neither the spawn nor the destroy; cancelling `ReceiveTick` handles both, but
  verify the mannequin lifecycle isn't expected to be mirrored (it spawns a
  `prop_C`, which may be in scope for prop sync later — coordinate with
  prop_lifecycle before shipping).
- **Per-player tickers (§3b) must NOT be touched.** They mutate local player
  state (food/sleep/tolerance) and local rendering (surveillance camera, widget
  render). Suppressing them would break the client's own simulation.
- **Interceptor table budget.** `kMaxInterceptors = 16` (game_thread.h:98).
  npc_sync + garbage_sync(2) + weather_fog + weather causeRain already consume
  several slots. Suppressing all ~14 ambient spawners would blow the table.
  Either (a) bump `kMaxInterceptors`, or (b) — cleaner — add **one shared
  interceptor registered per ticker class** that dispatches on `self`'s class
  (a single callback, N registrations) the way npc_sync uses one callback for
  the allowlist. Recommend a dedicated `coop/ticker_sync.{h,cpp}` module that
  owns a class list + one callback (mirrors `garbage_sync`'s spawner table at
  `garbage_sync.cpp:108-206`). **Start with firefly only (1 slot)**; design the
  module to take a list so the rest are config, not code.

### 4.5 Recommended now vs later

- **NOW (fixes the reported bug, 1 interceptor slot, zero gameplay risk):**
  `Aticker_fireflySpawner_C::ReceiveTick`. Pure cosmetic, no state, no npc_sync
  overlap. Also cheap to verify (client should stop painting fireflies; host
  unchanged).
- **NOW-ish, low risk, same pattern (cosmetic/ambient, not allowlisted):**
  wisp, yellowWisp, deer, tree, susHole, bush, beehive — all create
  ambient world entities the host should own. Bundle into the `ticker_sync`
  module.
- **LATER / needs RE first:** mannequin (prop-sync overlap), bp7 / hexahive /
  eyers / tick (anomaly/event semantics — confirm they're not story-gated in a
  way that must stay per-peer), roachSummoner (suppress at the gamemode roach
  spawn, not the ticker), flickerer / serverBreaker / disher / dishUncalib
  (shared-world STATE — these likely need host-auth STATE SYNC, not mere
  suppression; out of scope here).
- **NEVER suppress:** insomniac/fossilhound tickers (owned by npc_sync), and all
  §3b per-player tickers.

---

## 5. Evidence index (file : line/offset)

- Firefly cadence `TickInterval=30.0`: `ticker_fireflySpawner.json:3176-3177`.
- Firefly `ReceiveTick`→`ExecuteUbergraph(10)`: `…json:2940-2957`.
- Firefly reads `daynightCycle.sun_height`: `…json:1228-1283`.
- Firefly weight math (FClamp·2 → 1- → ·4 → RandomBoolWithWeight): `…json:1287-1478`.
- GATE 1 RandomBool, GATE 2 LineTrace, GATE 3 PhysMat==grass: `…json:1481,2180,2476`.
- Position ring around PlayerCameraManager(0), dist 3000-10000, ang 0-360:
  `…json:1498-1841`.
- Spawn `eff_fireflies` via `SpawnEmitterAtLocation`: `…json:2732-2800`;
  imports `eff_fireflies` ParticleSystem `…json:4011-4019`, `SpawnEmitterAtLocation`
  Function `…json:3786-3793`, `grass` PhysicalMaterial `…json:4040-4045`.
- Header signatures: `CXXHeaderDump/ticker_fireflySpawner.hpp:1-12`,
  `ticker_base.hpp:1-31` (`GameMode` @0x0230).
- Gamemode spawns tickers via `BeginDeferredActorSpawnFromClass`+`FinishSpawningActor`
  at BeginPlay: `mainGamemode.json` ExecuteUbergraph stmts 1612-1625
  (firefly@1613, ref `-542`).
- NPC allowlist (insomniac/fossilhound in, ticker classes + deer/wisp/etc out):
  `src/votv-coop/include/ue_wrap/sdk_profile.h:688-701`.
- npc_sync intercepts `BeginDeferredActorSpawnFromClass`, allowlist-gated:
  `src/votv-coop/src/coop/npc_sync.cpp:315-491,742`.
- Suppression precedent — PRE-interceptor returns true to cancel BP body:
  `weather_sync.cpp:252-259,395-406`; `garbage_sync.cpp:236-251`.
- Interceptor API + `kMaxInterceptors=16`:
  `src/votv-coop/include/ue_wrap/game_thread.h:97-108`.
- No interceptor on `SpawnEmitterAtLocation` anywhere (grep: 0 hits in
  `src/votv-coop` outside third_party).

## 6. UNVERIFIED / open items

- Exact gamemode function that triggers the ticker-spawn block (it lives in
  `ExecuteUbergraph_mainGamemode`; the entry is almost certainly the BeginPlay
  path, but the specific `intComs_*`/event that jumps there was not traced to
  an offset). Not load-bearing for the fix — what matters (tickers are
  gamemode-spawned, one-per-world, via BeginDeferred) is proven.
- Whether any ticker is ALSO present as a level-placed actor in the `.umap`
  (the gamemode clearly spawns them; a belt-and-braces level placement was not
  checked). If both, suppression still holds (it's per-class on ReceiveTick).
- Per-ticker `ReceiveTick` side-effect audit for everything past firefly
  (flagged in §4.4) — required before extending suppression to each.
- `ticker_flickerer` / `ticker_serverBreaker` / `ticker_disher` /
  `ticker_dishUncalib` write semantics (cosmetic vs shared-state) — need their
  ubergraphs disassembled before deciding suppress-vs-state-sync.
- `ticker_roachSummoner` actual spawn path inside `mainGamemode_C`.
