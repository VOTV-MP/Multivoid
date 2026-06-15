# VOTV base lockers + drone-call box (door-state) — RE + coop sync recipe

> **STATUS 2026-06-12: IMPLEMENTED as designed (same night)** -- LockerDoorState=50 (v62),
> ue_wrap/door_box (locker Open verb + console write+refresh + timeline force-snap verify)
> riding the interactable Channel as g_doorBoxAdapter (Symmetric, actor-FName identity).
> Smoke: 20 doors indexed + all verbs resolved on both peers. davyJones trigger risk
> accepted (events pass owns it). Hands-on pending.

**Date:** 2026-06-11. **Scope:** the ~20 tall metal lockers on/around the base and the
locker-like boxes with a hinged E-door (the drone-call box; the "tower/mast" box) — door
open/close state + interaction sync for coop.

**Method:** cooked-BP kismet disassembly via `tools/bp_reflect.py` (repak +
kismet-analyzer `to-json`) -> `research/bp_reflection/locker.json` (generated this pass,
138 functions), `droneConsole.json`, `radiotower.json` (generated this pass), read with
`research/bp_reflection/_fn.py` / `_cfg.py` / `_scan.py` / `_dumpstmt.py`; field offsets
from the CXXHeaderDump
(`Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/*.hpp`); placement census
by direct parse of the cooked level package summaries + export tables
(`research/pak_re/extracted/VotV/Content/Maps/*.umap`, UE4.27 104-byte export records) +
tagged-property walk of the instance blobs in `untitled_1.uexp`. All bytecode offsets
below are byte offsets inside `ExecuteUbergraph_<class>` unless a function name is given.

---

## TL;DR for the implementer

- Two classes carry the user's "box with a hinged E-door": **`Alocker_C`** (the metal
  lockers; subclasses `Alocker_personal_C`, `Alocker_death_C`) and **`AdroneConsole_C`**
  (the drone-call box at the drone pad — the console IS the box with the door). An
  exhaustive header sweep found no third class with the `door`-mesh + `opened`-bool shape.
- The "box up on the tower/mast" is (most likely — see §1.3) the **radiotower fuse-box**,
  whose two door leaves are **`prop_swinger_C` child actors** — that class is **already
  synced** by the existing container channel. Nothing new to build; verify its child-actor
  Key stability at runtime via the channel's existing `keysHash` log.
- **Neither locker_C nor droneConsole_C has a save Key** (bytecode-proven: `getKey` /
  `getOnlyKey` return `None`, `processKeys` returns `false`, `setKey` is a no-op). Use the
  **level-export actor FName** (`locker22` … `droneConsole_2`) as the cross-peer identity
  — deterministic from the cooked level package, unique per world, fits WireKey, readable
  with the existing `R::ToString(R::NameOf(actor))`. Grime-style position quantization is
  the documented fallback.
- Both ride the **existing `KeyedToggle` Channel** with **one new Adapter + one new
  `ue_wrap` wrapper** (the appliance multi-class-dispatch shape), **Symmetric** mode (no
  sensor/autoclose anywhere in either BP — no auto-revert, no oscillation). One new
  `ReliableKind` (next free = **49**; protocol bump v60 -> v61). No request path, no
  HostAuth machinery.
- Apply verbs: locker = call the public BP event **`Open(bool opened)`** (full native
  swing + sound + collision); droneConsole has **no public verb** — write `opened`
  @0x0298 + call **`setButtonsCollision()`** + drive Timeline `A` (or snap). Both get the
  door.cpp **verify + force-snap** treatment for the far-peer frozen-timeline case (snap =
  write timeline alpha + direction, call `a__UpdateFunc` [+ `a__FinishedFunc` for locker]).
- Door state is **not save-persisted** for either class (no `getData`/`loadData`; CDO
  `opened` defaults false) — every load starts closed on every peer; the Channel's
  connect-snapshot covers live divergence.
- One special case: `locker_davyJones` (`locker_death_C`) is wired to
  `batchSpawner_waterlocker_2` via `triggerOnOpen` — a mirror-apply fires that spawner on
  the applying peer too (double-spawn risk vs the host spawn-watcher). Flagged for the
  events-system pass; the other 18 lockers have **no** trigger wiring (uexp-proven).

---

## 1. The classes (Q1)

### 1.1 Candidate sweep

`CXXHeaderDump` grep for locker/box/cabinet/console/tower candidates, then a targeted
sweep for the "hinged door" shape — classes owning BOTH a `UStaticMeshComponent* door`
AND a `bool opened/Open/isOpened`:

```
== droneConsole.hpp   AdroneConsole_C : AActor   door@0x0270  A(timeline)@0x0290  opened@0x0298
== locker.hpp         Alocker_C : AActor         door@0x0230  A(timeline)@0x0268  opened@0x0270
```

That sweep is exhaustive over the dump: the only other `Door`-mesh owner is `prop_bbq.hpp`
(the BBQ lid — an `Aprop_C`, rides the prop pipeline; out of scope). Ruled out by header
inspection:

