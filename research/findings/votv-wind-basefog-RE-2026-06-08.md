# VOTV coop — WIND + BASE-FOG divergence RE (2026-06-08)

**Bug:** Host + friend both start CLEAR. After the friend joins as CLIENT, the
client's world shows STRONG WIND and (per the user) LOCAL FOG the clear host
doesn't have. Log evidence (last hands-on, `weather_probe=1`):

| field | HOST | CLIENT |
|---|---|---|
| `finalFogDensity` (cycle @0x0418) | **0.0300** (steady) | **0.0000** |
| `rollFogActor` (fogEventObject @0x0338) | null | null |
| `isRaining` (@0x02E4) | 0 | 0 |
| wire `ws` (`rainWindSpeed` @0x041C) | — | 0.00 |

So the **base height-fog density diverges with NO fog actor on either side**,
and the wind ("strong wind" on client) **never logs** because the mod never
touches the wind actor (`AdirectionalWind_C`).

**Tooling used:** `tools/bp_reflect.py` (repak + kismet-analyzer `to-json`) for
function decompilation, and **`kismet-analyzer gen-cfg`** (offset-aware control-flow
disassembly — the `.txt` with byte offsets + block structure) to trace the
`ExecuteUbergraph_*` flow-stack. (Note: the `gen-cfg` referenced in the task brief
is a *native kismet-analyzer subcommand*, not a `bp_reflect.py` mode — `bp_reflect.py`
only does `to-json`.) Scratch decompiler: `research/pak_re/dump_fn.py`. No IDA needed —
the wind/fog logic is **pure Blueprint**, fully visible in kismet bytecode. Field
offsets cross-checked against `CXXHeaderDump/{directionalWind,daynightCycle}.hpp`.

Raw artifacts (gitignored): `research/bp_reflection/{directionalWind,daynightCycle,mainGamemode}.json`,
`research/pak_re/cfg/{directionalWind,daynightCycle,mainGamemode}/*.txt`.

---

## TL;DR

1. **Both peers independently run their own `AdaynightCycle_C` ReceiveTick AND
   their own `AdirectionalWind_C` (ReceiveTick + a 1 s `updateDirWind` timer).**
   The mod syncs only the cycle's rain scalars + fog *actor* presence. It does
   **NOT** sync the directionalWind's persistent wind fields nor the cycle's
   base-fog density when there is no fog actor → both drift.

2. **WIND root cause.** The visible/audible/physics wind on a peer =
   `windSpeed_total = windSpeed_rain + windSpeed_background` and
   `windStrength_total = windStrength_rain + windStrength_background`, everything
   scaled by a per-tick spring-physics `intensity`. None of these 4 persistent
   fields are synced:
   - `windSpeed_rain` / `windStrength_rain` are set by the cycle's
     `setWindParameters()` — VERIFIED it writes `windSpeed_rain = rainWindSpeed`
     and `windStrength_rain = (rainStrength + 0.5) * rain`. So they depend on the
     cycle's **`rain` accumulator**, which **free-runs per peer** (eases 0↔1 every
     tick at `rainSpeed=120`). If the client's `rain` ≠ host's `rain`, the rain
     wind diverges.
   - `windSpeed_background` / `windStrength_background` are set by the cycle's
     **day-rollover Ease** (and `AdirectionalWind_C::setParameters`, which has
     **no caller in normal play**). They default to `windSpeed_background = 5.0`,
     `windStrength_background = 0`. They are **never synced**, so any day-rollover
     mismatch or a stale value persists. **This is the "strong wind."**
   - `setWindParameters()` is **no-arg** and **only** touches the rain pair. The
     existing protocol.h comment ("the only wind-state field we need to sync is
     rainWindSpeed … receiver calls setWindParameters()") is **WRONG/incomplete** —
     proven below.

3. **BASE-FOG root cause.** The cycle's per-tick fog block computes a hardcoded
   **0.03 clear-weather floor**: `finalFogDensity = FMax(Lerp(0.03, …, rain) *
   Lerp(1,15,thickFog), …)`; with `rain=0, thickFog=0` that is `FMax(0.03, 0) =
   0.03`. The host shows exactly that. The client shows `0.0000` = the field's
   **untouched default**, i.e. the client's per-tick fog computation is **not
   producing the floor** (cycle tick fog-pass not effectively running on the
   client, OR a transient capture). The mod only ever writes `finalFogDensity`
   inside the *host-has-a-fog-actor* branch of `weather_fog::ApplyFromHost`
   (weather_fog.cpp:215); when the host is **clear**, the client is never told
   the base density, so it sits at default `0`. **Fix = always carry + apply the
   host's `finalFogDensity`, not only when a fog actor exists.**

4. **Fix shape (both):** extend the existing host-authoritative weather sync —
   **WIND:** add the 4 `AdirectionalWind_C` fields to `WeatherStatePayload`; the
   client suppresses its local drift by writing them every apply (the cycle's
   `setWindParameters()` + the directionalWind's own 1 s `updateDirWind` then
   push them to the engine `WindDirectionalSource`). **BASE-FOG:** make the
   already-synced `finalFogDensity` write **unconditional** (apply it in the
   host-clear branch too), and add the cycle's `rain` accumulator (already on the
   wire @0x02E0) as the anchor so the client's fog floor matches.

