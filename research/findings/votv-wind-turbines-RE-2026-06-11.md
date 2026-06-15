# VOTV coop — GIANT WIND TURBINES (windmills) direction/spin RE + sync recipe — 2026-06-11

> **STATUS 2026-06-12: IMPLEMENTED as designed (same night)** -- TurbineState=49 (v61),
> ue_wrap/windturbine + coop/turbine_sync (PosKey identity, host 1 Hz epsilon-gated poll,
> raw-float client writes, connect snapshot). Smoke: 4 turbines indexed on both peers
> (start sub-levels). Hands-on convergence check pending.

User report: the giant map windmills FACE DIFFERENT DIRECTIONS on host vs client
(divergence persists WITH the v50 windTarget sync). This doc pins the turbine class,
its full driver (byte-grounded kismet disassembly), the divergence mechanism, and the
minimal mirror recipe.

**Method:** `tools/bp_reflect.py windturbine` → `research/bp_reflection/windturbine.json`
(+ `_fn.py` / `_cfg.py` disassembly; full ubergraph CFG saved at
`research/bp_reflection/_windturbine_uber_full.txt`), CXXHeaderDump for offsets,
repak + kismet-analyzer `to-json` of the placement sub-levels
(`research/bp_reflection/_map_untitled_*.json`), the prior wind RE
(`research/findings/votv-wind-event-driver-RE-2026-06-09.md`) + its CFG artifact
(`research/pak_re/cfg/directionalWind/directionalWind.txt`). All artifacts gitignored;
every claim cites file + byte/line offset.

---

## TL;DR

- Turbine = **`Awindturbine_C : Aactor_save_C`** (`CXXHeaderDump/windturbine.hpp`).
  Facing = **`headRotation@0x0300`** (float, WORLD yaw degrees), applied every tick by
  `Set Rots`: `axis_room.K2_SetWorldRotation(Yaw=headRotation)`. Blade spin =
  **`alpha_blades@0x02F8`** (accumulating pitch degrees on `axis_blades`).
- The driver is a **per-tick bang-bang servo (±1°/s)** chasing the **directionalWind
  ACTOR's forward vector** — and (new finding, absent from the 06-09 wind RE):
  **`directionalWind.ReceiveTick` sets its OWN actor yaw = yaw(windOffset.RelativeLocation)
  every tick** (`K2_SetActorRotation`, directionalWind.txt:579-580, byte 4655, Block @6287).
  So the turbines' attractor = the smoothed gust direction, which v50 ALREADY converges.
- `setRandRot` (per-instance RNG re-target, rand(-30..30)° every rand(5..30) s) is
  **DEAD CODE**: BeginPlay never arms it (the chain ends at `bladesMomentum := tot`),
  no external caller exists (gamemode + all turbine maps grepped: 0 refs), and even if
  called, the tick overwrites its `targetRot` next frame. **No client-side RNG to
  suppress** (unlike `changeWindOrigin`).
- Divergence WITH v50 = **initial-state + integrator drift**, not a live RNG: `rot`
  (the servo integrator @0x0340) is NOT save-persisted and has no per-instance default
  → resets to 0 on every world load; the head then chases from 0 at only ~1°/s through
  a very soft spring (stiffness `tot/2` ≈ 0.05–0.3, mass 100 → minutes to converge),
  while the host's `rot` is wherever ITS integration history left it. Plus, during calm
  (windOffset ≈ 0) the attractor yaw is numeric noise, so each peer's `rot` random-walks
  ±1°/s independently.
- **Recipe:** host polls each turbine at ~1 Hz, broadcasts a keyed 40 B payload
  (grime-style **quantized-position WireKey** — NOT the save Key: two placed turbines
  share one Key, §5), client **writes 3 floats (`headRotation`, `targetRot`, `rot`)
  + optionally 3 more (`alpha_blades`, `bladesMomentum`, `mult`) directly** — no
  UFunction calls, no suppression, no interp; the turbine's own tick IS the
  interpolator/applier. ~13 actors × 1 Hz × 40 B ≈ 0.5 KB/s. Cost ≈ sky_sync.

