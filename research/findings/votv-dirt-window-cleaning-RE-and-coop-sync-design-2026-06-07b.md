# VOTV dirt / grime / window-cleaning — byte-exact RE + coop sync design (2026-06-07b, Agent B)

**Task:** RE the DIRT/GRIME/WINDOW-CLEANING system and design a coop sync that mirrors dirt on
the base's huge window + walls/ceiling/floor, and mirrors a peer's sponge-cleaning in real time.

**Method:** byte-exact Blueprint disassembly via `tools/bp_reflect.py` (repak + kismet-analyzer) +
the offset-aware CFG renderer `research/bp_reflection/_cfg.py`. Every claim below cites the function
+ ubergraph byte offset. Cross-checked against the CXX reflection dumps
(`Game_0.9.0n/.../CXXHeaderDump/*.hpp`) for field offsets and the existing coop subsystems
(`weather_sync`, `interactable_sync`, `prop_snapshot`, `npc_pose_host`) for the architecture fit.

---

## 0 — HEADLINE

VOTV has **THREE distinct dirt substrates**, each needing a different sync model:

| # | Substrate | Class | State lives in | Cleaning is | Sync model |
|---|-----------|-------|----------------|-------------|------------|
| **A** | base huge window (the "main window" the user means) | `Ad_window_C` | **render-target pixels** (`rt_w`, painted brush stamps) | **localized** (wash brush at hit UV) | **action-replay** (broadcast each dirty-stamp + each clean-stroke; eid identity) |
| **B** | wall/ceiling/floor stains, leaks, oil, blood, graffiti, dust | `Agrime_C` (+22 subclasses) | scalar **`process`** @0x0250 + decal `dynmat` | whole-decal scalar decrement | **host-authoritative state** (`process` per grime, eid identity) + spawn/destroy via the existing `BeginDeferredActorSpawnFromClass` interceptor |
| **C** | a *second*, simpler window class | `AbaseWindow_C : Aactor_save_C` | scalar **`clean`** @0x0260 (CustomPrimitiveData float) | whole-surface scalar decrement | **host-authoritative state** (one float, **KEYED** — rides the existing keyed channel shape) |

The user almost certainly means **A** (`Ad_window_C` — the one with localized sponge-wipe streaks
and the drone-signal picture). **B** covers "walls, ceiling, floor". **C** is a legacy/simple window
variant that is trivially covered as a keyed scalar.

**One-line decision:** build a new `coop::dirt_sync` module (its own `.cpp/.h`, modular rule). It
host-authoritatively streams `Agrime_C::process` (B) + `AbaseWindow_C::clean` (C) as keyed/eid state
(weather-style poll + broadcast-on-change + connect-snapshot), AND action-replays the `Ad_window_C`
render-target strokes (A) — each `dirty()` stamp (with its RNG seed) and each `cleanAtPoint/cleanPhys`
wash (window eid + world Location + sponge params) is broadcast and re-executed on every peer via the
native UFunction. Grime spawn/destroy rides the existing prop/NPC `BeginDeferredActorSpawnFromClass`
interceptor + the Element Registry (grime has **no stable Key** — host-allocated eid identity, exactly
like NPCs / the trash clump).

---

## 1 — Q1: which class is the "main huge window"; what are walls/ceiling/floor?

### `Ad_window_C` — the render-target dirty glass (substrate A) — the "main window"
- `Ad_window_C : AActor` (CXX `d_window.hpp`). Fields that matter:
  `dynmat`@0x0248, `Ratio`@0x0250, `rt_w` (`UTextureRenderTarget2D*`)@0x0258, `canv` (`UCanvas*`)@0x0260,
  `cont` (`FDrawToRenderTargetContext`)@0x0268, `Size` (`FIntPoint`)@0x0278, `cv` (bool)@0x0288.
- Three static-mesh "sig" components (`newbaseWindow3_sig_002/003/004` @0x0228..0x0238) → this is the
  **multi-pane base observatory window**. Placed in `VotV/Content/objects/d_window.uasset`; instances
  are placed in the base level / `baseBuilding.uasset` (referenced there). It owns BOTH the dirt
  render-target AND the drone-signal picture drawing (`setCode`/`getCode`/`picHash`/`gotCode` +
  `texturePicker`) — but **DIRT and PICTURE are two independent draws onto the same `rt_w`**.
- Instance count: small (the base has one big multi-pane window assembly; expect 1–few `Ad_window_C`).

