# VOTV laptop PC (Alaptop_C) — static bytecode RE for the E-interaction sync lane (2026-07-17)

> Scope: the base-station stationary PC — BP class `laptop` (`Alaptop_C : Aactor_save_C`). Grounds the
> coop sync of its two unsynced E-axes: ACTIVATE (power/boot) and the FLOPPY/ZIP slots. RE only — NO
> sync design here.
>
> Sources: `research/bp_reflection/laptop.json` (bytecode; uber pre-dump `_laptop_uber.txt`),
> `laptop.functions.txt` (76 fns), `CXXHeaderDump/laptop.hpp`, `prop_floppyDisc.hpp`,
> `prop_floppyBox.hpp`, `ui_floppyDatablock.hpp`, `ui_laptop.functions.txt` + targeted `_fn.py` /
> `_dumpstmt.py` / `_scan.py` disassembly. Statement cites are `uber@N` (ExecuteUbergraph_laptop
> offset) or `<fn>@N`. Confidence: [MEASURED] = read from the dump this session; [RD] = from a
> project doc / prior RE; [?] = unverified.

---

## §1 Class map (key fields, laptop.hpp offsets)

`Alaptop_C : Aactor_save_C`, size 0x509. The PC is a desk unit built from FIVE `UChildActorComponent`
pcparts (real physics prop actors: `pcpart_block` @0x3C0, `pcpart_keyboard` @0x3B0, `pcpart_mouse`
@0x3A8, `pcpart_speakerR` @0x3A0, `pcpart_zip` @0x2E0) cabled/constrained to the frame. [MEASURED]

| Field | Offset | Meaning (measured from bytecode) |
|---|---|---|
| `isOpened` | 0x418 | PC booted/ON (the ACTIVATE axis output). Runtime-only. |
| `Widget` (`Uui_laptop_C*`) | 0x420 | THE SHARED widget — `widget := gamemode.laptop` (uber@4763); one instance serves the base laptop AND every `portablePcTop_C`. |
| `canUse` | 0x428 | interaction latch during boot anim (CDO default true). |
| `Anim` | 0x42A | boot/shutdown animation in progress. |
| `powered` | 0x42B | wall power — mirrors `gamemode.powerChanged` arg `active_light` (uber@5925). |
| `beginplayTurnOn` | 0x42C | if set, auto-presses the power button 1 s after BeginPlay (uber@5804→@769). |
| `pcLaunched` | 0x440 | multicast delegate, broadcast on every power state edge. |
| `floppyType` | 0x450 | inserted disc type index; **-1 = empty** (CDO). Persisted. |
| `floppyData` | 0x458 | TArray<FString> — the inserted disc's data blocks. Persisted. |
| `fltr_A` | 0x470 | FTransform — the world transform of the disc at insert (used by the insert timeline lerp). |
| `floppyNametype` | 0x4A0 | display name of the disc type. Persisted. |
| `use_powerbutton/use_floppybox` | 0x4B0/0x4B1 | lookAt latches: aimed component == `boxBlock`/`floppyHitbox` (lookAt@210/@439). |
| `floppyBuffer` / `floppyBufferUIDs` | 0x4B8 / 0x4D8 | data copied onto the laptop itself (the floppy tab buffer). Persisted. |
| `floppyObjectData` | 0x4C8 | JSON of the inserted disc's FULL `struct_save` (getData → StructToJsonObject, uber@6871-6978). Persisted. |
| `floppyReadwrites` | 0x4E8 | disc read/write counter copied from the disc (uber@6411); CDO -1. Persisted. |
| `floppyIn` (`Aprop_floppyDisc_C*`) | 0x4F0 | set ONCE at insert (uber@6239) then the actor is DESTROYED — stale afterwards; never re-nulled in the graph. |
| `zip` / `use_zipBox` | 0x4F8/0x4F9 | inserted-disc-is-zip flag / lookAt latch (zipHitbox). `zip` persisted. |
| `floppyProcess` | 0x4FA | insert/eject animation re-entry latch. |
| `instantHit` | 0x4FB | "hit the PC to speed the boot" flag (pcHit → 0.1 s delays). |
| `seat` | 0x500 | `Aprop_seat_C*` — the chair; `stabilizeSeat()` pins it while in UI. |
| `screenUI` | 0x508 | enter-mode variant (fullscreen UI vs in-world screen camera path, enter@0). |
| Components | — | `floppyHitbox` 0x2F8 + `zipHitbox` 0x2B8 (insert triggers), `floppyMesh/floppyMove` 0x310/0x318 + `zipMesh/zipMove` 0x2A8/0x2B0 (the DUMMY disc visuals animated by the timelines), `boxBlock` 0x330 (power-button hit target), `boxScreen` 0x3C8 (screen use target), `screen` (WidgetComponent) 0x3E8, `startup` (AudioComponent) 0x398, `eff_button` (Particle) 0x3B8, `MediaSound_floppy` 0x308, `TL_floppy` 0x410 (insert TL), `Timeline_0` 0x3F8 (eject TL). |

