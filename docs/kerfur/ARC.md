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
| **J9** | **process signals at the workstation** (USER 2026-09-04) | **works broadly — 29 lanes built, 1 verified, much of it on crutches** (§0.6) | **HARDEN** | not a robot job — it is the game's core loop | signal desk; **upgrades OPEN-3** |

**J3/J4/J5 are the jobs that make a base run, and none of the three has been measured to the
benchmark cross-peer.** That is this round's headline: the macro-goal's centre of gravity is not the
robot at all — it is how well a second pair of human hands holds up doing server, report and
transformer work.

**Consequence for foundation-first:** the POWER CHAIN base (parked at `/qf` R9) moves from "blocks
one side lane" to **main-line blocker**, because J5 is a third of the core work.

### 0.4 Milestones

| | bar | met when |
|---|---|---|
| **M0 — measure** | J3/J4/J5 driven by a client and measured against the benchmark; **J9's crutches censused into `docs/CRUTCHES.md`** | three runs or three defects filed, plus a crutch census |
| **M1 — the worker** | every job in §0.3 holds to the benchmark; the BUILD rows exist | **a client alone can keep a base running AND run the signal desk** |
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
a first-class consumer of that pending decision, and a stronger argument for it than anything listed
there today.**

**But J3 is NOT BLOCKED on WP-1, and the doc must not be read that way.** Tier 5 — per-site
reconcile — is available now and fits: the client already holds a host-driven `isBroken` per box, so
an un-commanded local `broken -> fixed` transition on the client IS the observable, no verb
interception required. The lane shape that follows: **client detects the flip, sends a fix INTENT
naming the server index; the host validates (that box is broken on the host; the sender is plausibly
near it) and runs the real `fix()` on its own copy; the existing host->client mirror carries the
result back** — the `order_sync` shape, with the existing `ServerState` broadcast as the return
path. WP-1 would later replace the poll with a clean cancel-capable seam; it is an upgrade, not a
precondition.

### J4 — a client collects reports by hand: ENTIRELY UNSYNCED (mechanism `[V]`, symptom `[RD]`)

`[V]` `AserverBox_C` carries the same floppy state as the kerfur, and it is **FOUR fields, not
three**: `floppyType`, `floppyReadwrites`, `floppyData : TArray<FString>` and
**`floppyObjectData : FString`** (the fourth found in the offsets listing's property block, missed
on the first pass) — with `insertFloppy(Aprop_floppyDisc_C*)` (`serverBox.cpp:870`) and
`ejectFloppy()` (`:862`), and `getActionOptions` tracking a dedicated `lookatFloppyButton`
component. That is the player's report collection: put a disc in the server, the server writes
report data onto it.

**None of those four fields is on any wire.** `floppybox_sync` is a different thing — it syncs the
disc CRATE's LIFO stack (`Aprop_floppyBox_C`), not the server's slot. So a client inserting a disc
into a server produces data that exists on exactly one machine.

That the kerfur and the server hold the identical state is the useful part: **one floppy-data lane
serves J4 by hand AND `get_reports` by robot.** Design it once.

### The W1 seam question, measured `[V]` — and the answer is better than the verbs suggest

W1's plan named one open `[?]`: the dispatch of `insertFloppy` / `ejectFloppy`. Measured
2026-09-04 by resolving `StackNode` / `VirtualFunctionName` in the kismet dump
(`tools/bp_reflect.py serverBox`):

| symbol | dispatch | verdict |
|---|---|---|
| `insertFloppy` | `EX_LocalVirtualFunction` from `pocessFloppy` | invisible |
| `ejectFloppy` | `EX_LocalVirtualFunction` from the ubergraph | invisible |
| `fix` / `breakServer` / `check` | `EX_LocalVirtualFunction` | invisible |
| `launchServerMinigame` | `EX_LocalVirtualFunction` | invisible |

So every VERB is invisible, as feared. **But the ENTRY POINTS that reach them are not**, and that is
what the design actually needs:

- **EJECT is reached from `actionOptionIndex`** — `[V]` ubergraph `@4519 if (action == Use(4))` ->
  `@4560 if (lookatFloppyButton)` -> `@4574 ejectFloppy()`. `actionOptionIndex` is PE-dispatched and
  **this project already intercepts that exact UFunction** (the kerfur turn-on rides
  `actionOptionIndex Action==8`). Tier 1, cancellable, with the player and the component in hand.
