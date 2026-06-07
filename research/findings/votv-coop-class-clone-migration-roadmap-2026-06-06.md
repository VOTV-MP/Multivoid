# Coop class-clone migration roadmap — fragile-BP-mirror recon (2026-06-06)

**Goal:** every gameplay mechanic we currently replicate by OBSERVING or FIELD-MIRRORING a
pure-Blueprint we don't own is fragile (the doors/keypad lesson). This recon disassembles each
candidate BP (`tools/bp_reflect.py` + `kismet-analyzer gen-cfg`) and judges whether it needs an
authoritative C++ "class clone" (own the state, drive the engine BP as a puppet — MTA "be the
engine"; the `RemotePlayer` / `coop::Door` / `coop::Keypad` shape). Method: 7 parallel per-mechanic
RE agents. **DONE already:** `coop::Door` (Phase A) + `coop::Keypad` (Phase B).

## Priority table

| # | Mechanic | BP | Current coop | Verdict | Priority | Effort |
|---|---|---|---|---|---|---|
| 1 | **Trash-clump DUP** | `prop_garbageClump` | owner-mirror + death-watch | **DONE (mirror notify-off)** | HIGH (user bug) | S |
| 2 | **Time-of-day / dark world** | `daynightCycle` | NOT replicated | **DONE (`coop::time_sync`)** | HIGH (visible bug) | S |
| 3 | **NPC / monster AI** | `npc_*` (ACharacter) | spawn ships, pose STUBBED | **`coop::Npc` (RemotePlayer twin)** | **HIGH** (scope) | **M** |
| 4 | **Grab / hold / throw** | `mainPlayer` + props | works; no arbitration | `coop::GrabState` + GrabRequest | MED | M |
| 5 | **Flashlight** | `mainPlayer` | 5-UFunction observe hedge | `coop::Flashlight` (Phase C) | MED | S–M |
| 6 | **Weather phenomena** | `daynightCycle` | 3-way observe/poll split | fold into `coop::WorldState` | MED | M |
| 7 | **Delivery drone** | `drone`/`droneConsole` | UNBUILT | `coop::Drone` (host singleton) | MED | L |
| — | **Lightswitch + container** | `lightswitch`/`prop_swinger` | symmetric mirror | **leave as-is (sufficient)** | LOW | — |

---

## 1. Trash-clump DUP — HIGH / S (root-caused) — the user's reported bug

**Mechanic:** `chipPile --E--> garbageClump (ball, held) --throw+land--> chipPile`. The clump->pile
spawn lives in the StaticMesh `OnComponentHit` delegate (ubergraph 2702, auto-bound at construction),
gated `canConvert` + slope/physics, doing `BeginDeferredActorSpawnFromClass(pile)` + `K2_DestroyActor`.
**DUP root cause:** the client's mirror clump (spawned via `BeginDeferred`->`FinishSpawning`) has that
hit handler AUTO-BOUND; on release we re-enable collision+physics+throw velocity
(`remote_prop.cpp:495-520`), so the mirror flies, lands ON THE CLIENT, and fires its OWN handler ->
spawns a local pile (#1) — while the host's death-watcher (`BroadcastLandedPileNear`) broadcasts the
authoritative pile (#2). Two piles. (Predicted in `votv-chippile-clump-morph-RE-2026-05-27.md` §6.6.)
**FIX (RULE 1):** at the mirror spawn (`remote_prop_spawn.cpp`, after `FinishSpawningActor` for a
garbageClump mirror) `StaticMesh.SetNotifyRigidBodyCollision(false)` — the mirror still lands visually
but never fires the convert; the host broadcast becomes the sole pile source. (`canConvert=false`@0x0248
does NOT work — the hit handler re-sets it true at entry.) Secondary: tighten `BroadcastLandedPileNear`
`FindNearestChipPile` 200cm->~30cm + age-check (drop-without-throw spurious dup). Class-clone later =
`coop::TrashClump` (explicit `ClumpSpawn`/`ClumpLand` events replacing the position/liveness heuristics).
**Caveat:** only verifiable HANDS-ON (physics landing; the static-host smoke hid it before).

## 2. Time-of-day / dark world — DONE 2026-06-07 (`coop::time_sync`) — the dark-client-world bug

> **IMPLEMENTED + SMOKE-VERIFIED (uncommitted).** `ue_wrap::daynightcycle` (resolve the cycle +
> ReadClock/ApplyClock the 3 floats) + `coop::time_sync` (host throttled 2s poll+broadcast + connect-
> snapshot; client direct-writes -> its ReceiveTick re-derives the sun). Protocol `TimeSyncPayload`
> (12B) / `TimeSync=29` / v36; event_feed routes it (host trust-gate + isfinite/range validation);
> net_pump wires Install/Tick/connect/disconnect. The smoke PROVED convergence: host clock
> `3156->3176`, client applied `3156->3184` (tracks within the 2s free-run window). `loadtime` turned
> out to be a pure setter (no fan-out), so direct field-writes were the clean drive. The WEATHER
> consolidation below (folding rain/fog/echo-suppress into a unified `coop::WorldState`) is STILL
> deferred — this shipped only the focused clock fix. NOTE: the cycle's `Day` field reads == `totalTime`
> (a clock-like float, not a day-number) -- synced correctly host->client regardless.

**Mechanic:** one actor `AdaynightCycle_C` owns the clock (`totalTime`@0x02B0, `Day`@0x0298,
`TimeScale`@0x02B4) + the weather scheduler; the sun is a pure function of `totalTime` via
`setSunAndMoonRotation`. **Gap:** the clock is NOT on the wire (no offset even resolved) — a fresh
joiner free-runs its own clock from day-0/night while the host is at midday -> the client world is dark.
**FIX:** host pushes `totalTime`+`Day`+`TimeScale` every N ms; client applies the native setter
**`loadtime(totalTime, Day)`** (the save-restore fn — no offset guessing) + free-runs between packets at
`TimeScale`; engine re-derives the sun. Ship as `coop::WorldState`. **Gotcha:** drive the CLOCK and let
the engine derive the sun — do NOT push `light_sun`/sky-curve fields (the purple-light lesson).

## 3. NPC / monster AI — HIGH / M — the entity half (user-picked scope)

**Mechanic:** every NPC is an `ACharacter` subclass; movement = native `AAIController` + `AIMoveTo` +
navmesh at a state-derived `MaxWalkSpeed` (`setSpeed`); the AnimBP reads CMC velocity (same as the
player). Target = `chaseActor`@0x0570; health@0x0564 / `isDead`@0x06C0; death spawns a separate
`ragdoll_zombie_C`. All AI/state is per-subclass BP. **Current:** spawn/despawn + connect-snapshot ship,
but the pose stream is a STUB -> mirrors are FROZEN; no health/target/death. `coop::element::Npc` is a
bare shell. **Class-clone `coop::Npc` = a `RemotePlayer` twin:** REUSE its interp members + `SetTargetPose`/
`Tick`/`AdvanceInterp` (incl. the advance-before-rebase fix) + the CMC-velocity drive (the mirror's own
AnimBP blends natively). Add a batched **`EntityPose`(=32) unreliable lane keyed by `elementId`**, folding
health/`isDead`/`chaseActor` in (vitals-piggyback shape). Host read-pass reads each bound NPC's
transform+velocity+state. **Gotchas:** keep `coop::Npc` opaque (orborb has no health/target — guarded
reflected reads); translate `chaseActor`->elementId; death->separate ragdoll; the dev-spawn POST gap
blocks autonomous verification; per-NPC targeting RE (nearest-of-all-players, not `GetPlayerPawn(0)`).

## 4. Grab / hold / throw — MED / M — works today; add arbitration

**Mechanic:** light grab = `UPhysicsHandleComponent` chasing a camera target; heavy = a
`UPhysicsConstraintComponent` triad; throw = inherited PhysX kinematic velocity at release (already
SOLVED). Grab verbs are BP-internal (bypass ProcessEvent). **Current:** `grab_observer` is OBSERVE-only;
the held-prop sync is the owner-driven PropPose stream (`net_pump`+`remote_prop`) — pose/drop/throw all
replicate. **Fragility MED:** the one genuinely-missing MTA piece is **ownership arbitration** (both peers
can grab the same prop -> dueling pose streams). Plus heavy-flag not replicated, Key-coupled identity.
**Class-clone `coop::GrabState`:** holder owns the held relationship + drives the puppet (relocate the
working code); add a reliable `GrabRequest->GrabGrant/Deny` (host holds `propOwner[eid]`); replicate the
heavy flag; standardize on `elementId`. **Gotcha:** don't regress the solved throw (velocity read AFTER
the re-sim toggle is load-bearing).

## 5. Flashlight — MED / S–M — Phase C (works today, biggest deletion)

**Mechanic:** state on `mainPlayer` (`flashlight`@0x0838/`hasFlashlight`/`crankFlashlight`/`flashlightMode`
+ per-player `light_R`@0x0678); battery is `saveSlot.battery` (0–100, **shared save state**).
`updateFlashlight`/`Flashlight Update` are BP-internal; only the input trampolines are observable (the
current "5-UFunction observe hedge" works by that fragile coincidence). **Class-clone `coop::Flashlight`:**
own on/mode/equipType, reimplement the disassembled constant table (intensity 0.2/0.3/0.1 × mode, cone,
IES) and drive `light_R` via the native light UFunctions; owner-authoritative (replicate the resolved
tap/hold INPUT). DELETE the 5-UFunction hedge + the latched-intensity guessing. **Design Q:** battery is
shared save state -> host-authoritative (like balance); a remote flashlight's empty-cutoff drives from the
host battery. **Gotcha:** `item_activate.cpp` is 793 LOC — land `coop/flashlight.{h,cpp}` fresh + gut it.

## 6. Weather phenomena — MED / M — fold into coop::WorldState

Fog is already the exemplar host-auth flag-poll (LOW). Lightning + redsky are fine discrete events
(observable spawn/toggle, LOW-MED) — MONITOR, leave unless they degrade. Rain is OBSERVE (MED, needs an
echo-suppress crutch). When `coop::WorldState` lands for #2, fold rain into a flag-poll (delete the
echo-suppress + 8 observers/5 suppressors) and absorb fog as a sub-component — retires the 3-way split.

## 7. Delivery drone — MED / L — unbuilt (8/10 prior probes now RESOLVED from disassembly)

**Mechanic (disassembly-confirmed):** `mainGamemode.drone` is a placed SINGLETON (never spawned per
delivery; its `container` is a keyed lookup). `flyingType` is a 3-state FLIGHT PHASE (-1 dormant / 0
outbound / 1 return), NOT a delivery selector (prior doc guessed wrong). Order ingest = `checkOrders()`
pops `saveSlot.orders[0]` -> `sendShop` -> `beginFly()`. Console call = `AdroneConsole_C::actionOptionIndex`
(ProcessEvent-dispatched, hookable) -> `triggerFly` (BP-internal gate: denies if a sack exists / radiotower
broken). Flight = a per-tick physics-FORCE integrator (`AddForce`/`AddTorque`/`VInterpTo` + global time
dilation) -> **must stream the transform, never the inputs.** Cargo (boxes on `compileOrder` at landing;
the dronesack on `dropSack` via player action) are all `Aprop_C` -> the EXISTING prop pipeline. Sell =
host-save economy. **Class-clone `coop::Drone` (host-auth singleton):** host owns the phase flags + runs
the integrator; client resolves the same singleton via `mainGamemode.drone`, SUPPRESSES its local drone
`ReceiveTick` (like garbage_sync), and drives the mirror from the stream. Wire: `DronePose` (unreliable
transform, only while `active`) + `DroneState` (reliable edges: active/flyingType/hasSack/dropSack/alarm,
host detects by PER-TICK FLAG POLL — never the BP verbs) + `DroneCallRequest` (client->host, hook
`actionOptionIndex`) + cargo via the prop pipeline (no packet) + **economy via the EXISTING balance_sync**
(host-side deduction -> no double-charge). **Gotchas:** stream transform (flight non-deterministic);
host-side order deduction; single-sack enforcement; keep the radar `radarPoint` registration when
suppressing the tick. Build on `votv-delivery-drone-RE-and-coop-sync-design-2026-06-03.md`. **2 residual
runtime-only probes:** is the laptop shop reachable on a client (decides if `OrderRequest` is needed) +
does the mirror's `radarPoint` self-register.

## — Lightswitch + container — LOW — leave as-is (verified sufficient)

`lightswitch.use()` is a verified BIDIRECTIONAL toggle (no one-way hazard) with no auto-revert; the
swinger's auto-close is a per-instance ONE-SHOT physics re-seat (`isLockable=true` lids only), not the
door's per-tick sensor re-drive — both self-converge under the symmetric mirror. No class-clone needed.
(Latent landmine: dead `CallSetActive` in `lightswitch.cpp` writes the power bool not `IsActive` — unused
today, would bite a future toucher.)

---

## Recommended order
1. **Trash-clump dup fix** (S, bug) — completes the user's original bug trio (purple/door/clump).
2. **Time-of-day sync** (S, bug) — `coop::WorldState` clock; fixes the dark world.
3. **`coop::Npc` pose stream** (M, scope) — the entity half; reuses `RemotePlayer`.
4. **Grab arbitration** + **`coop::Flashlight`** (MED) — robustness + the cleanest pattern demo.
5. **`coop::Drone`** + **weather consolidation** (MED/L) — new loop + retire the 3-way split.

Skip the door-pattern fold for lightswitch/container (no bug to fix).
