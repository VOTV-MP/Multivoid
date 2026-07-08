# food (prop_food family + foodBox) — a use-counter consumable that morphs on empty   (STATUS: RE DONE + DESIGN RATIFIED, NOT BUILT)

RE DONE 2026-07-08 (full static bytecode pass, cited). Design RATIFIED via the `/qf` thread
(R1-R7): **FOOD is eaten only while HELD → it is client-authoritative → its sync is an INSTANCE of
the rock-drop held-item→world-prop pattern, NOT a new lane.** Sync NOT BUILT (resolves with the
deferred rock-drop MINIMAL-A authoring — `[[project-destroy-seam-hostwipe-rock-2026-07-08]]`).
The user's "multi-use N/N" ask splinters by the held-vs-world measurement: FOOD = held/rock-drop
(client-owned, this doc); concrete/cement = a world POUR that PRODUCES a world actor (pile/
wallbuilder lanes, host-owned, DIFFERENT); "special trash piles" = the already-fixed pile system.
See `[[project-food-multiuse-sync-2026-07-08]]`.

## 1. Native behavior (ground truth)

**Classes.** `prop_foodBox_C → prop_food_C → prop_C → … Actor` [bytecode prop_foodBox.json
SuperStruct=-32=prop_food_C]. The food FAMILY (`prop_food_{banana,mushroom,pinecone,snuskLoaf,
soap}`) all subclass `prop_food` and inherit the same `uses`/`used`/`bitten`/`eaten`/`getData`/
`loadData` machinery. foodBox overrides only `ExecuteUbergraph_prop_foodBox` + `eaten`.

**The counter.** Top-level instance var `uses` (IntProperty, `CPF_Edit|CPF_BlueprintVisible`)
on prop_food_C. Seeded in `init()`: `GetDataTableRowFromName(list_food, self.name)` →
`foodData = OutRow`, then `if uses<=0: uses = foodData.uses`. foodBox CDO `name="foodbox"`
[prop_foodBox.json:1010] → the row `list_food["foodbox"].uses` supplies the "8". **The literal
8 lives in the DataTable `/Game/main/datatables/list_food`, NOT in bytecode — UNMEASURED here**
(consistent with the observed 8/8).

**The use state machine** (inherited `ExecuteUbergraph_prop_food`, reached from the
`playerHandUse_RMB` self-thunk @4557 — eating is the RMB path):
```
uses = uses - 1                         [EX_CallMath Subtract_IntInt, plain, NO clamp — prop_food.json:3114-3135]
if (uses-1) <= 0:                        [LessEqual_IntInt vs 0, JumpIfNot — :3217]
    bitten(player)                       [poison application to the EATER]
    eaten(player)                        [VIRTUAL → foodBox.eaten override — spawns the morph, see below]
    save.stats.food_eaten += 1           [host save stat]
    K2_DestroyActor(self)                [destroys the box — :3599]
else:
    bitten(player)                       [poison to the eater]
    SetPhysicsAngularVelocityInDegrees(StaticMesh, RandomUnitVector*200)   [transient physics jolt — :3730]
```
Nourishment: `getMainPlayer().addFood(foodData.food…)` — applies to the **eater** [prop_food.json
`EX_LocalVirtualFunction addFood`]. `bitten()`/`eaten()` bodies = poison gate (poison_strength/
poison_delay) on the eater.

**The terminal morph** (foodBox.`eaten` override — prop_foodBox.json:425-676):
```
xf = GetTransform(self)
spawned = BeginDeferredActorSpawnFromClass(self, prop_C, xf, 0)
SetNamePropertyByName(spawned, "name", "g_foodbox")     [EX_NameConst "g_foodbox" — :549]
FinishSpawningActor(spawned, xf)
```
So the morph = **spawn a generic `prop_C name="g_foodbox"` (trash box) at the box transform**
(in `eaten`), **then the inherited handler `K2_DestroyActor`s the box** (order: spawn then
destroy). The g_foodbox mesh/props come from prop_C's own name-keyed DataTable lookup.

**NOT a dispenser.** One actor eaten 8 times in place; each use pushes `addFood` to the eater;
no per-use item spawn. (The only `BeginDeferredActorSpawn` sites in prop_food are the two
TEMPERATURE morphs on `ReceiveTick` — celsius>150 → `prop_burntFood_C`, celsius<=-273.151 →
`prop_freezerBurntFood_C` — off the `uses` path entirely.)

**`uses` has NO world-visible expression as it decrements** [G1, MEASURED]. Zero hits across
the whole asset for `SetStaticMesh`/`SetSkeletalMesh`/`SetMaterial`/`Set*Scale3D`/`SetVisibility`/
`AddStaticMeshComponent`. A box at 5/8 is visually identical to 8/8. The only per-use visible
effect is the transient physics spin jolt (non-persistent). **Only the terminal morph
(destroy + g_foodbox spawn) is a world-visible identity change.**