Disc prop: `Aprop_floppyDisc_C : Aprop_C` — `Data` TArray<FString> @0x368, `zip` @0x378,
`readWrites` @0x37C; has its own `getData/loadData`. Colored subclasses `prop_floppyDisc_{B,Bl,G,O,R,Wh,Y}_C`;
**`_Wh` (white) = the ZIP disc** (see §2 FD-Q3). [MEASURED]

---

## §2 Question answers

### FD-Q1 — insert DESTROYS the disc actor; eject SPAWNS A FRESH one [MEASURED]

**insertFloppy(floppy)** = stub → uber@6224. Chain (uber@6224-7483):
1. `floppyProcess` re-entry guard (@6224); occupied guard `floppyType >= 0` → hint + ret (@6258-6306).
2. `floppyIn := floppy` (@6239); `floppyReadwrites := floppyIn.readWrites` (@6411);
   `lib_C.typeFromFloppy(GetObjectClass(floppyIn))` → `floppyType` + `zip` (@6489-6580).
3. Disc state capture: `floppyIn.getData(data)` (via `icast` interface, @6678; fallback empty struct
   @7484), first string kept, `StructToJsonObject → JsonObjectToString` → **`floppyObjectData`**
   (@6871-6978); **`floppyData := floppyIn.data`** (@7166); `fltr_A := floppyIn.GetTransform()` (@7005).
4. Dummy visual: `getFloppy()` (zip-selected mesh/move pair) gets the disc's world transform + the
   type's static mesh (`lib_C.floppyFromType`, @7082-7349).
5. **`floppyIn.K2_DestroyActor()` — uber@7404. The real `Aprop_floppyDisc_C` actor is DESTROYED.**
   The disc lives on only as laptop scalars (`floppyType/floppyData/floppyObjectData/floppyReadwrites/zip`).
6. `floppyProcess := true`; `TL_floppy.PlayFromStart()` (@7440-7451) — the dummy mesh lerps into the
   slot (update uber@9778→@9314/@9166: TLerp `fltr_A`-relative → slot transform). Finished →
   uber@10213→@9083: `widget.updFloppy()`, `floppyProcess := false`.

**ejectFloppy()** = stub → uber@7730. Chain:
1. Guards: `floppyProcess` (@7730); `floppyType >= 0` else "nothing to eject" hint (@7745, @8310).
2. Synchronously (@7793-8265): dummy mesh reset to slot-relative transform; **locals capture**
   `floppyType`→`Value__Object`, `floppyData`→`Value__1_Object` (@7944-7971); then
   `Array_Clear(floppyData)`, **`floppyType := -1`**, `widget.updFloppy()` (@8165-8229);
   `floppyProcess := true`; `Timeline_0.PlayFromStart()` (@8266) — dummy mesh lerps OUT (update
   uber@1782, VLerp on `Timeline_0_a`).
