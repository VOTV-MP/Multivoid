# VOTV — ApowerControl_C base power panel: RE + coop-sync apply recipe (2026-06-08)

**Target:** `ApowerControl_C` (the base "power panel" with 5 system breakers:
coords / downloading / playing / calc / light). We are adding cross-peer sync so a
remote peer mirrors the panel (levers positioned, LEDs lit) given a 5-tuple of
`press_*` states. This doc gives the **byte-exact apply path**, decisively.

---

## 0. CRITICAL METHODOLOGY NOTE — this class is NOT in IDA (not native)

The task brief said "BP-nativized/Shipping build; BP functions are real native
functions." **That premise is FALSE for this build.** Verified decisively in the
loaded `VotV-Win64-Shipping.exe.i64` (md5 `28139c7c314659f8d3122155a6f33a69`):

- **No BP class/function/property name is in the exe as a string.** `find_regex`
  for `powerControl`, `powerChanged`, `moveLevers`, `mainPlayer`, `kerfur`,
  `Aprop_`, `serverBox` → **0 hits**. Only `/Script/VotV` (the engine module path)
  exists. The 8.17 GB `VotV-WindowsNoEditor.pak` holds all BP names + bytecode.
- **Only TWO VOTV-native exec thunks exist in the whole binary:**
  `execImpactDamageCPP_mainPlayer` @ `0x14112ca60` and
  `execImpactSquishCPP_mainPlayer` @ `0x14112cc60` (the hand-written C++ helpers
  the `mainPlayer` BP calls). Every other `exec*` is stock engine
  (UGameplayStatics, ACharacter, …). There is **no `execpowerChanged`, no
  `execbuttonsVisibility`, no `execmoveLevers`, no `ExecuteUbergraph_powerControl`,
  no `ExecuteUbergraph_*` of any VOTV class** in `.text`.
- **204,547 functions, only 822 named** — a nativized BP project would have
  hundreds of thousands of *named* `ExecuteUbergraph_*`. There are zero. Segment
  layout is a stock UE4.27 Shipping PE (11 segments, no nativized-asset code
  section).
- The decompiled `execImpactDamageCPP_mainPlayer` thunk (and its prior-RE comment
  in this IDB) confirms the dispatch model: BP funcs run through `UFunction->Func`
  (`UObject::ProcessInternal`), which is why our ProcessEvent detour misses them
  (the same IDA-proven fact the door/garage RE used to choose POLL-not-observe).

**Conclusion:** `ApowerControl_C` and ALL its functions (`powerChanged`,
`buttonsVisibility`, `moveLevers`, `ExecuteUbergraph_powerControl`,
`actionOptionIndex`, `player_use`, `sendPower`, `setPowered`) are **pure Blueprint
kismet bytecode**. IDA cannot disassemble them. Per the project's escalation ladder
and the sibling RE docs (`votv-garage-door-button-sync-RE`,
`votv-keypad-door-BP-disassembly`), the authoritative tool is the **cooked-BP
disassembler**: `tools/bp_reflect.py powerControl` →
`research/bp_reflection/powerControl.json` (repak + trumank kismet-analyzer,
offline, RULE-3 dev tools), rendered with `research/bp_reflection/_cfg.py`. All
addresses below are **kismet bytecode offsets in that JSON**, not exe RVAs.
(An orienting comment to this effect was left on `0x14112ca60` in the IDB.)

> This is not a gap in the RE — it is the correct, complete answer. Every claim
> below is cited to the real bytecode, byte-exact.

---

## 1. Function map (each BP event is a thin trampoline into the ubergraph)

`powerControl.json` (asset `VotV/Content/objects/powerControl.uasset`, 52 functions):

| BP function | Body | Ubergraph entry |
|---|---|---|
| `actionOptionIndex(Player,Hit,Action,lookAtComponent)` | store args → call ubergraph | `ExecuteUbergraph_powerControl(3878)` |
| `player_use(Player,Hit)` | store args → call ubergraph | `(3876)` → **no-op** (`@3876: POP->ret`) |
| `powerChanged(active_calc,active_downl,active_coords,active_play,active_light)` | store 5 args → call ubergraph | `(3777)` |
| `moveLevers()` | call ubergraph | `(4074)` |
| `buttonsVisibility()` | **real body** (184 stmts, 5496 B, NOT a trampoline) | — |

