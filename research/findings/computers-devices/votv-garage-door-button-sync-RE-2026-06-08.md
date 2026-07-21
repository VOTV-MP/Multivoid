# VOTV garage-door + button: RE + coop-sync design (2026-06-08)

> **SUPERSEDED IN PART (2026-07-21, take-4 R9):** the identity decision below -- "key the garage by its
> save-persistent `AtriggerBase_C::Key`" -- was WRONG in practice and is RETIRED. The belt-and-suspenders
> note in section 3 ("if a Key ever came back None... a non-None Key is expected") is the exact case that
> bit us: the gamemode's keying (`mainGamemode::loadObjects` -> `GetAllActorsWithInterface` + `loadTriggers`,
> **one-shot + sublevel-gated** -- kismet-analyzer bytecode 2026-07-21) leaves the garage `Key=None` on the
> host when its tile is mid-recycle during the menu->save world transition, so the None-key filter drops it
> forever (host garage index 1->0, never recovered). **v123 keys the garage by its LEVEL-EXPORT FName**
> (`ue_wrap::garage::GetNameKey`, the `door_box` pattern) -- baked into the cooked package, cross-peer
> stable, race-free. Everything else in this doc (the Open field, acivae animate-vs-snap, SYMMETRIC mode,
> the button->Open poll) still holds. See `votv-take4-hands-on-bugs-2026-07-21.md` (R9).

**Bug (user hands-on, 2026-06-08):** "The button to OPEN the GARAGE doors isn't
synced or mirrored at all." When one peer presses the garage-door button, the
other peer's garage door doesn't move.

**Verdict (decisive):** The garage door is `Agarage_C` — a *self-contained,
level-placed, KEYED `AtriggerBase_C` descendant* with a `bool Open` state field
(0x02E8). It is **unsynced today purely because its class is in none of our
observer/poll sets** (`interactable_sync` only walks `door_C` / lightswitch /
swinger). The fix is a **direct extension of the existing keyed-interactable
channel**: a new `ue_wrap::garage` wrapper + a `g_garageAdapter` + a `Channel
g_garage` in `coop/interactable_sync` + a new `ReliableKind::GarageDoorState`.
Because the garage has **no auto-revert** (no sensor / autoclose / ReceiveTick),
it goes in **SYMMETRIC mode** (like lights/containers), NOT the doors' HostAuth
single-syncer — that complexity is unnecessary here. ~~No new identity scheme is
needed: the garage carries the same save-persistent `AtriggerBase_C::Key` FName
(0x0260) the door channel already keys on.~~ **[RETIRED v123 -- the save Key comes
back None on the host after a menu->save reload; identity is now the level-export
FName. See the banner above + R9.]**

All evidence below is from the standalone SDK CXX header dump
(`Game_0.9.0n/.../CXXHeaderDump/`, RULE 3) and the cooked-BP kismet disassembly
via `tools/bp_reflect.py` + `research/bp_reflection/_cfg.py` (RULE 3 dev tools).

---

## 1. The two classes

### 1a. Garage door — `Agarage_C`
`CXXHeaderDump/garage.hpp` (asset `VotV/Content/objects/garage.uasset`):