3. `Timeline_0__FinishedFunc` → uber@8417: `disableFloppyBoxCollision()` (both hitboxes off 0.5 s,
   @9944→re-enable @1158 — prevents instant re-insert of the ejected disc);
   `lib_C.floppyFromType(savedType)` → class; **`BeginDeferredActorSpawnFromClass(class, dummy-mesh
   world transform)` @8593 + `FinishSpawningActor` @8717 — a NEW disc actor is SPAWNED**; then its
   state is restored: `loadData(stringToSaveData(floppyObjectData))` @8810, `.data := savedFloppyData`
   @8864, `.readWrites := floppyReadwrites` @8933; `floppyObjectData := ''` @8913.
4. → uber@8983: dummy move reset, `floppyProcess := false`.

**Verdict: insert = DESTROY (uber@7404), state captured into laptop fields; eject = fresh
`BeginDeferredActorSpawnFromClass`/`FinishSpawningActor` (uber@8593/8717) with `loadData` +
`data` + `readWrites` restored. `floppyIn` @0x4F0 is a stale pointer after insert.** [MEASURED]

Coop identity consequence (fact, not design): the physical disc crossing the slot is a
destroy-then-respawn pair on ONE peer — the same shape as the wallunit reel slots (L7) and the
prop-birth seam; the disc's identity does NOT survive insert on the acting peer.

### FD-Q2 — ACTIVATE axis: `actionOptionIndex(action=b8)`, two-stage latent boot, `isOpened` [MEASURED]

- Entry: the E radial/press dispatches `actionOptionIndex(player, hit, action, lookAtComponent)`
  (stub → uber@2079). Power button option = **action byte b8** when aiming `boxBlock`
  (`use_powerbutton` set by `lookAt`@210). Proof b8 = power-toggle: `beginplayTurnOn` auto-press
  passes b8 (uber@815, struct arg + `b8`). Action **b4** = plain Use (screen enter / eject, see below).
- Gates: `Anim` busy → ret (@2079); **`powered` required** (@2094) — a dead-wall PC ignores E entirely.
- b8 chain (@2195-2632): if booting (`isOpened==false`) `widget.genStore()` first (@4435 — store
  regenerated每 boot); `anim := true`; `startup.SetSound(isOpened ? hiveShutdown : hiveBootup)` +
  `Activate(true)` (@2343-2428); `vloppy_mediaplayer.Close()` (@2220); RetriggerableDelay
  0.5 s(shutdown)/2.0 s(boot) (0.1 if `instantHit`) → resume **uber@15** [MEASURED skip-offset 15
  in the @2574 latent struct].
- @15: `powered` re-check (power died mid-boot → screen off + `BROADCAST pcLaunched` @112);
  `widget.rootswitch.SetActiveWidgetIndex(0)` (boot screen); booting shows `screen.SetVisibility(powered)`
  (@604); then RetriggerableDelay **RandomFloatInRange(4.0, 5.0) s** (0.1 if instantHit) → resume
  **uber@315** [MEASURED skip-offset 315 at @256].
- @315→@442: `rootswitch` index 1 (desktop); shutdown path hides the screen (@407); finally
  **`isOpened := !isOpened && powered`** (@520), `canUse := true`, `anim := false`,
  **`BROADCAST pcLaunched`** (@561), `updButton()` (green/red `eff_button` particle by `isOpened`,
  updButton@107).
- Power-loss side: `powerChanged(active_calc, …, active_light)` — custom event BOUND to the
  **`gamemode.powerChanged` multicast delegate** at BeginPlay (uber@5735-5758) → uber@5925:
  `powered := active_light`, `updButton()`, `canUse := true`, `anim := false`; if power lost while
  ON: `isOpened := false`, screen off, `startup.Deactivate()`, rootswitch 0, `BROADCAST pcLaunched`,
  `vloppy_mediaplayer.Close()` (@5995-6223).
- The boot RNG (4-5 s) is a per-peer local roll [MEASURED]; `pcHit()` (from `damageByPlayer`,
  uber@10198→@10135) sets `instantHit` + `audio_hit` and restarts the delay at 0.1 s — hitting the
  PC skips the boot wait.