### `Agrime_C` — walls / ceiling / floor / surfaces (substrate B)
Walls/ceiling/floor dirt is **`Agrime_C` decal actors**, NOT extra window surfaces and NOT material
params on the walls. `Agrime_C : AActor` (CXX `grime.hpp`) carries a `UDecalComponent* Decal`@0x0228
that it stamps onto whatever surface it sits on. 22 subclasses (`grime_oil`, `grime_wine`, `grime_blood2`,
`grime_coffee`, `grime_dusty`, `grime_leaky*`, `grime_arirGraffiti`, `grime_fallenLeaves`,
`grime_asoLogoShadow`, `grime_explosionScorch`, `grime_uv`, `grime_gasoline`, …). They are placed both by
the level AND spawned at runtime (see Q6). A floor puddle, a wall leak, graffiti — all `Agrime_C`.

### `AbaseWindow_C` — a simpler scalar window (substrate C)
`AbaseWindow_C : Aactor_save_C` (CXX `baseWindow.hpp`), one `StaticMeshComponent* StaticMesh1`@0x0250,
scalar `clean`@0x0260, `cleanVec`@0x0264. This is a **different, simpler window** whose dirt is a single
scalar pushed to a material via CustomPrimitiveData (no render target). Likely a smaller window / an
earlier implementation. It is trivially synced as a keyed scalar and included for completeness.

---

## 2 — Q2 + Q3: where window dirt LIVES; whole-surface vs localized

### Substrate A (`Ad_window_C`) — **render-target pixels, LOCALIZED** → action-replay
`Ad_window_C::dirty()` (trampoline → `ExecuteUbergraph_d_window` @544; `dirtify()` @3978 just calls
`dirty()`). Disassembly of `dirty` (block @128):
```
@  46: IFNOT(getMainGamemode.analogPanels.isPlayingSignal) JUMP @109   ; skip if a signal is playing
@ 109: IFNOT(cv) JUMP @128                                              ; skip if cv (clear/disabled)
@ 128: BeginDrawCanvasToRenderTarget(self, rt_w, Canvas, Size, Context)
@ 175: RandomFloatInRange(10,200)            -> brush size b (square)
@ 286: RandomFloatInRange(0,360)             -> rotation
@ 454: RandomFloatInRange(0, size+200) x2    -> random position
@ 676: Canvas.K2_DrawMaterial(obj<mat_cleanBrush_glassDirt>, pos, b, (0,0),(1,1), rot, (0.5,0.5))
@ 816: EndDrawCanvasToRenderTarget(self, Context)
```
**Dirt = a `mat_cleanBrush_glassDirt` material stamped at a RANDOM position/size/rotation onto `rt_w`.**
It accumulates pixel-by-pixel; there is **no scalar "dirtiness" field** for A. `Ratio`@0x0250 is the
aspect ratio (set in `prepareRT`/UCS, not a dirt amount).

Cleaning is **localized** to the hit UV. `cleanAtPoint(sponge, Location)` (→ uber @831, clean branch @1055):
```
@1055: IFNOT(sponge.power > 0) POP                                    ; needs a non-empty sponge
@1121: sponge.wash(0.20, 0.01, FloatOut, wash_dynmat, wash_size)     ; depletes sponge, returns brush mat+size
@1347: InverseTransformLocation(GetTransform(), Location)            ; world hit -> local
@ ... : convert local XZ -> canvas UV via Size                       ; (the @1393..1828 vector math)
@  561: canvas()                                                      ; bind canv to rt_w
@  663: canv.K2_DrawMaterial(wash_dynmat, a /*UV*/, size, (0,0),(1,1), 0, (0.5,0.5))   ; ERASE patch at UV
```
`cleanPhys(sponge, Hit)` (→ uber @2914, clean branch @3138) is the same but uses
`FindCollisionUV(Hit, 0)` for the **exact** hit UV (block @3687) and is velocity-gated
(`!IsSimulatingPhysics OR VSize(vel) > 25` AND `sponge.power > 0`, @3138..3567), then `canv.K2_DrawMaterial`
at the UV (@2038) and a 0.1s `Delay` (@1923). `cleanOnHit(sponge, Hit)` (→ uber @2179) does **not** draw
— it spawns a `cleanball_C` projectile along the aim trace (`BeginDeferredActorSpawnFromClass(cleanball_C)`,
`fuckYou`=trace dir ×10000, `spogen`=self, `sponge`=sponge; @2318..2874), which flies and on hitting the
window calls `spogen.cleanPhys` (see Q5).

> **Conclusion A:** localized RT paint. Dirt is not a number — it's accumulated brush stamps; cleaning
> erases a brush-sized patch at the hit UV. ⇒ **state-copy is impossible without shipping the texture;
> use ACTION-REPLAY** (replay each `dirty()` stamp and each `cleanAtPoint/cleanPhys` stroke).