---

## 1. WIND drivers — full disassembly

### 1.1 `AdirectionalWind_C` layout (CXXHeaderDump/directionalWind.hpp)

Singleton, held by `AmainGamemode_C::directionalWind @ 0x0F70` (confirmed in
mainGamemode.hpp:290). Relevant fields:

| field | offset | size | role |
|---|---|---|---|
| `Intensity` (BP refs it as `intensity`) | 0x02D0 | f32 | **per-tick spring-physics multiplier** (scales all wind) |
| `windAdd` | 0x02E0 | f32 | per-tick scroll accumulator (cosmetic) |
| `windSpeed_rain` | 0x02E4 | f32 | rain-driven wind speed (set by cycle setWindParameters) |
| `windStrength_rain` | 0x02E8 | f32 | rain-driven wind strength (set by cycle setWindParameters) |
| `windSpeed_background` | 0x02EC | f32 | **ambient** wind speed (day-rollover Ease / setParameters; default 5.0) |
| `windStrength_background` | 0x02F0 | f32 | **ambient** wind strength (default 0) |
| `windSpeed_total` | 0x02F4 | f32 | `= windSpeed_rain + windSpeed_background` (recomputed per tick) |
| `windStrength_total` | 0x02F8 | f32 | `= windStrength_rain + windStrength_background` (recomputed per tick) |
| `windTexStr` | 0x02FC | f32 | material-collection scalar (cosmetic) |
| `windSource` | 0x0288 | ptr | `AWindDirectionalSource*` — the ENGINE wind actor (physics) |
| `tickable` | 0x02A0 | bool | gates updState (hide/active); default True |

CDO defaults (from `Default__directionalWind_C`): `tickable=True`,
`windSpeed_rain=5.0`, `windSpeed_background=5.0`, `windSpeed_total=5.0`;
strengths default 0.

### 1.2 `setParameters(Intensity, Angle, Speed, Strength)` — the BG-wind setter

(`dump_fn.py … setParameters`) — relevant writes:
```
windTarget->K2_SetRelativeLocation( RotateAngleAxis(MakeVector(intensity*50,0,0), angle, +Z) )
windOffset->K2_SetRelativeLocation( … same … )
ResetVectorSpringState(spring)
windSpeed_background    = Speed       // <-- arg
windStrength_background = Strength     // <-- arg
```
**So `windSpeed_background` / `windStrength_background` are set ONLY here (and by
the day-rollover Ease, §1.5).** `setParameters` has **NO caller** anywhere in
`directionalWind`, `daynightCycle`, or `mainGamemode` (grepped all three CFGs).
It is an externally-invokable function (settings/intercom event) — i.e. in normal
clear play the background wind is the CDO default (5.0 / 0) plus whatever the
day-rollover Ease last wrote.

