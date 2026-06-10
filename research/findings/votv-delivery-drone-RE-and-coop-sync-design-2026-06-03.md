# VOTV delivery drone — RE + host-authoritative coop-sync design (2026-06-03)

**Game version:** Alpha 0.9.0-n. **Method:** CXX reflection dump (offsets, signatures,
lineage — VERIFIED) + cross-refs + the existing coop sync code. **IDA UPDATE (came online
mid-session):** IDA-CONFIRMED there is **ZERO native drone code** — `list_funcs` for `*drone*`,
`*triggerFly*`, `*dropSack*` all return EMPTY in `VotV-Win64-Shipping.exe` → the drone is 100%
Blueprint (validates the reflection/UFunction-hook design; no native patching). `UObject::
ProcessEvent` @0x141465930 is the only dispatch entry our hook detours. HOWEVER, BP bytecode
(the `ExecuteUbergraph_*` graph logic) lives in the cooked `.uasset`, **NOT in the EXE**, so IDA
CANNOT read the drone's control-flow — the BP graph/state-transition semantics remain structural
inferences, marked **[INFER]**, and the runtime confirmations stay marked **[PROBE]** (they are
BP-graph/runtime facts, not IDA-answerable; UE4SS BP-dump or the hands-on probe resolves them). No prior drone RE
doc existed (this is the first). Sources: `Game_0.9.0n/.../CXXHeaderDump/` (drone.hpp,
droneConsole.hpp, droneSpline.hpp, droneSellLocation.hpp, prop_dronesack.hpp,
prop_inventoryContainer_drone.hpp, prop_container_orderbox/giftbox/crate.hpp, prop_xmasgift.hpp,
prop_rdrone.hpp, orderPlace.hpp, mainGamemode.hpp, saveSlot.hpp, struct_storeOrder.hpp,
struct_save.hpp, struct_signalDataDynamic.hpp, ui_laptop.hpp); existing patterns in
`src/votv-coop/src/coop/` (npc_sync, weather_sync, garbage_sync, trash_collect_sync,
remote_prop, interactable_sync, net_pump).

> **Scope of THIS doc:** RE + design only (the user asked to "dig deep into syncing/mirroring").
> NOT implemented. The 5 [PROBE]s below are the gating next step (a single hands-on drone
> delivery with a probe build resolves them all). Per CLAUDE.md RULE 2026-05-28, the shape
> follows MTA's server-simulated-entity model (CClientVehicle/CClient Ped dead-reckoning +
> reliable state RPC) adapted to our standalone substrate.

---

## BYTECODE-VERIFIED UPDATE (2026-06-08) — static BP-reflection RE resolved 6/10 probes + 2 doc corrections

Two `bp_reflect` passes (drone state machine; economy order path) turned six [PROBE]s from
runtime-inference into **bytecode-verified fact** and CORRECTED two structural inferences below.
Method: `tools/bp_reflect.py` + `research/bp_reflection/_cfg.py` / `_fn.py` over
drone / droneConsole / ui_laptop / orderPlace / mainPlayer / lib disassembly. (VOTV is a STOCK
non-nativized UE4.27 Shipping build → all BP logic is Kismet bytecode in the `.uasset`, NOT in the
EXE; IDA cannot read it — `bp_reflect`, not IDA, is the tool for BP functions.)