- **INSERT is reached from a COMPONENT OVERLAP DELEGATE** — `[V]` the `Box` component's
  `ComponentBeginOverlap` handler passes `OtherActor` into `pocessFloppy`, which calls
  `insertFloppy`. Delegate -> ProcessEvent dispatch is VISIBLE
  (`COOP_DISPATCH_VISIBILITY.md:81`). Tier 1 again, with the inserted actor as an argument.

**So J4 needs no new substrate and no WP-1.** Both halves are interceptable today at tier 1. Only
J3's `fix` stays on the tier-5 poll, because its entry (`launchServerMinigame`) is invisible too.

### A second J3 defect the same pass turned up `[V]`

`breakServer` calls **`getRandomServerMinigameType`**, and the chosen puzzle lands in the server's
`minigame : int` / `staticMinigame : bool` fields. **`ServerStatePayload` carries neither.** The
client's own `ticker_serverBreaker` is deliberately neutralised by `serverbox_sync`, so nothing on
the client ever writes `minigame` at all — it holds whatever the save loaded, while the host holds
the roll that actually broke the box.

That is the same defect class as J5, one size smaller: **which puzzle a peer is solving is
per-peer**, so the two peers do not even agree on the task. It is cheaper here — `minigame` is one
int rolled host-side, so mirroring it is a field on the existing payload rather than a new lane —
but it must be in W1's design, and it was not.

### J5 — a client fixes a transformer `[V]` NO LANE EXISTS, and it is the hardest of the three

**The actor is `Agenerator_C`** (`VotV/Content/objects/generator.uasset`, parent `Aactor_save_C`).
The game's own vocabulary is split — the player-facing word is "transformer", the class is
`generator` — which is part of why it was never censused. `[V]` the kerfur names it for us:
`kerfurOmega_C::findTransformer` does `GetAllActorsOfClass(generator_C)` and returns the first
element whose `isBroken` is true.

`[V]` **`generator_C` and `Agenerator` appear ZERO times in `src/votv-coop/`.** Separately, all 24
`transformer` hits are the kerfur verb string `fix_transformers` (the relay's enum, its name
mapping, its bounds check, comments). No element, no payload, no poll, no receiver.

**Persistent state** (in `getData`/`loadData`): `isBroken`, `index` (its save-stable identity, the
same shape as the servers' array index), `upgradeLevel`, `cycle` (reset to 100 on every fix),
`opened` (door). Verbs: `break()`, `damage()`, `upd()`, `update()`, `updUpgrades()`,
`openDoor(bool)`.

**The player's path is a THREE-PART MINIGAME**, and this is where J5 stops resembling J3. The
generator holds `panelObj : AtransformerMGPanel_C*`; the panel holds three independent sub-puzzles —
`isSineComplete` (match a waveform's offset / frequency / amplitude), `isRotatorsComplete` (a colour
grid of `Fstruct_generatorRotator{top,right,bottom,left}` tiles rotated to match, live state in
`rotators_states : TArray<byte>`), and `isSwitchesComplete` (a switch bank). On success the
generator runs `cycle = 100; isBroken = false; turnedOn->Broadcast(); upd()`
(`generator.cpp:1313-1318`).

**`[V]` THE PUZZLE IS UNSEEDED PER-PEER RNG — this is the finding that shapes the whole lane.**
`transformerMGPanel.cpp:2413-2441` rolls `RandomIntegerInRange(0,15)` x2 + `RandomIntegerInRange(1,15)`
for the sine target and EIGHT `RandomBool()`s packed to a byte for the switches; `:2805` rolls
`RandomIntegerInRange(0,3)` per tile and `assignRandomColors` per grid. Nothing seeds any of it.

So **the host and a client who open the same transformer are looking at DIFFERENT puzzles**, and a
client solving its own local puzzle is not evidence to the host of anything. J3's design (observe
the local outcome flip, send an intent, let the host perform it) does not transfer unchanged: here
the host cannot re-run the work, and trusting the client's word is exactly the class of client-
authored shared-world write §2b forbids.

That puts J5 squarely in `COOP_RNG_AUTHORITY.md` — *host rolls all shared-world RNG, clients
mirror*. The lane needs the PUZZLE mirrored before the OUTCOME can mean anything, which is a
different and larger shape than either J3 or J4.