| class | why not |
|---|---|
| `Amisc_box_C` (misc_box.hpp:4-9) | bare `UBoxComponent` trigger volume; **no door, no state** (7 placed: `misc_box_alphaBunker/basement1/basement2/cave/hole/tunnel/wineCellar`) |
| `AserverBox_C` (serverBox.hpp:4) | the repairable base server (an `Aactor_save_C`); no hinged door; **already synced** by the appliance adapter (`Active`@0x03D5) |
| `AlockerCorpse_C` (lockerCorpse.hpp) | event-spawned corpse prop; no door fields |
| `Alockerguy_C` (lockerguy.hpp:9-16) | the scare dummy `Alocker_C::open` spawns when `head=true`; no door |
| `prop_container_wardrobe` etc. | `Aprop_container_C` lids = `prop_swinger_C` — already synced (container channel) |

### 1.2 The family

| class | base | role | placed in main map (`untitled_1`) |
|---|---|---|---|
| `Alocker_C` (locker.hpp:4) | `AActor` | tall metal locker | 14 (`locker22..27, locker28_27, locker29..32, locker37, locker38, locker4`) |
| `Alocker_personal_C` (locker_personal.hpp:4-6) | `Alocker_C` | the base personal lockers; **zero new fields/logic** (pure reskin, size 0x2B8 identical) | 4 (`locker33_34, locker34_1, locker35_3, locker36_5`) |
| `Alocker_death_C` (locker_death.hpp) | `Alocker_C` | the Davy Jones event locker; only adds two intComs stubs | 1 (`locker_davyJones`) |
| `AdroneConsole_C` (droneConsole.hpp:4) | `AActor` | **the drone-call box**: cabinet on a pole at the drone pad; hinged door covers the call/leave-time buttons | 1 (`droneConsole_2`) |

The user's "box where you call the delivery drone" **is** `droneConsole_C` — the header
shows it is literally a box with a hinged door: `door`@0x0270 swung by `doorAx`@0x0278 via
Timeline `A`@0x0290, opened-state `opened`@0x0298, the door's own E-target
`button_door`@0x0260 (a UBoxComponent ON the door leaf), plus the two interior buttons
`button_call`@0x0258 / `button_changeLeave`@0x0250 that only get collision while open
(§3.2). SCS tree (droneConsole.json SCS_Node walk):
`StaticMesh{ doorAx{ door{ button_door{ audio_door } } }, eff_blinklight2, button_call{...}, button_changeLeave{...} }`.

### 1.3 The "box up on the tower/mast"

No standalone "tower box" class exists. The structure that matches "a locker-like box
mounted on the tower/mast with doors you open" is the **radiotower fuse/puzzle box**:
`Aradiotower_C` (radiotower.hpp:4, an `Aactor_save_C`) carries `door_L`@0x0298 /
`door_R`@0x0290 as `UChildActorComponent`s, and the cooked asset resolves both child-actor
classes to **`prop_swinger_C`** (radiotower.json exports:
`door_R_GEN_VARIABLE_prop_swinger_C_CAT`, `door_R1_GEN_VARIABLE_prop_swinger_C_CAT`;
`ChildActorClass` import = `prop_swinger_C`).

**Consequence:** those two door leaves are instances of the class the **existing
container channel already syncs** (`g_containerAdapter`, interactable_sync.cpp:93-101,
`ReliableKind::ContainerState`). No new code. **Caveat to verify at runtime** (already
instrumented): child-actor props may carry per-peer GUID Keys — exactly the concern the
Channel's `RebuildIndex` keysHash log line exists for (interactable_channel.h:462-495,
"critical for the swinger/container channel whose child-actor Keys may be per-peer
GUIDs"). Open the tower box on the host during a smoke and check (a) host/client keysHash
match, (b) the mirror swings. If the Keys turn out per-peer, the fix is to extend the
swinger adapter's GetKey with the same actor-FName identity recommended in §2 (the child
actor's name derives from the owning radiotower component — also deterministic).

If the user instead means the drone console seen in OTHER maps (it sits on a mast-like
pole), that is the same `droneConsole_C` — covered.

`transformerMG*` was checked: only a UI panel class exists (`transformerMGPanel.hpp`); no
world transformer-box actor with a door is in the dump.

---

## 2. Identity — NO save Key; use the level-export actor FName (Q2)

### 2.1 Proof there is no Key

Neither class descends from `AtriggerBase_C` / `Aactor_save_C` (both are direct `AActor`
children — locker.hpp:4, droneConsole.hpp:4) and neither has a `Key` property anywhere in
its layout. The key-interface functions every interactable implements are **stubs** here
(cooked bytecode, this pass):

| function | locker_C | droneConsole_C |
|---|---|---|
| `getKey` | `key := name'None'` (@0) | `key := name'None'` (@0) |
| `getOnlyKey` | `key := name'None'` (@0) | `key := name'None'` (@0) |
| `processKeys` | `return := false` (@0) | `return := false` (@0) |
| `setKey` | uber@1138 = `POP` (no-op) | uber@464 = `POP` (no-op) |

So the gamemode key system (`intComs_gamemodeMakeKeys`/`PostKeys` — both POP no-ops,
uber@1124/@1127) never assigns these actors anything. The doors/lights/garage Key recipe
does not apply.

### 2.2 Recommended identity: the actor's level-export FName

Every placed instance is a **cooked-level export** whose ObjectName is baked into the
package and therefore **identical on every peer** (same .pak, same level). Census names
(parsed from the `untitled_1.umap` export table, §5): `locker22`, `locker23`, …,
`locker28_27`, `locker4`, `locker33_34` …, `locker_davyJones`, `droneConsole_2`.