### Substrate C (`AbaseWindow_C`) — **one scalar, WHOLE-SURFACE** → state-copy
`cleanSponge(clean, player, sponge, hit)` → `ExecuteUbergraph_baseWindow(29)`:
```
@127: IFNOT(clean /*field*/ <= 0) JUMP @180     ; already clean -> return
@180: IsValid(player) ...                       ; (if valid, lerp cleanVec toward hit Location 50% — a stored hit pos, NOT the dirt amount)
@746: Multiply(K2Node_Event_clean, 0.01)        ; arg "clean" * 0.01
@788: VictoryFloatMinusEquals(clean, that)      ; *** clean (FIELD @0x0260) -= arg*0.01 ***
@825: FMax(result, 0); @867: clean := that      ; clamp >= 0
@894: PlaySound2D(poop_Cue); @954: setClean()   ; setClean() -> StaticMesh1.SetCustomPrimitiveDataFloat(0, clean)
```
`setClean()`: `StaticMesh1.SetCustomPrimitiveDataFloat(0, clean)` (one line). So C's dirt is the scalar
`clean`@0x0260, pushed to material CustomPrimitiveData index 0. **Whole-surface, one float.** ⇒ state-copy.

---

## 3 — Q4: `Agrime_C` mechanics (substrate B)

`Agrime_C::clean(sponge, Sub, noSound, &return)` — full disassembly (`grime.clean`, 22 stmts):
```
@  0: IFNOT(isCleanable) JUMP @449            ; @449: return:=false  (uncleanable grime is a no-op)
@ 14: Multiply(Sub, cleanStrength)            ; Sub * cleanStrength
@ 60: Multiply(that, 1.2)                     ; * 1.2
@102: VictoryFloatMinusEquals(process, that)  ; *** process (@0x0250) -= Sub*cleanStrength*1.2 ***
@139: Divide(process, 100); @181: dynmat.SetScalarParameterValue(cleanParameter, process/100)  ; drive the decal fade
@371: Less(process, 0)
@405: IFNOT(process < 0) JUMP @465            ; not yet fully clean
@419: K2_DestroyActor(); @433: return:=true   ; *** process<0 -> DESTROY the grime actor ***
@465: IFNOT(noSound) JUMP @484; @484: sound() ; else just play a clean sound
```
- **`process` = dirt amount** (`maxProcess`@0x0268 is the cap; `process` counts down to 0).
- `clean` **decrements** `process` by `Sub × cleanStrength × 1.2`; `Sub` is derived from the sponge (see Q5);
  `cleanStrength`@0x026C is per-grime.