**Save.** `getData` writes `ints[0]=uses` (+ bools[0]=ignoreRotting, floats[1]=temperature,
floats[2]=ripeness); `loadData` restores them. A partially-eaten box (e.g. 5/8) **persists its
count** across save/load.

## 2. Dispatch / seam visibility (THE seam truth) [G2, MEASURED]

Per `docs/COOP_DISPATCH_VISIBILITY.md:34-35,100` — BP→BP calls dispatch via
`CallFunction→Invoke→ProcessInternal`, one layer BELOW ProcessEvent, so BOTH `EX_VirtualFunction`
AND `EX_LocalVirtualFunction` (self OR cross-object) are **INVISIBLE** to our detour.

| link | node | visibility |
|---|---|---|
| `used()` | `EX_LocalVirtualFunction` self-call to an EMPTY stub (`EX_Return`, 0 params) | INVISIBLE + carries no state — do NOT hook |
| mainPlayer → `holding_actor.playerHandUse_RMB(self)` | `EX_Context + EX_LocalVirtualFunction` (cross-object BP→BP) | INVISIBLE |
| `uses = uses-1` | `EX_CallMath Subtract_IntInt` inline | INVISIBLE |
| **native `InpActEvt_use`** (input → PE) | engine input dispatch | **VISIBLE** (but a bare thunk into the ubergraph; all gameplay after is invisible) |

=> The sync seam is the **`InpActEvt_use` PRE-intercept** (the existing `trash_use_intercept`
shape) OR a **POLL of the `uses` field / the morph result** on the food actor — never a hook on
`used()`/`playerHandUse_RMB`. Same standing pattern as the pile grab (`:83`) and device verbs
(`:101`).

**G2b — PRESS, not hold [MEASURED, mainPlayer disasm 2026-07-08].** Eating fires on a discrete
`IE_Pressed` of the `rotate` (RMB) InputAction: `InpActEvt_rotate_K2Node_InputActionEvent_16`
(`BlueprintInputActionDelegateBinding` action=`rotate`, `IE_Pressed`) →
`ExecuteUbergraph_mainPlayer(37543)` → @38593 `playerHandUse_RMB(self)`. **One press = one use, no
tick/hold re-fire** (the only other `input_rotate` consumers are two Tick blocks that rotate a
physics-grabbed body, not the use). `canBeUsedHold` (@38375/@38421) only GATES REACHABILITY, it is
not a poll. So a discrete, ProcessEvent-dispatched, **PRE-catchable** press seam exists;
`[[lesson_use_hold_bypasses_press_seams]]` does NOT apply.

**G-own — HELD-ONLY [MEASURED, mainPlayer disasm 2026-07-08].** @38593 is gated on
`IsValid(holding_actor)` (@38203) and invoked on **`holding_actor`** — the in-hands/equipped item.
The not-held branch (@38753) is the disjoint world path (place/kick on the aimed surface —
`BreakHitResult`, physMat grass/snow) and **never calls `playerHandUse_RMB`**. `holding_actor` is a
distinct concept from the aimed `hitResult` and the physics-grabbed `grabbing_actor`. Corroboration:
`prop_food.isButtonUsed → false`, `getActionOptions` has no world-aim eat action. **=> a food is
eaten ONLY while HELD; you cannot eat a world-placed box by aiming at it.** (Caveat: the exact write
site that assigns `holding_actor` the equipped hotbar actor is in a non-ubergraph UFunction not
disassembled; the HELD-ONLY verdict rests on the @38203 gate + disjoint @38753 branch.)

## 3. Sync-axis table (after the G-own=HELD-ONLY measurement)

A food is eaten **only while HELD** (§2 G-own), so the eater OWNS the item — it is
**client-authoritative**, exactly like a rock the client picked up. The host-authority framing
(below, "shape C") was built on the false premise that the box is a host-owned world prop at
eat-time; measurement killed it.

| axis | owner (who simulates) | peers need | carried by (lane/wire) |
|---|---|---|---|
| `uses` decrement 1..N-1 (while held) | the HOLDER (client owns its held item) | NO display (invisible, measured) | nothing — client-local (like rock-in-hand pose) |
| per-use physics spin | holder-local | negligible cosmetic | skip |
| nourishment `addFood` + poison (bitten/eaten) | the HOLDER (own player state) | no | holder runs locally |
| held-box DISPLAY on the puppet | holder → puppet | yes (see the box in hand) | existing HAND-ITEM axis (v105 HandItem) |
| **terminal morph: destroy held box + spawn `g_foodbox`** | the HOLDER (client) | **YES — world-visible** | the **rock-drop held-item→world-prop authoring** (FinishSpawn seam) — see §4 |
| drop a partial box (uses=5) → world Aprop | the HOLDER (client) | yes (world prop @ uses=5) | rock-drop authoring; `uses` rides the prop's save data (getData ints[0]) |
| `food_eaten` save stat | holder save | no | holder-local (per-peer save) |
| late-join (partial box 5/8, held or world) | whoever owns it | the actor + `uses` | held→hand-item axis; world→host save (getData persists) |