- Read with the existing zero-alloc API: `R::ToString(R::NameOf(actor))` — the engine
  `FName::ToString` thunk renders the `_N` number suffix (reflection.cpp:187-210), so the
  runtime string equals the census name exactly. Precedent: the snapshot-adoption
  white-cube identity already keys mirrors by `Name@0x0258` (memory
  2026-06-10), and `Channel::RebuildIndex` already calls `R::ToString(R::NameOf(obj))` on
  every candidate for the `Default__` filter (interactable_channel.h:474).
- Fits the wire: `KeyedTogglePayload.key` is a 32-byte WireKey = 31 chars
  (protocol.h:1564-1569); longest census name is `locker_davyJones` (16) /
  `droneConsole_2` (14).
- Stable across sessions and across peers; immune to the actor being moved (it can't be:
  not a prop, no physics root — but name-identity wouldn't care even if it were).
- Collision analysis: names are unique within a package by UE rule; only ONE world is
  loaded at a time, and **no sl_* sublevel contains any locker/droneConsole** (census,
  §5), so cross-level duplicates (`locker_2` exists in untitled_111 AND untitled_121)
  can never coexist in one session. The `_C` Q-menu/sandbox spawner cannot spawn these
  classes (props only), so no runtime-generated names enter the index.

**Fallback** (if hands-on falsifies name stability, e.g. a future game patch renames
exports): the grime position-quantized key — these are static level actors, so
`PosKey` applies verbatim (grime_sync.cpp:118-130, `g_<qx>_<qy>_<qz>_<type>` on a 2 cm
grid; ue_wrap/grime.h:13-18 documents the model). Not needed today.

### 2.3 State persistence

Neither class implements `getData`/`loadData`/`gatherDataFromKey` (absent from both .hpp
function lists; present on e.g. `AserverBox_C` serverBox.hpp:96-98 for contrast), and the
CDO carries no `opened` override (locker.json `Default__locker_C` Data has no `opened`
entry; bool default = false). The 19 placed locker instances and the droneConsole carry
**no `opened` override** in their level blobs either (uexp tagged-property scan, §5.2).
=> **every load starts closed on every peer**; only live divergence needs syncing, which
the Channel's connect-snapshot already handles (interactable_channel.h:357-390).

---

## 3. Door state + the interaction chain (Q3)

### 3.1 `Alocker_C` — fields, chain, swing

**State + components** (locker.hpp, Alpha 0.9.0-n offsets):

| field | offset | meaning |
|---|---|---|
| `opened` (bool) | **0x0270** | THE door state. Written only by the toggle (@1667) and by `open(opened)` itself (@2087→ the event writes its param into the field). |
| `door` (UStaticMeshComponent*) | 0x0230 | the door leaf; child of `Axis` (SCS: `DefaultSceneRoot{StaticMesh, axis{door}, audio, stair, Box}`) |
| `Axis` (UArrowComponent*) | 0x0248 | the hinge the timeline rotates |
| `A` (UTimelineComponent*) | 0x0268 | the swing timeline, **length 0.5 s** (locker.json `a_Template.TimelineLength=0.5`) |
| `a_a_80ED…` (float) | **0x0260** | timeline alpha 0=closed → 1=open |
| `a__Direction_80ED…` (byte) | **0x0264** | ETimelineDirection: 0=Forward(opening), 1=Backward(closing) |
| `head` (bool) | 0x0271 | event artifact: "a head is inside"; `open(true)` spawns `lockerguy_C` + clears it |
| `blockedBy` (int32) | 0x0274 | plank-block refcount; gates the E-toggle only |
| `triggerOnOpen` (AActor*) / `triggerKey` (FName) | 0x0280 / 0x0288 | optional level-wired trigger fired on open (only `locker_davyJones` uses it — §5.2) |
| `BlockedbyPlanks` / `_Keys` | 0x0298 / 0x02A8 | plank actor refs / their save Keys |

**E-press chain (bytecode-proven):**

1. `player_use(Player, Hit)` is a **stub**: trampoline → uber@1186 = `EX_PopExecutionFlow`
   (it sits in the stub run @1115-@1188 of unimplemented interface events). The locker
   does NOT open via player_use.
2. The real handler is **`actionOptionIndex(player, hit, action, lookAtComponent)`** →
   uber@1860:
   - @1860-@1995: `if (blockedBy > 0) { lib_C::addHint(...); return; }` (the planks gate)
   - @1996-@2086: `if (action != 10 && action != 11) return;` — interaction-enum values
     **10/11 = the open/close pair** (`enum_interactionActions` cooked names are anonymized
     `NewEnumerator10/11`; `getActionOptions` offers exactly one of them selected by
     `opened` — getActionOptions @77 `SWITCHVAL(opened; 2 cases)`).
   - **@1638: `opened = !opened`** (@1667 store) → @1686 `player.lookAtActor = null` →
     **@1719 `open(opened)`**.
   (The single-option radial collapses to plain E in VOTV's interaction system, which is
   why E toggles directly. `lookAt` (locker.lookAt@0-70) returns `boundObjectReplace=door`,
   number=0 — any aimed locker part binds the prompt to the door.)
3. **`open(bool opened)`** — the public swing verb (cooked name lowercase `open`;
   CXXHeaderDump renders `Open` — see §4.4 case note). Trampoline → uber@2087:
   - @2087 `opened := param` (idempotent re-write)
   - @1203 branch on the param:
   - **OPEN (true):** @1217 `play(locker_open)` (the spatialized `Audio` comp) → pushes
     continuations @1240-@1250 then: @1255-@1412 `if IsValid(triggerOnOpen) →
     icast(Ttrigger).runTrigger(self, 0)`; @1082 **`A.Play()`** (timeline forward);
     @1413-@1570 `if (head) { BeginDeferredActorSpawnFromClass(lockerguy_C,
     GetTransform()) @1451; FinishSpawningActor @1521; head=false @1559 }`;
     @1599 `door.SetCollisionObjectType(15)` — ALL of this runs synchronously inside the
     one `open()` dispatch (push/pop flow), only the timeline alpha animates afterwards.
   - **CLOSE (false):** @1571 `play(locker_move)` → @1049 **`A.Reverse()`**.
4. **Timeline:** `a__UpdateFunc` → uber@2155 → @1743
   `axis.K2_SetRelativeRotation(RLerp(rot(0,0,0), rot(0, -100, 0), a_a))` @1807 — the door
   swings **-100° yaw on `Axis`**. `a__FinishedFunc` → uber@15: `if direction == 1
   (Backward)` → @61 `play(locker_close)` + @84 `door.SetCollisionObjectType(1)`; on
   Forward-finish it does nothing (@60 POP). So the **close sound + collision restore
   happen only at swing END** — i.e. they need the timeline to actually tick to completion.
5. `ReceiveBeginPlay` → uber@2150: `door.SetVisibility(true)` @2111 + gamemode cache @123.
   No tick gating, no state restore (consistent with §2.3 no-persistence).
6. `npcOpen()` is **vestigial** in 0.9.0-n: @0 `if (opened) return;` → @19
   `lib_C::getMainPlayer` → @65 `player_use(player, zero-FHitResult)` — and player_use is
   the @1186 no-op stub. (NPCs cannot actually open lockers through it; no NPC auto-revert
   exists → Symmetric is safe.)
7. `intComs_propRenderer_finishProps` → uber@2160: clears `BlockedbyPlanks`, then
   @287-@873 loops `BlockedbyPlanks_Keys` → `gamemode.getObjectFromKey(key)` →
   re-links each `prop_plankLockLocker_C` (@703 add, @771 `plank.locker = self`, @804/@873
   `blockedBy += 1`). I.e. the plank block state is **rebuilt from save Keys at load** on
   every peer identically; only LIVE crowbar removal mid-session can diverge it (§6).

**Timeline-freeze exposure (the door.cpp lesson):** the locker BP itself contains **no**
tick manipulation (scans for `Tick`/`SetPlayRate`/`SetNewTime` over all 138 functions:
zero hits), but the door's probe-proven behavior (ue_wrap/door.h:110-118, door.cpp:267-301:
the swing freezes beyond tick range / when far) is an actor-level effect we must assume
applies to any UTimelineComponent-driven swing. Unlike the door, locker `opened` is set
synchronously by `open()` (NOT by the FinishedFunc), so **state converges even if the
visual freezes** — but a far-frozen CLOSE leaves the door collision at type 15 and the
leaf mid-swing. Adopt the door's verify+force-snap (§4.2). Mark: the freeze itself is
**unverified for lockers specifically** (defensive adoption of the proven pattern).