- Visual: `dynmat.SetScalarParameterValue(cleanParameter@0x0260, process/100)` — the decal fades with `process`.
- **Destroy at `process < 0`** via `K2_DestroyActor`.
- Flags: `isCleanable`@0x027A (uncleanable grime ignores the sponge), `resistRain`@0x027B (rain doesn't wash it).

**Identity / save:** grime is a save actor but **has NO stable cross-peer Key**:
- `gatherDataFromKey` → `gather:=false, loadTransform:=false` (`grime.gatherDataFromKey`, byte-exact).
- `getData` stores `class + Transform + name'None' (key) + mFloat[] + mInt[]` (key literally `None`).
- `getPrimitiveData`/`loadPrimitiveData` persist **`process` + `type`** as JSON primitive data.
⇒ Grime identity for coop MUST be a **host-allocated Element id** (it cannot ride the keyed channel).

**Spawned vs placed:** BOTH. Placed by the level; spawned at runtime by `AgrimeProjectile_C`
(`grimeProjectile.uasset`): on its `ReceiveTick` sphere-trace hitting a static surface it
`BeginDeferredActorSpawnFromClass(Grunge /*a TSubclassOf<Agrime_C>*/)` at the impact (block @1239,
`@1619 BeginDeferred… @2041 FinishSpawningActor`). Also `oilStainSpawn.uasset` and story events spawn grime.
All go through `BeginDeferredActorSpawnFromClass` — **the exact UFunction the project already intercepts**
for NPCs/props.

---

## 4 — Q5: sponge end-to-end (the cleaning ACTION)

Two dispatch paths, both off `Aprop_sponge_C`:

**(i) Sponge physically swiped (ComponentHit) → cleans nearby GRIME** —
`BndEvt__…ComponentHit…` → `ExecuteUbergraph_prop_sponge(4087)`:
```
@4087: IFNOT(washCollision) POP                              ; sponge must be in wash mode
@4097: IFNOT(col <= 0) POP                                   ; col = soap cooldown gate
@4146: VSize(GetVelocity()) > 50 ?                           ; must be MOVING (@4215)
@4264: obj_trigger; @4310: GetComponentBounds(StaticMesh)
@4367: SphereOverlapActors(origin, cleanRadius, …, grime_C, OutActors)   ; *** find Agrime_C within cleanRadius@0x03C0 ***
@3651: getStrength(strength); @3674: Multiply(strength, 1.5)
@3716: AsGrime.clean(self, strength*1.5, noSound=false)      ; *** call grime.clean with Sub = sponge strength * 1.5 ***
@4579: collided(...)                                         ; (empty BP hook for subclasses)
```
So the grime `Sub` = `sponge.getStrength() × 1.5`. The sponge does NOT need to hit the grime exactly —
it sphere-overlaps `cleanRadius` and cleans every grime in range, velocity-gated.

**(ii) Sponge / cleanball touches the WINDOW (`Ad_window_C`) → cleans render-target at UV** — the window's
own `cleanOnHit`/`cleanPhys`/`cleanAtPoint` fire (Q2). The sponge's StaticMesh ComponentHit on the window
component triggers the window's collision handler; the sponge also explicitly spawns the `cleanball_C`
(via `cleanOnHit`) for distance cleaning. `wash(Replace, Sub, …)` (`prop_sponge.wash`, byte-exact) depletes
`power`@0x0378 (`power -= clamp; FMax 0`, @127..206) and returns the brush `dynmat` (`name'opac'` scaled by
velocity+strength, @654) + `size` — i.e. it's the per-stroke "consume soap, hand back the brush to draw."

**Per-frame work while held:** none of these are per-frame. They are **edge events** (ComponentHit,
projectile hit). `playerHandUse_LMB/RMB` arm `washCollision`; `cleanBubbles()` is a particle puff. No
per-tick polling of the sponge is needed — the clean is hit-driven.

---

## 5 — Q6: accumulation authority

- **Substrate B (grime):** **event-driven, NOT a self-building timer.** Grime appears via
  `AgrimeProjectile_C` impacts (leaks, thrown grime, gunk), `oilStainSpawn`, and story/level placement —
  all `BeginDeferredActorSpawnFromClass(Agrime_C-subclass)`. `resistRain`@0x027B + rain interaction implies
  rain can *wash* some grime (the cycle drives that), but grime is not self-accumulating on a timer. The
  spawn point is deterministic-ish (projectile physics) but uses sphere traces, so we treat spawns as
  **host-authoritative observed events**, not re-simulated on the client.
- **Substrate A (`Ad_window_C` dirty):** `dirty()`/`dirtify()` use `RandomFloatInRange` (non-deterministic
  position/size/rotation). The CALLER (some base/gamemode timer or event — not in the disassembled small BPs;
  lives in the level/gamemode) decides WHEN to add a stamp. Because the stamp is RNG, **it cannot be
  reproduced independently on each peer** — the host must broadcast the resolved stamp params, OR (simpler)
  the host owns the accumulation and replays its stamps (see design A).
- **Substrate C (`AbaseWindow_C` clean):** the scalar only ever *decreases* (cleaning). Any increase
  (re-dirtying) would be an external writer; treated as host-authoritative state (poll the field).

⇒ **All three: HOST is the accumulation authority.** Clients never originate dirt; they may originate
*cleaning* (their sponge), which we relay (symmetric-with-host-relay, like `interactable_sync`).

---

## 6 — Q7: persistence / world-load divergence (the connect-snapshot requirement)

A joining client loads its **OWN save** (the project's established model — `prop_snapshot.cpp` exists
precisely because the joiner's world diverges from the host's). So on connect:
- The joiner's grime set, window dirt RT, and `baseWindow.clean` reflect the **client's** save, not the host's.
- Existing precedent: `prop_snapshot::StartEnumerationFor` walks the host's keyed-prop Elements and replays
  spawn+pose to the joiner; `interactable_sync::QueueConnectBroadcastForSlot` sends the host's full door/light
  state; `weather_sync`/`time_sync::QueueConnectBroadcastForSlot` push current state. **Dirt needs the same.**

**Connect-snapshot requirement per substrate:**
- **B (grime):** the host must (a) destroy client-only grime that the host doesn't have, and (b) spawn its own
  grime on the joiner + push each `process`. Because grime has no Key, this rides the **Element Registry +
  EntitySpawn path** (host-allocated eid), exactly like the NPC connect-snapshot
  (`npc_sync::QueueConnectBroadcastForSlot`). Divergent client-only grime is the hard part — see design.
- **A (`Ad_window_C` RT):** the render target cannot be diffed. Concrete solution: on connect the host
  **resets the joiner's window RT to clean** (`Ad_window_C` has the canvas ops; a full-RT clear draw or
  re-`prepareRT`) and then **replays the host's accumulated dirty-stamp list** (the host keeps an ordered
  list of the stamps it has applied since session start / save-load) so the joiner's window converges to the
  host's appearance. (Cheaper alternative discussed in design: accept a coarse "dirty count" and let the host
  re-stamp N times — visually close, not pixel-identical; acceptable for dirt.)