---

## 1. The class + field map (`CXXHeaderDump/windturbine.hpp`)

`Awindturbine_C : Aactor_save_C` (size 0x350). `Aactor_save_C` base contributes
**`Key` FName @0x0230** + `skipSave@0x0239` + `GameMode@0x0240`
(`CXXHeaderDump/actor_save.hpp:8-11`).

| field | offset | type | role (verified in §2) |
|---|---|---|---|
| `windArrow` | 0x0258 | UArrowComponent* | servo feedback vane — attached under `axis_room` (rotates with the head; SCS_Node_2 children list, windturbine.json) |
| `axis_blades` | 0x02B8 | USceneComponent* | blade spin pivot (relative pitch = `alpha_blades`) |
| `axis_motor` | 0x02C0 | USceneComponent* | inner motor axle visual (fast spin, `doRot`-gated) |
| `axis_room` | 0x02E8 | USceneComponent* | **THE NACELLE YAW PIVOT** — `K2_SetWorldRotation(Yaw=headRotation)`; parent of head/blades/arrow (`SCS_Node_2`) |
| `alpha_blades` | 0x02F8 | float | **blade spin phase** (degrees, unbounded accumulator) |
| `Speed` | 0x02FC | float | spin rate knob; CDO default **0.5**, no per-instance override |
| `headRotation` | 0x0300 | float | **THE FACING** (world yaw deg; spring output; SAVE-PERSISTED) |
| `headSpring` | 0x0304 | FFloatSpringState (0x8) | head spring state |
| `targetRot` | 0x030C | float | spring target (:= `rot` every tick) |
| `woosh` | 0x0310 | int32 | blade-crossing index for the swing sound |
| `subs` | 0x0318 | TArray<Awindturbine_C*> | per-instance cluster list (map-baked); functionally NEUTERED (§2.3) |
| `mult` | 0x0328 | float | per-instance spin multiplier, **BeginPlay RNG rand(0.9,1.0)** (CDO 1.0) |
| `doRot` | 0x032C | bool | LOD: player cam < 2500 uu (gates ONLY the inner `axis_motor` visual) |
| `doSpin` | 0x032D | bool | LOD: WasRecentlyRendered(0.2) (gates ONLY the blade visual apply) |
| `tot` | 0x0330 | float | wind energy := (windStrength_total + windSpeed_total)/10, per tick |
| `bladesMomentum` | 0x0334 | float | blades spring output toward `tot` |
| `rot` | 0x0340 | float | **THE SERVO INTEGRATOR** (unbounded deg; NOT persisted, NOT instance-overridden → 0 at every load) |
| `prevRot` | 0x0344 | FVector | wind fwd snapshot — **WRITE-ONLY/dead** (2 JSON refs = the one EX_Let) |

Component hierarchy (SCS nodes, windturbine.json exports 42-64):
`DefaultSceneRoot → base (tower) → axis_room → { head, axis_blades→blades,
axis_motor→motorIn+motor, axis_motor1→motorDoor, windArrow, ladders/indoors/woosh }`.
**`windArrow` under `axis_room`** = the chase has closed-loop feedback (the arrow turns
with the applied yaw).

Not the windmills: `Aprop_wireComponent_turbine_C` (prop_wireComponent_turbine.hpp) is
the power-wire turbine attachment; `windturbineBendy_shaft` (untitled_216 name table)
is a bent-wreck component of some other decorative actor.

## 2. The driver, byte-grounded (`_windturbine_uber_full.txt`)

Entry points: `ReceiveBeginPlay → uber @2030`, `ReceiveTick → @2408`,
`setRandRot → @2174`, `rotats → @2833`, BndEvt overlaps → @3103/@3118 (both call the
**EMPTY** `setCollide` — dead), Delay-resume → @1574 (EX_SkipOffsetConst Value=1574 in
the latent FLatentActionInfo).

### 2.1 ReceiveTick (every frame, EVERY turbine, ungated) — the whole live driver