### 3.2 `AdroneConsole_C` — fields, chain, swing

**State + components** (droneConsole.hpp):

| field | offset | meaning |
|---|---|---|
| `opened` (bool) | **0x0298** | THE door state |
| `door` | 0x0270 | door leaf, child of `doorAx`; carries `button_door`(@0x0260) + `audio_door`(@0x0240) |
| `doorAx` (UBillboardComponent*) | 0x0278 | hinge |
| `A` (UTimelineComponent*) | 0x0290 | swing timeline |
| `a_a_A523…` (float) | **0x0288** | timeline alpha |
| `a__Direction_A523…` (byte) | **0x028C** | 0=Forward(opening) 1=Backward |
| `button_call` / `button_changeLeave` | 0x0258 / 0x0250 | interior buttons; collision gated by `opened` |
| `drone` (Adrone_C*) | 0x02A0 | the singleton drone |
| `lookAtDoor`/`lookatKeyboard`/`lookat_changeTime` | 0x02B0/0x02B1/0x02C8 | lookAt-set focus flags |

**Chain (bytecode-proven):**

1. `player_use` → uber@512 = `POP` stub (same as locker).
2. **`actionOptionIndex`** → uber@761:
   - action **10/11** → @897 `player.lookAtActor = null` → **@529 the toggle**:
     @529-@558 `opened = !opened` → @577 **`setButtonsCollision()`** → @334
     **`audio_door.Play(0)`** → @375 `opened ? A.Play() (@389) : A.Reverse() (@422)`.
     There is **NO public open(bool) verb** — the toggle is ubergraph-internal.
   - action **4** → @935: if `lookat_changeTime` → @949-@1041 toggle
     `drone.leaveAfter5min` + `audio_change.Play` (the leave-time button); else @1083 if
     `lookatKeyboard` → **@713 `if (opened)` → @723 `drone.triggerFly(self)`** — the drone
     call **is gated on the door being open**, which is why this door state is the
     gameplay enabler for a client calling the drone.
