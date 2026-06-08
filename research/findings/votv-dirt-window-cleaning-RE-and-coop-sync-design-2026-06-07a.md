# VOTV dirt / grime / window-cleaning — byte-exact RE + coop sync design (2026-06-07, AGENT A)

**Goal:** sync & mirror (a) the dirt on the base's huge WINDOW, (b) surface grime on
walls/ceiling/floor, and (c) the real-time act of a peer wiping it with a sponge.

**Method:** byte-exact BP disassembly via `tools/bp_reflect.py` (repak + kismet-analyzer) +
the offset-aware CFG (`research/bp_reflection/_cfg.py`). Every claim cites a CFG block offset
in `ExecuteUbergraph_*`. IDA could not help — these are PURE-Blueprint classes (kismet
bytecode in the cooked `.uasset`, no native code; `lookup_funcs` for all six returned "Not
found", as expected). The BP disassembly is the authority. Offsets verified against the CXX
SDK dump (`Game_0.9.0n/.../CXXHeaderDump/*.hpp`).

---

## 0 — The three dirt mechanisms (the headline)

VOTV has **three independent dirt systems**, each with a different state model. The sync must
treat them separately:

| System | Class | Dirt state | Cleaning model | Sync model |
|---|---|---|---|---|
| **Surface grime** (walls/ceiling/floor) | `Agrime_C` (+ ~20 subclasses) | scalar `process`@0x0250 | sphere-overlap → `grime.clean(sponge, Sub)` decrements `process`; `process<0` → self-destruct | **state sync** (per-grime `process` float) + **Element tracking** (grime is UNKEYED) |
| **Panoramic "clean scalar" window** | `AbaseWindow_C` | scalar `clean`@0x0260 | player-hit → `cleanSponge(clean,player,sponge,Hit)` decrements `clean` whole-surface | **state sync** (one `clean` float per window, keyed by save Key) |
| **Signal/picture window** (3-pane base panel) | `Ad_window_C` | **painted pixels in render-target `rt_w`@0x0258** | localized `cleanAtPoint(sponge, Location)` paints a clean brush at the hit UV | **action-replay** (broadcast each clean stroke's world `Location`; receiver replays `cleanAtPoint`) + coarse re-baseline for joiners |

Which one is the user's "main huge window"? See Q1 — almost certainly **`AbaseWindow_C`**
(the panoramic observation window), a whole-surface scalar. The `Ad_window_C` is the
in-base **radio-signal decode panel** (its dirt only exists while a signal is being drawn;
gated on `analogPanels.isPlayingSignal`). Both can be present. Surface grime is the
walls/ceiling/floor case explicitly.

---

## 1 — `Agrime_C` (surface grime) — byte-exact

### Fields (CXXHeaderDump/grime.hpp, VERIFIED)
`UDecalComponent* Decal`@0x0228, `int32 Type`@0x024C, **`float process`@0x0250** (live dirt
amount), `UMaterialInstanceDynamic* dynmat`@0x0258, **`FName cleanParameter`@0x0260** (the
shader scalar name), **`float maxProcess`@0x0268**, `float cleanStrength`@0x026C,
`bool isCleanable`@0x027A, `bool resistRain`@0x027B.

### `clean(sponge, Sub, noSound, &return)` — the decrement (kdec, full body)
```
JUMP_IF_NOT(isCleanable) -> 0x1C1                 ; not cleanable -> return false
CallFunc_Multiply = Multiply(Sub, cleanStrength)
CallFunc_Multiply_1 = Multiply(CallFunc_Multiply, 1.2)
VictoryFloatMinusEquals(process, CallFunc_Multiply_1, ...)   ; process -= Sub*cleanStrength*1.2
dynmat.SetScalarParameterValue(cleanParameter, process/100.0)   ; visual update
PrintString(self, process/100.0, ...)              ; debug spam (always on!)
CallFunc_Less = Less(process, 0)
JUMP_IF_NOT(CallFunc_Less) -> 0x1D1                ; if process<0 ...
  K2_DestroyActor(); return=true                   ;   ... destroy self
; else if noSound -> return; else sound(); return=true
```
- **`process -= Sub * cleanStrength * 1.2`**, clamped implicitly by the `<0` destroy.
- The `sponge` param is NOT read in the math (only carried for the sound). `Sub` is the only
  driver from the sponge side.
- Visual = `dynmat.SetScalarParameterValue(cleanParameter, process/100)` (note `clean` uses
  /100; `applyMaterial` below uses `/maxProcess` — the shader param is the ratio).
- **`process < 0` → `K2_DestroyActor()`** (the grime disappears).

### `applyMaterial` (the dirt VISUAL + init)
```
dynmat = CreateDynamicMaterialInstance(material); Decal.SetDecalMaterial(dynmat)
dynmat.SetScalarParameterValue(cleanParameter, process/maxProcess)   ; *** the visible dirtiness
dynmat.SetScalarParameterValue('scale', Lerp(scaleY, scaleZ, 0.5))
```
`setMaterial` resolves `material = getGrunge_dirt(type)` — so the look is chosen by `type`.
**The visible dirt = `process/maxProcess`.** Mirroring `process` (and `maxProcess`, which is
a per-instance constant) reproduces the look exactly.

### Sub source (sponge → grime), from the sponge hit handler (see §3)
`Sub = getStrength() * 1.5` (@3674). `getStrength` = `Strength`, doubled if `soapAmount>0`.

### Save identity — grime is UNKEYED (decisive for the sync model)
- `gatherDataFromKey` → `gather=false, loadTransform=false` (NOT a key-gathered save actor).
- `getData` → `data.key = n'None'` — **grime has no Key** (always None). It saves `[process]`
  (floats) + `[type]` (ints) + class + transform.
- `ignoreSave → true` and `getPrimitiveData` packs `MakeJson([type, process])` — grime
  persists as a JSON primitive blob indexed by save-slot order, NOT a stable Key.
- `loadData`/`loadPrimitiveData` restore `type`+`process` then call `applyMaterial()`.

**Consequence:** grime CANNOT ride the keyed-interactable Channel (no Key). It is exactly the
shape of the non-keyable trash clump → it must be a **host-allocated Element** (eid), tracked
like NPCs/props. Identity across peers = the host's eid, established at spawn/connect-snapshot.

### Placement / spawn
- Hand-authored grime is placed in the level (loaded with the world; both peers' saves load
  their own, which is why they diverge → connect-snapshot needed).
- Runtime grime is spawned by `AgrimeProjectile_C` (`BeginDeferredActorSpawnFromClass(grunge)`
  + `FinishSpawningActor` on impact — blood/oil/wine splatter) and `oilStainSpawn`. These go
  through the SAME `BeginDeferredActorSpawnFromClass` path the host already observes for NPCs
  + lightning, so the host's spawn observer can catch them.
- `Abase Cleaner_C` is a CLEANUP volume (BoxOverlap → K2_DestroyActor of `cleanObjects`
  classes, then self-destruct) — the "clean the base" task remover. Not a dirt source.

---

## 2 — `AbaseWindow_C` (the huge scalar window) — byte-exact

### Fields (baseWindow.hpp, VERIFIED)
`UStaticMeshComponent* StaticMesh1`@0x0250, **`float clean`@0x0260** (dirt amount, mesh
`window2_window1`), `FVector cleanVec`@0x0264 (last-wiped spot, debounce only).

### `setClean()`
```
StaticMesh1.SetCustomPrimitiveDataFloat(0, clean)   ; push `clean` into shader slot 0
```
Whole-surface; one float drives the entire window's grime shader.

### `cleanSponge(clean, player, sponge, Hit)` → `ExecuteUbergraph_baseWindow(29)` (full CFG)
```
@29  : PrintString(clean)                            ; debug
@127 : IF clean(field) <= 0 -> RETURN                ; already clean
@180 : IF IsValid(player):
@223 :   BreakHitResult(player.hitResult) -> Location
@417 :   IF cleanVec != Location (tol 2.0):          ; wiping a NEW spot?
@668 :     cleanVec = VLerp(cleanVec, Location, 0.5) ; move debounce point, fall to @746
         ; (if player invalid OR new-spot, fall to @746; if same spot, RETURN @460)
@746 : clean -= K2Node_Event_clean * 0.01            ; *** THE DECREMENT (param `clean` = strength)
@825 : clean = FMax(clean, 0)
@894 : PlaySound2D(poop_Cue, 0.1, 2.0)
@954 : setClean()                                    ; push to shader
```
- **`clean`@0x0260 is a single whole-surface scalar.** One wipe lowers it by `param*0.01`,
  clamped ≥0. `cleanVec` is purely a debounce so re-wiping the exact same spot doesn't
  double-decrement (NOT a localized dirt map).
- **WHOLE-surface, not localized.** Q3 answer: state-copy, not action-replay.
- Field `clean` = dirt amount; param `clean` (`K2Node_Event_clean`) = wipe strength.

### Save
`AbaseWindow_C : Aactor_save_C` with `loadData/getData/gatherDataFromKey/setClean`. It IS a
keyed save actor (unlike grime). Identity = its `AtriggerBase_C`/`Aactor_save_C` Key (the same
Key family doors/props use). So baseWindow CAN ride a keyed channel by its save Key.

---

## 3 — `Aprop_sponge_C` — the sponge mechanism end-to-end (byte-exact)

### Fields (prop_sponge.hpp): `power`@0x0378, `Strength`@0x038C, `Size`@0x0390,
`Player`@0x0398, `washCollision`@0x03B8, `soapAmount`@0x03BC, `cleanRadius`@0x03C0.

### `collided(...)` — **EMPTY** (`return` only). All logic is in the ubergraph, dispatched
from `BndEvt__...ComponentHit` (entry **4087**), `playerHandUse_LMB` (entry **4641**),
`playerHandRelease_LMB` (entry 6187).

### Hit handler — `ExecuteUbergraph_prop_sponge` @4087 (the cleaning core, traced)
```
@4087: IFNOT(washCollision) POP                        ; gated on washCollision
@4097: IF col <= 0 ... (per-use cooldown counter `col`)
@4141: PUSH @4579 (fallthrough = collided() no-op)
@4146: vel = VSize(GetVelocity())
@4215: IF vel > 50:                                    ; *** must be MOVING the sponge
@4259:   obj_trigger(...)                              ; splash trigger
@4310:   GetComponentBounds(StaticMesh) -> Origin
@4367:   SphereOverlapActors(Origin, cleanRadius, class=grime_C) -> OutActors
@3332:   for each grime in OutActors:
@3651:     getStrength(&strength)                       ; strength (x2 if soapAmount>0)
@3674:     Sub = strength * 1.5
@3716:     grime.clean(self, Sub, noSound=false, &ret)  ; *** GRIME CLEAN
@4457: IF OtherActor is d_window_C:
@4532:   d_window.cleanPhys(self, Hit)                  ; *** SIGNAL-WINDOW RT CLEAN
```
So **one sponge collision both (a) sphere-cleans nearby grime AND (b) RT-cleans a d_window**
it touches. It's COLLISION-driven (the sponge's physical mesh hitting the surface), velocity-
gated (>50 cm/s) and cooldown-gated (`col<=0`). There is NO per-frame Tick cleaning — cleaning
fires on physics ComponentHit events.

### `playerHandUse_LMB` @4641 — does NOT clean
Arm line-trace → if hit `waterVolume_C` (wets the sponge: `power=1`, plays `wade_Cue`, montage)
or `prop_bucket_C` (dip). This is the sponge-recharge/animation path, not cleaning.

### `wash(replace, sub, &out, &dynmat, &size)`
Consumes the sponge's own `power` (`power -= ...; power = FMax(power,0)`), sets the sponge
`dynmat`'s `'opac'` param (visual wetness), spawns bubbles. Called BY `d_window.cleanAtPoint`
to charge the brush opacity — NOT a cleaner of the surface itself.

### Where `AbaseWindow_C::cleanSponge` is called from
Not from the sponge ubergraph (that branch handles only `grime_C` + `d_window_C`). `cleanSponge`
takes `player`+`Hit` and reads `player.hitResult`, so it is driven by the **player's interaction
trace** (the player's "use sponge on baseWindow" verb, in `AmainPlayer_C`'s interaction handler
— not in the disassembled set, but the signature + `player.hitResult` read prove the player is
the caller). For the sync this is immaterial: we replicate the RESULT (`clean` scalar), and the
host's own player runs the real `cleanSponge` natively.

---

## 4 — `Ad_window_C` (signal/picture window) — byte-exact RT paint

### Fields (d_window.hpp, VERIFIED): `dynmat`@0x0248, `Ratio`@0x0250, **`rt_w`@0x0258**
(UTextureRenderTarget2D), `canv`@0x0260 (UCanvas), `Size`@0x0278 (FIntPoint), `cv`@0x0288
(bool). Meshes: `newbaseWindow2_sig2_newbaseWindow3_sig_002/003/004` (3 panes in the base).

### `dirty()` — dirt ACCUMULATION = random brush into rt_w
```
IFNOT(getMainGamemode().analogPanels.isPlayingSignal) RETURN    ; dirt only while decoding a signal
IFNOT(cv) RETURN
BeginDrawCanvasToRenderTarget(rt_w) -> Canvas
b = MakeVector2D(RandomFloatInRange(10,200), <same>)            ; random splotch size
... random position over (size+200) ...
Canvas.K2_DrawMaterial(mat_cleanBrush_glassDirt, randPos, b, ..., RandomFloatInRange(0,360))
EndDrawCanvasToRenderTarget()
```
Dirt builds up as **random `mat_cleanBrush_glassDirt` splotches painted into `rt_w`**. NON-
deterministic (RandomFloatInRange). Accumulation is event-driven (`dirtify()` schedules it).

### `cleanAtPoint(sponge, Location)` → `ExecuteUbergraph_d_window(831)` — localized CLEAN paint
```
@831 : IFNOT(analogPanels.isPlayingSignal) -> addHint + RETURN   ; only while signal playing
@1055: IF sponge.power > 0:
@1121:   sponge.wash(0.2, 0.01, &FloatOut, &dynmat, &size)        ; charge brush from sponge
@1347:   local = InverseTransformLocation(GetTransform(), Location); BreakVector -> X,Z
         ... maps local X,Z -> render-target pixel `a` (deterministic affine of Location) ...
@561 : canvas(); canv.K2_DrawMaterial(wash_dynmat, a, size*2, ..., centre 0.5,0.5)  ; *** CLEAN STROKE
```
- **`cleanAtPoint(sponge, Location)` is the single canonical replay entry**: given a world
  `Location` + a sponge, it paints ONE clean stroke into `rt_w` at the UV derived from
  `Location` by `InverseTransformLocation` (deterministic — same world Location + same window
  transform ⇒ same pixel on every peer).
- `cleanOnHit` (2179) spawns an `Acleanball_C` projectile (camera→hit) that flies and calls
  back `spogen.cleanPhys(sponge, Hit)` on its sphere-hit — a ranged squeegee.
- `cleanPhys` (2914) is the physical-touch path; also gated on `isPlayingSignal`; resolves a
  `Location` from the Hit and converges on the same `cleanAtPoint`/@561 draw.

**Q2/Q3 for d_window:** dirt lives in `rt_w` PIXELS (a render target), cleaned LOCALLY at the
hit UV. This is the ONLY surface needing action-replay. The `Ratio`@0x0250 scalar exists but
the dirt the player sees is the RT contents.

---

## Answers to Q1–Q7

**Q1 — which is the "main huge window"?** Almost certainly **`AbaseWindow_C`** — the panoramic
base observation window (single huge pane, mesh `window2_window1`/`inst_window_small`), whose
dirt is the whole-surface scalar `clean`. `Ad_window_C` is the in-base **radio-signal decode
panel** (3 sub-panes `newbaseWindow3_sig_002/003/004`); its dirt only exists while decoding a
signal (`analogPanels.isPlayingSignal`). BOTH may be present. I could NOT get an exact level
instance count — the extracted `NewWorld.umap` files are 1.9 KB stubs/redirectors; the real
persistent level/sublevels weren't extracted (and IDA has no BP instances). Instance count is
not load-bearing for the design (both classes are handled). **Recommend confirming hands-on
which window the user means**, but the design covers all three classes regardless.

**Q2 — where does window dirt live?** `AbaseWindow_C`: scalar `clean`@0x0260 (→
`SetCustomPrimitiveDataFloat(0)`). `Ad_window_C`: painted pixels in `rt_w`@0x0258 (render
target), plus an unused-for-dirt `Ratio` scalar. They are DIFFERENT models.

**Q3 — whole-surface or localized?** `AbaseWindow_C` = WHOLE-surface (one scalar, `cleanVec`
is only a debounce). `Ad_window_C` = LOCALIZED (RT brush at the hit UV). `Agrime_C` = per-actor
whole-decal scalar (`process`), localized only in that each grime decal is a separate small
actor.

**Q4 — surface grime.** `process`@0x0250 is the dirt amount; `clean(sponge, Sub,...)`:
`process -= Sub*cleanStrength*1.2`; `process<0` → `K2_DestroyActor`. Trigger = sponge
ComponentHit → velocity>50 → `SphereOverlapActors(grime_C, cleanRadius)` → `grime.clean(self,
getStrength()*1.5)`. `Sub` derives from sponge `Strength` (×2 if `soapAmount>0`) ×1.5. Grime
is **UNKEYED** (key=None, gather=false, ignoreSave=true; persists as a JSON primitive
`[type,process]`). Placement = hand-authored in level OR runtime via `grimeProjectile`/
`oilStainSpawn` (both `BeginDeferredActorSpawnFromClass`).

**Q5 — sponge end-to-end.** `collided` empty; logic in ubergraph. ComponentHit (4087): gated
`washCollision` + cooldown `col<=0` + velocity>50; sphere-overlap grime → `grime.clean`; and
if OtherActor is d_window → `cleanPhys`. LMB-use (4641) = water/bucket recharge + arm montage,
NO clean. No per-frame Tick clean. Final clean calls: `Agrime_C::clean`, `Ad_window_C::
cleanAtPoint`/`cleanPhys`/`cleanOnHit`, `AbaseWindow_C::cleanSponge` (player-driven).

**Q6 — accumulation authority.** Grime: event-driven (level placement + `grimeProjectile`
throws + story); NOT a passive timer. `resistRain` exists (rain can clean/affect non-resistant
grime) — so the day-night/weather actors can lower grime; that's host-side, deterministic-ish
but a host concern. `Ad_window` dirt: `dirtify()`/`dirty()` paint RANDOM splotches
(`RandomFloatInRange`) while a signal plays — **non-deterministic**, so accumulation must be
host-authoritative (the host paints, peers don't roll their own). `AbaseWindow` `clean` only
changes via cleaning (no auto-accumulation seen).

**Q7 — persistence / world-load.** All three load from the JOINER'S OWN save: `AbaseWindow_C::
loadData` restores `clean`; `Agrime_C::loadData`/`loadPrimitiveData` restore `process`+`type`
then `applyMaterial`. A joining client therefore boots with ITS OWN dirt baseline, which can
differ from the host (different save progression) — the SAME divergence prop_snapshot.cpp
exists to fix. Hence a **connect-snapshot is mandatory** for all three.

---

## DESIGN — minimal host-authoritative coop sync (fits weather/interactable/prop/snapshot)

**MTA precedent shapes:**
- Grime + baseWindow scalar state → MTA **per-entity sync from the single authority +
  connect-time bulk** (`CObjectSync`/`CElementRPC` continuous-value push; the new-joiner
  receives the world via `CMapManager`/InitialDataStream). Our local precedent = `weather_sync`
  (host poll → broadcast-on-change + connect-snapshot) and `interactable_sync` Channel.
- d_window stroke replay → MTA **input/effect replication** (the non-authority sends the EDGE,
  every peer replays the deterministic effect): `CClientGame` projectile/effect replication;
  our local precedent = `keypad_sync` input-replay + `interactable_sync` symmetric edge relay.
- Grime as an Element → MTA **CClientObject under the element id space**; our `coop::element::
  Registry` Prop/Npc tracking + `prop_snapshot`.

### PART A — surface grime (`Agrime_C`): Element-tracked host-authoritative `process` stream

Grime is unkeyed, runtime-spawnable, and self-destructs — identical constraints to NPCs. Build
a **`coop::grime_sync`** module mirroring `npc_sync` + `weather_sync`:

1. **Tracking (Element).** Host registers every live `Agrime_C` (descendant check) as a
   **Grime Element** (host eid) via a single GUObjectArray seed walk at Install + a POST
   observer on `BeginDeferredActorSpawnFromClass`/`Agrime_C::ReceiveBeginPlay` to catch
   runtime spawns (`grimeProjectile`). This is exactly `npc_sync::RegisterExistingWorldNpcs` +
   the spawn interceptor, reused. (New `ue_wrap::grime` wrapper: `IsGrime`, `ReadProcess`,
   `ReadMaxProcess`, `ReadType`, `GetTransform`, `CallClean(sponge,Sub)`/`SetProcess`,
   `Destroy` — one class, reflected thunks, no gameplay logic.)

2. **Spawn replication.** On host-side grime spawn → broadcast an **EntitySpawn-style** packet
   carrying className (`grime_oil_C`…), eid, transform, AND `type`+`maxProcess`+initial
   `process`. Client materializes the mirror grime (`BeginDeferredActorSpawnFromClass` +
   set type/process + `FinishSpawningActor` + `applyMaterial`), AI/autonomy N/A (grime has no
   AI). Register the mirror under the host eid (Registry::RegisterMirror).

3. **State stream (the dirty/clean amount).** HOST polls each tracked grime's `process` each
   net-pump tick (cheap float read over the maintained Element set — NOT a per-frame
   GUObjectArray walk; the set is index-maintained like interactable_sync). Broadcast on
   CHANGE (FNV1a dedup like weather_sync) a **`GrimeState`** delta: `{eid, process}`. Batch
   multiple grime into one datagram (like the NPC pose batch) since a sponge swipe cleans
   several at once. Lane = reliable (state must not be lost — a dropped "now clean" leaves a
   ghost decal); or unreliable+periodic-resync if volume is high. **Reliable-on-change** is
   the right default (grime changes are rare/bursty, not continuous).

4. **Apply (receiver).** Resolve grime by eid; **directly write `process`** + call
   `applyMaterial()` (or `SetScalarParameterValue(cleanParameter, process/maxProcess)`) — do
   NOT call `clean()` on apply (its `*cleanStrength*1.2` math + Random-free but PrintString +
   self-destruct-at-<0 belong to the authority). When the host's `process` reaches the
   destroy threshold, the host's grime self-destructs → host broadcasts an **EntityDestroy**
   `{eid}` (death-watch like `trash_collect_sync`, since the self-`K2_DestroyActor` is
   BP-internal and bypasses the K2_DestroyActor observer) → client destroys its mirror. This
   is the SAME eid-routed destroy `prop_snapshot`/clump uses (`PropDestroy{key=None,eid}`).

5. **Connect-snapshot.** `QueueConnectBroadcastForSlot(slot)`: host re-sends EntitySpawn(+
   process) for every tracked Grime Element to the joiner (mirrors `npc_sync::
   QueueConnectBroadcastForSlot` + `prop_snapshot` per-slot drain). The joiner FIRST destroys
   its own save-loaded grime that the host doesn't have (host-authoritative world), then
   spawns/sets the host's. Simplest robust rule (RULE 1): the joiner's grime world is REPLACED
   by the host's — destroy local unmatched grime, spawn/process-set host grime. (Matches the
   interactable connect-snapshot "send full state, receiver idempotently converges" + the
   prop snapshot model.)

> Why host-authoritative and not symmetric: grime accumulation has host-only inputs
> (`grimeProjectile` story throws, rain via `resistRain`), and `process<0` self-destruct is an
> authority decision. A symmetric poll would fight on the destroy edge. Doors proved symmetric-
> on-autonomy oscillates; grime has the same self-driving (rain) property → HostAuth.

### PART B — `AbaseWindow_C` scalar window: keyed host-authoritative `clean` float

This is the cheapest case — one float per window, and it HAS a save Key.

- Reuse the **keyed channel shape** but for a FLOAT, not a toggle. Either (preferred, RULE-2-
  minimal) extend a tiny **`coop::window_sync`** that polls each `AbaseWindow_C`'s `clean`
  scalar per tick and broadcasts on change a `{WireKey, float clean}` packet keyed by the
  window's `Aactor_save_C` Key; OR add a `FloatState` variant to `interactable_sync`. Given
  `clean` is a continuous float (not a bool), a dedicated `window_sync` (host-auth, like
  weather) is cleaner than bending the bool Channel.
- HOST polls `clean` (catches the player wipe + any script). Receiver writes `clean` directly
  + calls `setClean()` (which does `SetCustomPrimitiveDataFloat(0, clean)` — a pure setter,
  game-thread). Connect-snapshot: send every window's current `clean` to the joiner.
- Authoritative model: **HostAuth** if a client can also wipe this window (the host runs the
  real `cleanSponge`; a CLIENT wipe sends a request edge OR — simpler — since `clean` is a
  monotone-decreasing whole-surface scalar with no autonomy, **Symmetric** also works: each
  peer broadcasts its resulting `clean`, lower-wins. Symmetric is fine here because, unlike
  doors, nothing re-raises `clean` locally (no autoclose-equivalent), so no oscillation. Pick
  **Symmetric, host-relays-client-edges** to match interactable_sync's default and give
  real-time client wiping for free.) Identity = the window save Key (cross-peer stable).

### PART C — `Ad_window_C` signal panel: action-replay of clean STROKES + host-painted dirt

Dirt is RT pixels painted at random (non-deterministic) — you canNOT copy `rt_w` over the wire
(megabytes; RULE-flagged anti-pattern). Split authority by direction:

- **Dirt accumulation = HOST-authoritative, replayed.** `dirty()` rolls Random splotches; let
  ONLY the host roll. Observe the host's `dirty()`/`dirtify()` and broadcast each splotch's
  parameters (position seed + size + rotation, the 4 RandomFloat outputs) OR — simpler — since
  the splotch params are computed in BP-internal Random, intercept at the draw and broadcast
  the resolved `{window Key, randPos, size, rotation}`; the client replays the same
  `K2_DrawMaterial(mat_cleanBrush_glassDirt, …)` into its own `rt_w`. (If interception is hard,
  fall back: suppress client `dirty()` and periodically resend a COARSE dirtiness level — but
  the RT has no single scalar, so stroke-replay is the correct model.)
- **Cleaning = action-replay (symmetric, host-relays).** When ANY peer's sponge runs
  `cleanAtPoint(sponge, Location)` (or `cleanPhys`/`cleanOnHit` resolve a Location), broadcast
  a **`WindowCleanStroke`** packet: `{WireKey window, FVector worldLocation, float subStrength}`.
  Every receiver calls `Ad_window_C::cleanAtPoint(localSponge, worldLocation)` on its matching
  window (resolved by Key) → each peer paints its own `rt_w` at the deterministic UV (the UV is
  `InverseTransformLocation(window.transform, worldLocation)` — identical on all peers). Use a
  shared/dummy local sponge for the replay (the receiver may not have the sender's sponge; the
  sponge is only read for `power` to charge brush opacity — pass a fixed strength so the brush
  is full). **Observation point:** the sponge's BndEvt/cleanPhys path is BP-internal
  (CallFunction → ProcessInternal, bypasses ProcessEvent — proven for doors), so a POST
  observer won't fire. Mirror the **keypad_sync / interactable poll** approach: either (a) hook
  the player interaction edge (`AmainPlayer_C` use, like the door E-press observer) and derive
  the Location, or (b) — most robust — observe `Ad_window_C::cleanAtPoint` itself if it's
  ProcessEvent-dispatched (it's a BlueprintCallable event → likely dispatches; VERIFY with a
  one-frame observer probe; if it's CallFunction-internal, fall back to the player-edge hook).
- **Joiner-missed-strokes.** Strokes are transient paints; a joiner can't replay history. The
  host-painted dirt is the baseline, so on connect the host **re-sends its current accumulated
  dirt as a coarse re-paint** — simplest correct approach: on connect, the host CLEARS the
  joiner's window (`prepareRT()` re-creates a blank `rt_w`) then **re-broadcasts the set of
  dirt splotches it currently has** (host keeps a list of the splotch params it rolled since
  the last `prepareRT`), and the joiner replays them. Since the signal window's dirt only
  matters during an active signal decode and is short-lived, an acceptable RULE-1 baseline is:
  on connect, host sends "blank + current splotch list"; thereafter live strokes (dirt + clean)
  replay symmetrically. (If maintaining the splotch list is too invasive, the fallback is
  "joiner blanks its window to match host's `isPlayingSignal` state" — acceptable because this
  window is a minor mechanic vs the huge window + walls.)

### Protocol additions (protocol.h, bump `kProtocolVersion` 40 → 41)

New `ReliableKind`s (reliable lane; grime/window state must not drop):
- `GrimeSpawn` — `{className(WireClassName), eid(u32), loc/rot, type(i32), maxProcess(f),
  process(f)}` (host→all; spawn + connect-snapshot). Or reuse `EntitySpawn` with a grime flag.
- `GrimeState` — batch `{count, [ {eid(u32), process(f)} … ]}` (host→all, on-change). MTU-capped
  like the NPC pose batch.
- `GrimeDestroy` — `{eid(u32)}` (host→all, death-watch). Or reuse eid-routed `EntityDestroy`.
- `WindowScalarState` — `{WireKey, clean(f)}` for `AbaseWindow_C` (symmetric, host-relays).
- `WindowCleanStroke` — `{WireKey, worldLoc(FVector), subStrength(f)}` for `Ad_window_C`
  (symmetric, host-relays). Optionally `WindowDirtStroke` — `{WireKey, rtPos(FVector2D),
  size(f), rot(f)}` for host-painted accumulation.

All host-originated state packets trust-gate `senderPeerSlot==0`; the two symmetric `Window*`
kinds go in `IsClientRelayableReliableKind` so the host relays a client's wipe to other clients
(real-time mirroring). Grime is **Element**-tracked (eid), NOT keyed-interactable. baseWindow +
d_window are **keyed** by their save Key (ride a Key, no Element needed).

### Engine UFunctions our `ue_wrap` calls to APPLY a remote action (all GAME-THREAD-ONLY)

- Grime: write `process`@0x0250 (direct) + `Agrime_C::applyMaterial()` (or
  `dynmat.SetScalarParameterValue(cleanParameter, process/maxProcess)`); destroy via
  `K2_DestroyActor`. (Do NOT call `clean()` on the receiver — authority-only.)
- baseWindow: write `clean`@0x0260 (direct) + `AbaseWindow_C::setClean()`.
- d_window: `Ad_window_C::cleanAtPoint(sponge, FVector Location)` to replay a clean stroke;
  `Ad_window_C::dirty()` / `K2_DrawMaterial`-equivalent to replay a dirt splotch; `prepareRT()`
  to blank on connect. All are BlueprintCallable events on a game-thread actor.

Param layouts come from the CXX dump (e.g. `cleanAtPoint(Aprop_sponge_C* sponge, FVector
Location)`). Confirm each is ProcessEvent-dispatchable for our reflected `ue_wrap::Call`
(BlueprintEvent → yes); the SENDER detection (not the apply) is the part that's BP-internal and
needs the poll/player-edge approach above.

### Anti-patterns explicitly avoided
- **No per-frame GUObjectArray walk** — grime/window sets are index-maintained (seed-once +
  observer insert/evict), polled over the small live set, exactly like interactable_sync.
- **No render-target over the wire** — d_window uses action-replay of deterministic strokes,
  never a pixel copy.
- **No per-pixel / per-decal-vertex sync** — grime is one float (`process`); the window scalar
  is one float; d_window is a stroke list, not pixels.
- **No `clean()` re-invocation on the receiver** for grime (would double-apply the
  `*cleanStrength*1.2` + re-trigger self-destruct). Write the resolved `process`.
- **No symmetric poll on a self-driving value** — grime (rain-cleanable, self-destructing) is
  HostAuth; only the inert scalars (baseWindow `clean`) + the deterministic stroke replays are
  symmetric.

---

## Could NOT determine (flagged)
1. **Exact level instance count / which physical window the user means.** Extracted umaps are
   1.9 KB stubs; the real level wasn't unpacked, and IDA has no BP instances. Design covers all
   three classes; recommend a quick hands-on `FindObjectByClass` count of `AbaseWindow_C` /
   `Ad_window_C` / `Agrime_C` on the live world to confirm.
2. **Whether `Ad_window_C::cleanAtPoint` / `AbaseWindow_C::cleanSponge` dispatch via
   ProcessEvent** (so a POST observer fires) or CallFunction-internal (needs the player-edge
   poll). The door/keypad precedent says BP-internal verbs bypass ProcessEvent → assume the
   poll/player-edge model; VERIFY with a one-frame observer probe before building the SENDER.
3. **The exact `AmainPlayer_C` interaction call site** that invokes `baseWindow.cleanSponge` —
   not in the disassembled set (mainPlayer is huge). Immaterial for apply (we replicate the
   `clean` result); relevant only if we choose to observe the player edge for d_window strokes.
4. **`resistRain` / rain-cleans-grime path** — `resistRain` exists but the rain→grime cleaning
   driver wasn't traced; if rain lowers grime, that's host-side and the host-authoritative
   `process` stream covers it automatically (host computes, peers mirror).