```cpp
class Agarage_C : public AtriggerBase_C
{
    FPointerToUberGraphFrame UberGraphFrame;                 // 0x0290
    UNavModifierComponent*   NavModifier;                    // 0x0298  (blocks AI nav while open)
    UAudioComponent*         s2;                             // 0x02A0
    USkeletalMeshComponent*  SkeletalMesh;                   // 0x02A8  (the animated door mesh)
    UBoxComponent*           cull;                           // 0x02B0
    UAudioComponent*         S;                              // 0x02B8
    UAudioComponent*         ac;                             // 0x02C0  (open/close sfx)
    UAudioComponent*         loop;                           // 0x02C8  (motor loop)
    UBoxComponent*           Box;                            // 0x02D0  (player-blocking collision)
    float                    move_a_<guid>;                  // 0x02D8  TIMELINE ALPHA (0=closed .. 1=open)
    TEnumAsByte<ETimelineDirection::Type> move__Direction_<guid>; // 0x02DC (0=Forward/open, 1=Backward/close)
    UTimelineComponent*      move;                           // 0x02E0  the open/close animation timeline
    bool                     Open;                           // 0x02E8  <-- PERSISTENT OPEN STATE (the sync field)
    bool                     mov;                            // 0x02E9  "currently animating" guard
    // methods:
    void set(float InputPin);          // resize Box + move it from the 0..1 alpha (the physical extent)
    void settime(bool Open);           // SNAP to state (montage @100x + move.SetNewTime) -- restore/mirror verb
    void acivae();                     // ANIMATED toggle w/ sound (the player-facing open) [sic: "activate"]
    void runTrigger(AActor* Owner, int32 Index);  // trigger fan-IN (what the button fires)
    void move__UpdateFunc();           // timeline tick -> set() (drives Box) -- ubergraph @1103
    void move__FinishedFunc();         // timeline complete -> ubergraph @2159
    void getTriggerData(Fstruct_triggerSave&);  // save  -> Open is save-persistent
    void loadTriggerData(Fstruct_triggerSave, bool&);  // load
    void ignoreSave_trigger(bool&);
    void ExecuteUbergraph_garage(int32 EntryPoint);
}; // Size: 0x2EA
```

Key facts:
- **Inherits `AtriggerBase_C`** → it has `FName Key` @ **0x0260** (the save-
  persistent cross-peer identity our DoorState already uses), `TArray<AActor*>
  Objects` @ 0x0230, `runTrigger`, `runAll`, `fireTrigger`, `setActiveTrigger`.
- **It is its OWN actor, not a child-actor component of any building.** Grepping
  the entire CXX dump, `garage_C` is referenced *only* in `garage.hpp` (contrast
  `Adoor_C`, referenced as `UChildActorComponent* door` across breakroomCounter
  (5), bunkhouse, dish, cremator, cargoliftDoor (door_L/door_R), etc.). So the
  garage is **directly placed in the level** — almost certainly ONE per base.
- **It has NO `ReceiveTick`, NO `checkSensor`, NO `autoclose`, NO `sensor`** (the
  whole function list is the 20 above). → **the garage NEVER auto-reverts.** Once
  `Open` is set, it stays; a player must press the button again to close it.
  This is the decisive difference from `Adoor_C` (which auto-closes via its
  `sensor` + `autoclose` every tick — the reason doors needed HostAuth).

### 1b. The button — `Atrigger_button_C`
`CXXHeaderDump/trigger_button.hpp` (asset
`VotV/Content/objects/triggers/trigger_button.uasset`):

```cpp
class Atrigger_button_C : public AtriggerBase_C
{
    FPointerToUberGraphFrame UberGraphFrame;  // 0x0290
    UStaticMeshComponent*    cube;            // 0x0298  (the pressable cube mesh)
    bool                     Label;           // 0x02A0
    // (the relevant verbs:)
    void player_use(AmainPlayer_C* Player, FHitResult Hit);   // E-press handler -- NO-OP (see disasm)
    void actionOptionIndex(AmainPlayer_C* Player, FHitResult Hit, ...); // interaction-menu dispatch -> runAll
    void isButtonUsed(bool& failed);          // -> failed=false (always usable)
    ...
};
```

The button is a generic `AtriggerBase_C` whose `Objects[]` array points at the
garage. Pressing it calls `runAll(-1)`, which fans out `runTrigger` to each
target (the garage). **This button is NOT one of our synced classes either, but
we do NOT need to sync the button** — see §5 (we sync the garage's resulting
`Open` state, which captures every activation source uniformly).

> The garage is unambiguously the base garage door. Two look-alikes are NOT it:
> `AcargoliftDoor_C` is just a holder of two **regular `Adoor_C`** child-actors
> (`door_L`/`door_R`, already DoorState-synced); `AvillageGates_C` holds
> child-actor swinger gates (village, not the base garage).

---

## 2. The OPEN mechanism (byte-exact kismet disassembly)