So the panel's interactive logic lives in **`ExecuteUbergraph_powerControl`** (entry
@3878 for a press) and in **`buttonsVisibility`** (the refresh + fan-out). `player_use`
is dead (`POP->ret`) exactly like the door/garage `player_use`; the real activation
is `actionOptionIndex`.

---

## 2. Q1 — PRESS FLOW (byte-exact)

`actionOptionIndex` → `ExecuteUbergraph_powerControl(3878)`:

```
@3878: BreakHitResult(hit, ...) ; comp := HitComponent      ; @4050 comp <- the box collider hit
@4069: JUMP @2554
@2554: PUSH @2578 ; IFNOT(tutorial) JUMP @2593              ; tutorial -> addHint (no toggle); else @2593
@2593: IF comp == power_coordinates -> @2645  else @2717
   @2645: press_coord := Not(press_coord) ; playSND(press_coord) ; POP->ret   ; <-- TOGGLE (coords)
@2717: IF comp == power_downloading -> @2769 else @2841
   @2769: press_downl := Not(press_downl) ; playSND(press_downl) ; POP->ret   ; <-- TOGGLE (downl)
@2841: IF comp == power_playing -> @2893 else @2965
   @2893: press_play := Not(press_play)  ; playSND(press_play)  ; POP->ret   ; <-- TOGGLE (play)
@2965: IF comp == power_calculating -> @3017 else @3089
   @3017: press_calc := Not(press_calc)  ; playSND(press_calc)  ; POP->ret   ; <-- TOGGLE (calc)
@3089: IF comp == power_light -> @3137 (else POP)
   @3137: press_light := Not(press_light); playSND(press_light); POP->ret   ; <-- TOGGLE (light)
```

**The press handler toggles EXACTLY ONE `press_*` bool** (the one whose box collider
`comp` was hit) and calls `playSND(newState)`. **That is the whole toggle.** It does
**NOT** call `buttonsVisibility`, `moveLevers`, `powerChanged`, `sendPower`, or
`setPowered` from this path.

> So what refreshes the panel + fans out after a toggle? Not the press handler. The
> panel is repainted + the base is driven by **`buttonsVisibility()`**, which is
> called on a refresh cadence (it is the panel's `Tick`/refresh entry — it is also
> the ubergraph's `@2578` continuation and is invoked from the BeginPlay/refresh
> chain; the `actionOptionIndex` path's PUSHed `@2578` runs `buttonsVisibility()`
> after the toggle frame returns: `@2554: PUSH @2578` → toggle → `@2578:
> buttonsVisibility()`). Net effect of one player press: toggle one press_ →
> `buttonsVisibility()` repaints + fans out. The two are separate functions, which
> is exactly what lets the mirror call a refresh without re-toggling.)

**Ordered sequence on a real press:** `press_X ^= 1` → `playSND(press_X)` →
`buttonsVisibility()` (which internally repaints LEDs/bulbs, calls `moveLevers()`,
and — gated — fans out to servers/lights/doors/cords + `gamemode.setPower`).

---

## 3. Q2 — PANEL-VISUAL SELF-REFRESH (LEDs, bulbs, levers)

### 3a. `buttonsVisibility` @116-1346 — LED particles + powerblock bulbs (ALWAYS runs)

For each of the 5 subsystems (coords/downl/calc/light/play), reading the `press_*`
bool directly:

```
@116:  eff_coords_off.SetVisibility(Not(press_coord)) ; eff_coords_on.SetVisibility(press_coord)
       powerblock.SetMaterial(5, press_coord ? onBulb : offBulb)         ; SWITCHVAL on press_coord
@381:  eff_downl_off/on  <- press_downl ; powerblock.SetMaterial(4, press_downl?…)
@646:  eef_calc_off/on   <- press_calc  ; powerblock.SetMaterial(3, press_calc?…)
@911:  eff_light_off/on  <- press_light ; powerblock.SetMaterial(7, press_light?…)
@1176: eff_play_off/on   <- press_play  ; powerblock.SetMaterial(6, press_play?…)
```

**Decisive: YES — `buttonsVisibility()` reads the 5 `press_*` bools and sets the
`eff_*_on`/`eff_*_off` particle-component visibility (the panel LEDs) + the
`powerblock` bulb materials.** This block is ungated (always runs when
`buttonsVisibility` is called). It is the panel's LED/bulb self-refresh from press_
state. (`powerblock` material slots: 3=calc, 4=downl, 5=coords, 6=play, 7=light.)

