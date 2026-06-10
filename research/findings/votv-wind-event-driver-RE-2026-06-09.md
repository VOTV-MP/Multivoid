# VOTV coop — WIND-EVENT DRIVER RE (what energizes the spring) — 2026-06-09

Follow-up to `research/findings/votv-wind-basefog-RE-2026-06-08.md`, which left a
GAP: it claimed `setParameters` "has no caller" and never disassembled
`doWind / updateVars / updState / changeWindOrigin / setWindLoc /
timer_spawnLeaves / ReceiveBeginPlay`. This doc fills that gap from the Kismet
bytecode (VOTV is stock non-nativized UE4.27 Shipping — all BP logic is bytecode,
IDA-invisible). Every claim cites the function + byte offset.

**Artifacts reused (gitignored, no re-dump needed):**
`research/pak_re/cfg/directionalWind/directionalWind.txt` (gen-cfg, offset-aware),
`research/bp_reflection/directionalWind.json`. Layout cross-checked against
`CXXHeaderDump/{directionalWind,Engine}.hpp`.

---

## TL;DR (the divergence source)

The wind is energized **per-peer by RNG timers**, not by any synced field. In
`ReceiveBeginPlay` the actor arms a **`changeWindOrigin` timer with a RANDOM
interval `RandomFloatInRange(1, 60)` s** (non-looping, re-arms itself each fire)
that, every fire, **re-rolls `windTarget`'s RelativeLocation** to
`RandomPointInBoundingBox(0,0,0 .. 150,150,150) * Ease(0,1, RandomFloat(), exp=Lerp(4,2,rain))`.
`ReceiveTick` then springs `windOffset` toward `windTarget` and derives
`intensity = FMax(VSize(windOffset.RelativeLocation) − 5, 0) / 50`. **`intensity`
is the entire visible/audible/physics wind**, and it is driven ONLY by
`windTarget`, which each peer rolls independently with its own RNG stream and its
own random timer cadence. So at any instant the host is mid-gust (windTarget far
from origin → high intensity → leaves shake) while the client is between gusts
(windTarget near origin → intensity ≈ 0 → calm leaves). **The 4 fields the mod
syncs today (`windSpeed/Strength_rain/_background`) do not contain `intensity`
nor `windTarget`, and `ReceiveTick` overwrites `windStrength_background =
intensity` every frame — so the current v43 wind sync cannot fix this.** The
minimal fix is to sync **`windTarget.RelativeLocation` (Vec3 @ component+0x011C)**
host→client and **suppress the client's `changeWindOrigin`** (the only writer of
`windTarget`) so its local RNG roll stops fighting the synced target.

---

## Q1 — What energizes the wind? (caller chain to windTarget / spring / intensity)

### Writers of `windTarget.RelativeLocation` — there are exactly TWO, both reached via the RNG block @393