3. **`setButtonsCollision()`** (public no-arg UFunction, setButtonsCollision@0-321):
   `button_changeLeave.SetCollisionEnabled(opened ? 1 : 0)` @59,
   `button_call.SetCollisionEnabled(opened ? 1 : 0)` @148, blinklight visibility @237/@279.
   One call refreshes every opened-dependent piece except the door leaf itself.
4. **Timeline:** `a__UpdateFunc` → uber@1283 → @596
   `doorAx.K2_SetRelativeRotation(RLerp(rot(0,0,0), rot(0, +135, 0), a_a))` @660 — swings
   **+135° yaw on `doorAx`**. **`a__FinishedFunc` is a no-op** (uber@1103 = POP) — no
   end-of-swing side effects at all (collision is setButtonsCollision's job, done at
   toggle time).
5. Keys: all stubs (§2.1). `chagedTower(bool broken)` exists (droneConsole.hpp:148) but is
   unrelated to the door (radio-tower break notification).

---

## 4. The APPLY recipe (Q4) — ride the existing KeyedToggle Channel

### 4.1 Architecture verdict

**Yes — the existing Channel carries this whole feature** (interactable_channel.h Adapter
+ Channel). Exactly the appliance shape (interactable_sync.cpp:117-132: ONE adapter, one
wrapper with per-class dispatch over 6 classes):

- **One new `ue_wrap` wrapper** (suggested `ue_wrap/door_box.{h,cpp}`, or `locker.{h,cpp}`
  if the droneConsole half is split — one feature, one file pair per the modular rule;
  both classes in one wrapper mirrors `appliance.cpp`'s Desc-dispatch and keeps it ~300
  LOC).
- **One new Adapter + one file-static `Channel` (Symmetric)** in interactable_sync.cpp
  (+ the 5 facade lines: ChannelForKind, IndexChannels, Install/Tick/OnDisconnect/
  QueueConnectBroadcastForSlot — exactly how garage/appliance landed).
- **One new `ReliableKind`** — next free is **49** (48 = ChatMessage, protocol.h:1102);
  bump `kProtocolVersion` 60 → 61 (protocol.h:507). Payload = the existing
  `KeyedTogglePayload` (40 B) — nothing new on the wire.
- **Mode: Symmetric.** Justification (the door-vs-garage test, interactable_sync.cpp:102-107):
  neither BP has a sensor, autoclose, or any other writer of `opened` besides the player
  toggle and (locker) `open()` itself — bytecode scan of all functions: `opened` writers
  are exactly locker uber@1667/@2106(param store) and droneConsole uber@558. No
  auto-revert => a symmetric poll cannot oscillate => no HostAuth, no hold-register, no
  E-press observer, no request kind. (All six HostAuth Adapter slots = nullptr, like
  appliance interactable_sync.cpp:131.)

The Channel then provides for free: per-tick `opened` polling with silent baseline priming,
echo-suppression, key→actor index with 2 s rebuild throttle (catches streamed-in
instances), deferred-apply with 25 s TTL, the connect-snapshot to joiners, and the
keysHash cross-peer stability log.

### 4.2 The Adapter functions (concrete)

```
const Adapter g_doorBoxAdapter = {
    "doorbox", coop::net::ReliableKind::LockerDoorState /* = 49 */,
    &ue_wrap::door_box::EnsureResolved,
    &ue_wrap::door_box::IsDoorBox,        // class == locker_C-descendant || droneConsole_C-descendant
    &ue_wrap::door_box::GetNameKey,       // R::ToString(R::NameOf(actor)) -- §2.2
    &ue_wrap::door_box::TryReadOpened,    // locker: opened@0x0270 ; console: opened@0x0298 (per-class Desc)
    [](void* a, bool on) -> bool { return ue_wrap::door_box::ApplyOpened(a, on); },
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,  // Symmetric -- no HostAuth hooks
};
Channel g_doorBox{g_doorBoxAdapter};      // Symmetric (no auto-revert -- §4.1)
```

**EnsureResolved** (the appliance pattern, appliance.h:30-34): resolve both UClasses
lazily (`FindClass(L"locker_C")`, `FindClass(L"droneConsole_C")`) — operational when
either is up; per-class `FindPropertyOffset(cls, L"opened")` with the documented fallbacks
(0x0270 / 0x0298); resolve the verbs below. NOTE `FindPropertyOffset` does not climb
supers (door.cpp:115-119) — `opened` is declared on each leaf class itself, fine; the
subclasses (`locker_personal_C`, `locker_death_C`) add no fields so `IsDescendantOfAny`
against `locker_C` covers them with the same offsets.

**TryReadOpened:** read the per-class bool. (The poll reads this every tick over the
index — ~20 bools, same cost class as the appliance/door channels.)

**ApplyOpened — locker (full-native verb + verify/snap):**

1. Call **`Open(bool opened)`** via the established ParamFrame/Call pattern
   (door.cpp:224-230 shape): `f.Set<bool>(L"opened", want); Call(actor, f)`. This is the
   one-call full SP behavior: writes `opened`, plays the right sound, swings the timeline,
   fires `triggerOnOpen` if wired, spawns the `head` scare if armed, sets door collision
   (§3.1.3). The Channel's Symmetric `cur != want` guard (interactable_channel.h:528-536)
   makes it an absolute set; double delivery is idempotent (`open` re-writes `opened`
   to the same value; the cur==want skip prevents the re-dispatch anyway).