Additionally @3178 (gated only by gens/disabled, see §4) sets the global panel
look: `powerblock.SetScalarParameterValueOnMaterials('isOn', (!disabled && gensFine)?1:0)`
and `lights/text_*.SetHiddenInGame(disabled||!gensFine)`.

### 3b. `moveLevers` @4074 — lever positions from press_* (purely visual)

```
@4074: PUSH @2366; PUSH @2185; PUSH @2005; PUSH @1825; JUMP @1201
@1201: MoveComponentTo(lever_3, vec, SelectRotator(rotOff, rotOn, press_calc),  0.25s)
@1825: MoveComponentTo(lever_2, vec, SelectRotator(rotOff, rotOn, press_play),  0.25s)
@2005: MoveComponentTo(lever_1, vec, SelectRotator(rotOff, rotOn, press_downl), 0.25s)
@2185: MoveComponentTo(lever_0, vec, SelectRotator(rotOff, rotOn, press_coord), 0.25s)
@2366: MoveComponentTo(lever_4, vec, SelectRotator(rotOff, rotOn, press_light), 0.25s)
```

**Decisive: YES — `moveLevers()` positions `lever_0..4` from the `press_*` bools**
via `MoveComponentTo` over 0.25s (`SelectRotator(off, on, press_X)`). It is purely
visual (component motion only, no field writes, no fan-out). **`moveLevers()` is
also called from inside `buttonsVisibility` @3894** — so `buttonsVisibility()` alone
already animates the levers.

**Lever ↔ press_ mapping (component → bool):**

| lever | press_ |
|---|---|
| `lever_0` | `press_coord` |
| `lever_1` | `press_downl` |
| `lever_2` | `press_play` |
| `lever_3` | `press_calc` |
| `lever_4` | `press_light` |

---

## 4. Q3 — FAN-OUT (the downstream base drive — AVOID re-running on a remote peer)

`buttonsVisibility()` is **NOT purely visual** — after the LED/bulb repaint it fans
out to the base (this is the thing a mirror must avoid re-running, because doors /
lights / servers are synced by their own channels). Three gated stages + an ungated
cord stage:

```
@1441 (gate: IFNOT(disabled); then press_calc != check_calc; then isGeneratorsFine):
        for s in servers[]:   s.setActive(press_calc)             ; <-- drives AserverBox_C
        serversSound.root.SetActive(press_calc) ; check_calc := press_calc
@1985 (gate: IFNOT(disabled); then press_light != check_light; then gens fine):
        for r in lighRoots[]: r.runTrigger(self, 2) ; r.setActive(press_light)   ; <-- drives Atrigger_lightRoot_C
        for t in gamemode.baseSockets[]: t.setActiveTrigger(self, press_light)
        check_light := press_light
@3178 -> @3797 (gate: IFNOT(disabled OR !gensFine) JUMP @3812):
   @3812: gamemode.setPower(press_calc, press_downl, press_coords, press_play, press_light)
          ; <-- pushes the 5-tuple to the GAME MODE; the gamemode fires its
          ;     `powerChanged` multicast delegate, to which THIS panel bound at
          ;     BeginPlay (@3286 BIND powerChanged / @3309 ADDDEL gamemode.powerChanged += ...).
@3894: moveLevers()
@3909 (gate: press_light && areGensFine) — UNGATED by check_*/disabled:
        if (press_light && areGensFine): wallMounts[] + miscObjects[].cordPlugged()
        else:                            wallMounts[] + miscObjects[].cordUnplugged()
```

**And the gamemode delegate re-enters this panel's `powerChanged` body
@3777** (the §5 callback), which loops `doorsOpen[]` and opens the blackout doors:

```
@3777 (powerChanged body): IFNOT(active_light) JUMP @3792 ; (both paths converge)
@3792 -> @3620: for d in doorsOpen[]: if NOT d.ignoreBlackout: d.doorOpen(true)   ; <-- drives Adoor_C
@3843 (blackout path): disabled := true ; buttonsVisibility() ; ...
```