**`setParameters(Intensity, Angle, Speed, Strength)`** — *(function body, txt:1557)*
writes windTarget, BUT confirmed it has **no in-class caller** (it is an
externally-invokable event; not on any timer). Body (block @10, txt:1588):
```
loc = RotateAngleAxis( MakeVector(intensity*50, 0, 0), angle, +Z )   // reads INSTANCE intensity@0x02D0, not the arg
windTarget.K2_SetRelativeLocation(loc)        // txt:1605
windOffset.K2_SetRelativeLocation(loc)        // txt:1612
ResetVectorSpringState(spring@0x02B8)         // txt:1619
windSpeed_background    = speed   (arg)       // block @282, txt:1572
windStrength_background = strength (arg)
```
Not the normal-play energizer (no caller). *(Note: the body reads the instance
field `intensity`, NOT the `Intensity` arg — UNVERIFIED whether intentional, but
irrelevant since it's never called.)*

**The ACTUAL energizer = the RNG block `ExecuteUbergraph_directionalWind @393`**,
which is the body of **`changeWindOrigin`** *(and is also run once at BeginPlay)*.
`changeWindOrigin` → ubergraph @5921 *(txt:880)*: `PushExecutionFlow 393`. Block
@393 *(txt:52)*:
```
@393:
  RandomFloatInRange(1, 60) -> CallFunc_RandomFloatInRange_ReturnValue   // txt:57-59
  BindDelegate changeWindOrigin                                          // txt:60  (RE-ARMS ITSELF)
  K2_SetTimerDelegate(changeWindOrigin, RandomFloatInRange, looping=False)// txt:65-70
  BindDelegate setWindLoc                                                // txt:71
  K2_SetTimerDelegate(setWindLoc, 3.0, looping=True)                     // txt:76-81
  RandomPointInBoundingBox({0,0,0}, {150,150,150}) -> rndPt              // txt:84-86
  RandomFloat() -> rf                                                    // txt:88-89
  Lerp(4, 2, gamemode.daynightCycle.rain) -> exp                        // txt:91-99
  Ease(A=0, B=1, Alpha=rf, EaseFunc=5/*EaseInOut*/, BlendExp=exp, Steps=2) -> e // txt:101-108
  Multiply_VectorFloat(rndPt, e) -> tgt                                  // txt:110-113
  windTarget.K2_SetRelativeLocation(tgt)                                 // txt:114-117
  PopExecutionFlow
```
So **`windTarget`'s displacement-from-origin is a fresh random vector** (magnitude
up to `|RandomPointInBoundingBox(150³)| * Ease(0..1)` ≈ up to ~260 units),
re-rolled every `RandomFloatInRange(1,60)` seconds by a self-re-arming timer. The
spring then converts that displacement into `intensity` (Q3/Q5). **This is what
"energizes the wind."** RNG sources: `RandomFloatInRange`, `RandomPointInBoundingBox`,
`RandomFloat`. Cross-peer dependency: also reads `daynightCycle.rain` (the Ease
exponent), itself per-peer.

### The other timer'd functions — NOT energizers (collateral map for Q4)

- **`setWindLoc`** *(ubergraph @6277, txt:857)*: only
  `Sphere.K2_SetWorldLocation( GetPlayerCameraManager(0).GetActorLocation() )`.
  Keeps the overlap `Sphere` on the local camera. **No RNG, never touches
  windTarget/spring/intensity.** (3 s looping; armed in @393.)
- **`timer_spawnLeaves`** *(ubergraph @6282, txt:784)*: 20 s looping timer (armed
  in BeginPlay @922). Computes a probability from the rain wind fields, then
  `RandomBoolWithWeight(p)` → if true iterates `gamemode.autumnLeafSpawner[]` and
  calls `spawnLeaves` on each. **Spawns AUTUMN-LEAF PARTICLES only; reads wind
  fields, WRITES NOTHING to wind state.** (RNG-driven, cosmetic.)
- **`updateDirWind`** *(ubergraph @6624, txt:371)*: 1 s looping (armed in
  BeginPlay @937). Pushes the engine source:
  `windSource.Component.SetSpeed/SetStrength = (windSpeed_total*intensity*windStrength_total)/2`.
  Consumes intensity; does not create it.