- PE-visibility: the `actionOptionIndex` ENTRY is dispatched by mainPlayer BP via
  `EX_LocalVirtualFunction` → **INVISIBLE** [RD: COOP_DISPATCH_VISIBILITY.md:101, the measured
  device-verb table row]. `powerChanged` enters via delegate `Broadcast()` → **VISIBLE** [RD: doc
  taxonomy — multicast-delegate Broadcast dispatches through PE].

### FD-Q3 — floppy detection: hitbox overlap (thrown/carried-in) OR use-while-holding; class-gated per slot [MEASURED]

- `BndEvt__laptop_floppyHitbox…ComponentBeginOverlap` → uber@1088:
  `processFloppy(OverlappedComponent, player=NULL, manual=OtherActor, errorNotif=false)`; zipHitbox
  twin → uber@1123. **Delegate-bound component overlap events — PE-VISIBLE** [RD: doc taxonomy].
- `playerUsedOn(player, hit, lookAtComponent, holdObject, …)` (E while HOLDING something on the
  laptop) → uber@9131: `processFloppy(lookAtComponent, player, NULL, errorNotif=true)`. Entry is
  mainPlayer-BP-dispatched → INVISIBLE [RD:101].
- `processFloppy(slot, player, manual, errorNotif)` [MEASURED, processFloppy@0-892]:
  - `actor := IsValid(player) ? player.holding_actor : manual` (@10-94, @741) — so BOTH a held disc
    (use-press) and a free/thrown disc (overlap) insert. **No velocity / held-state check** in the
    handler itself; whether a held disc physically fires BeginOverlap is unverified [?].
  - slot==floppyHitbox: `cast<prop_floppyDisc_C>` must succeed AND `cast<prop_floppyDisc_Wh_C>` must
    FAIL (white/zip disc rejected with hint @305) → `insertFloppy(disc)` @869.
  - slot==zipHitbox: must BE `prop_floppyDisc_Wh_C` → `insertFloppy` @609; else hint (errorNotif-gated).
  - No `powered`/`isOpened` gate — **insert works with the PC off** [MEASURED: no such test in the
    chain].
- Eject entries: (a) E on an occupied slot — action b4 with `use_floppybox||use_zipBox` → uber@2633→
  `ejectFloppy()` @2685 (requires `powered` per the @2094 gate); (b) the widget's floppy-tab eject
  button — `ui_laptop` uber@22852 `laptop.ejectFloppy()` (`EX_Context`+`EX_LocalVirtualFunction`
  [MEASURED raw dump]).

### FD-Q4 — processFloppy / getFloppy / disableFloppyBoxCollision / timelines [MEASURED]

| Fn | What it does | Where it runs / side effects |
|---|---|---|
| `processFloppy` | slot router + class gate (see FD-Q3). Pure routing; hints via `lib_C.addHint`. | acting peer only. |
| `getFloppy` (BlueprintPure) | selector: `zip ? (zipMesh, zipMove) : (floppyMesh, floppyMove)` (getFloppy@0-164). | pure. |
| `disableFloppyBoxCollision` | both hitboxes `SetCollisionEnabled(off)`, RetriggerableDelay 0.5 s → re-enable (uber@9944→@1158). Called at eject-spawn (uber@8422). | acting peer; guards the fresh-spawned disc from instant re-insert. |
| `TL_floppy` (insert TL) | dummy disc mesh lerp world→slot (uber@9314 TLerp from `fltr_A`); finished: `widget.updFloppy()`, `floppyProcess := false` (@9083). | cosmetic on the acting peer (dummy mesh relative-transform never crosses today). |
| `Timeline_0` (eject TL) | dummy mesh lerp out (uber@1782); **finished = the SPAWN of the restored disc** (uber@8417, FD-Q1). | the finished handler is WORLD state (actor birth), not cosmetic. |
| `MediaSound_floppy` | volume 2.0 at BeginPlay (uber@4874); `ui_laptop` wires it to the shared `vloppy_mediaplayer` (`ui_laptop` uber@23004 `laptop.MediaSound_floppy.SetMediaPlayer(vloppy_mediaplayer)`) — video/music floppy audio is emitted as 3D sound at the laptop actor. | per-laptop 3D audio of a SHARED media player. |