**Decisive: YES — the panel propagates downstream.** `buttonsVisibility` writes
`AserverBox_C::setActive`, drives `Atrigger_lightRoot_C` (runTrigger/setActive) +
`gamemode.baseSockets`, calls `cordPlugged/cordUnplugged` on `wallMounts`/`miscObjects`,
and calls `gamemode.setPower(5)`; the gamemode delegate then re-enters `powerChanged`
which opens the `doorsOpen[]` `Adoor_C` array. **All of servers / lights / doors are
already synced by their own coop channels** (ApplianceState serverBox, LightState
lightRoot, DoorState doors). Re-running this fan-out on a remote peer would
double-drive them → fight those channels. **A mirror must NOT trigger the fan-out.**

**Gating summary (what suppresses the fan-out):**
- `disabled` (false at BeginPlay @98; true only in blackout @3843) hard-gates stages
  @1441, @1985, and (via `disabled OR !gensFine`) the `gamemode.setPower` @3812.
- `check_calc` / `check_light` (mirror of the last-applied press_calc/press_light)
  change-detect-gate the servers (@1461) and lights (@2000) loops: **if
  `press_calc==check_calc` the servers loop is skipped; if `press_light==check_light`
  the lights loop is skipped.**
- The cord stage @3909 and `moveLevers()` @3894 and the LED/bulb repaint are **NOT**
  gated by `check_*`/`disabled` — they run on every `buttonsVisibility()` call.

---

## 5. Q5 — `powerChanged` ARG ↔ press_ MAPPING (confirmed)

`powerChanged(active_calc, active_downl, active_coords, active_play, active_light)`
(`@0`-`@90`) stores its 5 params and jumps to ubergraph @3777. The body @3777 reads
ONLY `active_light` (to gate the blackout-door loop) and does **not** write any
`press_*` (it is the delegate *callback*, the consumer, not the toggler). The
producer side is `buttonsVisibility @3812 gamemode.setPower(press_calc, press_downl,
press_coords, press_play, press_light)` — i.e. the gamemode's `setPower`/`powerChanged`
multicast is fed **by name** from the press_ bools in this exact order:

| `powerChanged` / `setPower` param | fed from | press_ offset |
|---|---|---|
| `active_calc`   | `press_calc`   | 0x0383 |
| `active_downl`  | `press_downl`  | 0x0381 |
| `active_coords` | `press_coord`  | 0x0380 |
| `active_play`   | `press_play`   | 0x0382 |
| `active_light`  | `press_light`  | 0x0384 |