- **`doWind`** *(standalone function, txt:1088)*: iterates the `objs` overlap
  array, applies wind force to nearby physics bodies / characters
  (`CharacterMovement.AddForce`, line-traces, reads `winds[]`,
  `windSource.Component.Speed/Strength`, `intensity`, `direction`). **Pushes
  loose objects; does NOT write windTarget/spring/intensity.** *(No caller found
  in this CFG — UNVERIFIED who invokes `doWind`; likely another timer/tick not in
  this dump. Irrelevant to the divergence: it consumes wind, doesn't create it.)*
- **`updateVars`** *(txt:1517)*: `lib.updateCollision(Sphere,false,self)`. Collision
  refresh. Not wind.
- **`updState`** *(txt:1530)*: gates tick/collision/hidden + `eff_wind.SetActive` +
  `Audio.SetActive` on `tickable`. Visibility/enable only.

### `ReceiveBeginPlay` wiring (the timer arm-up), byte-grounded

`ReceiveBeginPlay` → ubergraph @7019 *(txt:987)* → `Jump 30`. Flow:
```
@30  (txt:148): gamemode = lib.getMainGamemode(); if IsValid(gamemode) -> @138 else @338(Delay+retry)
@138 (txt:25):  PushExecutionFlow 937 ; fall @143
@143 (txt:947): PushExecutionFlow 922 ; fall @148
@148 (txt:962): PushExecutionFlow 393 ; fall @153
@153 (txt:967): windSource = Cast<WindDirectionalSource>(windActor.ChildActor); if ok -> @250
@250 (txt:123): updState(); Delay(0.2) -> @15
@15  (txt:139): updateVars(); PopExecutionFlow            -> pops @393  (arms changeWindOrigin+setWindLoc, sets initial windTarget)
@393:           ... (above) ... PopExecutionFlow          -> pops @922
@922 (txt:46):  timer_spawnLeaves(); PopExecutionFlow     -> pops @937
@937 (txt:30):  K2_SetTimerDelegate(updateDirWind, 1.0, looping=True); PopExecutionFlow
```
So at BeginPlay the actor: sets an **initial random `windTarget`**, **arms
`changeWindOrigin` (random 1–60 s, self-re-arming)**, arms `setWindLoc` (3 s) and
`updateDirWind` (1 s), and kicks `timer_spawnLeaves` (20 s). The wind-event driver
is the `changeWindOrigin` chain.

---

## Q2 — Is the driver a per-peer RNG timer? — **CONFIRMED**

Yes. `changeWindOrigin` is bound as a timer with interval `RandomFloatInRange(1,60)`
(`K2_SetTimerDelegate(..., looping=False)`, txt:65) and **re-arms itself with a
fresh random interval on every fire** (the `BindDelegate changeWindOrigin` +
`K2_SetTimerDelegate` are inside its own body @393, txt:60-70). Each fire reads
**three independent RNG calls** (`RandomFloatInRange`, `RandomPointInBoundingBox`,
`RandomFloat`, txt:57/84/88) plus the per-peer `daynightCycle.rain` (txt:99). It
free-runs on each peer with an uncorrelated RNG stream and an uncorrelated random
cadence → the two peers' `windTarget` trajectories diverge immediately and
permanently. **This is the divergence** (host gusty while client calm, and
vice-versa, at any given moment). Confirms candidate (a) from the prior doc
("the client's directionalWind ticks independently and diverges") and pins the
mechanism to `changeWindOrigin`'s RNG, not the rain pair.

---

## Q3 — Minimal divergent state to sync host→client

`ReceiveTick @6287` *(txt:418)* derives everything from `windTarget` via the spring:
```
windOffset.Rel = VectorSpringInterp(windOffset.Rel, windTarget.Rel, spring, 0.25, 0.1, dt, 500)  // txt:421
windOffset.Rel = VectorSpringInterp(windOffset.Rel, {0,0,0},        spring, 0.25, 0.1, dt, 500)  // txt:442  (damped pull to origin)
intensity = FMax( VSize(windOffset.Rel) - 5, 0 ) / 50                                             // txt:462-485
windStrength_background = intensity                                                               // txt:487  (OVERWRITES the synced field every frame)
windSpeed_total    = windSpeed_rain + windSpeed_background                                        // txt:490-497
windStrength_total = windStrength_rain + windStrength_background                                  // txt:498-505
```

**Verdict on candidates:**

- **(a) Sync `windTarget.RelativeLocation` (Vec3) — YES, this is the minimal fix.**
  Writing `windTarget.Rel` on the client makes the client's own `ReceiveTick`
  spring `windOffset` toward the same target → reproduce the host's `intensity`
  within the spring's settling time (a few frames; stiffness 0.25, damping 0.1,
  mass 500). The spring (`spring@0x02B8`) is a low-pass filter of windTarget, so
  exact spring-state sync is **not required for parity** — the client converges to
  the host's intensity as long as windTarget matches. **Calling
  `ResetVectorSpringState(spring)` on apply is OPTIONAL**: only useful to make a
  *late joiner* snap instantly instead of easing in over ~0.5 s. Recommend NOT
  resetting on the steady-state stream (it would jolt the vane); optionally reset
  once on the connect-snapshot apply.
  - *windTarget.RelativeLocation reachability*: `windTarget` is a
    `UBillboardComponent*` @ `directionalWind+0x0238`; `RelativeLocation` is at
    `USceneComponent+0x011C` (size 0xC) — a **fixed offset**, so the host read /
    client write is a plain `*(FVector*)((*(uint8**)(wind+0x0238)) + 0x011C)` (no
    reflection call needed). *(Writing the field directly does not mark the
    component transform dirty, but `ReceiveTick`'s very next `VectorSpringInterp`
    reads `windTarget.RelativeLocation` as a plain value (txt:427-429) and
    `K2_SetRelativeLocation`s `windOffset` from it — so the spring picks up the new
    target without us needing K2_SetRelativeLocation on windTarget. UNVERIFIED edge:
    if anything else reads windTarget's *world* transform, a direct field poke
    wouldn't refresh ComponentToWorld; nothing in this actor does — only the spring
    reads `.RelativeLocation` directly.)*