### 1.3 `ReceiveTick` → `ExecuteUbergraph_directionalWind(6287)` — per-frame

The per-frame chain (gen-cfg block @6287 → 5931 → 3872 → …):
```
// spring physics on the wind-vane:
windOffset.RelativeLocation = VectorSpringInterp(windOffset.Rel, windTarget.Rel, spring, 0.25,0.1, dt, 500)
windOffset.RelativeLocation = VectorSpringInterp(windOffset.Rel, {0,0,0}, spring, …)   // damped toward origin
// derive intensity from the vane displacement:
intensity = FMax( VSize(windOffset.RelativeLocation) - 5, 0 ) / 50          // block @5931..6134
// recompute the totals every tick:
windStrength_background = intensity                                          // @3872  (NB: tick OVERWRITES bg-strength with intensity)
windSpeed_total    = windSpeed_rain + windSpeed_background                   // @3908..3963
windStrength_total = windStrength_rain + windStrength_background             // @3972..4036
// drive visible/audible/material from the totals * intensity:
Audio->SetVolumeMultiplier( FClamp( (windStrength_total/10 * intensity)/1.25, 0, 2 ) )   // @4675..4883
Audio->SetPitchMultiplier( Lerp(1, 1.25, windSpeed_total*intensity/25) )                  // @4893..5059
eff_wind->SetVectorParameter("speed",  windSpeed_total*intensity*1000 + 1000)            // @5069..5294
eff_wind->SetFloatParameter("lifetime", 20/(windSpeed_total*intensity) * 2)               // @5304..5482
eff_wind->SetFloatParameter("rate",    (windSpeed_total*intensity*1.25/25)*windStrength_total) // @5492..5716
params_weather::SetScalar("windStrength", windTexStr=windStrength_total/2 + 0.5)          // @5805..1046
windAdd += windSpeed_total * dt * (intensity+0.05) * 100                                   // @1163..1386
```
**Conclusion:** everything a player SEES/HEARS as "wind" is a deterministic
function of `windSpeed_total`, `windStrength_total`, and `intensity`. `intensity`
is spring-physics; the spring is driven only by `windTarget` (set by
`setParameters`), so with no setParameters call the spring settles and `intensity`
**converges to the same steady value on both peers**. Therefore the only wind
DIVERGENCE sources are the 4 persistent fields `windSpeed_rain / windStrength_rain
/ windSpeed_background / windStrength_background`.

> NOTE: the tick line `windStrength_background = intensity` (@3872) means the tick
> continuously rebases bg-strength to the (converging) spring intensity. The
> *speed* side (`windSpeed_background`) is NOT rebased — it persists from
> setParameters / the day Ease / the 5.0 default. So **`windSpeed_background` is
> the durable divergence** and **`windStrength_rain` (= (rainStrength+0.5)*rain)
> is the rain-coupled divergence**.

### 1.4 `updateDirWind` → `ExecuteUbergraph_directionalWind(6624)` — 1 s timer

Bound in BeginPlay as a 1 s repeating timer (`K2_SetTimerDelegate updateDirWind,
1.0, looping`). Pushes the totals to the ENGINE wind source (block @6624):
```
v = (windSpeed_total * intensity * windStrength_total) / 2
windSource->Component->SetSpeed(v)        // WindDirectionalSourceComponent::SetSpeed
windSource->Component->SetStrength(v)      // WindDirectionalSourceComponent::SetStrength
```
So once the 4 BP fields match across peers (and intensity converges), the engine
`AWindDirectionalSource` ends up identical within ≤1 s — **no need to sync the
engine component directly**; syncing the 4 BP fields is sufficient.

### 1.5 `daynightCycle::setWindParameters()` — the rain-wind setter (no-arg)