**Decisive: the by-name mapping in the brief is CONFIRMED** (verified from the
`gamemode.setPower(press_calc, press_downl, press_coords, press_play, press_light)`
call at @3812 — the arg order is calc, downl, coords, play, light, matching the
param names). NOTE: `powerChanged` does NOT itself write the press_ bools; it is the
gamemode→panel notification that the 5-state changed (and only `active_light` is used
in this panel's callback — for the blackout doors). **`powerChanged` is therefore
NOT a usable "set the panel" verb** — it is a downstream notification. (Calling it on
a remote peer would just run the door-blackout loop, not set the levers/LEDs.)

---

## 6. Q6 — DISABLED / waterlogged / tutorial GATE on presses

- **Press toggle path gates ONLY on `tutorial`** (@2559 `IFNOT(tutorial) JUMP @2593`):
  if `tutorial` is set, the press shows a hint and does NOT toggle; otherwise it
  toggles the hit press_. 
- **`disabled` does NOT gate the press toggle** — presses still flip press_ when
  `disabled`. `disabled` only gates the *fan-out* (§4) and the `isOn` look (@3178).
  `disabled` is false at BeginPlay (@98), set true only by the blackout/virus path
  (@3843).
- **`waterlogged` is referenced NOWHERE in the ubergraph or buttonsVisibility** —
  it does not gate presses (vestigial / used elsewhere).
- `isButtonUsed` → always `failed=false` (the panel is always usable).

**Implication for the receiver:** writing press_ + refreshing visuals will not be
"fought" by a `disabled`/`waterlogged` gate on the input side. The one caveat:
if the panel is genuinely `disabled` (blackout) on one peer, its LEDs render OFF
(`isOn=0`, text hidden) regardless of press_ — so for visual fidelity during a
blackout the mirror would also need `disabled` mirrored. Blackout is a base-wide
gamemode state (driven via the same gamemode `powerChanged`/lightRoot path the
LightState channel already syncs), so v1 can ignore `disabled` and rely on the
press_ mirror; revisit only if a blackout desync is observed.

---

## 7. Q4 — THE DESIGN ANSWER: minimal remote-apply recipe

**Goal:** given a target 5-tuple `(press_coord, press_downl, press_play, press_calc,
press_light)`, make the remote panel's **levers + LEDs match**, WITHOUT re-driving
the base (servers/lights/doors/cords are synced by their own channels).

### SENDER (host/origin) read recipe — the 5 bools, canonical order
Poll the 5 press_ bools on the `ApowerControl_C` actor (it is keyed by the inherited
`AtriggerBase_C::Key` @ **0x0260** — save-persistent, cross-peer stable, same identity
the door/garage channels use). Canonical wire order (matches `setPower`):

```
[0] press_calc   @0x0383
[1] press_downl  @0x0381
[2] press_coord  @0x0380
[3] press_play   @0x0382
[4] press_light  @0x0384
```

Symmetric, no auto-revert (press_ is latched until re-pressed → no tick re-drive),
so this is a **Symmetric** keyed channel exactly like the garage/lights (poll, not
observe — the toggle dispatches BP-internally and bypasses ProcessEvent).

### RECEIVER apply — two valid recipes (pick by whether the cord tail matters)

**RECIPE A (RECOMMENDED — visual-only, ZERO fan-out, fully isolated):**
1. Write the 5 press_ bools directly to their offsets (0x0380-0x0384).
2. Call **`moveLevers()`** (ufunction, no args) → animates `lever_0..4` to match.
3. Refresh the LEDs/bulbs WITHOUT `buttonsVisibility` — drive them directly from the
   wrapper, replicating @116-1346 (per subsystem):
   - `eff_<sys>_on.SetVisibility(press_<sys>, false)`
   - `eff_<sys>_off.SetVisibility(!press_<sys>, false)`
   - `powerblock.SetMaterial(slot, press_<sys> ? onBulb : offBulb)` (slots
     3=calc,4=downl,5=coords,6=play,7=light) — OR, simpler and good enough, just set
     `powerblock.SetScalarParameterValueOnMaterials('isOn', 1.0)` and the per-bulb
     particles; the exact bulb material objects (`inst_powerblockBulb_0/1`) are panel
     assets you can fetch once at resolve.

   **This avoids `buttonsVisibility` entirely → no servers/lights/doors/cords/setPower
   re-drive.** It is the cleanest (RULE 1: drive exactly the panel's own visuals,
   nothing downstream). Cost: a little wrapper code to set the 12 particle visibilities
   + 5 materials. The lever half is free (`moveLevers()`).

**RECIPE B (cheaper code, ONE residual side-effect — `buttonsVisibility` with the
change-detect defeated):**
1. Write the 5 press_ bools (0x0380-0x0384).
2. **Pre-set `check_calc := press_calc` (@0x0429) and `check_light := press_light`
   (@0x042A)** so the servers/lights change-detect sees no change.
3. Call **`buttonsVisibility()`**.
   - LEDs/bulbs repaint correctly (ungated, @116-1346). Levers animate
     (`moveLevers()` @3894). Servers loop SKIPPED (`press_calc==check_calc` @1461).
     Lights loop SKIPPED (`press_light==check_light` @2000). `gamemode.setPower` @3812
     is gated by `disabled OR !gensFine` — to skip it as well, this recipe is only
     safe if you ALSO accept that `setPower` may fire (it re-pushes the gamemode
     delegate → blackout-door loop). **Residual:** the **cord stage @3909 always runs**
     (`cordPlugged`/`cordUnplugged` on `wallMounts`/`miscObjects`) and **`setPower`
     @3812 is NOT gated by check_***, so this recipe is NOT fully isolated.
   - **Not recommended** unless wallMounts/miscObjects cords and the gamemode setPower
     are confirmed harmless/idempotent on the receiver. Recipe A is strictly cleaner.

### DO NOT use `powerChanged(...)` as the apply verb
The brief listed `powerChanged(5 bools)` as a candidate setter. **It is not a setter
of the panel** — it is the gamemode→panel *notification callback* (@3777) that, in
this panel, only opens the blackout doors (`doorsOpen[].doorOpen(true)`) based on
`active_light`. It does not write press_, does not move levers, does not set LEDs.
Calling it on the receiver would drive doors (which are separately synced) and do
nothing visual. **Avoid `powerChanged` / `sendPower` / `setPowered` on the receiver.**
(`sendPower`/`setPowered` are likewise base-drive helpers, not panel-visual setters.)

### Net recommended design (RULE 1 / follow-MTA Symmetric keyed channel)
- **ReliableKind:** `PowerControlState` (next free is **35**; the sweep catalog and
  garage RE confirm 33=GarageDoorState, 34=SkyState taken; 32 was a phantom
  GrimeDestroy reservation). Either:
  - **Option A (reuse `KeyedTogglePayload`):** emit 5 sub-keyed toggles
    `"<panelKey>#calc|#downl|#coord|#play|#light"`, receiver accumulates the 5-tuple
    and applies via **Recipe A**. Drops into the existing generic `Channel`.
  - **Option B (one 5-bool `PowerPanelPayload`):** `WireKey key` + 5×uint8 (canonical
    order calc,downl,coord,play,light), one edge, applied via **Recipe A**. One packet
    per change; needs a small bespoke payload + apply. Cleaner on the wire (one panel,
    one packet) — **mild preference for B** since the 5 press_ change together
    conceptually and a single atomic 5-tuple avoids partial-state flicker.
- **Mode:** Symmetric (no auto-revert). Host relays a client edge
  (`IsClientRelayableReliableKind`). Connect-snapshot the 5-tuple to a joiner.
- **Identity:** `AtriggerBase_C::Key` @0x0260 (resolve `Key` against `triggerBase_C`,
  not the subclass — `FindPropertyOffset` does not climb to super; same gotcha
  `ue_wrap/garage.cpp` handles).
- **New `ue_wrap/power_control.{h,cpp}`** (one class = one wrapper): resolve UClass +
  the 5 press_ offsets + `Key` + the `moveLevers` UFunction + the
  `eff_*_on/off`/`powerblock` component pointers; `ReadState` polls the 5 bools;
  `ApplyState` = Recipe A (write 5 press_ → `moveLevers()` → drive 12 particle
  visibilities + powerblock isOn/materials). NO SuppressAutonomy/RequestApply
  (Symmetric).

---

## 8. Field / verb reference (Alpha 0.9.0-n; from CXX dump + powerControl.json disasm)

| Symbol | Class | Offset | Type | Role |
|---|---|---|---|---|
| `Key` | `AtriggerBase_C` | 0x0260 | FName | cross-peer identity (save-persistent) |
| `press_coord` | `ApowerControl_C` | 0x0380 | bool | coords breaker (lever_0, mat slot 5) |
| `press_downl` | `ApowerControl_C` | 0x0381 | bool | downloading breaker (lever_1, mat 4) |
| `press_play`  | `ApowerControl_C` | 0x0382 | bool | playing breaker (lever_2, mat 6) |
| `press_calc`  | `ApowerControl_C` | 0x0383 | bool | calc breaker (lever_3, mat 3) |
| `press_light` | `ApowerControl_C` | 0x0384 | bool | light breaker (lever_4, mat 7) |
| `check_calc`  | `ApowerControl_C` | 0x0429 | bool | last-applied press_calc (servers change-detect) |
| `check_light` | `ApowerControl_C` | 0x042A | bool | last-applied press_light (lights change-detect) |
| `Disabled`    | `ApowerControl_C` | 0x03E8 | bool | blackout gate (fan-out + isOn look only; NOT press) |
| `waterlogged` | `ApowerControl_C` | 0x042B | bool | NOT referenced in press/refresh paths |
| `tutorial`    | `ApowerControl_C` | 0x042C | bool | gates the press toggle (tutorial → hint, no toggle) |
| `areGensFine` | `ApowerControl_C` | 0x0428 | bool | generators-OK (gates fan-out / cord) |
| `lever_0..4`  | `ApowerControl_C` | 0x0368..0x0348 | UStaticMeshComponent* | the 5 lever meshes |
| `eff_*_on/off`| `ApowerControl_C` | 0x02D0..0x0318 | UParticleSystemComponent* | the 10 LED particles |
| `powerblock`  | `ApowerControl_C` | 0x0370 | UStaticMeshComponent* | the bulb-material block (isOn scalar) |
| `power_<sys>` | `ApowerControl_C` | 0x02A8..0x02C8 | UBoxComponent* | the 5 press colliders (`comp` match) |
| `servers` | `ApowerControl_C` | 0x03B0 | TArray<AserverBox_C*> | fan-out target (DON'T re-drive) |
| `lighRoots` | `ApowerControl_C` | 0x0388 | TArray<Atrigger_lightRoot_C*> | fan-out target (DON'T re-drive) |
| `doorsOpen` | `ApowerControl_C` | 0x03C8 | TArray<Adoor_C*> | blackout-door fan-out (DON'T re-drive) |
| `wallMounts`/`miscObjects` | `ApowerControl_C` | 0x0408 / 0x0418 | TArray<…> | cord plug/unplug fan-out |
| `actionOptionIndex` | — | — | UFunction | press handler → ubergraph @3878 (toggles one press_) |
| `buttonsVisibility` | — | — | UFunction | LED/bulb repaint + moveLevers + GATED base fan-out + cords |
| `moveLevers` | — | — | UFunction | levers from press_ (visual-only) — **safe to call on receiver** |
| `powerChanged(5 bools)` | — | — | UFunction | gamemode→panel callback (blackout doors only) — **do NOT use as apply** |
| `setPower(5 bools)` | gamemode | — | UFunction | gamemode 5-tuple push + multicast (producer side) |

---

## 9. Provenance

- **IDA (loaded `VotV-Win64-Shipping.exe.i64`):** confirmed the build is a stock
  UE4.27 **non-nativized** BP Shipping exe — only 2 VOTV exec thunks
  (`execImpactDamageCPP_mainPlayer`@0x14112ca60, `execImpactSquishCPP_mainPlayer`
  @0x14112cc60), zero `ExecuteUbergraph_*`/`execpowerChanged`/etc., no BP names in
  strings → `ApowerControl_C` is BP bytecode, not in `.text`. (Orienting comment
  left at 0x14112ca60.) This is why IDA could not answer Q1-Q6 directly; the BP
  disassembler did.
- **BP disasm (authoritative):** `tools/bp_reflect.py powerControl` →
  `research/bp_reflection/powerControl.json` (+`.functions.txt`), rendered with
  `python research/bp_reflection/_cfg.py powerControl <fn> [entry]`. Key renders:
  `actionOptionIndex`, `player_use`, `powerChanged`, `moveLevers`, `buttonsVisibility`,
  and `ExecuteUbergraph_powerControl` entries @3878 (press), @3777 (powerChanged
  body), @4074 (moveLevers). All offsets above are bytecode offsets in that JSON.
- **SDK (RULE 3):** `Game_0.9.0n/.../CXXHeaderDump/powerControl.hpp` (layout, offsets).
- **Pattern to extend:** `coop/interactable_sync.{h,cpp}`, `ue_wrap/garage.{h,cpp}`
  (closest sibling — Symmetric keyed trigger), `coop/net/protocol.h`
  (`KeyedTogglePayload`, `ReliableKind`), `session_lanes.h`
  (`IsClientRelayableReliableKind`). Sibling RE:
  `votv-garage-door-button-sync-RE-2026-06-08.md`,
  `votv-all-interactables-sweep-catalog-2026-06-08.md` (§2B pre-scopes this as
  `PowerControlState=36`; this RE refines: use **35** per the garage/sweep "next free
  35", and the apply is **Recipe A visual-only**, NOT `powerChanged`),
  `votv-keypad-door-BP-disassembly-2026-06-06.md`.

**Bottom line:** `ApowerControl_C` is pure Blueprint (not in IDA). A press toggles
one `press_*` (by hit collider) + `playSND`, then `buttonsVisibility()` repaints the
LEDs/bulbs (always), moves the levers, and — gated by `disabled` + `check_*` change-
detect — fans out to servers/lights/sockets/cords + `gamemode.setPower` (which opens
blackout doors via the gamemode `powerChanged` delegate). To MIRROR the panel
visually with ZERO base re-drive: **write the 5 press_ bools (calc 0x0383 / downl
0x0381 / coord 0x0380 / play 0x0382 / light 0x0384), call `moveLevers()`, and drive
the 10 LED particles + powerblock materials directly** (Recipe A) — do NOT call
`buttonsVisibility`/`powerChanged`/`sendPower`/`setPowered`. Symmetric keyed channel,
`AtriggerBase_C::Key`@0x0260, `ReliableKind` 35.