- **C (`baseWindow.clean`):** one keyed scalar; push it on connect (it has a Key — `Aactor_save_C`).

---

## 7 — DESIGN

New module **`coop/dirt_sync.{h,cpp}`** (principle 7 gameplay/network layer; modular rule — its own pair,
NOT bolted onto weather_sync/interactable_sync which are already large). Engine access behind a new
**`ue_wrap/dirt.{h,cpp}`** (or extend `ue_wrap/prop` + a small `ue_wrap/window`) holding the reflected
thunks for `Agrime_C::clean`, `Ad_window_C::cleanAtPoint/cleanPhys/dirty`, `AbaseWindow_C::cleanSponge`,
and the field reads (`process`, `clean`). RULE 3: all AOB/reflection-resolved, nothing UE4SS at runtime.

### 7A — Substrate A: `Ad_window_C` render target → **ACTION-REPLAY**
Identity: `Ad_window_C` has no Aprop Key; register each window instance as an **Element (eid)** at index time
(host-allocated; mirror via the Registry the same way the trash clump does). Few instances → cheap.

Two broadcast edges, both reliable (ordered, on a dedicated `Lane::Bulk`-style ordered lane so stamps and
washes apply in order):

1. **Dirty stamp** — host-only. The host hooks the dirt accumulation (a POST observer on `Ad_window_C::dirty`,
   OR — since BP-internal calls bypass our ProcessEvent detour, the same lesson as doors — the host re-routes by
   *driving* `dirty()` itself on a host-owned schedule and broadcasting). Because `dirty()` rolls RNG **internally**,
   a POST observer cannot recover the params. **Root-cause fix:** the host does NOT call the stock `dirty()`;
   instead `dirt_sync` (host) generates the stamp params (pos/size/rot via its own RNG) and calls a **new
   `ue_wrap::window::StampDirt(window, pos, size, rot)`** that performs the same `BeginDrawCanvasToRenderTarget`
   + `K2_DrawMaterial(mat_cleanBrush_glassDirt,…)` + `EndDraw` sequence with EXPLICIT params, then broadcasts
   `DirtStamp{ windowEid, pos.x, pos.y, size, rot }`. Every peer (incl. host-as-applier) runs the identical
   `StampDirt`. ⇒ pixel-identical across peers, no RNG divergence. (This *replaces* the stock self-dirtying on
   coop windows — RULE 2, no parallel "BP rolls its own + we roll ours".) If the stock dirty-timer is BP-internal
   and unsuppressable, suppress it on the client (interceptor on `dirty`) and let the host be the sole stamper.
2. **Clean stroke** — symmetric, host-relayed. The cleaning peer detects its sponge cleaning a window. The
   cleanest hook: a **POST observer on `AmainPlayer_C`'s sponge-use path is not reliable (BP-internal)**, so
   mirror the door pattern — observe the ONE ProcessEvent-visible edge. Here the window's own
   `cleanPhys`/`cleanAtPoint` are BP-internal too. Instead detect cleaning by **polling the window RT-change is
   impossible**; the workable hook is to observe the **`cleanball_C` spawn** (host already can via the
   `BeginDeferredActorSpawnFromClass` interceptor) AND the direct sponge-on-window hit. **Recommended:** add our
   own detour: when WE (local player) run a wash on a window, we are the ones calling the native fn via reflection
   only on the *receiver*; on the *origin* we need the local player's real swipe. The robust origin signal is the
   sponge `wash()` call paired with a window hit — observe `Aprop_sponge_C` `playerHandUse_LMB` (ProcessEvent-
   visible input action) to know the sponge is active, then the window `cleanPhys` ComponentHit. **Simplest
   correct origin detection:** poll, per held-sponge, the window the local player is looking at + LMB-down, and
   when a clean would fire, broadcast `WindowCleanStroke{ windowEid, worldLoc.x/y/z, sponge.power, sponge.Strength,
   sponge.cleanRadius }`. The receiver replays `ue_wrap::window::CleanAtPoint(windowEid→actor, sponge-proxy, worldLoc)`
   which calls the native `cleanAtPoint`/`cleanPhys` (drawing the wash brush at the UV). The host relays a client
   stroke to other clients (`IsClientRelayableReliableKind`). Mirror brush params so the erased patch size matches.

   > UFunction to APPLY a remote clean (game-thread only): `Ad_window_C::cleanAtPoint(sponge, Location)` (preferred
   > — it derives the UV from a world Location, no FHitResult to serialize) or `cleanPhys(sponge, Hit)` (needs a
   > reconstructed FHitResult — heavier). **Use `cleanAtPoint`.** The receiver needs a sponge object: reuse the
   > local player's held sponge if present, else a hidden host-owned `Aprop_sponge_C` proxy with the wire `power`/
   > `Strength`/`cleanRadius` written before the call (so `wash` inside `cleanAtPoint` draws the right brush size).