**One asset in our favour:** `turnedOn` is a DELEGATE, and delegate -> ProcessEvent dispatch is
VISIBLE (`COOP_DISPATCH_VISIBILITY.md:81`, the game's own inventory buttons). So the fix COMPLETION
is interceptable on both roles without any new substrate — unlike the server's `fix()`.

Related prior work: `generatorFuckuper_C` (the thing that breaks it, the `ticker_serverBreaker`
analogue) was already disassembled on 2026-09-02 by the power-chain pass — its offsets listing is on
disk. This is the same gap that study named from the other direction ("breaker panels synced,
reactor/generator/transformer outcomes in the gap list"). **J5 is not a bug in a lane; it is a lane
that was never built, on a base that is parked.**

### What this changes

- The macro-goal's real content is **three unbuilt/one-directional lanes**, not robot polish.
- **J3 and J4 share a root with each other** (the server is one actor holding both), and **J4 shares
  its root with the robot's `get_reports`** (the same floppy triple). One design covers three jobs.
- **J5 does NOT share their shape.** J3/J4 are outcome-intent lanes; J5 needs the host's RNG mirrored
  first, because the work itself is per-peer random. Designing all three as one lane would be wrong —
  design J3+J4 together, J5 on its own.
[withdrawn at the author's request]
  user-visible symptom — a stronger argument than anything listed there. It remains an upgrade, not
  a precondition.

### The three seams, side by side

| job | the moment to catch | dispatch | seam available today |
|---|---|---|---|
| J3 server fix | `launchServerMinigame` / `fix()` | `EX_LocalVirtualFunction` — invisible to both | tier 5: poll the client's own `isBroken` for an un-commanded flip |
| J4 report EJECT | `actionOptionIndex(Use)` + `lookatFloppyButton` | **PE — VISIBLE, and already intercepted by us** | tier 1, cancellable |
| J4 report INSERT | `Box::ComponentBeginOverlap` -> `pocessFloppy` | **delegate -> PE: VISIBLE** | tier 1, with the disc as an argument |
| J5 transformer fix | `turnedOn->Broadcast()` | **delegate -> PE: VISIBLE** | tier 1 interceptor, no new substrate |

**Only J3 lacks a visible seam.** Three of the four moments are tier-1 interceptable today with no
new substrate — the invisible LVF verbs sit one call below entry points the engine dispatches
normally, which is the general lesson: *census the entry, not the verb.* J5 keeps the worst state
problem (a per-peer random puzzle) and J3 the worst seam with the simplest state, and that
asymmetry — not how hard the job feels to a player — is what should order the build.

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

## 0.6 J9 — the signal workstation (USER 2026-09-04)

> «и в макроцель поставим еще работу с workstation» … «workstation уже мы делали и разбирали, но
> многое пока на костылях там» … «workstation я имею в виду сигналы где обрабатывать».

The signal desk — catch, tune freq/polarity, download, decode, play deck, drive, comp processing,
save to the meadow. **This is not a robot job; it is VOTV's actual gameplay loop**, which makes it
the strongest single answer to "is a second player useful". A kerfur can fetch reports; only a
person can run the desk. So J9 sits in the job list on its own terms, and it is pure HARDEN.

**Its home is `docs/signals/` and stays there** — `TRACKER.md` is element-by-element and this doc
does not duplicate it. What belongs here is only what bears on the macro-goal:

- `[V]` **The build is broad and the verification is not.** The master table has ~29 `AS-BUILT` rows
  and **exactly ONE `VERIFIED`**; the phrases "NOT hands-on" / "awaiting hands-on" appear **27
  times**. Against a benchmark of "no worse than the robot", a lane nobody has driven by hand is not
  yet evidence of anything.
- **The user's own verdict is that much of it is on crutches**, and `[V]` **`docs/CRUTCHES.md` has
  no workstation entry at all** — its five rows are the ATV, piles/clumps, the retired KO gate, the
  password floor and the coin gun. So the register is incomplete on the maintainer's own testimony.
  **A crutch census of the signal lanes is OWED**, and CLAUDE.md's standing rule ("add to it
  whenever a crutch is measured") makes that a debt, not an option.
- One smell is already visible in the tracker's own words and matches the doctrine's forbidden list
  verbatim: **OPEN-2 keeps `CR:` log lines "filtered off wire"** while recording that the premise
  for the filter was *measured false*. A filter whose justification has been falsified is the
  definition of a crutch left standing.
- **Foundation-first: OPEN-3, the upgrade levels.** `Fstruct_upgrades` (18 int32, 20 laptop-shop
  slots) parametrizes the download / ping / coord / comp / radar / detector sims and **has no lane**.
  The sims are host-authoritative but their INPUT is not synced, so their outputs cannot be right by
  construction. `docs/upgrades/SIGNAL_UPGRADES.md` holds the RE and a design of record.
- Named gaps beyond that: OPEN-4 (24-dish rest-pose/slew RNG), BUG-3 (detector gauge sounds are
  data-starved — the speeds they derive from are on no lane), PARTIAL (save/delete/lid verbs), and
  R-a (a ping's observers see no stage visuals — surfaced to the user as a product question on
  2026-07-17 and still unanswered).

**So J9's shape is the opposite of J3-J5.** Those are lanes to build; this is a large built surface
to bring up to the benchmark: verify by hand, census the crutches into the register, retire them,
and close OPEN-3 underneath. Its first task is therefore a CENSUS, not a design.

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

**Re-derived with `tools/bp_cfg.py` on 2026-09-04** (the first pass read this off the pseudo-C++
`Label_*` names, which is a control-flow claim made with the wrong lens — the CFG is the instrument
the rule names). It confirmed every row AND added the `dropObject` column, which the readable lens
did not surface:

| value | enum name | verb -> guard -> write | the write block | menu label |
|---|---|---|---|---|
| 0 | `follow` | `@20455` -> `@21624` -> `@21808` | `ByteConst 0` + `move` | Follow |
| 1 | `idle` | `@20504` -> `@21404` -> `@21588` | `ByteConst 1` + `move` | Idle |
| 2 | `patrol` | `@20555` -> `@21184` -> `@21368` | `ByteConst 2` + `move` | Patrol |
| 3 | `fix` | -> `@20950` -> `@21134` | `ByteConst 3` + `move` + **`dropObject`** | Fix_servers |
| 4 | `report` | `@20674` -> `@21874` -> `@22058` | `ByteConst 4` + `move` + **`dropObject`** | Get_reports |
| 5 | `transformer` | -> `@22108` -> `@22292` | `ByteConst 5` + `move` + **`dropObject`** | Fix_transformers |
| 6 | **`sitOnAtv`** | **no menu entry** — set at `kerfurOmega.cpp:5724` | — | — |

`[V]` **The three BUSY verbs each call `dropObject`; the three non-busy ones do not.** A kerfur sent
to a task drops whatever it is carrying — a shared-world write that no earlier pass recorded, and
one J4's design has to expect (a floppy handed to a kerfur that is then sent to fix servers).

`[V]` `turn_off` (`@20404` -> `@21844`) goes straight to `EX_LocalVirtualFunction dropKerfurProp`
with **NO state guard** — consistent with `getActionOptions` offering it while busy. `kill`
(`@21859`) is a bare `EX_LocalVirtualFunction startKill`.

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

**The goal's own work is W1-W3.** They are ordered by seam quality and shared roots, not by how hard
the job feels to a player.

- **W1 — the server maintenance lane (J3 + J4 together).** One actor, `AserverBox_C`, holds both
  jobs, so they get one design. J3: client polls its own host-driven `isBroken` for an un-commanded
  flip -> fix INTENT naming the server index -> host validates and runs the real `fix()` -> the
  existing `ServerState` broadcast returns the result. J4: the server's floppy triple
  (`floppyType` / `floppyReadwrites` / `floppyData`) becomes host-owned state with an
  insert/eject intent; the disc ACTOR crosses on the existing prop lanes. **The seam question is
  CLOSED (§0.5): both J4 halves are tier-1 interceptable today** — eject at `actionOptionIndex`,
  insert at the `Box` overlap delegate — so only J3 needs the poll. W1 must also carry
  **`minigame` / `staticMinigame`**, which §0.5 found diverging. Reference: `order_sync`
  (act-as-host), `serverbox_sync` (the return path already exists).
- **W2 — the transformer lane (J5).** Different shape: the puzzle must be host-rolled and mirrored
  BEFORE an outcome can be trusted (`COOP_RNG_AUTHORITY.md`), and the completion seam is already
  available (`turnedOn` delegate -> PE, tier 1). Foundation-first says the power base is the floor
  under this; W2's first task is to decide whether the generator's own `isBroken`/`index`/
  `upgradeLevel` can be a self-contained element or genuinely needs the parked base first.
- **W4 — the workstation (J9), and it starts with a CENSUS, not a design.** The surface is already
  built; what is missing is evidence and honesty about its shape. In order: (a) census the signal
  lanes for crutches and write each into `docs/CRUTCHES.md` with its measured evidence — the
  register has zero workstation rows against the user's "многое пока на костылях", and closing that
  gap is a standing CLAUDE.md duty, not a choice; (b) hands-on-verify the 27 rows that say they are
  not; (c) close **OPEN-3**, the upgrade levels, which is foundation-first for every desk sim; then
  (d) retire the crutches the census found, OPEN-2's falsified `CR:` filter first.
  **W4 can run in parallel with W1/W2** — it touches different files and its first phase is
  measurement, so it does not compete for design attention.
- **W3 — harden the rest of §0.3** (J2 edges, J7 container facets) once W1/W2 land.

**The robot lanes are OUT of this goal** (§0.2) and are listed here only so nobody re-derives them:
`pat`, `take_object`, `equipment`, `kill`, `sitOnAtv`, all of Kerfus, and the floppy half of
`get_reports`. **The floppy one is the exception worth noting** — W1's J4 design should be built so
the robot's `get_reports` can later ride the same lane, because it is literally the same three
fields on a different actor. Cheap to allow for now; expensive to retrofit.

Every lane owes, before it is DONE: its authority row (doctrine §2), its seam decision proven by
probe not assumption, its brain-parking statement, its identity-at-birth answer, its **mid-join
row** (principle 8), a protocol bump, and evidence from a real two-peer run.

---

## 8. Instruments + evidence

**Which lens produced which fact — stated, because the rule cares (`[[feedback-rebase-old-tool-facts-on-new-instruments]]`):**

| instrument | used for |
|---|---|
| `tools/bp_cpp.py` | the readable pass over `kerfurOmega`, `prop_kerfurOmega`, `kerfurOmega_0/1/2`, `p_kerfus`, `kerfusPawn`, `kerfusPossessor`, `prop_kerfusBody`, `prop_corded`, `serverBox`, `generator`, `transformerMGPanel`, `generatorFuckuper` — fields, function census, action lists |
| `tools/bp_cpp.py --offsets` | `kerfurOmega`, `serverBox` — the property blocks and the eject chain `@4519/@4560/@4574` |
| `tools/bp_reflect.py` | `serverBox` — the kismet JSON, from which every dispatch verdict in §0.5 was resolved by `StackNode` / `VirtualFunctionName` |
| **`tools/bp_cfg.py`** | **`kerfurOmega` ubergraph — §2.2's verb -> guard -> write chain.** Run as a CORRECTION: the first pass read that control flow off the pseudo-C++ `Label_*` names, which is the wrong lens for a branch claim. The CFG confirmed every row and added `dropObject`. |
| ad-hoc byte parsing | `enum_kerfurCommand.uexp`, `enum_kerfurDripType.uexp`, `enum_interactionActions.uexp`, `list_kerfurDrip.uasset`, `list_store.uasset` — enum display-name order and datatable rows, which none of the three wrappers reads |

Decompiles land in `research/bp_reflection/` (gitignored — derived game content).

**The method lesson, paid for in this round:** a control-flow claim read off the readable lens is
not wrong by default — every offset matched — but it is unverified, and it is INCOMPLETE in a way
you cannot see from inside it. `dropObject` was sitting in three blocks the pseudo-C++ rendered
without it. Use `bp_cpp` to learn what a graph SAYS and `bp_cfg` to state what it DOES.

Per `[[feedback-rebase-old-tool-facts-on-new-instruments]]`, the June 2026 kerfur facts in
`docs/kerfur/0*.md` were derived with the older hand-walked `to-json` route. They are NOT re-based
yet; do that for any fact a fix will stand on. Nothing in this doc inherits them — §1-§4 were
re-derived from scratch on 2026-09-04.

**Not measured at runtime.** Everything here is static decompilation. No probe has run, no build has
been made, no claim in this doc is `[V]`-by-log.