- **(b) Sync `intensity@0x02D0` (+ windLocation + spring) directly — works but is
  NOT minimal and is fragile.** `intensity` is **recomputed from scratch every
  tick** (txt:484, `intensity = …/50`), so a written value is clobbered on the
  next frame unless you ALSO freeze the spring/windOffset — i.e. you'd have to sync
  windOffset.Rel + spring state + suppress the spring, which is strictly more state
  than (a). Reject. *(windLocation@0x02D4 is a cosmetic scroll accumulator —
  `windLocation += forward * windAdd` txt:774-781 — feeds only the MPC
  `windLocation` vector param; not the leaf-shake. Not needed.)*

- **(c) `winds` TArray @0x02A8 / `windLocation` — NO, not the wind-event state.**
  `winds[]` is the per-overlapping-object force vector array (parallel to `objs`),
  populated in `doWind` (txt:3026) and `BeginOverlap` (txt:3195); it is the
  *output* applied to loose objects, not the gust generator. `windLocation` is the
  cosmetic scroll (above). Neither drives `intensity`.

**Minimal set = `windTarget.RelativeLocation` (1 × FVector = 12 bytes).** Plus,
because the client's `changeWindOrigin` would otherwise keep overwriting it (Q4),
suppress that one function on the client.

> The 4 fields synced today are **mostly the wrong target** for the "strong leaves"
> bug. `windStrength_background` is overwritten by `intensity` every frame
> (txt:487) → syncing it is futile. `windSpeed_background` only scales the
> *particle/audio/engine speed*, not the leaf-shake magnitude (the MPC scalar is
> `windStrength_total/2+0.5`, which has NO `windSpeed` term — txt:708-726). The
> rain pair is only non-zero while raining. **None of them carry the gust
> (`intensity`/`windTarget`).** Keep the rain pair (correct for rain-wind), but the
> leaf-shake fix is `windTarget`.

---

## Q4 — Client suppression target

**Suppress `changeWindOrigin` on the client** (PRE-intercept → return true to
cancel the BP body). It is the **sole writer of `windTarget`** in normal play
(setParameters has no caller), and its whole purpose is the RNG re-roll +
self-re-arm. Cancelling it stops the client's local gust generator so the synced
`windTarget` is not overwritten ~every 1–60 s.

**Collateral verdict — SAFE, but with one required compensation:**
- `changeWindOrigin`'s body @393 ALSO re-arms `setWindLoc` (3 s camera-follow) and
  re-arms ITSELF. If you cancel the *whole* function on the client, the client's
  `setWindLoc` re-arm inside @393 won't run — **but `setWindLoc` was already armed
  once at BeginPlay** (the BeginPlay path runs the same @393 block before any
  suppression could be installed), and `setWindLoc` is a `looping=True` 3 s timer
  (txt:76-79), so it keeps firing on its own. The re-arm inside @393 is redundant
  for a looping timer. **So suppressing `changeWindOrigin` does NOT stop
  `setWindLoc`** (the Sphere keeps following the camera). No collateral there.
- `changeWindOrigin` does **not** touch leaves, audio, or visuals — those are
  `timer_spawnLeaves` / `updState` / `ReceiveTick`, all independent. No collateral.
- **`ReceiveTick` MUST stay un-suppressed** (it runs the spring + writes the MPC
  scalar + totals from the synced windTarget) — consistent with the brief.