2. **Verify + force-snap** (door.cpp:267-301 verbatim shape, because the swing may freeze
   on a far peer — §3.1 freeze note): register `{actor, want, deadline=now+1500ms}`;
   on the wrapper's Tick (call it from the channel facade Tick like
   `ue_wrap::door::TickSmartApply` is called at interactable_channel.h:394), if the
   timeline **alpha** (locker @0x0260) hasn't reached the target end (>=0.99 / <=0.01) by
   the deadline: write `alpha = want?1:0` (@0x0260) + `direction = want?0:1` (@0x0264),
   call **`a__UpdateFunc`** (rotates `Axis` — the visual snap; the door.cpp lesson that
   FinishedFunc alone does not move the mesh, door.cpp:245-262) then **`a__FinishedFunc`**
   (on close it plays the slam + restores door collision to type 1 — REQUIRED for the
   frozen-close case §3.1.4; on open it's a harmless no-op branch).
   The verify check uses the ALPHA, not `opened` (unlike door's isOpened) — `opened` is
   set synchronously by the verb (§3.1) so it can't indicate swing completion.
3. Do NOT pre-write `opened` — the verb writes it; a pre-write would only defeat nothing
   (the Channel doesn't re-read between) but adds a second writer for no reason.

**ApplyOpened — droneConsole (no public verb -> garage-style write + refresh):**

1. Write `opened = want` @0x0298 (direct field write — the garage `ApplyOpen` precedent,
   interactable_sync.cpp:108-115: "write Open := target THEN acivae() = native swing").
2. Call **`setButtonsCollision()`** (no-arg UFunction — buttons + blinklights, §3.2.3).
3. Drive the swing natively: resolve `Play`/`Reverse` off the live `A` component's class
   (UTimelineComponent natives; resolve-off-component precedent = door.cpp:86-105
   SetSensorOverlap) and call `want ? Play() : Reverse()`. Optionally `audio_door.Play(0)`
   (UAudioComponent::Play, same component-resolve pattern) for sound parity — the BP plays
   it on every toggle (§3.2.2).
4. Same **verify + force-snap** as the locker: alpha @0x0288, direction @0x028C, snap via
   `a__UpdateFunc` (rotates `doorAx`); `a__FinishedFunc` is a no-op (§3.2.4) so calling it
   is optional/skippable.
   - Alternative considered and REJECTED: replaying `actionOptionIndex(action=10)` with a
     synthesized player param (the `npcOpen` precedent shows the game itself does this
     shape) — it is toggle-semantics (fine under the Channel guard) but couples the apply
     to the LOCAL mainPlayer (clears its `lookAtActor` @897) and adds nothing the 3-step
     recipe above doesn't do explicitly. The garage adapter set the precedent for
     write+refresh over player-param synthesis.

### 4.3 What the sender side needs

Nothing new. The Channel's poll IS the sender (interactable_channel.h:103-184): any local
writer — host or client E-press via the native `actionOptionIndex` chain, an event
opening a locker, the davyJones trigger path — lands in `opened`, the poll sees the delta
and broadcasts; the host relays client edges (the existing event_feed routing for
KeyedToggle kinds; add kind 49 to `ChannelForKind` interactable_sync.cpp:144-153 and to
the event_feed reliable router + `IsClientRelayableReliableKind` exactly as
ApplianceState is listed). Client-side E works natively because (unlike doors) there is
no Active/power gate to suppress and no autonomy to fight — the locker toggle's only gate
is `blockedBy` (its own, local, save-derived).

### 4.4 Resolve-time gotchas (write these into the wrapper)

- **FName case:** the cooked export name is lowercase **`open`** (locker.functions.txt)
  while the live CXXHeaderDump renders **`Open`** (locker.hpp:162) — FName case is
  global-first-registration, so the live render can flip with load order. `R::NameEquals`
  is **case-sensitive** (reflection.cpp:212-219). Resolve `L"Open"` first (matches the
  live dump + the proven swinger `Open` resolve, swinger.cpp:43), fall back to `L"open"`;
  log which one hit. Same for `a__UpdateFunc`/`a__FinishedFunc` (cooked exactly as
  spelled) and `setButtonsCollision` (consistent lowercase-s in both sources).
- The timeline alpha/direction property names carry per-asset GUID suffixes
  (`a_a_80ED94194D3688F88DC22A9C1546EC4C`) — NOT name-resolvable; use the fixed offsets
  with a comment, exactly like door.cpp:38-41 (kMoveAlphaOff/kMoveDirOff precedent), or
  resolve by prefix-scan `NameStartsWith(L"a_a_")` over the class's properties if
  version-portability is wanted.
- `IsDoorBox` must use the cheap class-compare path (`R::IsDescendantOfAny` with the two
  resolved UClasses, door.cpp:185-191 shape) — it runs inside the full-GUObjectArray
  RebuildIndex walk.
- Install must be **latched** (the drone/ATV Install-latch lesson, memory 2026-06-08):
  IndexChannels in interactable_sync.cpp already latches per-channel (g_*Indexed bools,
  interactable_sync.cpp:284-302) — add `g_doorBoxIndexed` there, no new pattern.

---

## 5. Instance census + key stability (Q5)

### 5.1 Placement counts (parsed from the cooked level export tables, this pass)

Main world **`untitled_1`** (the map every coop session runs — phase-1 findings
2026-05-21): **14 locker_C + 4 locker_personal_C + 1 locker_death_C + 1 droneConsole_C**
(names in §1.2/§2.2). Matches the user's "~20+".

Other worlds (separate maps, never co-loaded; one droneConsole each, always named
`droneConsole_2`):

| map | locker_C | locker_personal_C | locker_death_C | droneConsole_C |
|---|---|---|---|---|
| untitled_1 (MAIN) | 14 | 4 | 1 (`locker_davyJones`) | 1 (`droneConsole_2`) |
| untitled_170 | 9 | — | — | 1 |
| untitled_55 | 4 | — | — | — |
| untitled_111/121/158/159/166 | 1 each | — | — | 121: 1 |
| untitled_47/80/120/189/196/206/207/209/211, tutorial3 | — | — | — | 1 each |
| **all sl_* sublevels, sandbox, NewFolder** | **0** | 0 | 0 | 0 (tutorial2: 1) |

(Zero lockers in any streamed sublevel => no name-collision risk inside a session, §2.2.)

### 5.2 Per-instance override scan (untitled_1.uexp tagged-property walk)

All 14 locker_C + 4 locker_personal_C blobs are uniform (342/346 B — transform/attach
only): **no `triggerOnOpen`, no `triggerKey`, no `head`, no planks, no `opened`**
overrides. The ONLY wired instance is **`locker_davyJones`** (502 B):
`triggerKey = 'DavyJones'`, `triggerOnOpen -> export 5810 = batchSpawner_waterlocker_2`
(class `batchSpawner_waterlocker_C`) + one `BlockedbyPlanks` entry. `droneConsole_2`
(536 B): component overrides only.

### 5.3 Key stability verdict

Level-export FNames are compile-time constants of the .pak — **stable across peers,
sessions, and saves** (the save never renames level actors; `opened` isn't even saved,
§2.3). Runtime confirmation is already built into the Channel: compare host vs client
`"doorbox: index rebuilt -- N live keyed instance(s), keysHash=0x…"` on the first smoke
(interactable_channel.h:490-495). Expect N=20 (19 lockers + 1 console) and identical
hashes on the main map.

---

## 6. Edges, risks, explicit unknowns

1. **`locker_davyJones` trigger double-fire (REAL, scoped).** Mirror-applying
   `Open(true)` runs `runTrigger` on `batchSpawner_waterlocker_2` on the applying peer
   too (§3.1.3). The spawner's output on the HOST is additionally mirrored by the
   HostSpawnWatcher/prop pipeline => a client that both (a) receives the host's
   PropSpawn mirrors and (b) fires its own local spawner would see doubles. This is the
   events-catalog M2/M3 territory (votv-events-triggers-catalog-2026-06-11.md:
   "hook the trigger, never the spawned actor's birth"). Options for the implementer,
   in rule-1 preference order: (a) handle it in the events pass (the batchSpawner's
   spawns become host-keyed like other watched spawns — no adapter special-case needed);
   (b) if doubles show up before that pass lands, the TARGETED fix is at the apply site
   for this one wired instance (null `triggerOnOpen` around the receiver's `Open` call
   and restore — a documented transitional crutch per principle 4, with the events pass
   as the retirement plan). The other 18 lockers are clean (§5.2).
2. **`head` scare on mirrors.** `head` is set at runtime by events (never in the level
   data, §5.2). If an event sets `head` on ONE peer only and the OTHER peer opens the
   locker, the opener's peer spawns `lockerguy_C` locally and the `head=true` peer's
   mirror-apply ALSO spawns its own (its local `head` is still true). Each peer sees one
   scare — actually coherent — but the spawned `lockerguy_C` actors are per-peer local
   (un-synced class). Cosmetic divergence only (lockerguy is a 0.5 s jump-scare prop);
   note for the events pass. UNKNOWN: which event(s) set `head` (not in this asset's
   bytecode — it has no local writer besides `open`'s @1559 clear; an external writer
   sets it by reflection/another BP — not traced this pass).
3. **`blockedBy` live divergence.** Plank state rebuilds identically from save Keys at
   load (§3.1.7), but a LIVE crowbar plank-removal mid-session decrements `blockedBy` via
   the plank's own BP on the acting peer, while the other peer's plank mirror is
   destroyed via the generic prop pipeline (which does not run the plank's
   crowbar chain). Result: the non-acting peer's locker may keep `blockedBy>0` => its
   LOCAL E refuses with the hint, but the acting peer's open still mirrors fine (our
   apply bypasses the gate — it is only in `actionOptionIndex`). Self-heals on reload.
   Accepted for v1; the plank prop (`prop_plankLockLocker_C`) was NOT disassembled this
   pass (UNKNOWN: its crowbar/destroy chain).
4. **Timeline freeze on far peers — UNVERIFIED for these classes** (§3.1). The recipe
   adopts the door-proven verify+snap regardless; if hands-on shows the swing never
   freezes for lockers, the snap path simply never fires (idempotent).
5. **droneConsole `drone.triggerFly` from a client** (@723) is OUT of this feature's
   scope: door sync makes the call button reachable+pressable on the client; whether the
   client's call replicates is drone_sync/economy territory (the drone is
   host-authoritative, v48; the client's suppressed mirror drone may need a
   call-request edge — flag for the drone/economy backlog).
6. **`misc_box_C` is not a door box** (§1.1) — if the user's "tower box" turns out to be
   one of the 7 named misc_box volumes (alphaBunker/basement/cave/...), those are
   doorless trigger volumes and nothing to sync. The radiotower fuse-box (§1.3) is the
   door-bearing tower candidate; confirm with the user/hands-on which one they meant.
7. **Sounds on snap:** the locker force-close snap fires `a__FinishedFunc` → the
   `locker_close` slam plays AT the locker (spatialized `Audio` comp) — inaudible far
   away, correct when near; matches the door's accepted snap behavior
   (door.cpp:249-254 double-sound guard rationale: our verify only snaps when the swing
   did NOT complete, so no double sound).

---

## 7. Citation index

- `Game_0.9.0n/.../CXXHeaderDump/locker.hpp` — class + all offsets (door 0x0230, A 0x0268,
  alpha 0x0260, dir 0x0264, opened 0x0270, head 0x0271, blockedBy 0x0274, triggerOnOpen
  0x0280, triggerKey 0x0288, planks 0x0298/0x02A8; `Open(bool opened)` :162,
  `player_use` :144, `actionOptionIndex` :161, `npcOpen` :68).
- `CXXHeaderDump/droneConsole.hpp` — (door 0x0270, doorAx 0x0278, A 0x0290, alpha 0x0288,
  dir 0x028C, opened 0x0298, button_door 0x0260, button_call 0x0258, button_changeLeave
  0x0250, drone 0x02A0, lookAt* 0x02B0/0x02B1/0x02C8; `setButtonsCollision` :68,
  `chagedTower` :148).
- `CXXHeaderDump/locker_personal.hpp:4-6`, `locker_death.hpp`, `lockerguy.hpp:9-16`,
  `misc_box.hpp:4-9`, `serverBox.hpp:4`, `radiotower.hpp:4,14-16`, `prop_bbq.hpp`.
- Bytecode (research/bp_reflection/locker.json via `_fn.py`/`_cfg.py`, offsets in §3.1):
  player_use→1186(POP); actionOptionIndex→1860/1996/1638; open→2087/1203/1217/1255/1369/
  1082/1413/1451/1521/1559/1599/1571/1049; a__UpdateFunc→2155→1743/1807 (RLerp 0→-100 yaw);
  a__FinishedFunc→15/61/84; ReceiveBeginPlay→2150/2111/123; getKey/getOnlyKey/processKeys
  @0 stubs; setKey→1138(POP); gamemodeMakeKeys/PostKeys→1124/1127(POP);
  npcOpen@0/19/65/334; finishProps→2160/287-873; timeline length: locker.json
  `a_Template.TimelineLength=0.5`; SCS tree nodes 146-152.
- Bytecode (droneConsole.json): player_use→512(POP); actionOptionIndex→761/935/897;
  toggle 529/558/577/334/389/422; call gate 1083/713/723 (`drone.triggerFly`);
  leave-time 949-1041; a__UpdateFunc→1283→596/660 (RLerp 0→+135 yaw);
  a__FinishedFunc→1103(POP); setButtonsCollision@59/148/237/279; setKey→464(POP);
  SCS tree (StaticMesh{doorAx{door{button_door{audio_door}}},…}).
- radiotower.json exports: `door_L/R_GEN_VARIABLE` ChildActorClass → `prop_swinger_C`
  (`door_R_GEN_VARIABLE_prop_swinger_C_CAT`, `door_R1_GEN_VARIABLE_prop_swinger_C_CAT`).
- Census: untitled_1.umap export table (104-B records; counts §5.1); untitled_1.uexp
  tagged-property walk (§5.2: davyJones `triggerKey='DavyJones'`,
  `triggerOnOpen→batchSpawner_waterlocker_2`); cross-map table §5.1.
- Our code: `src/votv-coop/include/coop/interactable_channel.h` (Adapter :67-82, Channel
  modes :85-94, poll :117-184, connect-snapshot :357-390, RebuildIndex+keysHash :461-501,
  TickSmartApply hook :394); `src/votv-coop/src/coop/interactable_sync.cpp` (adapters
  :44-137, garage :108-116, appliance :117-132, ChannelForKind :144-153, IndexChannels
  :284-302); `src/votv-coop/src/ue_wrap/door.cpp` (ForceTo :249-262, SmartApply :267-284,
  TickSmartApply :286-301, component-fn resolve :86-105, ParamFrame call :224-230);
  `src/votv-coop/src/ue_wrap/appliance.cpp` (ApplyState :145-178);
  `src/votv-coop/src/ue_wrap/swinger.cpp` (:34-58); `src/votv-coop/src/coop/grime_sync.cpp`
  (PosKey :118-130); `src/votv-coop/src/ue_wrap/reflection.cpp` (RenderNameToScratch/
  ToString/NameEquals :187-219, FindFunction :364-377);
  `src/votv-coop/include/coop/net/protocol.h` (KeyedTogglePayload :1564-1569,
  kProtocolVersion=60 :507, last kind ChatMessage=48 :1102).
