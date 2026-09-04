# KERFUR ARC — the client as a second worker, with the kerfur as the benchmark

*[↑ kerfur KB index](README.md) · [↑ docs index](../README.md)*

> **NEW 2026-09-04, USER MACRO-GOAL.** The LIVING doc for the goal below AND for the kerfur family
> that measures it. Read it before ANY kerfur work; the older `docs/kerfur/NN-*.md` files are
> point-in-time diagnoses of the June 2026 identity/conversion bugs and remain valid as history,
> but the capability census and the plan live HERE.
>
> **Status: RE + census, round 1. NOTHING BUILT in this arc yet.** Every fact below is tagged.

---

## 0. The goal

### 0.1 In the user's words

> «Даже если мультиплеер это где игроки у хоста тупо функции керфур роботов выполняют — это уже
> заебись. **ВОТ К ЭТОМУ ПРИЙТИ ХОТЯ БЫ. Act as host type of robot.**»
>
> «Это будет макро-цель — изучить и задокументировать все возможности керфура обычного и керфура
> апгрейднутого… И затем добиться полного синка, полной работы этих функционалов.»
>
> **The clarification that sets the real bar (2026-09-04, and it reframes everything above):**
> «В макро-цели я имею в виду не просто нажатие действия, а имеется в виду **сам клиент настолько
> могучий, что и сам по себе, своими действиями игровыми (ходьба, взаимодействие с серверами и тд)
> может быть по сути равен или более полезен как керфур робот хоста**.»

### 0.2 The macro-goal, stated so it can be closed

> **A client is a full second WORKER on the base. Every job a kerfur does for the host — fix
> servers, collect reports, fix transformers, carry things, patrol — a client can do WITH THEIR OWN
> HANDS, and the result lands in the shared world exactly as if the host had done it. The bar is
> "no worse than the robot"; the goal is "better than the robot".**

**The kerfur is the BENCHMARK, not the object of the work.** That is the whole reframe, and it has a
hard edge the user drew explicitly:

> **Commanding the ROBOT cross-peer is NOT part of this goal.** *(«Я не имел в виду взаимодействие с
> роботами, хотя это тоже норм но в будущем», 2026-09-04.)* It is a future nice-to-have. The
> existing robot-sync lanes stay built and stay working; nothing here retires them, and §4 keeps
> tracking their gaps — but **no robot gap blocks this goal, and none is scheduled by it.**

So the robot's capability list (§2/§3) earns its place for exactly one reason: **the game itself
already decided that this set of jobs is what keeps a base running**, which makes it a ready-made,
non-invented specification of what a client must be able to do. The single question per job is:

> **Can a CLIENT PLAYER do this job themselves, and does the result land in the shared world?**

Three clauses make it falsifiable rather than aspirational:

1. **The denominator is the job list (§0.3), not "everything".** A percentage with no divisor is
   what produced `COOP_SYNC_PROFILES.md` in the first place. **The list may GROW** — this round
   alone added `kill` and `sitOnAtv`, neither previously known. A growing honest denominator beats a
   fixed invented one.
2. **The unit is the whole job, not the verb.** Its state, its accumulated result, the items it
   carries. A relayed `get_reports` with an unsynced floppy is zero progress, not most of the way.
3. **Plus the late-join row.** A peer arriving mid-job sees it correctly (principle 8).

**Closure:** every row of §0.3 answers YES with evidence from a real two-peer run.

### 0.3 THE JOB LIST — the benchmark, and the thing that actually gets tracked

Derived from §2/§3 — the robot's capability set, read as a job spec. **The only tracked column is
"can a client do it by hand".** The robot column is context: it says why the job is on the list, and
its own cross-peer state is future work (§4), not a blocker here.

**The starting point is NOT zero, and the doc must not read as if it were (USER, 2026-09-04):**
*«Вообще-то клиент уже многое умеет, просто мы то что он умеет до эталона доведём, максимального
robustness, а что не умеет — сделаем.»* So each row asks one of two questions, never "does anything
work": **HARDEN** — it works, does it hold to the benchmark under every condition? — or **BUILD** —
it is genuinely absent. `[?]` below means *not yet measured against the benchmark*, not *unknown
whether it exists*.