> Is `changeWindOrigin` PRE-observable? It is a timer-delegate callback invoked by
> the engine `FTimerManager`. UE timer delegates dispatch through `ProcessEvent`
> for BP UFunctions, so our ProcessEvent detour SHOULD see it (same class of hook
> the appliance/door syncs rely on for BP UFunctions). **UNVERIFIED at runtime** —
> confirm `changeWindOrigin` actually traverses ProcessEvent (not a direct
> `ProcessInternal` like the cross-object BP-dispatched `playerGrabbed` that
> bypassed our detour on 2026-06-08). If it does NOT hit ProcessEvent, the
> fallback is to neutralize the roll by **re-writing `windTarget` every apply**
> (host stream at the pose-channel rate) so the client's stale roll is corrected
> within one packet — the sync alone largely masks the local roll even without
> suppression, because the host stream overwrites windTarget faster than the
> client re-rolls (client rolls at worst every 1 s; a pose stream is faster). Treat
> suppression as the clean fix, the high-rate overwrite as the guaranteed
> fallback.

---

## Q5 — Both leaf-shake paths flow from `intensity` — **CONFIRMED**

In `ReceiveTick @6287`:

1. **Material Parameter Collection scalar (the foliage/tree-leaf wind)** —
   txt:708-726:
   ```
   windTexStr = windStrength_total/2 + 0.5                                  // txt:708-720
   KismetMaterialLibrary.SetScalarParameterValue(params_weather, "windStrength", windTexStr)  // txt:722-726
   ```
   `windStrength_total = windStrength_rain + windStrength_background`, and
   `windStrength_background = intensity` (txt:487). So when clear (rain pair 0),
   the MPC `windStrength` = `intensity/2 + 0.5` — **directly the spring intensity**.
   This is the scalar VOTV foliage materials read for leaf sway → **leaf-shake
   flows from `intensity` → from `windTarget`.**

2. **Engine `windSource` (SpeedTree / engine-foliage wind)** — `updateDirWind @6624`,
   txt:374-415:
   ```
   v = (windSpeed_total * intensity * windStrength_total) / 2
   windSource.Component.SetSpeed(v) ; windSource.Component.SetStrength(v)
   ```
   Both engine wind setters carry an `intensity` factor → **also flow from
   `intensity` → from `windTarget`.** (Also gets a `windStrength_total`/`windSpeed_total`
   factor, both of which include the synced background/rain terms.)

3. The `eff_wind` particle params (`speed`/`lifetime`/`rate`, txt:628-698) and the
   Audio volume/pitch (txt:583-627) are likewise all `× intensity`. **No leaf-shake
   or wind visual/physics path bypasses `intensity`.** Therefore syncing the spring
   *input* (`windTarget`) fixes **both** the MPC-foliage shake and the
   SpeedTree/engine-foliage shake (and the particle + audio), in one shot.

---

## RECOMMENDED FIX

**Root cause:** the leaf-shake is `intensity`, a spring low-pass of
`windTarget.RelativeLocation`, which each peer re-rolls with private RNG on a
private random 1–60 s timer (`changeWindOrigin`). The current v43 sync of the 4
`windSpeed/Strength_*` fields does not carry `intensity`/`windTarget` and is
overwritten each tick — it cannot fix the bug. **Sync `windTarget`, suppress the
client's `changeWindOrigin`.**

### 1. Wire field to ADD
- **`windTarget.RelativeLocation`** — `FVector` (3 × f32 = 12 bytes).
  - Address: `*(FVector*)( *(uint8**)(wind + 0x0238) + 0x011C )`
    (`windTarget` ptr @ `directionalWind+0x0238`; `USceneComponent::RelativeLocation`
    @ `+0x011C`). Add to `WeatherStatePayload` as `windTargetX/Y/Z`; reuse the
    existing `kWindValid` flag (it already gates the wind read/apply).
  - **Keep** the existing 4 rain/background floats (correct for rain-wind +
    particle/audio speed); they're cheap and already wired. The NEW leaf-shake fix
    is the windTarget vector. *(Optionally drop `windStrength_background` from the
    wire — it's clobbered by `intensity` each tick so it's dead weight — but that's
    a RULE-2 cleanup, not required for the fix.)*

### 2. Host read (in `ReadCycleState`, alongside the existing `directionalwind::Read`)
```
FVector t;  // add ue_wrap::directionalwind::ReadTarget(t)
out.windTargetX = t.X; out.windTargetY = t.Y; out.windTargetZ = t.Z;
out.flags2 |= fog_flags2::kWindValid;   // already set by the existing wind read
```
New wrapper method `ue_wrap::directionalwind::ReadTarget(FVector&)`:
```
void* w = Resolve(); if (!w||!IsLive(w)) return false;
uint8_t* tgt = *(uint8_t**)((uint8_t*)w + 0x0238);   // windTarget UBillboardComponent*
if (!tgt) return false;
out = *(FVector*)(tgt + 0x011C);                      // RelativeLocation
```