The activation chain, fully traced:

```
Player aims at the wall button (Atrigger_button_C), opens the interaction menu,
picks "use"  ->  Atrigger_button_C::actionOptionIndex
    actionOptionIndex  -> ubergraph @10:  runAll(-1)              [trigger_button.json]
    runAll (AtriggerBase_C)  -> for each target in Objects[]: target.runTrigger(owner, idx)
        Agarage_C::runTrigger(owner, index)  -> ubergraph @1371   [garage.json]
```

**`Atrigger_button_C::player_use`** is a no-op (ubergraph @39 → JUMP @119 →
RETURN) — exactly like the base door's `player_use` (prior door RE). The real
activation is `actionOptionIndex → runAll(-1)`:

```
### actionOptionIndex -> ExecuteUbergraph_trigger_button @10 ###
  @10: runAll(-1)
```

**`Agarage_C::runTrigger` → ubergraph @1371** (resolved CFG):

```
@1371: IFNOT(mov) JUMP @1386          ; if already animating (mov), ignore the re-trigger
@1385: POP->ret                       ;   (mov==true path: do nothing)
@1386: CallFunc_Not_PreBool_ReturnValue := Not_PreBool(open)   ; toggle
@1415: open := CallFunc_Not_PreBool_ReturnValue                ; Open ^= 1   (EX_InstanceVariable, the 0x02E8 field -- raw stmt confirmed)
@1434: acivae()                                                ; run the animated open/close
@1448: POP->ret
```

**`Agarage_C::acivae` → ubergraph @1449** (the animated swing):

```
@1449: mov := true                    ; latch "animating"
@1460: JUMP @1328
@1328: ac.SetActive(true, true)       ; play open/close sfx
@1366: JUMP @1273
@1273: RetriggerableDelay(self, 0.5f, ...ExecuteUbergraph_garage...)  ; 0.5s, then re-enter @1465 (settime body)
```

**`settime(open)` body → ubergraph @1465** (SNAP-to-state + montage):

```
@1465: PUSH @1475 ; JUMP @598
@ 598: IFNOT(open) JUMP @678
@ 612:   NavModifier.SetAreaClass(_) ; move.Play()        ; open  -> timeline forward
@ 678:   move.Reverse()                                    ; close -> timeline reverse
@1475: set(BoolToFloat(open))                              ; resize/move the collision Box to final extent
@1616: CreateProxyObjectForPlayMontage(SkeletalMesh, garageDoor_Skeleton_Montage, rate=1.0, blend=100.0, open?open:close)
@2080: move.SetNewTime(BoolToFloat(open))                  ; SNAP the timeline alpha to 0.0 / 1.0 (the endpoint)
```

`set(InputPin)` (separate fn) lerps the `Box` extent + relative location from the
0..1 alpha — i.e. the player-blocking collision tracks the door's openness.

**`move__FinishedFunc` → ubergraph @2159 → @1108:**

```
@1108: loop.SetActive(false, false)   ; stop motor loop
@1146: <SetActive on the route-particle by direction>
@1257: mov := false                    ; clear "animating" guard
```

### Mechanism summary
- **State field:** `bool Open` @ **0x02E8** (the toggled, save-persistent truth).
- **Animation:** a `UTimelineComponent* move` (alpha `move_a` @0x02D8, dir
  @0x02DC) **plus** a parallel montage on `SkeletalMesh`
  (`garageDoor_Skeleton_Montage`). The timeline drives the collision `Box`
  (`set()`); the montage drives the mesh bones.
- **Re-entrancy guard:** `bool mov` @ 0x02E9 (true while animating; `runTrigger`
  ignores presses during the swing).
- **Snap-to-state verb:** `settime(bool Open)` — montage at blend 100.0
  (near-instant) + `move.SetNewTime(0/1)` jumps the timeline to the endpoint.
  This is the clean "complete the state regardless of where the animation is"
  entry (the garage's analogue of the door's
  `move_a + move__UpdateFunc + move__FinishedFunc` force-snap).