(`dump_fn.py … setWindParameters`) — the ONLY thing the cycle does to the wind:
```
directionalWind->windStrength_rain = (rainStrength + 0.5) * rain      // stmt[0..2]
directionalWind->windSpeed_rain    = rainWindSpeed                     // stmt[3]
rain_root->K2_SetWorldRotation( dirWind forward → rotator )            // wind direction
rain_tilt->K2_SetRelativeRotation( MakeRotator(0, DegAtan(windSpeed_total*windStrength_total*intensity/50)/2, 0) )
```
**This confirms `setWindParameters` only touches the RAIN pair** (and rotation).
It reads the cycle's `rainStrength @0x0404`, `rain @0x02E0`, `rainWindSpeed
@0x041C`. It NEVER touches `windSpeed_background` / `windStrength_background`.

### 1.6 Day-rollover Ease writes `windSpeed_background` (cycle ubergraph @5820)

In `ExecuteUbergraph_daynightCycle`, on the **newDay** path (block @5820, reached
in the cluster that also does `savedtime.Z >= 39`, `sleeplessDays++`,
`updateWeekDay`):
```
windSpeed_background = Ease(2, 10, 0, EaseFunc=5, BlendExp=2, Steps=2)
```
(`Ease(A=2,B=10,Alpha=0,…)` → result `2` here; the point is the value is set on a
day boundary, independently per peer.) **Confirms `windSpeed_background` is
day-of-game dependent and per-peer** → a joined client that has experienced a
different day-rollover history holds a different background wind.

### 1.7 WIND divergence — verdict

The client gets "strong wind" the clear host doesn't because **`AdirectionalWind_C`
runs locally on each peer and its persistent fields are never synced**. The two
durable divergence channels:
- **`windStrength_rain = (rainStrength + 0.5) * rain`** — the cycle's `rain`
  accumulator free-runs per peer (eases 0↔1 at `rainSpeed=120`). At a join, the
  client's `rain` is whatever its local sim produced; if non-zero (or transiently
  driven by a stray suppressed scheduler tick before suppression latched, or a
  default), `windStrength_rain` is non-zero → wind.
- **`windSpeed_background`** — defaults 5.0 and is only ever changed by the
  day-rollover Ease / `setParameters`; never reconciled to the host.

This matches candidate **(a)** in the task brief: *the client's directionalWind
ticks independently and diverges.* Candidate (c) `windParticles_check` is NOT the
mechanism — `eff_wind` activity is gated by `updState`/`tickable` (default on for
both), and the particle params are driven straight from the totals above.

---

## 2. BASE-FOG density — full disassembly

### 2.1 `setFogDensity()` (cycle) — pushes the field to the engine

(`dump_fn.py … setFogDensity`):
```
ExponentialHeightFog->SetFogDensity(finalFogDensity)     // @0x0418 -> ExpHeightFogComponent
```

### 2.2 Per-tick fog computation — `ExecuteUbergraph_daynightCycle(14170)`

`ReceiveTick → ExecuteUbergraph_daynightCycle(14170)`. The tick spine (gen-cfg):
```
@14170:  deltaSeconds = DeltaSeconds
         ExponentialHeightFog->SetFogHeightFalloff( Lerp(0.2, 0.05, thickFog) )