**Connect-snapshot (A):** on connect the host sends `WindowReset{ windowEid }` (receiver clears the RT via a new
`ue_wrap::window::ResetClean` — re-`prepareRT` / full-canvas clean draw) then replays its **accumulated DirtStamp
list** for that window (host keeps the ordered list it generated this session + a baseline count from the host save).
This converges the joiner's window to the host appearance. (Acceptable simplification if the stamp list is large:
send a single `WindowDirtLevel{ windowEid, stampCount }` and have the receiver re-stamp `stampCount` times with the
SAME shared RNG seed the host used — still deterministic if seeded identically; pixel-identical only if seed-shared.)

### 7B — Substrate B: `Agrime_C` → **HOST-AUTHORITATIVE STATE (eid) + spawn/destroy via interceptor**
- **Spawn:** grime spawns through `BeginDeferredActorSpawnFromClass`. The host's existing PRE observer (the one
  `npc_sync`/`weather_lightning` use) adds an allowlist branch for `Agrime_C` descendants → allocate an Element id,
  `EntitySpawn{ className, eid, loc, rot }`. The client suppresses its OWN grime spawns (interceptor) and
  materializes a mirror grime on receipt (`BeginDeferred + setKey-not-needed + FinishSpawning`) — identical to the
  NPC mirror path. (Grime has no AI; nothing to suppress beyond the spawn echo.)
- **State (`process`):** the host **polls** each tracked grime's `process`@0x0250 once per tick (cheap field read
  over the maintained grime Element set — NOT a per-frame GUObjectArray walk; the set is seeded once + kept via the
  spawn/destroy observers, exactly like `prop_lifecycle`'s keyed-prop set) and broadcasts on a CHANGE
  `GrimeProcess{ eid, process }` (FNV dedup, weather-style). The client direct-writes `process` and calls
  `dynmat.SetScalarParameterValue(cleanParameter, process/100)` (or just re-invokes the visual update) so the decal
  fades to match. This is the **weather/time `host poll + broadcast-on-change` shape**, generalized per-eid.
- **Destroy:** grime self-destroys at `process<0` (`K2_DestroyActor`). On the host that fires the existing
  K2_DestroyActor PRE observer → `EntityDestroy{ eid }`. The client mirror is destroyed by eid (the trash-clump v28
  eid-routable destroy precedent). The host's authoritative `process` stream already drives the client toward 0; the
  explicit destroy guarantees removal.
- **Cleaning origin (client cleans grime):** a client swiping its sponge over grime runs `grime.clean` locally
  (BP-internal). Because doors taught us BP-internal verbs bypass the detour, **don't observe `clean`** — instead the
  host's `process` poll is the single authority: the client's local `clean` lowers ITS mirror's `process`, but the
  AUTHORITATIVE value is the host's. To make a client's clean actually reduce dirt for everyone (host-authoritative,
  MTA single-syncer like doors): the client sends a `GrimeCleanRequest{ eid, sub }` (sub = its sponge strength×1.5);
  the host applies `grime.clean(hostSpongeProxy, sub, noSound=true)` on its authoritative grime → its `process` poll
  broadcasts the new value (and the destroy if it crosses 0). Symmetric relay not needed — host owns `process`.
  (This mirrors `DoorOpenRequest`: client sends the INPUT EDGE, host simulates, host broadcasts state.)

### 7C — Substrate C: `AbaseWindow_C` → **HOST-AUTHORITATIVE KEYED SCALAR**
`AbaseWindow_C` is `Aactor_save_C` → it HAS a Key (`gatherDataFromKey` returns `gather=true`). So it can ride a
keyed scalar channel:
- Host polls each `AbaseWindow_C::clean`@0x0260 (maintained set) → broadcast on change `WindowCleanScalar{ key, clean }`.
- Client direct-writes `clean` + calls `setClean()` (`StaticMesh1.SetCustomPrimitiveDataFloat(0, clean)`).
- Client cleaning → `WindowScalarCleanRequest{ key, amount }`; host applies `cleanSponge`-equivalent (or just
  `clean -= amount*0.01; FMax 0; setClean()`), host poll re-broadcasts. (Same host-authoritative request shape as B/doors.)
- Connect-snapshot: push every `AbaseWindow_C.clean` by Key (trivial — it's keyed, reuse the interactable connect
  shape with a scalar payload).