Floppy content semantics: `floppyType` int comes from `lib_C.typeFromFloppy(class)`; data blocks are
FStrings (`floppyData`) rendered by the widget's floppy tab as `ui_floppyDatablock` rows (decode
image / erase / move-to-buffer / eject buttons; `Type` int + `Data` FString per block). Buffer moves
mutate `laptop.floppyData` / `laptop.floppyBuffer` / `floppyBufferUIDs` DIRECTLY from the widget
(`ui_laptop.updFloppy` Array_Resize/Array_Clear on `laptop.floppyData`, `_scan` hits @723-1700).
Which type index = music vs data vs kerfur-eye: not resolved here [?] (lives in `lib_C.typeFromFloppy`
/ `floppyFromType`, not in `laptop`).

Per-peer-cosmetic vs shared-world split (facts only): boot sounds/particle/screen widgets = local
effects of state (`powered/isOpened/floppyType…`); the disc destroy (insert) + disc spawn (eject) +
the persisted floppy fields = shared-world state.

### FD-Q5 — whole-surface census of laptop_C interactive axes

The laptop actor OWNS: power/boot, the two disc slots, the pcparts assembly, the seat pin, and the
screen claim. Everything else lives in the SHARED `ui_laptop` widget (`gamemode.laptop` — one
instance for base laptop + every `portablePcTop_C`; uber@3786/@9744 push it into their screens too).

| Axis | Where | Existing coop lane? |
|---|---|---|
| Shop/store + cart + order (`genStore`, `makeAnOrder`, `addToCart`…) | ui_laptop | **CROSSING** — `order_sync` (`OrderRequest=39`, client re-commits via native `makeAnOrder(order, true)`), `balance_sync` for Points. [RD: coop/items/order_sync.h, protocol.h:1608] |
| Email tab (`updEmails/selectEmail/delEmail/addEmail`) | ui_laptop | **CROSSING** — `email_sync` (+ `EmailDelete=57`). [RD: coop/world/email_sync.h] |
| Signals tab (saved-signal list, play/delete/sort) | ui_laptop ↔ `gamemode.savedSignals_0` | **CROSSING** — `signal_sync` (SavedSignalAppend/Delete; laptop-photo PNG bulk = documented gap). [RD: coop/interactables/signal_sync.h] |
| Screen occupancy (one shared widget → one typist) | device_occupancy | **CROSSING** — the "laptop" claim already exists. [RD: device_occupancy.h:35, protocol.h:513] |
| Cameras tab (`genCams/setCam/updCam`) | ui_laptop | Camera SELECTION per-peer UI; CCTV world state has its own prior lane [RD memory: CCTV]. Not laptop state. |
| Upgrades tab (`stuffUpgraded`, buy) | ui_laptop → saveSlot | Purchase money = balance lane; the upgrade STATE lives in saveSlot (join inherits via save transfer); LIVE mid-session upgrade propagation unverified [?]. |
| Photos tab (`genPhotos/addPhoto/viewPhoto`) | ui_laptop → saveSlot | save-carried at join; live `addPhoto` cross-peer: no lane found [?] — laptop-adjacent gap. |
| **Power/boot (`powered/isOpened/anim/canUse`, pcLaunched, screen, eff_button, hive sounds)** | laptop actor | **UNSYNCED** (this task). `powered` itself converges via each peer's own `gamemode.powerChanged` (base power is host-synced) [RD]; `isOpened` does not cross. |
| **Floppy/zip slots (`floppyType/floppyData/floppyObjectData/floppyBuffer[UIDs]/floppyReadwrites/zip`, disc destroy/spawn, TL anims)** | laptop actor (+ widget floppy tab mutating the arrays) | **UNSYNCED** (this task). The disc PROP while free is ordinary keyed-prop traffic; the slot transition is not. |
| pcparts assembly (5 ChildActor props, constraints, `setPcPartsData/Defaults`) | laptop actor | ChildActor children are OUTSIDE the world-object prop universe [RD lesson_child_actors_excluded…]; poses persist in the laptop's OWN save row (FD-Q6). Enter-gate depends on keyboard+mouse upright/near (uber@2700-3686) [MEASURED]. No live lane [?]. |
| Seat (`seat`, `stabilizeSeat`) | laptop → `Aprop_seat_C` | the seat itself is a normal keyed prop (prop lanes); the pin (`setPropProps`) is a local effect. |
| RC minigame (`createRC/tick_RC`), char-focus, settings | ui_laptop | per-peer UI, local. |
| Damage (`damageByPlayer` → `pcHit`) | laptop actor | boot-speed-up cosmetic only; `broken`/`broken_fire`/`thrown`/`kicked`/`playerGrabbed*`/`sendName`/`updateStrAgl`/`driveDetached` are ALL no-op uber entries @1958-1974 [MEASURED]; `canPickup/canBeUsedHold/toolbox*/setPath` return false/none [MEASURED]. The laptop cannot be grabbed, broken, or sawed. |
| `intComs_gamemodeBeginPlay` | laptop actor | re-acquires `widget := gamemode.laptop`, re-sets `screen.SetWidget`, `widget.laptop := self`, `updAdvs` (uber@10075→@9898→@9683→@1975). Boot-time glue. |