@14285:  EX_PopExecutionFlowIfNot  isActive          // <-- gate: pops (early-out) if !isActive
@14295:  push 14625 …               (sun/time/etc. sub-tasks scheduled via the flow stack)
@14315:  push 15319                  // <-- FOG-DENSITY sub-task
@14320:  push 17336                  // <-- TIME-ADVANCE sub-task (day/totalTime VictoryFloatPlusEquals)
```
The fog-density block @15319 (straight-line; the `finalFogDensity` write at
offset 16462) computes:
```
rainDir = isRaining ? +1 : -1                                   // @15319..15393
rain   += (deltaSeconds / rainSpeed) * rainDir                  // VictoryFloatPlusEquals @15495, rainSpeed=120
rain    = FClamp(rain, 0, 1)                                    // @15541..15597
…
A = Lerp(1, 15, thickFog)                                       // @16044  (thickFog=0 -> A=1)
D = rainStrength/10 + 0.05                                      // @16133..16211
F = Lerp(0.03, D, rain)                                         // @16259  (rain=0 -> F=0.03)
G = Lerp(0, rainStrength/10 + 0.2, (rain*…))                    // @16310  (rain=0 -> G=0)
H = F * A                                                       // @16361  (=0.03 when clear)
finalFogDensity = FMax(H, G)                                    // @16407 -> write @16462
setFogDensity()                                                // @16480 -> push to ExpHeightFog
```
**Clear weather (`rain=0, thickFog=0`): `finalFogDensity = FMax(0.03, 0) = 0.03`.**
This is exactly the host's logged value. It is a **hardcoded ambient floor**, not
a ramp — it does not "warm up"; it is 0.03 on the first tick the block runs.

### 2.3 `isActive` (@0x02CC) gate + why the client reads 0.00

- `isActive` is referenced **once** in the whole cycle ubergraph (offset 14286,
  read-only) and **never written** by the cycle, `mainGamemode`, or any other
  disassembled BP. Its CDO default is **`isActive = True`** (verified in
  `Default__daynightCycle_C`). So a normally-spawned cycle ticks with the fog
  block active → would write 0.03.
- BOTH the fog-density block (@15319) AND the time-advance block (@17336, which
  does `day += …` @18129 and `totalTime += …` @18350) sit **behind** the
  `PopExecutionFlowIfNot isActive` gate. So if the client cycle's tick were truly
  not running, the client's clock wouldn't advance either — **except** the mod's
  `time_sync` direct-writes `totalTime`/`day` every 2 s (`ue_wrap::daynightcycle::ApplyClock`),
  which would MASK a stalled tick. (time_sync.cpp:23 even assumes "the client
  free-runs its own ReceiveTick".)
- **Net:** the client reading `finalFogDensity=0.0000` means the client's per-tick
  fog-floor is **not effectively applied** — either the client cycle's tick
  fog-pass isn't running, or a freshly (re)spawned cycle's first passes were
  captured, or a transient at the connect teleport. **This last-mile (isActive
  flipped vs tick disabled vs transient) is the one thing static BP RE cannot
  decide — it needs a runtime read of `*(bool*)(cycle+0x02CC)` on the client +
  whether the client cycle's `ReceiveTick` fires.** *[UNVERIFIED at runtime — see
  Recommendation; the fix is robust either way.]*

### 2.4 BASE-FOG divergence — verdict

The mod **already** snaps `finalFogDensity` onto the client — but **only inside
the `hostFogActive` branch** of `weather_fog::ApplyFromHost` (weather_fog.cpp:186,
write at :215). When the host is **clear** (no `fogEventObject` actor), the apply
takes the *other* branch (destroy stray client fog) and **never writes
`finalFogDensity`** → the client's base density is left at whatever its local
(possibly stalled) tick produced = default `0.00`. The host-clear case is exactly
the user's scenario. This is the gap. *(Whether the local tick "should" produce
0.03 is moot under RULE 1: host-authoritative means we push the host's value, not
rely on the client recomputing it identically.)*

> "LOCAL FOG actors" the user saw on the client: if the client's own (suppressed)
> fog scheduler ever fired a `spawnFog` before suppression latched, OR a super-fog
> spawned, the client would have a fog actor the host lacks. The clear-side apply
> (weather_fog.cpp:179) already destroys a stray *rolling* fog and (clear-only)
> super-fog — but it runs **only on an apply**, i.e. when a WeatherState packet
> arrives. If no packet arrives after the spurious spawn (host weather is static
> → no scheduler POST → no broadcast), the stray client fog persists. The
> base-density fix + a periodic clear-assert close this; flagged in §3.

---

## 3. Recommended fix (host-authoritative, consistent with weather_sync/weather_fog)

### 3.1 WIND — sync the 4 `AdirectionalWind_C` persistent fields

**Minimal sync field set (read on host, write on client):**

| field | offset (`AdirectionalWind_C`) | why |
|---|---|---|
| `windSpeed_background` | **0x02EC** | durable ambient divergence (default 5.0; day-Ease) |
| `windStrength_background` | **0x02F0** | ambient strength (tick rebases to intensity, but seed it) |
| `windSpeed_rain` | **0x02E4** | rain wind speed (= cycle `rainWindSpeed`) |
| `windStrength_rain` | **0x02E8** | rain wind strength (= `(rainStrength+0.5)*rain`) |

`intensity @0x02D0` and the engine `windSource->Component` do **NOT** need to be on
the wire: `intensity` is spring-physics that converges once the inputs match, and
the 1 s `updateDirWind` timer republishes `Component->SetSpeed/SetStrength` from
the totals. (If we later want instant convergence we could also stamp `intensity`,
but it's not required for parity.)

**Mechanism (RULE 1 host-authoritative, mirrors weather_fog):**
- New principle-7 wrapper `ue_wrap/directionalwind.{h,cpp}`: resolve the singleton
  via `mainGamemode.directionalWind @0x0F70` (or `FindObjectByClass(L"directionalWind_C")`
  to match the project's cycle-resolution pattern), with `ReadWind(out4)` /
  `WriteWind(in4)` (direct field writes — these are plain accumulators with no BP
  fan-out; writing them is the canonical path, like the cycle clock).
- Extend `WeatherStatePayload` with the 4 floats (`bgSpeed, bgStrength, rainSpeed_w,
  rainStrength_w`) → +16 bytes (40→56). Bump protocol version; `ParseHeader`
  rejects older peers. Add them to `ReadCycleState` (host) via the new wrapper.
- **Client suppression:** the client already suppresses the cycle's *scheduler*
  UFunctions. The directionalWind's divergence comes from its **ReceiveTick spring
  recompute + the rain pair**, NOT from a schedulable UFunction — so we do **not**
  need to suppress its tick. We just **overwrite the 4 fields every apply**; the
  client's own `ReceiveTick`/`updateDirWind` then converge the totals + engine
  source to the host's values within ≤1 s. (No echo risk: the client never
  broadcasts; the host's POST observers don't fire on the directionalWind.)
- Include the 4 wind fields in `SignaturePayload` so a wind-only change still
  broadcasts (they currently would be deduped away — same trap that v23 flags2 hit).
- Add the wind block to `QueueConnectBroadcastForSlot` (already calls
  `ReadCycleState`) so a late joiner snaps to the host's wind immediately.

**RULE 1 note:** delete/rewrite the now-false protocol.h comment block (lines
1482-1488: "the only wind-state field we need to sync is rainWindSpeed"). That
claim is disproven by §1.5 — `setWindParameters` does not sync background wind, and
the rain pair on the client is recomputed from the client's own `rain`.

### 3.2 BASE-FOG — make `finalFogDensity` unconditional + anchor `rain`

`finalFogDensity @0x0418` and `rain @0x02E0` are **already on the wire** (v24). The
fix is in `weather_fog::ApplyFromHost` (and the cycle apply for `rain`):

1. **Apply `finalFogDensity` in the host-CLEAR branch too** (today it's only
   written in the `hostFogActive` branch, weather_fog.cpp:215). Move the
   `*(float*)(cycle+0x0418) = payload.finalFogDensity; SetFogDensity();` to run
   **unconditionally** (after the actor branch), so the client matches the host's
   ambient 0.03 even with no fog actor. This is the single line that fixes the
   logged 0.03-vs-0.00.
2. **Anchor the cycle's `rain` accumulator** on apply (write `*(float*)(cycle+0x02E0)
   = payload.rain`). `weather_sync::ApplyFromHost` already does this (weather_sync.cpp:862),
   but only `if (cur.rain != payload.rain)` — keep it; it ensures the client's fog
   floor (which is `Lerp(0.03, …, rain)`) tracks the host instead of the client's
   free-running `rain` dragging it.
3. Because `setFogDensity()` is a one-line setter (`SetFogDensity(finalFogDensity)`),
   pushing the field + calling it makes the height fog instant (already done for the
   actor branch; just extend to clear).

**Driver choice — sync the VALUE, not the inputs:** `finalFogDensity` is the single
observable; its inputs (`rain`, `thickFog`, `rainStrength`) are *also* synced for
rain/fog-actor reasons, but the robust host-authoritative move is to push the
**resolved `finalFogDensity`** (+ `setFogDensity()`) every apply. That guarantees
parity regardless of whether the client's local tick runs the fog block. (RULE 1:
don't depend on the client recomputing the floor identically; push the host's
truth.)

### 3.3 RULE 5 — now vs later

- **NOW (worth it; small, closes the visible bug):**
  - Base-fog: make `finalFogDensity` apply unconditional (§3.2.1) — ~1 line, no
    protocol bump (field already on wire). **Highest ROI.**
  - Wind: add the 4 directionalWind fields + wrapper + apply (§3.1). One protocol
    bump, one new wrapper file. Fixes "strong wind on client."
- **LATER / flag (separate from this bug):**
  - Periodic host-clear **fog-actor assert** so a stray client `spawnFog`/super-fog
    that spawned with no subsequent host broadcast still gets destroyed (today the
    clear-assert only runs on an apply; static host weather → no apply). Cheap: the
    fog-edge detector (`HostFogStateChanged`) already runs ~3 Hz host-side — mirror
    a low-rate "no fog ⇒ broadcast clear" heartbeat, or have the client self-destroy
    a fog actor it spawned while suppressed. (RULE 1 root cause: client should never
    have made the fog — verify the spawnFog interceptor actually latched before the
    connect, since `weather_fog::Install` only latches after the interceptor
    registers; if a scheduler fired in the window before Install latched, that's the
    real leak to close.)
  - Optionally stamp `intensity @0x02D0` for instant (not ≤1 s) wind convergence.

---

## Appendix — exact UFunction / setter / offset reference

**Wind (`AdirectionalWind_C`, singleton @ `AmainGamemode_C::directionalWind 0x0F70`):**
- READ/WRITE fields: `windSpeed_rain 0x02E4`, `windStrength_rain 0x02E8`,
  `windSpeed_background 0x02EC`, `windStrength_background 0x02F0` (and totals
  `0x02F4/0x02F8` are derived — no need to write). `intensity 0x02D0` (spring).
- Drive-to-engine: the 1 s `updateDirWind` timer calls
  `windSource(0x0288)->Component->SetSpeed/SetStrength` — no manual call needed.
- Cycle setter that feeds the rain pair: `AdaynightCycle_C::setWindParameters()`
  (no-arg) → writes `windSpeed_rain = rainWindSpeed`, `windStrength_rain =
  (rainStrength+0.5)*rain`. Already resolved as `g_setWindParametersFn` in
  weather_sync.cpp.

**Base fog (`AdaynightCycle_C`):**
- `finalFogDensity 0x0418` (the value) — write it, then call `SetFogDensity()`
  (resolved as `weather_fog::g_setFogDensityFn`; body = `ExpHeightFog->SetFogDensity(finalFogDensity)`).
- `rain 0x02E0` (the ease target/accumulator that the floor `Lerp(0.03,…,rain)`
  depends on) — anchor on apply.
- `thickFog 0x0330` (rolling-fog target; 0 when clear → `A=Lerp(1,15,0)=1`).
- `isActive 0x02CC` (bool, default True) — the tick gate; runtime-confirm if the
  client base-fog still mismatches after the value-push fix.
- `rainSpeed 0x????` (=120 default; the per-tick `rain` ease rate) — not currently
  in `sdk_profile.h`; only needed if we ever sync the rate (we sync the value
  instead, so not required).