```
@2408: tot := (gamemode.directionalWind.windStrength_total + .windSpeed_total) / 10      // bytes 2408-2610
       alpha_blades += dt * 360 * Speed * mult * bladesMomentum                          // 2611-2827 (VictoryFloatPlusEquals)
       JUMP @15
@15:   setTargetRots(0)            // param IGNORED -> self.targetRot := self.rot (§2.3) // byte 15
       windFwd  = gamemode.directionalWind.GetActorForwardVector()                       // 54
       cross    = Cross(windFwd, (0,0,1))          // wind right-axis (EX_VectorConst Z=1)// 126
       dot      = Dot(cross, windArrow.GetForwardVector())                                // 176-271
       rot     += 1.0 * SignOfFloat(dot) * dt * 0.005556 * 180     // = ±1.0 deg/sec      // 272-517
       prevRot  = windFwd                                          // dead store          // 518-616
@618:  headRotation := FloatSpringInterp(headRotation, targetRot, headSpring,
                          stiffness = tot/2, damping 0.05, dt, mass 100)                  // 618-798
@800:  Set Rots()                                                                         // 800
@815:  woosh: when floor((alpha_blades-30)/360*3) % 3 != woosh -> update + SpawnSoundAtLocation(
         swingWindmill_Cue @ spawnAudioWoosh, vol 0.75, att_far)   // blade-pass swoosh   // 815-1433
@1434: bladesMomentum := FloatSpringInterp(bladesMomentum, tot, bladesSpring,
                          stiffness = tot, damping 0.1, dt, mass 1000)                    // 1434-1573
```

`Set Rots` (function, 497 B):
```
@25  [doSpin]  axis_blades.K2_SetRelativeRotation(Pitch = alpha_blades)        // blade visual
@135 [doRot]   axis_motor.K2_AddLocalRotation(Pitch += dt*360*Speed*30)        // inner axle visual
@394 [ALWAYS]  axis_room.K2_SetWorldRotation(Rotator(0,0, Yaw=headRotation))   // THE FACING APPLY
```
The facing apply is UNGATED — far/unrendered turbines still chase + apply. The LOD
bools gate only cosmetic sub-spins.

### 2.2 ReceiveBeginPlay — arms NO RNG re-targeter

```
@2030: mult := RandomFloatInRange(0.9, 1.0)        // per-instance, PER-PEER spin multiplier (±10%)
       JUMP @1887: BIND rotats; K2_SetTimerDelegate(rotats, 1.0, looping=True); base.SetMobility(Static)
       @2105: setCollide() /*empty*/; Delay(0.2) -> resume @1574
@1574: bladesMomentum := tot                        // startup spring snap. END of BeginPlay.
```
**`setRandRot` is never armed.** Its body (@2174) would: re-arm itself every
rand(5,30) s, set `targetRot := headRotation + rand(-30,30)`, and push
`subs[i].setTargetRots(targetRot)` (@2002→@1671→@1778 loop). But no first call exists:
0 references in mainGamemode.json, 0 in every turbine-bearing map JSON
(`grep setRandRot research/bp_reflection/_map_*.json` + mainGamemode.json → none), and
the in-class BIND is inside its own unreachable body. Even if something DID call it,
the next tick's `@15 setTargetRots(0)` overwrites `targetRot` before the spring ever
consumes the random value (tick order @15 → @618 within one frame). Dead twice over.

`rotats` (1 Hz timer, @2833): `doRot := dist(cam, axis_motor1) < 2500;
doSpin := WasRecentlyRendered(0.2)` — pure per-peer LOD, cosmetic gates only.

### 2.3 The `setTargetRots` parameter bug (why `subs` clusters are neutered)