### 7D — Protocol additions (`include/coop/net/protocol.h`, bump `kProtocolVersion` 40→41)
New `ReliableKind`s (reuse free slots; document each like the existing entries):
- `GrimeSpawn` / `GrimeDestroy` — **OR reuse the generic `EntitySpawn(5)`/`EntityDestroy(6)`** with the grime class
  in `WireClassName` (preferred — grime is just another eid-tracked entity, like an NPC; no new kinds, RULE 2).
  Grime then needs to be a tracked **Element** of a new `ElementType::Grime` (or rides `ElementType::Prop` if the
  Registry tolerates a non-Aprop actor — verify; NPC uses its own type, so add `ElementType::Grime`).
- `GrimeProcess` (host→all, unreliable-ok or reliable-on-change) — payload `{ uint32 eid; float process; }` (8 B).
  *Could* ride an `EntityPose`-style batch if many grime change at once; for a sponge wipe only a few change, so a
  per-eid reliable-on-change (weather dedup) is simplest. If batching is needed, add a `GrimeProcessBatch` mirroring
  `EntityPoseBatchHeader + N×{eid,process}`.
- `GrimeCleanRequest` (client→host) — `{ uint32 eid; float sub; }`.
- `WindowDirtStamp` (host→all, reliable ordered) — `{ uint32 windowEid; float posX, posY; float size; float rot; }` (20 B).
- `WindowCleanStroke` (symmetric, host-relayed) — `{ uint32 windowEid; float x,y,z; float power, strength, cleanRadius; }` (~28 B).
- `WindowReset` (host→joiner) — `{ uint32 windowEid; }`. (Connect-snapshot, substrate A.)
- `WindowScalar` (host→all) + `WindowScalarCleanRequest` (client→host) for substrate C — both keyed:
  reuse `KeyedTogglePayload`'s WireKey shape but with a float instead of the action byte → a new
  `KeyedScalarPayload{ WireKey key; float value; }`.

Trust gates (copy from existing): host-originated kinds (`WindowDirtStamp`, `GrimeProcess`, `WindowReset`, `WindowScalar`)
gate `senderPeerSlot==0`; client requests (`GrimeCleanRequest`, `WindowScalarCleanRequest`) gate `senderPeerSlot!=0`;
`WindowCleanStroke` symmetric + relayable (add to `IsClientRelayableReliableKind`).

### 7E — MTA precedent matched
- **Grime `process` host poll + broadcast-on-change + connect-snapshot:** MTA **map-object / world-state single-syncer**
  — the server owns the authoritative world state and streams it to joiners; matches the project's `weather_sync` /
  `time_sync` (cite `reference/mtasa-blue/.../CMapManager` + the existing `weather_sync.cpp` host-poll shape).
- **Client cleans → request → host simulates → host broadcasts state:** MTA **`CUnoccupiedVehicleSync` /
  `Packet_UnoccupiedVehiclePush` → OverrideSyncer** (the exact precedent already cited for `DoorOpenRequest`). The
  client sends the input edge; the host is the single syncer.
- **Window clean STROKE action-replay (symmetric, host-relayed):** MTA **`CCustomData` / projectile-and-effect sync**
  — a discrete action broadcast + replayed on each client (sibling of `LightningStrike`'s discrete-event replay).
- **Window dirt-stamp deterministic replay:** MTA's rule that **RNG-driven visuals are resolved on the authority and
  the resolved params are sent** (never "each client rolls its own"), matching how `weather_sync` echo-suppresses
  `causeRain`'s internal `RandomFloat` rolls on the receiver.
- **Connect-snapshot:** MTA **`CMapManager::SendMapElementsToPlayer` / `CTransferBox`** — already mirrored by
  `prop_snapshot` + `interactable_sync::QueueConnectBroadcastForSlot`; dirt reuses the same connect-edge hook.

### 7F — Anti-patterns to AVOID (CLAUDE.md audit rule)
- **No per-pixel / render-target wire sync** — never serialize `rt_w`. Action-replay only (7A).
- **No per-frame full-array scans** — the grime poll iterates a *maintained* Element set (seeded once + kept via the
  spawn/destroy observers), NOT a per-tick `FindObjectByClass`/GUObjectArray walk. The index rebuild is throttled
  (≥2 s), exactly like `interactable_sync::Tick`.
- **No wiring the render target** (no `BeginDrawCanvasToRenderTarget` on a hot path) — `StampDirt`/`CleanAtPoint` run
  only on the discrete stamp/stroke edges (host schedule / player swipe), never per tick.
- **No POST observer on the BP-internal clean verbs** (`grime.clean`, `window.cleanPhys`, `door.doorOpen`-style) — they
  dispatch via `CallFunction→ProcessInternal` and bypass our ProcessEvent detour (IDA-proven for doors 2026-06-04).
  Use host state polling + client request edges instead.
- **One feature per file** — `dirt_sync.{h,cpp}` + `ue_wrap/window` (+ grime thunks in `ue_wrap/dirt` or `ue_wrap/prop`);
  do not grow `weather_sync.cpp`/`interactable_sync.cpp`.

---

## 8 — UNDETERMINED / follow-ups (flag to orchestrator)
1. **`dirty()` accumulation CALLER** — the timer/event that calls `Ad_window_C::dirty()` is in the base level /
   gamemode BP (not in the small disassembled set). Need to disassemble `baseBuilding.uasset` /
   `mainGamemode`'s dirt timer to confirm the host can SUPPRESS the stock self-dirtying on the client (so only the
   host stamps). If unsuppressable, the host-sole-stamper design still holds, but the client's stock timer must be
   intercepted (a `dirty` PRE interceptor on the client, like the weather scheduler suppression).