| # | job | **client, by hand** (the goal) | mode | why it is on the list | base it rests on |
|---|---|---|---|---|---|
| J1 | move around the base | works — pose lane built | HARDEN | robot: follow/idle/patrol | — |
| J2 | carry / move objects | works — grab + `prop_drop_intent`; edges `[?]` | HARDEN | robot: `take_object` | prop identity |
| J3 | **fix servers** | **`[RD]` DOES NOT COUNT** — lane is one-directional, no client->host path (§0.5) | **BUILD** | robot: `fix_servers` + `findBrokenServer` | server system |
| J4 | **collect reports (floppy)** | **`[RD]` ENTIRELY UNSYNCED** — the server's floppy triple is on no wire (§0.5) | **BUILD** | robot: `get_reports` | signals + floppy props |
| J5 | **fix transformers** | **`[V]` NO LANE EXISTS** — 24/24 `transformer` hits are the kerfur verb string (§0.5) | **BUILD** | robot: `fix_transformers` + `goTransfo` | **POWER CHAIN — PARKED** |
| J6 | drive the ATV | works, on a C1 crutch | HARDEN | robot: `sitOnAtv` | **ATV — C1, PAUSED** |
| J7 | use equipment / inventory | works; facets broken | HARDEN | robot: `equipment` / drip | container facets |
| J8 | patrol / watch the base | works | — | robot: `patrol` | — |

**J3/J4/J5 are the jobs that make a base run, and none of the three has been measured to the
benchmark cross-peer.** That is this round's headline: the macro-goal's centre of gravity is not the
robot at all — it is how well a second pair of human hands holds up doing server, report and
transformer work.

**Consequence for foundation-first:** the POWER CHAIN base (parked at `/qf` R9) moves from "blocks
one side lane" to **main-line blocker**, because J5 is a third of the core work.

### 0.4 Milestones

| | bar | met when |
|---|---|---|
| **M0 — measure** | J3/J4/J5 driven by a client and measured against the benchmark | three runs, or three defects filed |
| **M1 — the worker** | every job in §0.3 holds to the benchmark; the BUILD rows exist | **a client alone can keep a base running** |
| *(future)* | the robot obeys a client too | §4 has no `NOT SYNCED` row — explicitly NOT scheduled by this goal |

**The rule that names the shape** is `COOP_SYNCER_MODEL.md` §2b — ACT-AS-HOST. A client authors an
INTENT naming WHAT; the host arbiter performs it; results flow back as ordinary state. A client
fixing a server by hand is exactly that class of shared-world write.

---

## 0.5 M0, static pass — the three core jobs, read off the code (2026-09-04)

Before spending a two-peer run, the same question was put to the source. **All three core jobs come
back negative, each for a different reason.**

**Read the tags precisely, because the two halves have different strength.** The MECHANISM is `[V]`
in all three cases — an absent send site, a field absent from a payload, and a grep with zero real
hits are measurements, not inferences; a lane with no client->host path cannot carry a client's work
by construction. What is `[RD]` is only the **user-visible symptom** — exactly what the player sees
and when. A run upgrades that half, and it now knows precisely what to look at.

### J3 — a client fixes a server by hand: DOES NOT COUNT (mechanism `[V]`, symptom `[RD]`)

The player-side verb exists: `AserverBox_C::fix()` (`serverBox.cpp:1268`), reached through a
minigame (`minigame` / `staticMinigame` fields); `getActionOptions` offers `Use(4)`.

Our lane `coop/interactables/serverbox_sync` is **host-authoritative and one-directional by
declaration** — its own header says *"Client never SENDS server state"*, and `[V]` the .cpp has
exactly two send sites (`:272` broadcast, `:288` per-slot connect seed), both host-side. There is no
client->host path of any kind.

`[V]` the wire carries four values and no more (`protocol.h:3908`):

```
ServerStatePayload { int32 brokenServers; float effCalc; float effDownl;
                     uint8 serverCount; uint64 isBrokenMask; }   // 24 bytes
```