`setTargetRots(float targetRot)` body — raw bytecode (windturbine.json):
`EX_Let dest=EX_InstanceVariable[targetRot] src=EX_InstanceVariable[rot]` — BOTH
instance variables; the float PARAM is never read (same BP-authoring bug family as
directionalWind.setParameters reading instance `intensity`). So a master pushing
`setTargetRots(X)` to its `subs` merely makes each sub do `sub.targetRot := sub.rot` —
which its own tick does anyway. The map-designed "cluster faces together" feature
(untitled_174's windturbine_2 has `subs=[windturbine3, windturbine2]`; untitled_178's
windturbine2 has `subs=[windturbine_2]`) is a no-op. **Ignore subs for sync.**

### 2.4 Save persistence (`getData`/`loadData`, _fn.py dumps)

- `getData`: stores **`[alpha_blades, headRotation]`** (floats_7 array inside
  data.floats_31[0]).
- `loadData`: restores `alpha_blades := f[0]; headRotation := f[1]; Set Rots()`.
- `gatherDataFromKey`: `gather=true, loadTransform=false` → keyed save participation,
  transform never loaded (turbines are immovable placed actors).
- **`rot` and `targetRot` are NOT persisted** → both reset to 0 (CDO has no override;
  placed instances carry no rot/headRotation/Speed/mult overrides — verified across all
  7 map dumps, §5) on every world load. First tick after load: `targetRot := rot = 0`
  → the head springs from the loaded `headRotation` toward 0, then re-chases the wind.
  (Happens identically in SP — masked there by having no second world to compare.)

## 3. The attractor: directionalWind's ACTOR YAW (new finding)

`directionalWind.ReceiveTick` (Block @6287, `research/pak_re/cfg/directionalWind/
directionalWind.txt`) — bytes 4165-4439 (txt:520-560) + 4441-4673 (txt:561-580):

```
yaw = BreakRotator(FindLookAtRotation((0,0,0), windOffset.RelativeLocation)).Yaw
eff_wind.K2_SetWorldRotation(0,0,yaw)        // txt:537-548
strength.K2_SetWorldRotation(0,0,yaw)        // txt:549-560
self.K2_SetActorRotation(Rotator(0,0,yaw))   // txt:579-580, byte 4655  <- THE ACTOR YAW
```

So `GetActorForwardVector()` (what every turbine chases) = the unit direction of
**`windOffset.RelativeLocation`** — the SPRING OUTPUT whose input is `windTarget`,
which **v50 already syncs** (host streams windTarget + client `changeWindOrigin`
suppressed — weather_sync.cpp / ue_wrap/directionalwind.h:46-53). Client windOffset
converges to the host's within the spring settle (~0.5 s). **The turbines' attractor
is therefore already (approximately) shared.** Two residual attractor mismatches:
1. **Calm-period noise:** between gusts the second spring pull (wind RE doc Q3,
   txt:442) decays windOffset → ~0; `FindLookAtRotation(0, ~0-vector)` yaw is numeric
   noise and flips erratically — independently per peer (each runs its own spring on
   its own dt). The turbines integrate `sign()` of that → uncorrelated ±1°/s walk
   while calm.
2. Sub-second spring transients after each windTarget update — negligible.

## 4. Why the user sees divergence GIVEN v50 (the answer to Q2)

Ranked, all from §2/§3 mechanics:

1. **Integrator reset at load + slow servo.** `rot` = 0 on every world load (§2.4)
   while the host's `rot` has integrated for its whole uptime. The chase rate is
   **1.0°/s** flat (bang-bang, @481) and the APPLIED yaw lags further behind through
   FloatSpringInterp with stiffness `tot/2` (typical 0.05–0.3) and mass 100 — a
   minutes-scale settle. A joiner's turbines can sit ~tens of degrees off for many
   minutes even with identical wind. The v56 save-transfer narrows `headRotation` to
   the host's at-SAVE-time value, but `rot`/`targetRot` still reset → the head first
   pulls back toward 0, then re-chases — visibly wrong vs the host's current facing.
2. **Calm-period random walk** (§3.1): independent ±1°/s drift accumulates whenever
   windOffset ≈ 0; never actively corrected between peers (the servo only re-converges
   them when both straddle the attractor — bang-bang does not contract same-side error:
   both move at the same ±1°/s, freezing their difference).