Related but separate device: `prop_floppyBox_C` (the disc storage crate — instanced-mesh disc rack,
own `floppyTypes/floppyData` arrays + `addFloppy/getFloppy` verbs + its own save row) — a sibling
unsynced floppy container, NOT part of laptop_C [MEASURED prop_floppyBox.hpp].

### FD-Q6 — save persistence (`getData`/`loadData`, `Aactor_save_C` chain) [MEASURED]

`getData` (getData@0-769; calls Super via self `getData` @0, then packs):
- `transforms[0]` = the 5 pcpart transforms (`getPcPartsData` — ChildActor world transforms);
- `strings[0]` = { `floppyNametype`, `floppyObjectData` }, `strings[1]` = `floppyData`,
  `strings[2]` = `floppyBuffer`;
- `ints[0]` = { `floppyType`, `floppyReadwrites` }, `ints[1]` = `floppyBufferUIDs`;
- `bools[0]` = { `zip` }.

`loadData` (loadData@0-1861) restores exactly those + `setPcPartsData` (teleports the 5 part actors,
zeroes their velocities) and refreshes UI: `widget.genFloppyBuffer()`, `widget.updFloppy()`,
`updButton()`. `gatherDataFromKey` = gather:=true, loadTransform:=false [MEASURED].

**NOT persisted → runtime-only:** `powered`, `isOpened`, `canUse`, `anim`, `floppyProcess`,
`instantHit`, `use_*`, `fltr_A`, `floppyIn`. A joining client that save-loads the host world inherits
the INSERTED-DISC content (type/data/buffer/readWrites/zip) + pcpart poses, but always arrives with
the PC screen OFF (`isOpened=false`) regardless of the host's live state. [MEASURED fields; join
mechanics RD]

### FD-Q7 — tick/self-simulation [MEASURED]

`laptop_C` has **NO ReceiveTick** (absent from the 76-function list). All progression is
event-driven + finite latents: the two boot RetriggerableDelays (2.0/0.5 s + random 4-5 s), the
1 s `beginplayTurnOn` delay, the 0.2 s `shitAssDelay` (widget focus), the 0.5 s hitbox-collision
re-enable, and the two timelines (insert/eject anims). Nothing accumulates over time — NOT in the
COOP_WORLD_PROP_DIVERGENCE self-sim class. (The shared `ui_laptop` widget DOES Tick — per-peer UI +
`tick_RC` — but that is widget-local.) The only RNG: the 4-5 s boot duration roll, per-peer local.

---

## §3 PE-visibility table (dispatch of each verb as it happens in-game)