- **Animated verb:** `acivae()` — the full toggle with sound + 0.5s delay.
- It is **binary**, not a synced-progress animation: there is one `Open` bool
  and a timeline that always runs to an endpoint. **A binary snap-open / snap-
  closed is the correct sync granularity** — there is no meaningful intermediate
  state to replicate (and the montage/timeline is short).

> Like doors, ALL of these verbs (`runAll`, `runTrigger`, `acivae`, `settime`)
> dispatch BP-internally (CallFunction → ProcessInternal), so a ProcessEvent
> POST observer on them NEVER fires — the same IDA-proven reason the door/
> light/container channels POLL the state field instead of observing the verb.

---

## 3. Cross-peer identity — the inherited `AtriggerBase_C::Key`

- `Agarage_C` inherits `FName Key` @ **0x0260** from `AtriggerBase_C`.
- It **implements `getTriggerData` / `loadTriggerData` / `ignoreSave_trigger`**,
  so it participates in the trigger-save system → its `Key` is assigned
  deterministically during `intComs_gamemodeMakeKeys` and is **save-persistent +
  stable across sessions** (the door RE proved this for the identical field on
  `Adoor_C`: a save-assigned FName, NOT a runtime-randomized `Aprop_C` GUID).
- The garage is a **pre-placed level actor** (not runtime-spawned), so both peers
  load it with the **same `Key`** → the Key is a free, reliable cross-peer
  identity. This is exactly the identity the existing `Channel` resolves by.

**Decision: key by `AtriggerBase_C::Key` (0x0260)** — the same `KeyedTogglePayload`
+ `Channel` machinery, no new identity scheme. (If a future map placed multiple
garages, the Key already disambiguates them; a class-singleton identity is
*unnecessary* and strictly weaker, so we don't use one.)

> Belt-and-suspenders: if a particular garage's Key ever came back `None`
> (un-keyed in a given save), the `Channel::RebuildIndex` already skips `None`
> keys and logs a per-channel `keysHash` for host-vs-client Key-stability
> verification — the smoke test will surface it. Given the save hooks are
> present, a non-None Key is expected.

---

## 4. Why it is unsynced today

Purely a **coverage gap**: `coop::interactable_sync` builds its poll/index over
three classes only — `ue_wrap::door::IsDoor` (`door_C`), `lightswitch::
IsLightSwitch`, `swinger::IsSwinger`. `Agarage_C` matches none of them, so it is
never indexed, never polled, and no packet kind carries its state. There is no
bug to fix in the existing channels — the garage class was simply never wired in.
(Confirmed: no `garage` / `Agarage` reference anywhere under `src/votv-coop/`.)

---

## 5. Sync design (decisive)

Extend the existing keyed-interactable engine. **One new feature file pair + one
adapter + one packet kind.** This is the catalogued extension point: the
`interactable_sync.h` header literally says "adding a fourth interactable later
is an adapter + a few lines."

### 5a. Mode: SYMMETRIC (not HostAuth)
The garage has **no auto-revert** (no sensor/autoclose/ReceiveTick), so the
symmetric poll model — which oscillated for doors *only because the door's sensor
re-drives `isOpened` every tick* — is **stable here**. Each peer is authoritative
over the change it causes:
- Each peer polls every indexed garage's `Open` field once per tick.
- On a delta, broadcast `GarageDoorState{ key, action=Open }`.
- The host relays a client-originated edge to the other clients
  (`IsClientRelayableReliableKind`).
- The receiver resolves the garage by Key and idempotently applies (skips if
  already matching → no echo; `Channel::ApplyResolved` already does this).
- On connect, the host snapshots every garage's current `Open` (ON *and* OFF) to
  the joiner (`QueueConnectBroadcastForSlot`) so a fresh joiner converges.

This is the **lights/containers shape**, and it reuses the `Channel` verbatim —
no hold-registers, no `DoorOpenRequest`, no autonomy suppression, no settle-
bridge needed. **No `GarageOpenRequest` kind is required** (that machinery exists
for doors solely to break the autoclose feedback loop, which the garage lacks).

> MTA precedent (the same one doors cite): the keyed-interactable channel is the
> single-syncer / state-broadcast pattern. For a non-auto-reverting toggle, the
> symmetric variant matches `CClientGame`'s "broadcast the resulting boolean
> state; the receiver applies it idempotently" — there is exactly one state
> source (the player who pressed), and the state is inert once set, so a
> last-writer-wins broadcast converges with no oscillation. The full HostAuth
> single-syncer (`CUnoccupiedVehicleSync` override + hold register) is reserved
> for entities whose local sim fights the applied state — doors, not the garage.

**Simultaneous-press edge (both peers press within one tick):** harmless. Both
read `Open=false`, both toggle locally to `true`, both broadcast `ON`. Receiver
applies `ON` (idempotent, already `true`) → both converge to OPEN. The only
loss is if peer A opens and peer B closes in the *same* tick (A→ON, B→OFF cross
in flight): last-writer-wins per the `seq`-ordered reliable channel; both
settle to whichever edge arrived last on each side, and the next poll re-broadcasts
any residual divergence. Acceptable for a garage (no functional harm; identical to
how lights already behave). If hands-on ever shows a visible flip-flop, promote to
HostAuth like doors — but the RE says it won't (inert state, no tick re-drive).

### 5b. Apply verb on the receiver: `settime(bool)` (snap), guarded by `mov`
On the receiver, drive the mirror with **`settime(target)`** — the snap-to-state
verb (montage @100x + `move.SetNewTime` to the endpoint). Rationale mirrors the
door `SmartApply`/`ForceTo` lesson: the animation only advances while the actor
ticks, and the receiving peer's player may be far from the garage (frozen
animation). `settime` completes the state (`move.SetNewTime` + `set()` resize the
Box + the montage at blend 100 snaps the mesh) regardless of proximity.