**RESOLVED:**
- **[#4] flyingType@0x0300 is a raw `int32` (NOT a UEnum), domain {-1, 0, 1}** — the doc's guessed
  `{0..4 delivery/pickup/sell}` map was WRONG. **0** = path-following cruise (BOTH outbound-deliver
  and homebound-return; the `Direction` bool picks which way along the spline); **1** = pickup /
  approach-and-grab (homes on `pickupLoc`, `AddForce` toward it, grabs within 25 cm); **-1** =
  idle / arrived-done / dormant (after `sell()` → `active:=false`; also the value `triggerFly` tests
  to decide a fresh `beginFly`). Persisted in the drone's `Fstruct_save` ints[1] (`loadData`).
- **[#5]+[#2] Verb dispatch CONFIRMED unobservable.** Every call to `triggerFly`, `beginFly`,
  `dropSack`, `soundAlarm`, `checkOrders`, `compileOrder`, `sendShop` — INCLUDING the console's
  cross-object `drone.triggerFly()` — compiles to **`EX_LocalVirtualFunction` → ProcessInternal**,
  the layer BELOW our `UObject::ProcessEvent` detour. A PRE/POST observer on any of them NEVER fires
  (the `doorOpen` / flashlight / clump-`playerGrabbed` trap). **The "poll the flags, don't hook the
  verbs" host design is now bytecode-correct.** Only `ReceiveTick` / `ReceiveBeginPlay` (`FUNC_Event`)
  are ProcessEvent-observable (→ the client tick-suppression interceptor). `AdroneConsole_C::player_use`
  is ALSO unobservable → the client call-intercept edge MUST be the proven-observable
  **`AmainPlayer_C::InpActEvt_use_*` (the `_41` variant), gated by `lookAtActor == the console`** — the
  exact edge door-sync and clump-grab already settled on.
- **[#6] Order-commit path:** `Uui_laptop_C::makeAnOrder(order, automatic)` → `addOrderCart(order)`
  which does **`Array_Add(GameMode.saveSlot.orders@0x0490, order)`** [the ONLY persistent order write]
  → then **`GameMode.drone.sendShop(orders[0])`** to start the flight. `sendShop` → `beginFly` +
  `hasOrder:=true` + `checkOrders`. It does NOT write `droneOrder`, NOT call `orderPlace.spawnOrder`,
  NOT route through `storeMiddleman`.

**CORRECTIONS to the sections below:**
- **§9.1 `storeMiddleman` was wrong.** `intComs_storeMiddleman` is a **near-empty STUB** on
  `ui_laptop` — it does NOT resolve the live drone/orderPlace. Its only real use is inside
  `drone.compileOrder`, invoked on a freshly-spawned cargo prop as a "this item expands into N
  objects" interface hook (cargo assembly), not a resolver. The actual resolver is the cached
  `laptop.GameMode` pointer chain; `lib::getMainGamemode` = `UGameplayStatics::GetGameMode` (the
  LOCAL world's GameMode).
- **§9.2 `droneOrder` was wrong.** `saveSlot.droneOrder@0x01F0` is **never written by any drone
  function**. The active queue is purely `saveSlot.orders@0x0490`; `checkOrders` reads `orders[0]`.

**DECISIVE VERDICT — a CLIENT laptop order is 100% CLIENT-LOCAL.** The resolver is `GetGameMode()`
and VOTV has no UE replication, so a client's `makeAnOrder` `Array_Add`s into the CLIENT's own
`saveSlot.orders` and calls `sendShop` on the CLIENT's own mirror drone. It NEVER reaches the host's
authoritative drone/queue. **→ a new reliable client→host `OrderRequest(Fstruct_storeOrder)` edge IS
REQUIRED** (high confidence, bytecode-settled; the only residual is the runtime [#7] UI half — can a
client open the shop at all).

### ECONOMY BUILD PLAN (bytecode-grounded — build NEXT session, after the probe confirms [#7]/[#3]/drain)
1. **Client interception = POLL, don't hook** (`makeAnOrder` is unobservable): poll the CLIENT's
   `saveSlot.orders.Num` each GT tick; on an INCREMENT, take the new order(s), emit reliable
   `OrderRequest` to the host, and **REMOVE them from the local queue** (so the client's
   suppressed-tick mirror drone never tries to fly + no phantom local order persists). The local
   `sendShop` already ran, but `drone_sync` suppresses the client drone's tick so it cannot actually
   fly — the probe's `Active` edge log confirms whether the stray local `Active:=true` causes any FX
   that need clearing.
2. **Wire payload `OrderRequest` = one `Fstruct_storeOrder`:** `items` TArray<`Fstruct_store`>, each
   { `price` i32, `name` FName-as-string, **`object` TSubclassOf serialized AS A CLASS PATH/NAME —
   the load-bearing field** (`spawnOrder`/`dropSack`/`compileOrder` spawn exactly `items[i].object`),
   `asProp` FName-string, `category` u8, `size` i32, `parseRowNameToObject` bool } + `time` f32.
   Cosmetic fields (`subcategory` FText, `achievementUnlock` FName) omitted — re-derivable host-side
   from `name`. The probe's `ORDER PLACED` line dumps `obj0` via `R::NameOf` to prove the TSubclassOf
   reads cleanly for the wire.
3. **Host ingest:** on `OrderRequest`, re-resolve each `items[i].object` UClass by the wired class
   name (`FindClass` — both peers load the same cooked classes, so it round-trips),
   `Array_Add(host saveSlot.orders, order)`, then host `drone.sendShop(orders[0])` to launch. Cargo
   then flows back via the EXISTING prop pipeline (PropSpawn) + the already-shipped DroneState/
   DronePose body stream. HOST-AUTH; clients never spawn cargo.
4. **Drain:** the probe's `ORDER QUEUE DRAINED` edge reveals where `orders` is consumed (no
   `Array_Remove` was found in any drone fn) → needed to avoid a host re-fly loop.

OUT (host-only / host-save-only, per §9.6 / §9.8): sell points + email, gifts / task rewards, the
auto-morning schedule — all on the host's clock/save; clients see only the delivered `Aprop_C` box.

### PROBE RESULTS — RESOLVED AUTONOMOUSLY (2026-06-09, RULE 1: ran the probe ourselves, no hands-on)
Added an ini-gated (`drone_probe_drive=1`) one-shot auto-drive to `coop/dev/drone_probe` that, on the
game thread ~10 s after connect, calls the game's OWN delivery path via ProcessEvent (the verbs are
unobservable to a PASSIVE hook but freely CALLABLE — all `FUNC_BlueprintEvent|Callable`): HOST
`Make Default Order`(out) → `laptop.makeAnOrder(order, true)` (== `func_newHour`, a real delivery);
CLIENT `Make Default Order` → `laptop.addOrderCart(order)` (commit-only). A 90 s LAN smoke (no crash,
RSS stable) captured the answers on BOTH peers:
- **[#7] CLIENT SHOP REACHABLE + ORDER IS CLIENT-LOCAL ✓ (the gating question).** Client log:
  `DRIVE(client): addOrderCart(default) ok=1` → `ORDER PLACED: saveSlot.orders.Num 0->1 (order[0]:
  items=7 obj0='prop_reelbox_C')`. A client CAN commit a shop order locally; it stays in the CLIENT's
  own queue (the host never sees it). → the `OrderRequest` client→host edge is REQUIRED and BUILDABLE
  (poll the client's `orders.Num` for the increment, forward to host, clear the local entry).
- **TSubclassOf serialization ✓.** `obj0='prop_reelbox_C'` read cleanly via `NameOf` on BOTH peers →
  the order's `object` class round-trips by name for the wire.
- **flyingType {-1,0,1} CONFIRMED at runtime ✓.** Host cycled `-1` (dormant) → `0` (cruise, on the
  trigger) → `1` (approach), matching the bytecode.
- **The DRAIN POINT (the static RE couldn't find it) ✓.** `orders.Num 1->0` at ARRIVAL (with
  `flyingType=1, hasOrder=0`) → the queue is consumed on delivery (the host's own `removeOrderCart`).
  So the economy build injects via the native `makeAnOrder`/`addOrderCart` commit and the native drain
  handles cleanup → no manual queue management / re-fly loop.
- **[#1] singleton ✓** (drone_C resolves once and stays stable — initially NULL pre-spawn, then bound).
  **[#10] radar ✓** (the drone is in `mainGamemode.radarObjects`, Num=16/17 → the mirror blips).
- **[#3] cargo partial:** the host delivery assembled cargo into the drone's
  `prop_inventoryContainer_drone_C` (Aprop_C) at arrival; the FREE-box drop is the manual `dropSack`
  (action-option 7) not auto-tested, but the cargo IS Aprop_C → rides the existing prop pipeline. Not
  blocking the economy.

**⇒ THE ECONOMY IS GREENLIT.** Every gating question is resolved; the build plan above stands. Next:
build `OrderRequest` (client poll-and-forward + host ingest via the native `makeAnOrder` commit) + the
drone state edges. The probe (`coop/dev/drone_probe.{h,cpp}`) stays as ini-gated dev tooling
(`drone_probe=1` observe, `drone_probe_drive=1` self-drive a delivery/order); both disarmed after this run.

### ECONOMY BUILT + e2e-VALIDATED (2026-06-09, protocol v49)
Shipped exactly the build plan above. New code:
- `ue_wrap/ftext_utils.{h,cpp}` -- a PINNED valid empty FText (via Kismet `Conv_StringToText("")`, ref never
  released) so the hand-built `Fstruct_store.subcategory` slot is never a null-TSharedRef (deref-safe even if
  the host opens the laptop orders UI; bytecode says commit/deliver never reads subcategory, but be robust).
- `ue_wrap/order_economy.{h,cpp}` -- engine substrate: `OrderCount`/`ReadOrder` (poll the local queue),
  `CommitOrder` (HOST: hand-build the native `Fstruct_storeOrder` -- items buffer the native deep-copies then
  we free; objects via `FindClass`; pinned empty FText -- then dispatch `Uui_laptop_C::makeAnOrder(order,
  automatic=true)`), `CanCommit` (drone+radiotower+laptop+sellLocation present; BUSY is fine -- native queues),
  `QuietLocalDrone` (CLIENT: reset Active@0x0370:=false/flyingType@0x0300:=-1/hasOrder@0x0360:=false after
  forwarding so the locally-run sendShop can't fake a takeoff -- RE Q2).
- `coop/order_sync.{h,cpp}` -- CLIENT polls saveSlot.orders.Num (a WATERMARK; the commit verb is BP-internal/
  unobservable), serializes each new order (each item's `object` CLASS NAME + price/size/category + time),
  CHUNKS it across reliable datagrams (kMaxReliablePayload=228 B; header `OrderRequestHeader`=16 B), forwards;
  HOST assembles per (senderSlot, orderId) then commits via order_economy. Wire is client->host only (NOT
  relayed). protocol v48->v49, `ReliableKind::OrderRequest=39`. event_feed dispatch + net_pump Install/Tick/
  OnDisconnect.
- The CLIENT does NOT remove its forwarded order (the only heap-safe remover, `removeOrderCart`, needs the
  laptop UI + is host-only -- RE Q3); the watermark means it's never re-forwarded, and the ephemeral client
  never saves, so the retained local entry is harmless + leak-free.

RE Q1-Q5 (the 5-question commit/remove/charge agent pass, this session) confirmed: `makeAnOrder(automatic=true)`
charges NO money (charging lives in the laptop Button_order graph, not makeAnOrder); the spawn uses
`items[i].object` DIRECTLY (no row-name->class lookup); subcategory is UI-cosmetic; the native arrival drain
(`removeOrderCart`/`Array_Remove(orders,0)`) cleans the host queue.

**e2e LAN smoke PASSED (2026-06-09):** client auto-placed a 7-item order -> `order_sync: forwarded order id=1
items=7 in 1 chunk(s)` -> host `order assembled (slot=1 id=1 items=7)` -> `ftext_utils: pinned one empty FText`
-> `order_economy: CommitOrder -- makeAnOrder(items=7, automatic=1) ok=1` -> shared drone flew the delivery ->
`ORDER QUEUE DRAINED: orders.Num 1->0`. items=7 both ways VALIDATES the 0x50 Fstruct_store stride (a wrong
stride would garble items 1-6 into FindClass failures). 2 agent audits: perf 0-CRIT/0-WARN (all per-tick paths
O(1) cached); correctness fixed C-1 (drop OrderRequest from an out-of-range senderPeerSlot, no 0xFF sentinel
bucket) + reverted a wrong I-2 (Install is the PER-TICK ensure path -> must NOT reset the watermark; OnDisconnect
resets, like all sibling subsystems). Build clean, 4-folder hash-MATCH. UNCOMMITTED. Drone-state-edge FX
(rotor dust + delivery-signal sound) were DEFERRED here but the user hit them hands-on 2026-06-09 -> now being
built (separate RE pass: the client's drone tick is suppressed, so the FX must be driven directly off synced
state edges).

### DRONE FX + INTERACTION MIRROR (2026-06-09, stateBits bit0=dust / bit1=canTakeOff / bit2=hasSack)
The CLIENT suppresses the drone ReceiveTick (so it can't fly), which also kills the tick-driven FX + leaves the
interaction-gate fields stale. RE (2 agents): the **dust** (`eff_droneDust`@0x0278) is driven by a downward
ground raycast (active iff near ground) + a `'dust'` float intensity param; the **arrival cue**
(`audio_alarm`@0x0230 + `light_alarm`@0x0240) fires on the `canTakeOff`@0x0500 false->true edge; the **interaction
gate** is `actionOptionIndex: IFNOT(canTakeOff) -> "the drone is in motion"`, and `getActionOptions` needs
`hasSack`@0x0501 for the open-inv/drop options; the inventory is `container`@0x04F8 =
`Aprop_inventoryContainer_drone_C` (a keyed Aprop_C the prop pipeline already mirrors as 'droneContainer').
BUILD: host `ReadFxBits` packs dust(IsActive)/canTakeOff/hasSack into stateBits; client (drone_sync OnReliable)
replays `SetDust`(SetActive + SetFloatParameter('dust',1.0)) on the dust-bit change, `PlayArrivalCue`
(audio_alarm.Activate) + `SetSignalLight`(light_alarm.SetVisibility) on the canTakeOff rising edge, and
`WriteGateFields`(writes canTakeOff+hasSack onto the mirror) + `RepointContainer`(points container@0x04F8 at the
prop-mirrored actor). All component UFunctions resolved on their OWNING class (FindFunction doesn't climb supers):
SetActive/Activate/IsActive on `ActorComponent`, SetVisibility on `SceneComponent`, **SetFloatParameter on
`FXSystemComponent`** (NOT ParticleSystemComponent -- the audit caught this; resolving on the subclass returned
NULL -> the 'dust' param was never set -> invisible dust = the user's "no dust" bug; FIXED).
- **STATUS:** code FIRES (smoke logs: client `dust ON`, `arrival cue + signal light ON`, FX resolved + economy
  re-validated, no crash). HANDS-ON of the CORRECTED build (the SetFloatParameter fix) is PENDING -- the user's
  "drone dust/signaling not synced" report may predate the fix.
- **OPEN (next session):** (1) if the dust is STILL invisible after the param fix -> the tick-suppressed mirror
  may not RENDER its particle/light components at all (SetActorTickEnabled(false) may stop component
  emission/render) -> may need to NOT suppress the whole tick (neutralize only the flight integrator) or
  re-enable/re-register the specific FX components. (2) **Drone SACK INVENTORY contents sync** (the user's "items
  inside sack not synced"): the container ACTOR mirrors via the prop pipeline, but the ITEMS INSIDE it
  (`Aprop_container_C.propInventory`) are a SEPARATE sync gap -- RE whether the prop getData/loadData Fstruct_save
  already carries the inventory or it needs its own packet.

---

## 0. The cast (verified lineage)

| Class | Base | Role |
|---|---|---|
| `Adrone_C` | **`AActor`** (direct; size 0x508) | the big delivery drone — a SINGLETON placed actor |
| `AdroneConsole_C` | `AActor` | player-facing CALL terminal (button_call) |
| `AdroneSpline_C` | `AActor` | world-placed flight PATH (USplineComponent) |
| `AdroneSellLocation_C` | `AActor` | dropoff/pickup/SELL pad; `sell()` returns Points/email |
| `Aprop_dronesack_C` | **`Aprop_C`** | the grabbable cargo SACK (holds `container` + `takenByDrone`) |
| `Aprop_inventoryContainer_drone_C` | `Aprop_container_C`→`Aprop_C` | delivered itembox contents |
| `Aprop_container_orderbox/giftbox/crate_C` | `Aprop_container_C`→`Aprop_C` | the delivered boxes (shop/gift/crate) |
| `Aprop_xmasgift_C` | `Aprop_C` | the gift item |
| `AorderPlace_C` | `AActor` | `spawnOrder(Fstruct_storeOrder)` helper |
| `Aprop_rdrone_C` | `Aprop_corded_C` | **NOT this drone** — player-piloted recon CAMERA drone (§7) |

**Identity-critical:** `Adrone_C : public AActor` directly — NOT `Aprop_C`, NOT `Aactor_save_C`.
It carries the full keyed-save surface (`setKey`/`GetKey`/`getData`/`loadData`/`gatherDataFromKeyT`)
but has **no top-level Key field** (drone @0x02E0 is `Points`, not a Key). Its key lives **inside
`Data` (Fstruct_save) @0x03D0**, sub-offset +0x40 → absolute **0x0410** (`Fstruct_save.key`). It is
a hand-rolled saveable actor. (Contrast: `Aprop_C.Key @0x02E0`, `Aactor_save_C.Key @0x0230` — the
drone matches NEITHER.)

---

## 1. Lifecycle / state machine

### 1.1 Existence model — ONE persistent placed drone [INFER, PROBE #1]
`AmainGamemode_C` holds a single `Adrone_C* drone @0x0560` + `AorderPlace_C* orderPlace @0x0548`
(NO array — the delivery drone is a singleton; contrast `TArray<Aprop_rdrone_C*> drones @0x06E8`,
the recon drones). The drone is referenced only by mainGamemode + the 3 sibling actors
(console/spline/sellLocation each back-point `drone`). It is NOT in any spawner/factory.
**[INFER]** the drone PRE-EXISTS in the level dormant, bound at `ReceiveBeginPlay`, and is
ACTIVATED (`Active @0x0370` toggled), not spawned-per-delivery — supported by: it's keyed+saved
(`saveSlot.drone @0x0310`), the spline is world-placed, `leaveAfter5min`+`timerAlarm` (it loiters
and returns dormant). **[PROBE #1 — HIGHEST PRIORITY]** confirm single placed actor persisting all
session (never destroyed). If true → identity is trivial singleton resolution; we sync only
state+transform, never spawn/despawn.

### 1.2 Order pipeline (data path, VERIFIED offsets)
Orders are SAVE-persisted: `saveSlot.droneOrder` (Fstruct_storeOrder) @0x01F0 (active order),
`saveSlot.orders` (TArray<Fstruct_storeOrder>) @0x0490 (the queue), `saveSlot.drone` (Fstruct_save)
@0x0310 (drone actor state). Placement entry: `Uui_laptop_C::makeAnOrder(Fstruct_storeOrder, bool
automatic)` + `addOrderCart`. `AorderPlace_C::spawnOrder(Fstruct_storeOrder)` spawns the physical
box. `Fstruct_storeOrder = { TArray<Fstruct_store> items; float time; }` (0x14).

### 1.3 Flight state machine [INFER from flag names + signatures]
```
DORMANT (Active=false @0x0370)
  | console button_call -> AdroneConsole_C::player_use -> triggerFly(Console) [drone.hpp:204] = ACTIVATION
  +-- checkOrders() [reads saveSlot.orders/droneOrder]
  +-- compileOrder() [builds order @0x0348, sets hasOrder @0x0360]
  +-- makePoint()/setPath(Points) [fills Points[] @0x02E0 from Spline @0x0378; ind @0x02F0 cursor]
  |     sets flyingType @0x0300 (delivery|pickup|sell selector -- [PROBE #4 enum map])
  v
beginFly() [drone.hpp:120] -> Active=true, canTakeOff @0x0500
  v
FLYING (ReceiveTick drives motion; DERIVED: Point @0x02F4, Damping/Speed/Pitch/dist @0x0338-44,
        lerpTorque @0x04E0, vecForce @0x03B0, Direction @0x0310, ind++; audio blades loop;
        eff_droneDust; soundAlarm()->timerAlarm() alarmCount @0x0504; light_alarm; radarPoint @0x0290)
  v
ARRIVAL (sellLocation @0x0330 / pickupLoc @0x0390)
  +-- DELIVERY: dropSack() [drone.hpp:112] -> sack detached, cargo container appears (sec.2); hasSack @0x0501 flips
  +-- PICKUP:   setPickupLocation()/putSackOn(sack,velComp) [grabs a sack to haul]
  +-- SELL:     sellLocation->sell(...) [droneSellLocation.hpp:14]; sellBox_data @0x0380 (signal disks)
  v
LOITER (leaveAfter5min @0x0503 -> 5-min timer) -> RETURN (Points reversed, Direction flips) -> DORMANT
```
**Flag inventory (verified offsets), INPUT/EVENT vs DERIVED:**
INPUT/EVENT: `Active @0x0370`, `hasOrder @0x0360`, `flyingType @0x0300`, `hasSack @0x0501`,
`hasIteembox @0x04D0`, `canTakeOff @0x0500`, `leaveAfter5min @0x0503`, `pickedUp @0x032D`.
DERIVED-skip: `ind`, `Point`, `Direction`, `Damping`, `Speed`, `Pitch`, `dist`, `lerpTorque`,
`vecForce`, `vecGrab`, `alarmCount`, `Points[]` (computed from the world-placed Spline locally).

---

## 2. Delivered cargo

- During flight the drone carries `sack` (USkeletalMeshComponent @0x0250) + sackConstraint/sackPhys/
  sackHitbox + `container` (Aprop_inventoryContainer_drone_C* @0x04F8, gated by `hasIteembox @0x04D0`).
- The grabbable ground sack is the separate actor `Aprop_dronesack_C : Aprop_C` — holds `container
  @0x0380` + `takenByDrone @0x0388`; has `ReceiveBeginPlay`/`ReceiveDestroyed` + a trigger-overlap
  BndEvt.
- `dropSack()` [INFER] detaches/spawns the cargo at the destination; `lastSpawn @0x03C0`
  (TSubclassOf<AActor>) records the spawned class. `putSackOn(sack, velComp)` is the inverse (attach
  for hauling, inherit velocity).
- **KEY GOOD-NEWS FINDING: every delivered actor is `Aprop_C`-lineage** (orderbox/giftbox/crate/
  inventoryContainer = `Aprop_container_C : Aprop_C`; xmasgift = `Aprop_C`; dronesack = `Aprop_C`).
  Verified vs `ue_wrap::prop::IsDescendantOfProp` (prop.cpp:49). **So once dropped, the cargo is a
  normal KEYED Aprop_C and rides the EXISTING prop pipeline** (PropSpawn/PropPose/PropDestroy + the
  held-prop pickup path) — same as the mannequin/dropped items already synced. **No special cargo
  packet needed.** [PROBE #3: confirm `dropSack` spawns through the observed `Aprop_C::Init` POST so
  the existing PropSpawn broadcast fires — else the trash-pile-delivery workaround applies.]
- SELL path: `sellBox_data` (TArray<Fstruct_signalDataDynamic> @0x0380) + `sellBox_zipInfo` @0x03A0;
  `AdroneSellLocation_C::sell(out Points, out responseEmail, out checked, out soldAmountSig, out
  sellList)` — selling signal disks for points = host-authoritative economy state (host's save).

---

## 3. Identity = SINGLETON resolution (not key matching)
The drone is keyed only via `Data.key @0x0410` (host-SAVE-only; clients don't save). IF it's a
single placed actor (§1.1 [PROBE #1]), both peers load the SAME placed drone from the SAME map →
**resolve it on each peer via `mainGamemode.drone @0x0560`** (singleton pointer; same shape as the
weather cycle `AdaynightCycle_C` / the fog actor), never via key. Fallback: a once-cached
`FindObjectByClass(Adrone_C)`. This is the cleanest identity case in the whole coop layer.

---

## 4. Host-authoritative sync classification (the design)
**Model = a fusion of three PROVEN in-repo patterns** (no new mechanism):
the host simulates `ReceiveTick`; clients SUPPRESS their own drone tick + mirror.
MTA precedent: server-simulated networked entity dead-reckoned from a streamed transform +
reliable state RPC (CClientVehicle/CClientPed). Closest in-repo analog: **npc_sync** (host-simulates-
moving-actor + client suppression) fused with **weather/interactable** (reliable state edges) +
the **existing prop pipeline** (dropped cargo).

**STREAM (unreliable, ~10-30 Hz, host->clients, only while `Active`):** new `MsgType::DronePose`
(sibling of PropPose/RagdollPose). World transform = `GetActorLocation`+`GetActorRotation`
(engine.h:74/82); client drives the mirror via `SetActorLocation`/`SetActorRotation`
(remote_prop.cpp:397 pattern). Rotation carries the lean/pitch for free. **Stream the TRANSFORM,
not the inputs** — the flight is a per-tick float integrator (Damping/Speed/Pitch/lerpTorque/vecForce
vs Points[]); reproducing it bit-exact on the client is fragile (the RagdollPose/PropPose precedent
chose dead-reckoning for the same reason). Client SUPPRESSES its own drone `ReceiveTick` (PRE-
interceptor returns true on the client, like garbage_sync / the weather scheduler suppression) so
only the host simulates.

**EVENT (reliable, host->clients):** new `ReliableKind::DroneState` (WeatherState/KeyedToggle style):
`Active` edge (takeoff/return -> start/stop the mirror drive + blade audio + radar), `flyingType`,
`hasSack`/`hasIteembox` (cargo-aboard visual), the `dropSack` DROP edge (drone-side drop anim/audio
only — the box itself rides PropSpawn), the `soundAlarm` ALARM edge (one-shot, like LightningStrike).
**Host detects these via PER-TICK FLAG POLLING off the live actor (Active/hasSack/flyingType/pickedUp),
NOT by hooking the BP-internal verbs** (triggerFly/dropSack/soundAlarm are almost certainly BP-internal
-> the door/flashlight trap; polling is trap-proof — the weather_sync precedent reads post-mutation
state rather than trusting a hook).

**DERIVED-LOCAL (no wire):** audio (blades loop gated off synced `Active`), alarm light/particle (off
the alarm event), eff_droneDust (off `Active`), and **the radar blip works for FREE** — the client's
mirror is a real `Adrone_C` carrying its own `radarPoint` (Ucomp_radarPoint_C @0x0290) that
self-registers with the local `Apanel_radar_C`; just ensure `radarPoint.Active` tracks the synced
drone-active state. No audio/radar bytes on the wire.

---

## 5. Player-interaction routing
- **Console CALL = client->host request** [PROBE #2, #1 RISK]: a CLIENT pressing `button_call` must
  reach the HOST (who owns the authoritative drone). Shape: client intercepts its local
  `AdroneConsole_C::player_use` edge -> reliable `DroneCallRequest` -> host runs `triggerFly` -> host's
  DroneState/DronePose stream flows back. Client must NOT locally `triggerFly` its mirror. **[PROBE #2]
  whether `player_use`/`playerHandUse_LMB`/the button overlap is ProcessEvent-dispatched** — if
  `triggerFly` is only BP-internal, hook the player-facing `player_use` edge (exactly as the door-sync
  had to re-hook onto the input action instead of the internal `doorOpen`).
- **Grabbing the drone itself** (`isGrabbing`/`pickedUp`/playerTryToGrab/thrown): DERIVED-skip / out of
  MVP — host owns the drone; a client tug is visually overridden by the host transform stream. Flag for
  COOP_SCOPE.
- **Grabbing the delivered box** = the EXISTING held-prop path (it's a normal Aprop_C once dropped, §2)
  — nothing drone-specific. Only requirement: the host's drop spawns through the observed PropSpawn
  path ([PROBE #3]).

---

## 6. Native vs BP
All drone logic is BP (`ExecuteUbergraph_drone` + `UberGraphFrame @0x0220`; uniform with all prior
VOTV RE). **IDA-CONFIRMED (this session): no native impl exists** — `list_funcs *drone*/*triggerFly*/
*dropSack*` are all EMPTY in the shipping EXE; `UObject::ProcessEvent` @0x141465930 is the lone hook
point. The BP bytecode is NOT in the EXE (it's in the `.uasset`), so IDA cannot read the graph —
the per-verb observability below stays a runtime [PROBE]. **The decisive
distinction is ProcessEvent-dispatched vs BP-internal (ProcessInternal):** `ReceiveTick`/
`ReceiveBeginPlay` are engine-invoked (observable); `triggerFly`/`beginFly`/`dropSack`/`soundAlarm`/
`checkOrders`/`compileOrder` are very likely BP-internal helpers (NOT observable via ProcessEvent —
the `Adoor_C::doorOpen` / `updateFlashlight` / `playerTryToCollect` trap that already broke door-sync
+ flashlight + small-trash). **Do NOT design around hooking those verbs.** Robust host-side detection
= POLL the drone flags each game-thread tick (the harness already ticks) + stream the transform while
`Active`. Polling is trap-proof; hooking internal verbs is not.

---

## 7. `prop_rdrone` is a DIFFERENT drone (do not conflate)
`Aprop_rdrone_C : Aprop_corded_C` = the player-piloted recon/IR CAMERA drone (cam/cam_ir
SceneCapture2D, `move(Forward,rot,Up)` piloting, energy+charging+cord tether, `Capture(ui_laptop)`,
held in `mainGamemode.drones @0x06E8` plural + `ui_laptop.drone`). Shares only the word "drone" + a
radarPoint with `Adrone_C`. Separate sync story (player-controlled vehicle-like), OUT OF SCOPE here.

---

## 8. Design summary + the gating PROBES
**Recommended shape (MVP):**
1. Identity = singleton via `mainGamemode.drone` (not key). [PROBE #1 placed-singleton]
2. Flight = unreliable `MsgType::DronePose` transform stream; host-only sender gated on `Active`;
   client suppresses its own drone `ReceiveTick` + drives the mirror.
3. State = reliable `ReliableKind::DroneState` edges (Active/flyingType/hasSack/drop/alarm), host
   detected by PER-TICK FLAG POLLING (trap-proof).
4. Cargo = the EXISTING Aprop_C prop pipeline (orderbox/giftbox/crate/inventoryContainer/xmasgift/
   dronesack all Aprop_C). [PROBE #3 dropSack spawns through the observed Init]
5. Console call = reliable client->host `DroneCallRequest`; host runs `triggerFly`. [PROBE #2 the
   console edge is ProcessEvent-dispatched — #1 risk]
6. Audio/FX = DERIVED-local, gated off synced state. Radar = DERIVED-local **with a caveat
   (CORRECTED in §9.7):** the radar is a CENTRAL REGISTRY (`mainGamemode.radarObjects @0x0688` +
   `addRadar`/`removeRadar`), NOT a world scan -> the mirror blips for free ONLY if it keeps a live
   `radarPoint` whose `ReceiveBeginPlay` self-registers into the CLIENT's local `radarObjects`; a
   spawn-suppressed mirror must `addRadar`/`removeRadar` explicitly on the Active edge (no fallback).

**The two risks both have SOLVED precedent in this codebase:**
- BP-internal-verb trap (door `doorOpen`, flashlight `updateFlashlight`): mitigate via flag-polling +
  hooking the `player_use` edge.
- Delivery-spawn-not-observed trap (trash-pile landed-pile, solved in trash_collect_sync.cpp via a
  liveness watcher + explicit broadcast): if `dropSack` bypasses `Init`, the box needs an explicit
  host broadcast.

**GATING PROBES (one hands-on drone delivery with a probe build resolves all 5; do this BEFORE coding):**
1. **[#1]** Drone is a single placed actor persisting all session (never destroyed)? -> identity scheme.
2. **[#2]** Is `AdroneConsole_C::player_use` / button-call ProcessEvent-dispatched (hookable)? -> call routing.
3. **[#3]** Does `dropSack`'s container spawn fire the `Aprop_C::Init` POST observer? -> cargo auto-mirror.
4. **[#4]** `flyingType` int -> {delivery,pickup,sell} value map.
5. **[#5]** Which of triggerFly/beginFly/dropSack/soundAlarm are ProcessEvent vs BP-internal -> detection.
6. **[#6]** Does `Uui_laptop_C::makeAnOrder` push to `saveSlot.orders @0x0490` and/or call
   `Adrone_C::sendShop`? Who writes `saveSlot.droneOrder @0x01F0`? -> order-commit path (§9.1-9.2).
7. **[#7]** Is the laptop SHOP reachable + functional on a CLIENT (does `intComs_storeMiddleman`
   resolve the host-owned drone)? -> decides if the `OrderRequest` client->host edge is needed (§9.8c).
8. **[#8]** Confirm `daynightCycle::sendDriveBox`/`Make Default Order` fire off a `newDay`/`newHour`
   edge gated by `saveSlot.dailyDelivery @0x0E40`, and `automatic=true` is the auto-drive flag (§9.3).
9. **[#9]** Which actor injects a giftbox/xmasgift order on good task performance
   (`saveSlot.Task`/`taskNew` grade -> `dailyTaskDriveSize` -> `Make Default Order`)? (§9.4)
10. **[#10]** Does the client mirror's `radarPoint.ReceiveBeginPlay` run + `addRadar` into the client's
   local `radarObjects`? -> validates the free-blip claim (§9.7).

**Probe shape:** ini-gated `drone_probe=1`; a per-tick log of `mainGamemode.drone` ptr +
Active/hasSack/flyingType/pickedUp transitions + a PRE/POST observer attempt on
player_use/triggerFly/dropSack/soundAlarm/Adrone_C::Init/AdroneConsole::player_use logging whether each
fires; plus dump `saveSlot.orders.Num`/`droneOrder`/`Points`/`dailyDelivery` + check `radarObjects`
contains the drone. One real delivery (call console -> watch the drone fly in -> drop -> leave) prints
the answers; a CLIENT-side run of the same probe answers [#7]/[#10].

**COOP_SCOPE note:** add the delivery drone as a host-authoritative world event (like weather).
Economy is HOST-ONLY/HOST-SAVE-ONLY (order contents, sell points/email, gifts, schedule). The ONLY
new wire edges are: DronePose stream + DroneState edges + DroneCallRequest (console) + **OrderRequest
(client->host laptop shop, §9.8c, IF [#7] shows the client can shop)**; cargo rides the existing prop
pipeline. Explicitly OUT: client-grab of the drone body, the sell-economy points HUD (host-save-only;
a separate "shared balance" sync if ever wanted), the recon `prop_rdrone`.

---

## 9. ECONOMY + DATA FLOW (order -> queue -> schedule -> trigger -> assemble -> deliver -> sell -> radar)
All offsets/signatures VERIFIED from the CXX dump; control-flow BETWEEN functions is BP bytecode =
[PROBE]. (Deep-RE pass 2, 2026-06-03.)

### 9.1 Order placement (laptop shop)
- `Fstruct_store` (struct_store.hpp, 0x4D) = line-item: `price`@0x00, `name`@0x04 (FName),
  `object`@0x10 (TSubclassOf<AActor> = the spawned class), `asProp`@0x18, `category`@0x20
  (enum_shopCats, 11 cats), `size`@0x40, `achievementUnlock`@0x44.
- `Fstruct_storeOrder` (0x14) = `items` (TArray<Fstruct_store>)@0x00 + `time`@0x10 (delivery ETA).
- UI path (ui_laptop.hpp): browse `storeSlots`@0x0BF0 -> `addToCart`:437/`addStoreCart`:326 ->
  `cart`@0x0B10 (+`storePrice`@0x0B40) -> `Button_order`@0x0368 -> **`makeAnOrder(Fstruct_storeOrder,
  bool automatic)`:339 = COMMIT**. Routing laptop->world = `intComs_storeMiddleman`:300 (present on
  ui_laptop+mainGamemode+daynightCycle) resolves the live drone/orderPlace. Drone INGEST entry =
  `Adrone_C::sendShop(Fstruct_storeOrder)` (drone.hpp:198). [PROBE#6 the commit verb.]

### 9.2 Persistence + queue (saveSlot.hpp)
`droneOrder`@0x01F0 (ACTIVE order), `orders` TArray@0x0490 (QUEUE), `drone` Fstruct_save@0x0310
(actor save), **`droneItembox` Fstruct_save@0x06A0 (NEW: cargo manifest in save)**, `Points`@0x0090
(money), **`dailyDelivery`@0x0E40 (NEW: auto-drive flag)**, `loan` Fstruct_loan@0x0528. Flow [PROBE]:
makeAnOrder -> push `orders` -> drone `checkOrders`:119 pops -> `compileOrder`:114 sets drone
`order`@0x0348 + `hasOrder`@0x0360 + writes back `droneOrder`.

### 9.3 Scheduling = TWO trigger paths
(A) **Manual** console `button_call`@0x0258 -> `triggerFly(Console)` (= the §5 DroneCallRequest path).
(B) **Automatic morning drive (NEW, daynightCycle.hpp):** day-tick delegates `newDay`@0x0380/
`newHour`@0x0370/`newMinute`@0x0320 fire **`sendDriveBox(self2, out OutputPin)`:117** (the morning
auto-delivery) + **`Make Default Order(out Fstruct_storeOrder)`:119** (builds the default supply box)
+ **`dailyTaskDriveSize`:118** (drive size from task grade). `makeAnOrder.automatic=true` = the
auto/unpaid discriminator [PROBE#8]. **HOST-ONLY** (host's authoritative daynightCycle clock + saveSlot;
same model as weather sync; client suppresses).

### 9.4 GIFTS/REWARDS for good task performance (the user's "gifts if good job")
- Task structs: `saveSlot.Task` Fstruct_task@0x0128 (`points`@0x04, levels, `signalsCompleted`),
  `taskNew` Fstruct_taskNew@0x0CA8 (**`rewardSig`@0x38, `rewardSat`@0x3C** grade rewards),
  `totalCompletedTaskParts`@0x08A0, `hasFailedWeek`@0x0898. Task creation `daynightCycle::
  createNewTask`:111 + `dailyTaskDriveSize`:118 (grade -> drive size = **the link between performance
  and gift**).
- Gift cargo: giftbox/crate = bare `Aprop_container_C` subclasses (visual only; contents from the
  order). **`Aprop_xmasgift_C::analyzePlayer(addDropHealth/Sleep/Heavymed/Points/Food/battery/Bonus/
  Store, out Drop)`** = the gift-content RANDOMIZER (rolls the reward item list from the player's state).
- Flow [PROBE#9]: good grade -> bigger/gift `Make Default Order` on the `newDay` edge. **HOST-AUTHORITATIVE**
  (host's task grade -> host assembles the gift box -> client sees only the delivered Aprop_C box).

### 9.5 Cargo assembly
Items live in the **`Aprop_container_C` base** (prop_container.hpp): `propInventory`@0x0398 +
`nameData`/`massData`/`volumeData`@0x03B8-D8 + `lootEntry`/`spawnLoot`, serialized via `getData`/
`loadData` (Fstruct_save) = the SAME keyed-prop save the existing pipeline mirrors. `compileOrder`
populates it; `dropSack` (drone.hpp:112) finalizes the ground box (`lastSpawn`@0x03C0 = the spawned
class); `putSackOn(sack,velComp)`:111 = the inverse PICKUP path. `Aprop_dronesack_C : Aprop_C`
(container@0x0380, takenByDrone@0x0388, ReceiveBeginPlay/ReceiveDestroyed observable).
`AorderPlace_C::spawnOrder(Fstruct_storeOrder)`:9 spawns the box at the `mainGamemode.orderPlace@0x0548`
pad. **HOST-ONLY; the box is Aprop_C -> EXISTING prop pipeline, no new packet** (the container's items
ride its keyed getData/loadData save). [PROBE#3 dropSack/spawnOrder spawn through Init.]

### 9.6 Sell transaction (signal disks) -- HOST-SAVE-ONLY
`AdroneSellLocation_C::sell(out Points, out responseEmail (enum_signalResponse), out checked, out
soldAmountSig, out sellList)`:14 + `canSell`:13 + `findDish`:12. Disks staged in `drone.sellBox_data`
TArray<Fstruct_signalDataDynamic>@0x0380 + `sellBox_zipInfo`@0x03A0. Earned points -> **`saveSlot.Points
@0x0090` via the single writer `mainGamemode::AddPoints(int32)`:447**; sold IDs -> `soldSignals`
TArray<FString>@0x0430; email -> `addEmail(Fstruct_email)`:449 -> `saveSlot.emails`@0x0118.
**HOST-AUTHORITATIVE economy, HOST-SAVE-ONLY** (clients don't save). OUT of drone MVP.

### 9.7 RADAR -- CORRECTS §4's "free blip" claim
`Ucomp_radarPoint_C : UActorComponent` (comp_radarPoint.hpp): `Owner`@0xB8, `important`@0xC0,
**`Active`@0xC1**, `Color`@0xC4; `ReceiveBeginPlay` (register) + `des(DestroyedActor)` (deregister).
**DECISIVE: the radar is a CENTRAL REGISTRY, not a world scan** -- `mainGamemode.radarObjects`
TArray<AActor*>@0x0688 + `addRadar(AActor*&)`:442 / `removeRadar(AActor*&)`:441. A radarPoint
self-registers its Owner into radarObjects in ReceiveBeginPlay; `Uui_radar_C` iterates radarObjects to
draw blips. **Consequence:** a real mirror `Adrone_C` auto-blips locally ONLY if its radarPoint's
ReceiveBeginPlay runs on the client (registering into the CLIENT's local radarObjects). A
spawn-suppressed/stripped mirror will NOT blip -- NO fallback. **Action:** the mirror must keep a live
radarPoint with `Active@0xC1` tracking the synced state; else explicit addRadar/removeRadar on the
DroneState Active edge. [PROBE#10 confirm the mirror radarPoint registers.]

### 9.8 Coop edges the economy adds (beyond §4)
| Element | Source | Class | Note |
|---|---|---|---|
| (a) Order contents | Fstruct_storeOrder / saveSlot.orders | **HOST-ONLY** | host assembles cargo; client sees the Aprop_C box only; not on wire |
| (b) Sell points + email | saveSlot.Points/emails/soldSignals (AddPoints/addEmail) | **HOST-SAVE-ONLY** | OUT of MVP; future "shared balance HUD" = separate sync |
| (c) Client places a shop order | client laptop makeAnOrder -> host | **CLIENT-REQUEST** | NEW edge: reliable `OrderRequest`(Fstruct_storeOrder) client->host; host pushes to saveSlot.orders. [PROBE#7 is the shop even reachable on a client?] |
| (d) Gifts/rewards | saveSlot.Task/taskNew, dailyTaskDriveSize, Make Default Order | **HOST-AUTH** | host grade triggers gift drive; client sees the box only |
| (e) Auto/morning schedule | daynightCycle day-tick -> sendDriveBox | **HOST-ONLY** | host clock; client suppresses |
| (f) Radar registry | mainGamemode.radarObjects + addRadar/removeRadar | **DERIVED-LOCAL** (caveat §9.7) | central registry, not a scan |

**Net new wire edge from the economy = exactly ONE: reliable client->host `OrderRequest` (carries
Fstruct_storeOrder)** so a client can use the laptop shop -- and ONLY IF [PROBE#7] shows the client
shop is reachable. Everything else is HOST-ONLY / HOST-SAVE-ONLY. Cargo still rides the prop pipeline;
the call still uses DroneCallRequest; the radar is a mirror-integrity requirement, not a packet.