So a client's `fix()` flips its OWN `isBroken` and nothing else happens: the host's
`brokenServers` still counts it, the host's efficiency is unchanged, the SAT console on the host
still reports it down. **And the host broadcasts ON CHANGE only** — so the client's phantom fix is
not even corrected promptly; it stands until the host's own state next moves, then is stomped.
Silent divergence, not a visible failure.

Root: the fix verb is `EX_LocalVirtualFunction` — invisible to BOTH the ProcessEvent detour and the
Func patch (measured when the lane was built). This is precisely the class `COOP_SYNC_DOCTRINE`
[withdrawn at the author's request]
a first-class consumer of that pending decision, and it is a stronger argument for it than any
listed there today.**

### J4 — a client collects reports by hand: ENTIRELY UNSYNCED (mechanism `[V]`, symptom `[RD]`)

`[V]` `AserverBox_C` carries the SAME floppy triple as the kerfur — `floppyType`,
`floppyReadwrites`, `floppyData: TArray<FString>` (`serverBox.cpp:18/24/68`) — with
`insertFloppy(Aprop_floppyDisc_C*)` (`:870`) and `ejectFloppy()` (`:862`), and
`getActionOptions` tracks a dedicated `lookatFloppyButton` component. That is the player's report
collection: put a disc in the server, the server writes report data onto it.

**None of those three fields is on any wire.** `floppybox_sync` is a different thing — it syncs the
disc CRATE's LIFO stack (`Aprop_floppyBox_C`), not the server's slot. So a client inserting a disc
into a server produces data that exists on exactly one machine.

That the kerfur and the server hold the identical triple is the useful part: **one floppy-data lane
serves J4 by hand AND `get_reports` by robot.** Design it once.

### J5 — a client fixes a transformer `[V]` NO LANE EXISTS

`[V]` `transformer` appears 24 times in `src/votv-coop/` and **every single hit is the kerfur verb
string** `fix_transformers` (the relay's enum, its name mapping, its bounds check, and comments).
There is no transformer element, no payload, no poll, no receiver.

The base POWER PANEL is synced (`power_sync.cpp`, a 5-breaker mask) and the serverbox lane
neutralizes a local `ticker_serverBreaker`. The transformer itself — the thing
`trigger_fuckUpTransformer` breaks and `goTransfo` sends the kerfur to — is not covered by either.

This is the same gap the power-chain study named on 2026-09-02 ("breaker panels synced,
reactor/generator/transformer outcomes in the gap list"), now reached from the other direction.
**J5 is not a bug to fix in a lane; it is a lane that was never built, on a base that is parked.**

### What this changes

- The macro-goal's real content is **three unbuilt/one-directional lanes**, not robot polish.
- **J3 and J4 share a root with each other** (the server is one actor holding both), and **J4 shares
  its root with the robot's `get_reports`** (the same floppy triple). One design covers three jobs.
[withdrawn at the author's request]
  /qf owed"; this arc gives it a named consumer with a user-visible symptom.

### WP status

| WP | what | status |
|---|---|---|
| **WP-0** | Capability census of both robots (this doc §1-§4) | **round 1 DONE** — §5 lists what is still `[?]` |
| WP-1 | Gap table: capability x sync-lane x evidence (§6) | **round 1 DONE**, per-row evidence still thin |
| WP-2 | Foundation-first audit — which bases must land first (§7) | **DONE, and it BLOCKS two lanes** |
| WP-3 | Seam decision per verb (dispatch ladder) | NOT STARTED |
| WP-4 | `/qf` the design to convergence | NOT STARTED — owed before any build |
| WP-5 | Build | NOT STARTED |

---

## 1. There are TWO robots, and they are not variants of one class `[V]`

Measured 2026-09-04 with `tools/bp_cpp.py` (BlueprintToCpp) over the 0.9.0n pak.

| | **Kerfus** — "the regular one" | **Kerfur Omega** — "the upgraded one" |
|---|---|---|
| class | `Ap_kerfus_C` | `UkerfurOmega_C` |
| **parent** | **`Aprop_corded_C`** -> `Aprop_C` | **`ACharacter`** |
| so it is… | a PROP with a POWER CORD | a full character with a movement component |
| off form | `prop_kerfusBody_C` `[?]` | `Aprop_kerfurOmega_C` (a real `Aprop_C`) |
| colour variants | `p_kerfus_p/_r/_y/_col/_col_gamer` (5) | ~30 data-only skin subclasses |
| radial actions | **3** | **10** |
| decompile | `research/bp_reflection/cpp/p_kerfus.cpp` (71 KB) | `.../kerfurOmega.cpp` (217 KB) + `.offsets.txt` |

**Why every previous grep missed the regular one: it is spelled `kerfus`, with an `s`.** Four months
of kerfur work in this repo searched for `kerfur*` and therefore censused exactly one of the two
robots. The name surfaced from `list_store.uasset`, which sells both (`kerfus`, `kerfuro`).

`[V]` **`p_kerfus_C` appears NOWHERE in our source.** All 14 hits for "kerfus" in `src/votv-coop/`
are about `kerfusFace_C`, the face actor we spawn for PLAYER skins (`coop/player/skin_effects.cpp`)
— a different thing that merely shares the name. **The regular robot has no sync of any kind.**

---

## 2. Kerfur Omega — the capability census `[V]`

### 2.1 The radial menu

`kerfurOmega_C::getActionOptions` (`kerfurOmega.cpp:6676`) builds the list:

```
busy  = (state == 3 || state == 4 || state == 5)
list  = busy ? [ "Turn_off" ]
             : [ "Turn_off","Follow","Idle","Patrol","Fix_servers",
                 "Get_reports","Fix_transformers","Take_object","Pat" ]
list += "Equipment"                      // always appended
```

So **10 entries when idle, 2 when busy.**

### 2.2 `state` IS `enum_kerfurCommand` `[V]`

The game's own enum (`main/enums/enum_kerfurCommand.uexp`, display names parsed in order) and the
`state` byte the verbs write are the same value space:

| value | enum name | set by verb | menu label |
|---|---|---|---|
| 0 | `follow` | `follow` -> `@21624` | Follow |
| 1 | `idle` | `idle` -> `@21404` | Idle |
| 2 | `patrol` | `patrol` -> `@21184` | Patrol |
| 3 | `fix` | `fix_servers` -> `@20950` | Fix_servers |
| 4 | `report` | `get_reports` -> `@21874` | Get_reports |
| 5 | `transformer` | `fix_transformers` -> `@22108` | Fix_transformers |
| 6 | **`sitOnAtv`** | **no menu entry** — set at `kerfurOmega.cpp:5724` | — |

**States 3/4/5 are the BUSY states**, and the guard is symmetric: while busy, the menu collapses AND
every state-changing branch in `actionName` refuses (each of the six branches re-tests
`state==3||4||5` before writing). That is a real in-BP mutual exclusion an arbiter must respect —
not something to reimplement, something to route through.

### 2.3 The verb switch — 11 verbs, one of them hidden `[V]`

`actionName` enters the ubergraph at `@20350`; the string switch, in bytecode order:

| # | verb string | jumps to | effect |
|---|---|---|---|
| 1 | `turn_off` | `@21844` | -> `dropKerfurProp()` — destroy NPC, spawn `prop_kerfurOmega` |
| 2 | `follow` | `@21624` | state := 0 |
| 3 | `idle` | `@21404` | state := 1 |
| 4 | `patrol` | `@21184` | state := 2 |
| 5 | `fix_servers` | `@20950` | state := 3 |
| 6 | **`kill`** | `@21859` | **not in `getActionOptions`** — scripted/hidden `[?]` |
| 7 | `get_reports` | `@21874` | state := 4 |
| 8 | `fix_transformers` | `@22108` | state := 5 |
| 9 | `take_object` | `@22342` | pick up / carry a prop |
| 10 | `pat` | `@22581` | affection + meow |
| 11 | `equipment` | `@23241` | opens `ui_objectUpgrades_C` |

### 2.4 Capabilities that are NOT menu verbs `[V]`

Functions with real bodies (not interface stubs, not ubergraph trampolines). This is where most of
the un-synced surface lives:

| function | LOC-ish | what it is |
|---|---|---|
| `holdObject_kerf` | 150 | the carried-object state machine (the biggest single body in the BP) |
| `unequipItem` / `equipItem` / `loadHoldItem` | 85/29/18 | equipment in and out |
| `updateDrip` | 75 | accessories: reads `list_kerfurDrip`, spawns/attaches meshes per bone |
| `findBrokenServer` | 98 | target selection for `fix_servers` |
| `attemptMurerfur` | 48 | the murderkerfur transformation |
| `sitOnCar` / `getOffCar` / `tryToOccupyCar` / `failCar` | 35/21/6/6 | **rides the ATV** (`state`=6) |
| `RC` | 34 | **remote control** (`remoteControlSpeed = 400`, `RC_vector`) |
| `dropKerfurProp` | 32 | the turn-off conversion |
| `findTask` / `findTransformer` / `targetLocation` | 30/23/18 | task target selection |
| `setStyle` / `makeFace` / `setFace` | 25/11/13 | face + skin |
| `makeSentient` | 19 | the `sentient` flag (carried across conversion) |
| `getData` / `loadData` | 27/40 | its save blob |

Plus ubergraph-resident events: `move`, `moveToServ`, `findServer`, `doTask`, `checkDoor`,
`goTransfo`, `grabAnimation`, `dropObject`, `stepped`, `makeMeow`, `startKill`, `bindedHoldObject`,
`upgradeTake`, `playerUsedOn`, `timer_face`, `timer_kerf`, `spookymove`, `ignite`, `addDamage`.

**Census totals:** 229 functions; 161 are ubergraph trampolines; 68 have their own bodies, of which
~40 are `int_objects` interface stubs returning a constant.

### 2.5 `Get_reports` — the floppy, in full `[V]`

The user's description ("даёшь кассетку и отправляешь собирать репорты — это дейлики") maps onto a
real, stateful mechanism:

- The player hands the kerfur a **floppy disc** — `Aprop_floppyDisc_C` or `Aprop_floppyDisc_Wh_C`
  (`kerfurOmega.cpp:1871-1884`); anything else prints *"You need to hold a proper floppy disc"*
  (`:1890`).
- It absorbs the disc into fields: `hasFloppy`, `floppyType` (via `lib::typeFromFloppy`),
  `floppyData: TArray<FString>`, `floppyReadWrites: int32` (`:1899-1925`).
- On task completion it appends: `task->getFloppyData(out)` -> `floppyData.Add(out)` and
  **`floppyReadWrites -= 1`**, gated on `floppyReadWrites > 0` (`:289-304`).
- Giving it back RE-SPAWNS a real actor from `lib::floppyFromType(floppyType)` and stamps
  `SetArrayPropertyByName(actor,"data",floppyData)` + `SetIntPropertyByName(actor,"readWrites",…)`
  (`:668-704`, and two more sites at `:1331` and `:5451`).

**Sync consequence.** This is a discrete persistent shared-world change carrying a per-entity data
payload across an actor destroy/create gap. It needs act-as-host for the give/take, host authority
for the accumulation, and identity carried across the floppy's actor gap — the
`prop_drop_intent.cpp` shape. **A relayed verb alone accomplishes nothing here.**

---

## 3. Kerfus — the capability census `[V]`

### 3.1 The radial menu

`p_kerfus_C::getActionOptions` (`p_kerfus.cpp:2155`) — this one returns raw
`enum_interactionActions` values, not strings:

```
active  -> options_enum = [ 8, 4, 6 ]     // Activate, Use, Pat
!active -> options_enum = [ 8 ]           // Activate
```

`enum_interactionActions` measured in order: `Grab(0) Hold(1) Collect(2) Put(3) Use(4) Toggle(5)
Pat(6) Take(7) Activate(8) Sit(9) Open(10) Close(11) Equip(12) Create(13) Edit(14)`.

**3 actions active, 1 inactive — against Omega's 10.** This is the difference the user named, and
`Get_reports` is genuinely absent: Kerfus has no report verb and no floppy fields.

### 3.2 What it can do `[V]`

| function | what |
|---|---|
| `cordPlugged` / `cordUnplugged` | **it runs off a power cord into an `AcordSocket_C`** |
| `possess` / `possessTimer` / `targetActor(possessLoc)` + `kerfusPossessor_C` | **the player POSSESSES it and drives it** |
| `jump` / `checkJump` / `animJump` / `jumpTimeline` / `RCjump` | it jumps (`kerfurJump` sound) |
| `movePawnTo` / `updatePath` / `checkPath` / `makeTurn` / `setDamping` | wheeled navigation |
| `findBrokenServer` | **it DOES have server targeting** — with no radial verb to reach it `[?]` |
| `task` | its task loop |
| `meowAnim` / `upd(skipFace)` | meow + face |
| `enterWater` / `leaveWater` / `enteredTheWater` / `exitTheWater` | water |
| `getData`/`loadData` + `getTriggerData`/`loadTriggerData` | **two** save blobs |
| `crafted` | it can be crafted |

**Possession is the headline.** `possess()` sets `active := true`, clears `moveTo`, and calls
`movePawnTo()`; there is a whole `kerfusPossessor_C` (35 KB decompile). A second player possessing a
robot is a player-authority question our model has never had to answer for a non-pawn.

---

## 4. What is synced TODAY

| capability | lane | status |
|---|---|---|
| Omega `follow` | `kerfur_command` Command::Follow(0) | BUILT — host-driven MoveTo toward the REQUESTING peer's body |
| Omega `idle`/`patrol`/`fix_servers`/`get_reports`/`fix_transformers` | `kerfur_command` 1-5 | BUILT — host re-runs the real `actionName` via ProcessEvent |
| Omega `turn_off` / turn-on | `kerfur_convert` + `kerfur_entity::BindFormActor` | BUILT — death-watch poll @5 Hz, `KerfurConvert` broadcast |
| Omega NPC pose | `npc_sync` / `npc_mirror` | BUILT |
| Omega identity across form flip | `KerfurId` + `BindFormActor` | BUILT |
| Omega head/body facing | `npc_pose_*` | BUILT |
| **Omega `take_object`** | — | **NOT SYNCED** — declared out of scope in `kerfur_command.h` |
| **Omega `pat`** | — | **NOT SYNCED** — same |
| **Omega `equipment`** | — | **NOT SYNCED** — same |
| **Omega `kill`** | — | **NOT SYNCED, and not previously known to exist** |
| **Omega `sitOnAtv` (state 6)** | — | **NOT SYNCED** |
| **Omega floppy state** (`hasFloppy`/`floppyType`/`floppyData`/`floppyReadWrites`) | — | **NOT SYNCED** — so `get_reports` relays the VERB and diverges on the RESULT |
| **Omega drip / accessories** | — | **NOT SYNCED** |
| **Omega carried object** (`holdObject_kerf`) | — | **NOT SYNCED** |
| **Omega `sentient`** | carried by `KerfurConvert` payload only | PARTIAL |
| **Omega `murderfur`** | — | **NOT SYNCED** |
| **Kerfus — everything** | — | **NOT SYNCED. The class is absent from our source.** |

Header comment of record, `coop/creatures/kerfur_command.h`: *"take_object/equipment/pat are Invalid
(per-player / UI / montage, out of scope)"*. **That scope line is what this arc retires.**

---

## 5. Open questions `[?]`

1. `kill` (verb 6) — who calls it, and does `startKill` / `attemptMurerfur` hang off it?
2. Kerfus's off form — is `prop_kerfusBody_C` the OFF actor, or a gib?
3. Kerfus `findBrokenServer` with no verb to reach it — dead code, or driven by `task()`?
4. Is `sitOnAtv` reachable by a player at all, or only by the kerfur's own AI?
5. Does the busy-state guard (3/4/5) also gate `take_object` / `pat` / `equipment`? (They are past
   the guarded branches in the switch; not yet read.)
6. `kerfusPossessor_C` — how does possession start and end, and what happens to the possessing
   player's own pawn?
7. Which of these fields are in the save blob (`getData`/`loadData`) — that decides the late-join
   answer for each.

---

## 6. Foundation-first audit `[V]` — two lanes are BLOCKED

Per `[[feedback-foundation-first-build-the-base-a-sync-rests-on]]`, before designing X we ask what
base X rests on. Two answers came back hard:

- **Kerfus rests on the POWER/CORD base.** It is an `Aprop_corded_C`; its `active` state is driven by
  `cordPlugged`/`cordUnplugged` against `AcordSocket_C`. **The power-chain base lane is PARKED**
  (`/qf` paused at R9, 2026-09-02 — `[[project-power-chain-base-and-pc-lane-2026-09-02]]`). A Kerfus
  sync built now would need a hold/retry register to tolerate power divergence — which is exactly
  the test that says STOP.
- **`sitOnAtv` rests on the ATV lane**, which is **CRUTCH C1** (`docs/CRUTCHES.md`) and PAUSED by the
  user with one arm parked ready (`docs/vehicles/ATV.md` §17). A kerfur riding a frozen-corpse mirror
  is not a capability, it is a second symptom of C1.

**Neither blocks the Omega work**, which is the larger half and the one the user's quote is about.
So the arc order is: **Omega first, Kerfus after the power base, `sitOnAtv` after C1.**

---

## 7. The plan (draft — `/qf` owed before any build)

Ordered by "closes a capability the user can see", with the doctrine's steps applied per lane.

- **K-1 — retire the "out of scope" line: `pat`.** The smallest complete act-as-host lane in this
  family (montage + meow + an affection counter `[?]`). Its value is that it proves the intent path
  for a verb that is NOT a `state` write, which is what 2-5 all are.
- **K-2 — `take_object` / `holdObject_kerf`.** The kerfur picking up a prop is a discrete persistent
  shared-world change over an entity that already has an eid. Reference lane: `prop_drop_intent`.
- **K-3 — the floppy lane (`get_reports` done properly).** Give/take intent + host-owned
  accumulation + identity across the disc's actor gap. This is what makes the dailies actually work
  cross-peer rather than just changing a state byte.
- **K-4 — `equipment` / drip.** Needs the `ui_objectUpgrades_C` seam and `upgradeTake`; the
  accessory set is datatable-driven so the wire can carry row NAMES, never meshes.
- **K-5 — `kill` / murderfur**, once §5.1 is answered.
- **K-6 — Kerfus, whole** — GATED on the power base.
- **K-7 — `sitOnAtv`** — GATED on C1.

Every lane owes, before it is DONE: its authority row (§2 of the doctrine), its seam decision proven
by probe not assumption, its brain-parking statement, its identity-at-birth answer, its **mid-join
row** (principle 8), a protocol bump, and evidence from a real two-peer run.

---

## 8. Instruments + evidence

All §1-§4 facts this round: `python tools/bp_cpp.py <BP> [--offsets]` (BlueprintToCpp `a504452`,
Yangff `3a7122b`) over the 0.9.0n pak, plus direct byte-parsing of `enum_kerfurCommand.uexp`,
`enum_kerfurDripType.uexp`, `enum_interactionActions.uexp`, `list_kerfurDrip.uasset`,
`list_store.uasset`. Decompiles land in `research/bp_reflection/cpp/` (gitignored — derived game
content).

Per `[[feedback-rebase-old-tool-facts-on-new-instruments]]`, the June 2026 kerfur facts in
`docs/kerfur/0*.md` were derived with the older hand-walked `to-json` route. They are NOT re-based
yet; do that for any fact a fix will stand on. Nothing in this doc inherits them — §1-§4 were
re-derived from scratch on 2026-09-04.

**Not measured at runtime.** Everything here is static decompilation. No probe has run, no build has
been made, no claim in this doc is `[V]`-by-log.