- **Idempotency / re-trigger:** check `Open == target` first and skip (the
  `Channel` already does `cur==want` → idempotent skip). Also respect `mov` — if
  the local garage is mid-animation toward the same target, let it finish (avoid
  a double-sound), same as door `SmartApply`'s "already going the right way"
  guard. Because `settime` sets `move.SetNewTime` directly it will *snap* rather
  than animate when far; for the near case a follow-up pass could prefer
  `acivae()` for a smooth swing, but **snap is correct and acceptable for v1**
  (a garage door seen opening/closing instantly is fine; polish later — same
  decision doors made: "correctness over a swing").
- The apply must run on the **game thread** (the reliable drain already does).

> NOTE on writing `Open` directly: `settime`/`acivae` set `Open` themselves; do
> NOT *also* poke 0x02E8 by hand on the receiver, or the poll baseline + the verb
> will disagree. The `Channel`'s `echo_` latch + `lastKnown_` update around the
> apply already prevent the applied state from being re-broadcast (same as the
> other three channels).

### 5c. Protocol
Add to `enum class ReliableKind`:

```cpp
GarageDoorState = 32,  // v44: garage door open/close (Agarage_C::Open @0x02E8, keyed by
                       //      AtriggerBase_C::Key). SYMMETRIC keyed-interactable (no auto-
                       //      revert -> no oscillation, unlike DoorState=9 which is HostAuth).
                       //      Payload: KeyedTogglePayload (40 B). Same generic Channel as
                       //      LightState/ContainerState. Relayed (IsClientRelayableReliableKind).
```

- **Reuse `KeyedTogglePayload`** (40 B: `WireKey key` + `uint8 action`) verbatim —
  exactly what DoorState/LightState/ContainerState use. `action` = the post-edge
  `Open`.