3. **Pre-v50 history**: any divergence accumulated before the v50 deploy persists in
   `headRotation` via the save (it IS persisted) — the servo erodes it only at the
   1°/s-when-straddling rate above.
4. (Cosmetic) **`mult` = rand(0.9,1.0) per peer per turbine** (§2.2) → blade SPIN RATE
   differs up to ~11%; `alpha_blades` phase arbitrary. Direction-irrelevant.

NOT causes: `setRandRot` (dead, §2.2), `subs` (neutered, §2.3), BeginPlay-only wind
sampling (none — the chase reads the wind actor EVERY tick), doRot/doSpin (visual-only
gates; the yaw apply is ungated).

## 5. Identity (Q3) — keyed, with ONE baked collision; recommend position keys

Every placed turbine has a per-instance **`key` FName baked in the umap** (read at
runtime via `Aactor_save_C::Key @0x0230`, same field the ATV/trash-pile syncs use).
Census from the map dumps (`_map_untitled_*.json`; placed-actor exports of class
windturbine_C):

| sub-level | count | instances (key) |
|---|---|---|
| untitled_1 | 4 | scaletest_2 (5aeh4sOqB6XXkSUUdi8RNQ), scaletest2_18 (GvPPt3rzrrXrmwIa2bslZw), scaletest3_20 (qOAQZ2BskbY6wSOLW8Unkw), scaletest4_22 (1iXf8mqaLEMJQ99hTeT8TQ) |
| untitled_174 | 3 | windturbine2 (BQA3y1Mi74ub9L-JmVDrhg), windturbine3 (PhSQk9YdRUGfghUbfeoVqQ), windturbine_2 (qQctLUCASn2fs_nRvFR0kg, subs=[the other two]) |
| untitled_178 | 2 | windturbine2 + windturbine_2 — **BOTH key = kSrQO9OzjT5bplRKWn-B1g** (one name-table entry referenced by two exports — a genuine shared key; a Key-only index collapses them and one of the pair would never sync) |
| untitled_180 | 1 | windturbine_2 (JFGR6z4VTtVeWN5dW-LzQg) |
| untitled_189 | 1 | windturbine_5 (p6Ab5n3CBILt6mcGmnwqkw) |
| untitled_211 | 1 | windturbine_2 (mgBY4_0xAnTbIM95xrNlEQ) |
| untitled_216 | ~1 | kismet-analyzer JSON parse fails; raw name table shows one placed set (+ the decorative windturbineBendy wreck) — count UNVERIFIED, ≥1 |