## 4. Coop design — FOOD = an INSTANCE of the rock-drop held-item pattern (shape D)

**Decisive measurement (§2): a food box is eaten ONLY while HELD → the eater is
client-authoritative over it.** So the client running `uses--` / reward / terminal morph **locally
is CORRECT** (faithful to SP; the client owns its held item — same as the deferred rock-drop
MINIMAL-A). There is **no host-authority to enforce here**, and concurrency is moot (a held box has
exactly one holder — no two-eater race).

What actually needs to cross to the host:
- `uses--` while held: **nothing** — client-local + invisible (no per-bite display, measured).
- **The terminal morph** (destroy held box + spawn `prop_C 'g_foodbox'`): the ONLY world-visible
  change. `g_foodbox` spawns via `FinishSpawningActor` and the box dies via `K2_DestroyActor`
  [G1b, cited §1] — the **exact seams the rock-drop MINIMAL-A authors** (`host_spawn_watcher` v106
  FinishSpawn + the destroy lane, which the host-auth skip `prop_lifecycle.cpp:210` currently hides
  from the host). So the food terminal morph is **an instance of the rock-drop held-item→world-prop
  authoring, NOT a new lane.**
- Dropping a partial box (uses=5) → a world Aprop carrying `uses=5` in its save data (getData
  `ints[0]=uses`, measured) → the same rock-drop authoring; the host learns the count when the box
  is dropped/spawned, not per-bite.

**=> NO new ReliableKind, NO host-authoritative use-intent, NO input intercept. Food resolves when
the rock-drop MINIMAL-A held-item→world-prop authoring lands** (see
`[[project-destroy-seam-hostwipe-rock-2026-07-08]]` — that work is deferred/not built).

**REJECTED — shape C (host-authoritative use-intent).** Earlier design rounds (the `/qf` thread,
R1-R6) built a host-authoritative "client sends use-intent → host runs its own count--+morph"
design, to avoid the getMainPlayer reward-trap. It is **wrong**: the G-own measurement proved the
box is HELD (client-owned) at eat-time, so there is no host-owned prop to be authoritative over —
shape C over-built a subsystem for a problem that doesn't exist. Kept here only as the rejected
alternative (RULE-2: do not resurrect it).

**The user's "generalized N/N approach":** there is NO single multi-use lane. Each N/N item
resolves to an ALREADY-KNOWN axis, gated by the **held-vs-world** measurement:
- **FOOD** = held consumable → held-item / rock-drop pattern (client-owned). *This doc.*
- **CONCRETE bucket / CEMENT bag** = a world POUR that PRODUCES a world actor
  (`actorChipPile_wetConcrete` / `customWall_wetConcrete` neighbors) → the pile / wallbuilder lanes
  (host-owned). DIFFERENT axis, uncensused — a separate doc when RE'd.
- **"special trash piles"** = the already-fixed pile system. Do not touch.
The generalization is the **held-vs-world authority split + reuse the existing seam per side**, not
a new subsystem.

## 5. Verification

- **RE: DONE** (static bytecode, cited) 2026-07-08 — §1/§2, incl. the decisive G2b (PRESS) +
  G-own (HELD-ONLY) mainPlayer disasm.
- **Design: RATIFIED** via the `/qf` thread (R1-R7, converged) — shape D (rock-drop instance);
  shape C rejected by measurement.
- **G3a (PENDING, human hands-on):** clean food-only repro (settled peers, no piles) — client
  HOLDS a box, eats to 0/8; does the terminal morph (destroy + `g_foodbox`) cross to the host?
  **Predicted NO** (it rides the same `:210`-skipped FinishSpawn seam as the rock drop). This is the
  one remaining hands-on measurement; the "terminal already crosses" belief is only from the
  BRAIDED host-wipe log.
- **Shared gate — client-inventory-persistence** (the world→hand→world `uses` round-trip: drop a
  5/8 box, other peer re-grabs it — does `uses` survive the pickup→inventory?): UNMEASURED, and the
  SAME gap the rock-drop MINIMAL-A flagged. Food's handoff fidelity lands with that work.
- **NOT built.** Food's fix is shared with / blocked on the deferred rock-drop authoring; no
  food-specific code.