| Verb | How it is reached (measured caller opcode) | PE-visible? | Reflected-callable by us? |
|---|---|---|---|
| `insertFloppy` | only from `processFloppy` self-calls @609/@869 — `EX_LocalVirtualFunction` [MEASURED] | **INVISIBLE** | yes (BlueprintCallable event; our `reflection::CallFunction` enters PE) |
| `ejectFloppy` | uber@2685 self `EX_LocalVirtualFunction` [MEASURED raw]; `ui_laptop` uber@22852 `EX_Context`+`EX_LocalVirtualFunction` [MEASURED raw] | **INVISIBLE** (both call sites) | yes |
| `processFloppy` | uber@1088/@1123/@9131 self `EX_LocalVirtualFunction` [MEASURED raw @1088/@9131] | **INVISIBLE** | yes |
| `getFloppy` | BlueprintPure, BP-internal | INVISIBLE | yes (pure) |
| `powerChanged` | bound to `gamemode.powerChanged` multicast (uber@5758); delegate `Broadcast()` | **VISIBLE** [RD doc taxonomy] | yes |
| `actionOptionIndex` / `playerUsedOn` / `player_use` (the E entries) | dispatched by mainPlayer BP — `EX_LocalVirtualFunction` | **INVISIBLE** [RD COOP_DISPATCH_VISIBILITY.md:101 measured row] | yes |
| `BndEvt__…floppyHitbox/zipHitbox…BeginOverlap` | component multicast delegate broadcast | **VISIBLE** [RD taxonomy] — but only fires for the free-disc (thrown/carried) insert path | n/a (event) |
| `TL_floppy__*` / `Timeline_0__*` | UTimelineComponent tick invoking bound UFunctions | VISIBLE [RD taxonomy: delegate-driven] [?-not-live-verified] | n/a |
| eject's disc spawn (`BeginDeferredActorSpawnFromClass`/`FinishSpawningActor` uber@8593/@8717) | `EX_CallMath` from the ubergraph | **INVISIBLE as a call** [RD: the GameplayStatics both-lists rule]; the SPAWNED actor's own PE lifecycle fires normally | — |
| insert's `floppyIn.K2_DestroyActor()` uber@7404 | BP cross-object call | INVISIBLE as a call [RD §1.2]; our Func-patch destroy seam (if installed on Aprop) is the native-side catch [?] | — |
| `getData`/`loadData` | save system BP callers (cross-object BP) | presumed INVISIBLE [?] | yes |
| `ReceiveBeginPlay` | engine lifecycle | VISIBLE [RD] | — |

Flags note [MEASURED laptop.json FunctionFlags]: `insertFloppy`/`ejectFloppy`/`powerChanged` are
`FUNC_BlueprintCallable|FUNC_BlueprintEvent` (no FUNC_Native) — all reflected-callable by our
`CallFunction` regardless of dispatch visibility. `processFloppy`, `updButton`, `actionOptionIndex`,
`playerUsedOn` additionally `FUNC_Public`.

---

## §4 Open unknowns

1. **[?] Timeline update/finished PE-visibility not live-verified** — taxonomy says
   delegate-dispatched (visible), but no smoke has counted `TL_floppy__FinishedFunc` PE fires.
2. **[?] Does a HELD disc trigger the hitbox BeginOverlap** (held-prop collision profile), or is the
   overlap path reachable only by thrown/placed discs? Affects which entry a client's insert uses.
3. **[?] `lib_C.typeFromFloppy` / `floppyFromType` type-index map** (which int = music / data /
   kerfur-eye / zip; the class↔type↔mesh tables live in lib_C, not dumped here).
4. **[?] `pcLaunched` delegate subscribers** — who binds it (portablePcTop? tutorial?); only the
   broadcast sites are measured.
5. **[?] Live mid-session propagation of upgrades/photos tabs** (save-carried at join; no live lane
   found — flagged in the census, out of this task's floppy/power scope).
6. **[?] `floppyReadwrites` decrement site** — the counter is copied at insert and displayed by
   `updFloppy`; which widget action decrements it (transferBuffer/progressUID?) was not traced.
7. **[?] `screenUI` mode** — which laptop instances set it (fullscreen UI vs in-world screen);
   affects `enter` flow only.