- **Slot 32 is free in the `ReliableKind` enum.** The current highest
  `ReliableKind` is `GrimeState = 31`. (`EntityPose = 32` lives in the *separate*
  `MsgType` enum — no collision.) NOTE: the `KeyedScalarPayload` *comment* (and a
  protocol.h note) loosely "reserved 32" for a never-implemented `GrimeDestroy`
  follow-up; that slot was never added to the enum, so it is available. To avoid
  any future confusion, either (a) take 32 for `GarageDoorState` and delete the
  stale "GrimeDestroy/32" reservation note, or (b) take **33** and leave 32 for a
  possible GrimeDestroy. Recommend **(a): GarageDoorState = 32**, and scrub the
  `GrimeDestroy` reservation comment (RULE 2 — don't keep a phantom reservation).
- Bump `kProtocolVersion` to **v44** (it is currently 43, protocol.h:409).
- Add `GarageDoorState` to `IsClientRelayableReliableKind` (symmetric → the host
  relays a client's edge to the other clients), beside `LightState` /
  `ContainerState`.

### 5d. New files (principle 7 + the modular-file rule)
1. **`src/votv-coop/include/ue_wrap/garage.h` + `.../src/ue_wrap/garage.cpp`** —
   the engine wrapper (one class = one wrapper). Mirrors `ue_wrap/door` but far
   smaller (no autoclose/sensor/hold/force machinery). API:
   - `bool EnsureResolved();` — resolve `garage_C` UClass; `Key` off (query
     `triggerBase_C`, since `FindPropertyOffset` does not climb to super — same
     gotcha door.cpp handles); `Open` off (query `garage_C`); the `settime`
     UFunction (and optionally `acivae`). Fallback constants from this dump:
     `Key 0x0260`, `Open 0x02E8`, `mov 0x02E9`.
   - `bool IsGarage(void* obj);` — `garage_C`-or-subclass check.
   - ~~`std::wstring GetKeyString(void* g);` — read `Key` (0x0260).~~ **[v123: replaced by
     `std::wstring GetNameKey(void* g);` = `R::ToString(R::NameOf(g))`, the level-export FName
     -- the save Key is unreliable, see the banner. GetKeyString/g_keyOff are deleted (RULE 2).]**
   - `bool TryReadOpen(void* g, bool& open);` — read `Open` (0x02E8).
   - `bool ApplyOpen(void* g, bool open);` — idempotent (skip if `Open==open` or
     `mov` is mid-animation toward `open`); else `settime(open)` via a ParamFrame
     `Set<bool>(L"Open", open)` + `Call`. Game thread.
   - (No `SuppressAutonomy` / `RequestApply` / `SuppressHeld` — Symmetric.)
2. **Wire it into `coop/interactable_sync.cpp`** (this file is ~782 LOC; adding an
   adapter + a `Channel` keeps it under the 800 soft cap — borderline, so the
   audit should confirm; if it tips over, the clean extraction is to split the
   three+ adapters into an `interactable_adapters.cpp`, but that's optional and
   should be a separate prior commit per the extraction rule):
   - `const Adapter g_garageAdapter = { "garage", ReliableKind::GarageDoorState,
     &ue_wrap::garage::EnsureResolved, &ue_wrap::garage::IsGarage,
     &ue_wrap::garage::GetKeyString, &ue_wrap::garage::TryReadOpen,
     [](void* a, bool on){ return ue_wrap::garage::ApplyOpen(a, on); },
     nullptr × 6 };` (Symmetric → all HostAuth hooks null, exactly like
     `g_lightAdapter` / `g_containerAdapter`).
   - `Channel g_garage{g_garageAdapter};` (default `Mode::Symmetric`).
   - Add `case ReliableKind::GarageDoorState: return &g_garage;` to
     `ChannelForKind`.
   - Index it in `IndexChannels` (`g_garageIndexed`), and add `g_garage` to
     `Install` / `QueueConnectBroadcastForSlot` / `Tick` / `OnDisconnect`
     (mechanical, one line each, beside the other three).
3. **`event_feed`** — route the new kind to `interactable_sync::OnReliable`
   exactly like `DoorState` / `LightState` / `ContainerState` (it already
   switches on the ReliableKind; add the `GarageDoorState` case to that switch).

### 5e. Hook points (exact)
- **Sender:** none new — `Channel::PollAndBroadcast` (driven by `Tick`) reads
  `Open` over the garage index every net-pump tick and broadcasts deltas. This
  captures *every* activation source uniformly (the wall button via
  `runAll→runTrigger`, any future exterior/vehicle trigger, a script, the save-
  load `loadTriggerData`) — which is precisely why we sync the **garage's `Open`**
  and do NOT need to observe the button at all.
- **Receiver:** `Channel::OnReliable → ApplyResolved → g_garageAdapter.ApplyState
  → ue_wrap::garage::ApplyOpen → settime(open)`. Game thread (reliable drain).
- **Connect snapshot:** `Channel::QueueConnectBroadcastForSlot` (already called
  per-slot on the net-pump connect edge) sends each garage's current `Open`.
- **Relay:** add `GarageDoorState` to `IsClientRelayableReliableKind`.

### 5f. What we deliberately do NOT do
- Do **not** sync the button (`Atrigger_button_C`) — its press is just one source
  of the garage's `Open` change, which the poll already catches. Syncing the
  button mesh press would be redundant cosmetics.
- Do **not** add HostAuth / hold-registers / `GarageOpenRequest` / autoclose
  suppression — the garage has no auto-revert, so none of the door machinery
  applies (RULE 1: don't carry door complexity the garage doesn't need; RULE 2:
  the symmetric path already exists).
- Do **not** sync the timeline progress / `move_a` continuously — binary
  snap-open/closed is the right granularity (short montage; no meaningful
  intermediate state).

---

## 6. Field/verb reference (Alpha 0.9.0-n; from this CXX dump + disasm)

| Symbol | Class (declaring) | Offset | Type | Role |
|---|---|---|---|---|
| `Key` | `AtriggerBase_C` | 0x0260 | FName | cross-peer identity (save-persistent) |
| `Open` | `Agarage_C` | 0x02E8 | bool | **the synced open/closed state** |
| `mov` | `Agarage_C` | 0x02E9 | bool | "animating" guard (skip re-apply while set) |
| `move` | `Agarage_C` | 0x02E0 | UTimelineComponent* | open/close animation |
| `move_a_<guid>` | `Agarage_C` | 0x02D8 | float | timeline alpha 0..1 |
| `move__Direction_<guid>` | `Agarage_C` | 0x02DC | uint8 | 0=Forward/open, 1=Backward/close |
| `settime(bool Open)` | `Agarage_C` | — | UFunction | **snap-to-state apply verb** (montage@100x + SetNewTime) |
| `acivae()` | `Agarage_C` | — | UFunction | animated toggle w/ sound (player path) |
| `runTrigger(AActor*, int32)` | `AtriggerBase_C`/`Agarage_C` | — | UFunction | trigger fan-in (button → garage) |
| `runAll(int32)` | `AtriggerBase_C` | — | UFunction | button fan-out to `Objects[]` |

**Protocol:** `ReliableKind::GarageDoorState = 32` (v44), payload
`KeyedTogglePayload` (40 B), Symmetric, relayed. Modules:
`ue_wrap/garage` (new) + extend `coop/interactable_sync` + `event_feed` routing.

---

## 7. RE provenance
- SDK: `Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/{garage,
  trigger_button,triggerBase,cargoliftDoor,villageGates,door}.hpp`.
- Disasm: `tools/bp_reflect.py garage trigger_button` →
  `research/bp_reflection/{garage,trigger_button}.json`; rendered with
  `research/bp_reflection/_cfg.py <asset> <fn> [entry]` and raw stmts via
  `_dumpstmt.py garage ExecuteUbergraph_garage 1386 1415` (confirmed `Open`
  @0x02E8 is an `EX_InstanceVariable`, not a frame temp).
- Pattern to extend: `coop/interactable_sync.{h,cpp}`, `ue_wrap/door.{h,cpp}`,
  `coop/net/protocol.h` (KeyedTogglePayload + ReliableKind), `session_lanes.h`
  (`IsClientRelayableReliableKind`). Prior RE:
  `votv-doors-and-lightswitches-RE-2026-05-25.md`,
  `votv-keypad-door-BP-disassembly-2026-06-06.md`.