2. **Render-target clear primitive** — confirm the exact `Ad_window_C` op to RESET the RT to clean for `WindowReset`
   (re-`prepareRT`, or a full-canvas `K2_DrawMaterial` with a clear material). `prepareRT`/`Canvas`/`setDraw`/`endDraw`
   exist; verify which produces a clean baseline without wiping the drone-signal picture (dirt + picture share `rt_w`!
   — may need to redraw the picture after the dirt clear via `setCode`/the stored `gotCode`).
3. **Picture vs dirt coupling on `rt_w`** — both the signal picture AND dirt draw onto `rt_w`. A dirt reset/replay must
   not erase a drawn signal. Confirm draw order (dirt is gated `!isPlayingSignal`) and whether the picture is redrawn
   atop. Likely fine (dirt stamps are translucent brush; picture is a separate full draw) but verify before shipping A.
4. **`ElementType::Grime`** — confirm the Element Registry accepts a non-Aprop, non-NPC actor type, or add the type.
5. **Sponge proxy for receiver-applied cleans** — confirm a hidden `Aprop_sponge_C` can be spawned/kept host-side and
   have `power`/`Strength`/`cleanRadius` written so `cleanAtPoint`'s internal `wash` draws the correct brush; else reuse
   the receiver's local sponge when present and skip the stroke when absent (acceptable — the host `process`/RT state
   converges anyway).
6. **Instance counts** — get live counts of `Ad_window_C` / `AbaseWindow_C` / `Agrime_C` in a loaded base save to size
   the poll set + connect-snapshot (expected: windows few; grime tens). A quick reflection probe at runtime.

---

### Appendix — byte-exact citations (function → ubergraph offset)
- `Agrime_C::clean` — `grime.clean` @0(isCleanable gate) /102(`process -=`) /181(decal param) /419(destroy `process<0`).
- `Agrime_C::gatherDataFromKey` — `gather:=false` (no Key); `getPrimitiveData`/`loadPrimitiveData` persist `process`+`type`.
- `AbaseWindow_C::cleanSponge` → uber@29; decrement @788 (`clean -= arg*0.01`), @867 FMax0, @954 `setClean()`.
- `AbaseWindow_C::setClean` — `StaticMesh1.SetCustomPrimitiveDataFloat(0, clean)`.
- `AbaseWindow_C::gatherDataFromKey` — `gather:=true` (KEYED).
- `Ad_window_C::dirty` → uber@544; @676 `K2_DrawMaterial(mat_cleanBrush_glassDirt, randPos, randSize, …, randRot)`; `dirtify`→@3978→`dirty`.
- `Ad_window_C::cleanAtPoint` → uber@831/clean@1055; @1121 `sponge.wash(0.2,0.01,…)`, @663 `canv.K2_DrawMaterial(wash_dynmat, UV, size,…)`.
- `Ad_window_C::cleanPhys` → uber@2914/clean@3138; vel-gate @3138..3567, @3687 `FindCollisionUV(Hit)`, @2038 draw, @1923 0.1s Delay.
- `Ad_window_C::cleanOnHit` → uber@2179; spawns `cleanball_C` (`BeginDeferred`@2318, `fuckYou`/`spogen`/`sponge` set, `FinishSpawning`@2874).
- `Aprop_sponge_C` ComponentHit → uber@4087; washCollision/col/vel gates, @4367 `SphereOverlapActors(cleanRadius, grime_C)`, @3716 `grime.clean(self, strength*1.5, false)`.
- `Aprop_sponge_C::wash` — `power -=` @127, FMax0 @206, brush opac @654, returns dynmat+size.
- `Acleanball_C` ComponentHit → uber@29 `spogen.cleanPhys(sponge, Hit)` → @10 `K2_DestroyActor`; BeginPlay@88 `SetPhysicsLinearVelocity(fuckYou)`.
- `AgrimeProjectile_C::ReceiveTick` → uber; @1619 `BeginDeferredActorSpawnFromClass(Grunge)` @2041 `FinishSpawningActor` (grime spawn on static-surface impact).