### 3. Client write (in `ApplyFromHost`, in the existing `if (flags2 & kWindValid)` block)
```
ue_wrap::directionalwind::WriteTarget({ payload.windTargetX, payload.windTargetY, payload.windTargetZ });
```
New wrapper method `WriteTarget(FVector)`: mirror of ReadTarget, writing
`*(FVector*)(tgt + 0x011C) = in;`. The client's own `ReceiveTick` springs to it
and reproduces `intensity` → the MPC scalar + engine wind (Q5). Keep the existing
4-field write (rain pair, etc.).

### 4. Client suppression UFunction
**PRE-intercept `changeWindOrigin`** (the `AdirectionalWind_C` UFunction) → return
true to cancel the BP body, on the client only (host streams; client mirrors).
This stops the client's RNG re-roll of `windTarget`.
- **Collateral: SAFE.** `changeWindOrigin` writes only `windTarget` + re-arms
  timers; `setWindLoc` stays alive (looping timer already armed at BeginPlay), and
  leaves/audio/visuals are driven by independent functions/`ReceiveTick`. Do NOT
  suppress `ReceiveTick` (it must run the spring + MPC scalar from the synced
  target).
- **UNVERIFIED (runtime): confirm `changeWindOrigin` dispatches through
  ProcessEvent** (so our detour sees it), like the BP UFunctions the
  door/appliance syncs intercept — and unlike the `playerGrabbed`
  `ProcessInternal`-direct case that bypassed the detour (2026-06-08). If it does
  not reach ProcessEvent, **fallback**: rely on the host windTarget STREAM (push it
  on the pose channel / weather pulse at > 1 Hz) to overwrite the client's stale
  roll faster than it re-rolls — the sync alone substantially masks the local roll
  even without suppression.

### 5. ResetVectorSpringState on apply?
**Not required for steady-state parity** — the spring (`spring@0x02B8`) is a
low-pass of windTarget; once windTarget matches, intensity converges in a few
frames. **Optional**: call `ResetVectorSpringState(spring)` ONCE on the
connect-snapshot apply (`QueueConnectBroadcastForSlot` path) so a late joiner
snaps instantly to the host gust instead of easing in over ~0.5 s. Do **not**
reset on every streamed apply (it would jolt the vane each packet). If added, it's
a reflected `KismetMathLibrary::ResetVectorSpringState(spring&)` call (game thread)
or a memset-to-zero of the 0x18-byte `spring` struct *(UNVERIFIED that
FVectorSpringState is zero-initialisable by memset; prefer the UFunction call)*.

### Correction to the prior doc / current code
`votv-wind-basefog-RE-2026-06-08.md` §1.7 and the v43 code comment
(`weather_sync.cpp:984`, `directionalwind.h:5-9`) state "no tick suppression
needed -- the divergence is in these persistent fields." **That is wrong for the
leaf-shake bug:** the persistent divergence is `windTarget` (via the
`changeWindOrigin` RNG timer), and `windStrength_background` is overwritten by
`intensity` every tick. The v43 4-field sync correctly handles rain-wind + the
particle/audio *speed*, but the user-visible "strong leaves on host, calm on
client" is the gust (`intensity`), which needs `windTarget` synced +
`changeWindOrigin` suppressed. Update both comments per RULE 2 when implementing.

### Things I could NOT verify from bytecode (marked UNVERIFIED above)
- Whether `changeWindOrigin` (timer-delegate callback) traverses ProcessEvent (vs
  ProcessInternal-direct). Decides suppression vs high-rate-overwrite fallback.
- The caller of the standalone `doWind` (not in this CFG). Irrelevant to the fix
  (doWind consumes wind, doesn't create it).
- Whether a direct field-poke of `windTarget.RelativeLocation` needs a
  ComponentToWorld refresh for any consumer — none found in this actor (only the
  spring reads `.RelativeLocation` as a plain value), so a direct write suffices.
- Whether `setParameters` reading the instance `intensity` instead of its `Intensity`
  arg is a BP bug — moot (no caller).