**Total ≈ 13.** They never move (`SetMobility(Static)` at BeginPlay @1963;
`loadTransform=false`). → **Use the grime-style QUANTIZED-WORLD-POSITION WireKey**
(KeyedScalarPayload precedent, protocol.h:1573-1585: "a QUANTIZED WORLD-POSITION
string for the static, unkeyed grime decals"). That sidesteps the untitled_178 shared
Key cleanly; position is unique and immutable. (Compound Key+pos works too — more code
for no benefit.)

## 6. The sync recipe (Q4)

**Shape: sky_sync cadence + ATV-style keyed index — but NO pose-stream machinery.**
Read both before building: `src/votv-coop/src/coop/sky_sync.cpp` (1 Hz host-auth
broadcast, ~100 LOC) and `src/votv-coop/src/coop/atv_sync.cpp`. Explicitly **recommend
AGAINST the ATV channel shape** for turbines: its 20 Hz occupant stream
(atv_sync.cpp:46 `kSendIntervalMs=50`), LerpWindow interp (:54, :109-149) and
PrepareMirror physics/tick-off (:229) exist because the ATV mirror's own sim must be
killed and the wire is the only animator. The turbine is the opposite: **its BP tick
must KEEP running** (it applies the yaw, spins the blades, springs everything); the
wire only has to pin 3 spring/integrator inputs. No interp (the head spring IS the
interp), no mirror-prepare, no suppression interceptor (nothing RNG-driven is alive,
§2.2 — the changeWindOrigin precedent does NOT recur here).

Concretely:

1. **Payload** (new `ReliableKind::TurbineState = 49`, next free after ChatMessage=48,
   protocol.h:1102; bump kProtocolVersion 60→61):
   ```
   struct TurbineStatePayload {        // 56 B, <= 228 B reliable datagram budget
       WireKey key;                    // 32 -- quantized actor world position (grime style)
       float   headRotation;           // @0x0300  the facing (world yaw deg)
       float   targetRot;              // @0x030C
       float   rot;                    // @0x0340  the integrator (unbounded deg -- send raw)
       float   alphaBlades;            // @0x02F8  blade phase (optional-but-cheap)
       float   bladesMomentum;         // @0x0334  (snaps spin rate at join)
       float   mult;                   // @0x0328  kill the per-peer rand(0.9,1.0) rate skew
   };
   ```
2. **Host** (Tick, throttled to ~1 Hz like sky_sync.cpp:29 kPushInterval): walk a
   cached index (ATV-style RebuildIndex on a 2 s throttle + g_installed latch,
   atv_sync.cpp:159-209 — turbines never spawn/despawn, so the walk is effectively
   one-time; keep the throttle for level-streaming robustness), read the 6 floats per
   turbine (plain field reads at the §1 offsets), `SendReliable` one payload per
   turbine per second. 13 × 56 B/s ≈ 0.7 KB/s. Optionally only-on-change (>0.5° delta)
   — saves nothing that matters at this rate.
3. **Client** (`OnReliable`): resolve by key from its own index;
   `IsLiveByIndex` guard; **write the 6 floats directly** (no engine calls — every one
   is consumed as a plain value by the BP's own tick: FloatSpringInterp reads
   headRotation/targetRot @618, VictoryFloatPlusEquals mutates rot in place @481,
   Set Rots applies @394/@25 next frame). Host-authoritative: client never sends,
   host never applies (sky_sync.cpp:56 role guard).
4. **Connect snapshot**: `QueueConnectBroadcastForSlot` sends the same payloads once
   at join (atv_sync.cpp:233 shape) → the joiner's heads snap to the host's CURRENT
   facing (fixing §4.1 instantly; the spring eats the visual snap — at stiffness
   tot/2 it's a smooth swing, and an exact write of all three of
   headRotation+targetRot+rot means there is no pull-back-to-0 at all).
5. **No suppression needed.** Between 1 Hz packets the client's own servo drifts
   ≤1° (the chase rate cap, §2.1) before the next correction overwrites it.
   `headSpring`/`bladesSpring` states are NOT synced (8 B each; low-pass transients —
   same verdict as the wind RE doc's spring-state decision).
6. **OnDisconnect**: clear the index + latch (atv_sync.cpp:293). Nothing to restore —
   we never disabled anything; SP behavior resumes untouched (principle 6).

**Hot-path cost:** zero per-frame work. 1 Hz × 13 plain-field reads (host) / writes
(client) + one 2 s-throttled GUObjectArray walk shared with the existing rebuild
pattern. No per-ProcessEvent observer, no UFunction hook, no interceptor slot
(weather RE noted the interceptor table at 15/16 — this recipe consumes none).

## 7. Unknowns / UNVERIFIED

- untitled_216's exact turbine count (JSON parse failure; ≥1 from the raw name table).
  Runtime confirm: the index log will print the live count (ATV keysHash precedent).
- Whether the untitled_* sub-levels ever stream OUT at runtime (turbines unloading
  far away). The 2 s rebuild + per-payload IsLive guard handles either answer; if they
  DO stream, a re-loaded turbine resets rot:=0 and the next 1 Hz packet re-pins it.
- FFloatSpringState internal layout (assumed 2 floats; we don't touch it).
- The exact visual meaning of `axis_room` yaw 0 (which world direction the nacelle
  faces at headRotation=0) — irrelevant to mirroring (we copy absolute values).
- `Speed`(hpp)/`speed`(CDO) case differs (FName case-insensitivity); CDO value 0.5
  confirmed; no map override found.
