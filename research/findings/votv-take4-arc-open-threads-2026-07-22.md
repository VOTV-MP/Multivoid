# Take-4 arc -- open-thread ledger (2026-07-22)

**Why this file exists.** The take-4 arc went deep (21 symptoms -> 16 roots -> the R11/R11b container
work -> a `/qf` that dissolved its own premise). Individual threads were kept attributable throughout,
but they were never collected in ONE place, and at the end of a long arc that is exactly what drops:
not any single thread, but the ability to see which ones are still open and WHO gates each.

**Status vocabulary.** `OPEN` = no fix exists. `BUILT` = code shipped, not hands-on verified.
`CONVERGED` = design settled, not built. `NOT RE'd` = not even reverse-engineered yet.
**Nothing in this ledger is VERIFIED.**

**Gate vocabulary.** `gate:me` = a measurement I owe before work can start. `gate:user` = a decision
or a hands-on run only the user can supply. `gate:none` = ready to work when picked up.

---

## 1. The container / birth cluster (this session's arc)

| Thread | Status | Gate | Where |
|---|---|---|---|
| **`:279` birth-emission** -- a client's fresh unparked keyed `Aprop_C` birth is dropped unless its class is whitelisted (reel/module/drive). A container-extracted item is neither parked nor whitelisted. | OPEN, root confirmed by code-reading | **gate:me** -- the firing set of a widened `freshBirth` (who newly passes) AND the physics axis (`freshBirth` also sets `kSleep`; correct for a reel, likely wrong for an extracted item). Counting probe, not reasoning. | `prop_drop_intent.cpp:279` |
| **d14b6644 (R14/15/16) drive-disc birth content** | CONVERGED (8 rounds), NOT built | **gate:user** -- the `savedScalar` RETIRE is gated on a reel+disc hands-on green (design 4.4: "the add ships, the deletion does not"). The ADD is not blocked. | design commit `d14b6644` |
| **Relationship between the two above** | measured: ONE design, TWO axes | -- | `:279` (who passes) -> `:293` `ReadSavedScalarForClass` (what the passer carries) -> `:307` `NoteLocalDriveBirth` (same whitelist). A funnel, not a stack: a burger never reaches the content stage. |
| **Q-STACK: the v125 client->host container decrement** | **GREEN 2026-07-22 -- SEQUENTIAL ONLY.** Hands-on, measured on both logs. **CONCURRENT (`CONFLICT>0`) UNMEASURED** -- the take had `CONFLICT=0` on both peers, so the CAS never ran once. | sequential half closed; concurrent half **gate:user** (needs a run that PRODUCES a conflict) | see the take record below |
| **Q-PROP / C5: does the extracted item reach the host's world** | **RED 2026-07-22.** Root `:279` is `[RD]`, **not `[V]`** -- the log cannot separate "dropped at `:279`" from "never enqueued"; both are silence. | **gate:user** -- the discriminator run below, BEFORE gate:me | see the take record below |
| **The CAS refusal path leaves the item with the refused client** | OPEN | none -- **do NOT build separately**; it is a sub-case of `:279` and dissolves with it | `container_contents_sync.cpp:598-607` |
| **"R11b: a client's extraction"** as its own item | DISSOLVED into `:279` | -- | superseded framing; see below |
| **`updateVolumesAndMass` never re-derives on extraction** | OPEN -- measured: records 7->6 while `currVol` stayed 28579.0 on BOTH peers | gate:none -- narrow fix at the call site | co-requisite of R11b |
| **`putObjectIn_overlap` (the INSERT direction)** | NOT measured -- no claim either way | gate:none | -- |
| **Increment 2: nested container-in-container transitive walk** | OPEN by design | gate:none | receiver-side bounds needed |
| **Slot 0 -- the player's personal container into canon** | OPEN, "the second story" | **gate:user** -- a privacy/product fork; also a BOUNDARY 1 redesign | `IsWorldContainerInventory` fail-closed |
| **The `hold` end of the chain** | NOT measured (+187 B in `hold` vs `equipment`) | gate:none | -- |

**RETRACTED framing, recorded so it is not re-derived:** "the extracted item lands in the client's
slot 0 and the refusal must reclaim it from their inventory." `takeObj` does NOT write an inventory
record -- it SPAWNS A WORLD ACTOR (`BeginDeferredActorSpawnFromClass -> loadData ->
FinishSpawningActor`, RE doc section 4). Every design built on the inventory-reclaim framing is void.

**Also retracted:** `prop_container_extract.cpp`'s header calls its `takeObj` POST observer "the
canonical broadcaster for container extracts". It fired ZERO times on both peers -- `takeObj` is
`EX_LocalVirtualFunction` (0x45), invisible to ProcessEvent. Host-side extracts have always been
broadcast by `host_spawn_watcher`'s `FinishSpawningActor` hook instead. The comment is false.

---

## 1b. The 2026-07-22 20:47 take -- the record

Both peers on b125, run 20:47-20:54. Container `eid=2139`, taken down 3 -> 0 by both peers in turn:

| time | peer | line | reading |
|---|---|---|---|
| 20:52:49 | host | `shipped 3 records` | host authored 3 |
| 20:52:49 | client | `applied 3 records` | client received |
| 20:53:04 | host | `shipped 2 records` | host took one |
| 20:53:04 | client | `applied 2 records` | client received |
| 20:53:10 | **client** | `shipped 1 records [client-authored]` | **the client took one** |
| 20:53:10 | **host** | `applied 1 records` | **the host accepted it** |
| 20:53:16 | host | `shipped 0 records` | host took the last |
| 20:53:16 | client | `applied 0 records` | client received |

**Q-STACK GREEN -- FOR THE SEQUENTIAL CASE ONLY.** Both peers took in TURN, and `CONFLICT` was **0 on
both peers** for the whole run: the compare-and-swap never executed once. So this take says nothing
about a CONCURRENT edit, which is precisely where the overwrite risk lives. A run aiming at that must
be built to PRODUCE `CONFLICT>0` -- both peers on the SAME slot in the SAME window -- or it will come
back `CONFLICT=0` again and read as "the CAS held" when the CAS simply never ran. The CAS is only
tested by a non-zero conflict, exactly as a positive control is only tested by a known-positive.

With that boundary stated: counters agree at every step, in both directions. This is the first hands-on
confirmation of the v125 client->host half, and it closes the user's original b124 symptom
("3 burgers from an order of 2").

**Q-PROP RED.** The client extracted a real prop at 20:53:10 and authored **zero** `PROP-DROP`
intents in the whole run, while `prop_drop_intent: FinishSpawningActor post-hook installed` confirms
the module was live and would have logged an authoring. Consistent with `:279` dropping the birth
(not parked, not a whitelisted class).

**Strength of the Q-PROP verdict -- one notch below airtight.** The log cannot discriminate "dropped
at `:279`" from "never enqueued at all"; both produce silence. Root stays `[RD]` until a discriminator
runs.

**The discriminator, and why the target must be CHOSEN, not convenient.** Have the client pick up a
prop and place it back down: a parked place takes the `parked == true` branch at `:279`, so
`PROP-DROP` MUST fire. Its presence proves the whole authoring path downstream of the gate works,
which isolates the burger's failure to admission; its absence moves the root upstream to the enqueue.

**Pick the prop by the property that makes the stimulus valid, not by "any prop".** The park is only
inserted when the client's pickup-DESTROY actually crossed to the host, so the target must be a
**pre-existing world prop that both peers can see** -- not something the client itself just extracted
(that one is exactly the unparked case under test, and would prove nothing). If the chosen prop
happens to be reel / module / drive class it can also pass via `freshBirth`, which still proves
downstream works but no longer tells you WHICH branch admitted it -- so prefer an ordinary prop, and
note the class in the report either way. This is the same trap that made the R11b instrument fire on
empty containers and report nothing: a causing probe must pick its target by what makes the stimulus
VALID. See `[[feedback-probe-must-count-not-confirm]]`.

**Instrument defect found and fixed in the same take.** The runbook told the user to grep
`PROP-DROP|SPAWN broadcast` as the HOST-side positive control. Both are wrong channels for the host
(`PROP-DROP` is client-only; `SPAWN broadcast` is the never-firing `takeObj` POST observer), so the
control returned 0 on a healthy run and the take briefly read as void. The host's real broadcast line
is `host_spawn_watcher: spawn-seam adopted` -- 5 of them in this run. A control naming the wrong
channel is worse than none: it manufactures a false negative out of a good run. Runbook corrected.

---

## 2. Threads outside the container cluster

| Thread | Status | Gate | Note |
|---|---|---|---|
| **`reflection.cpp` `FindFunction` does not walk the superclass chain** | **AUDITED 2026-07-22 -- no RED found.** 19 `ClassOf(instance)` sites + the wider leaf-class sweep: 1 dead resolve (`door_probe.cpp:81`), never invoked, zero gameplay effect. **CLOSED FOR ONE AXIS ONLY: the audit proves each resolve returns NON-NULL. It does NOT prove the call LANDS** -- `ParamFrame` validity, parameter-name spelling and `EX_*`-invisible dispatch were never checked and remain open. | resolve axis closed; dispatch axis **UNAUDITED** | see the audit record in section 2b |
| **`spawn_menu.cpp:130` + `:165` -- silent on a null resolve, and LOAD-BEARING** | SAFE today; would go dead **wordlessly** after a game recook that moves/renames `SetInputMode_GameAndUIEx` / `SetInputMode_GameOnly` | gate:none -- **highest-priority latent item on this page** | The pair gates the input-mode restore the file's own comment at `:158` calls "THE LOAD-BEARING UN-STICK". A silent no-op leaves the player's input trapped in `GameAndUI` -- i.e. the player loses control of the game. Not the same weight as the row below. |
| **`save_browser.cpp:188` -- silent on a null resolve** | SAFE today; deliberate fail-open ("list rather than hide") | gate:none -- low priority | Worst case is a mis-listed save name. Grouped with the row above ONLY by mechanism; the consequences are not comparable and they must not be deferred as one item. |
| **SACK-PHYS** -- our `SuppressTick` silences the AnimBP feed | OPEN | gate:none -- own fix | from take-4 |
| **KERFUR-BLUE** | NOT RE'd | gate:none -- own approach | from take-4 |
| **droneConsole** -- a client pressing E to send the drone back is a no-op | NOT RE'd | gate:none -- own hook | from take-4 |

---

## 2b. The `FindFunction` audit, 2026-07-22 -- CLOSED

Ran because the thread was flagged "possible RED inside lanes currently labelled verified". **It is
not.** Result: of the 19 `ClassOf(instance)` sites, **zero** are dead. Every one either resolves a
library CDO and calls that library's own function (safe by construction: `asset_load`,
`save_browser` x2, `spawn_menu` x2) or resolves the instance via `FindObjectByClass` / a typed field
whose class is exactly the class declaring the verb.

**Evidence base, recorded because finding it cost time:** the SDK ownership authority is
`Game_0.9.0n_HOST/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/*.hpp` -- **2645 files**, one per
package, each block listing ONLY the functions that class itself declares. That is precisely the
exact-owner relation `FindFunction` tests, so it is the correct instrument. Cross-check:
`research/bp_reflection/*.functions.txt`.

**The one dead resolve:** `coop/dev/door_probe.cpp:81` --
`FindFunction(g_doorCls, L"SetActorTickEnabled")`. `SetActorTickEnabled` is declared on `AActor`
(`Engine.hpp:6754`), not on `Adoor_C`, so the resolve is permanently null. **Verified independently:**
`g_setTickFn` has exactly three references in the file -- the declaration (:40), the resolve (:81),
and a log (:84). **It is never invoked.** Nothing in the game breaks. What is damaged is the
instrument's credibility: the install line has been printing `setTick=0000000000000000` next to a
genuinely-resolved `moveFinish=<ptr>`, unremarked, in ini-gated dev diagnostics.

**The finding that actually matters -- the search predicate was WRONG.** The thread was scoped as "the
19 sites passing `ClassOf(instance)`". But the single confirmed corpse in this batch passes
`FindClass(L"door_C")`, not `ClassOf(instance)`, so a sweep filtered on `ClassOf(` alone would have
MISSED it. The real risk predicate is **"a LEAF class resolving a BASE-declared function, however that
class was obtained"**. Re-swept on the corrected predicate: sites passing a base class name
(`Actor`, `PrimitiveComponent`, `SkeletalMeshComponent`) are correct by construction, and all eight
cached-BP-class shipping sites were checked against the dump and are SAFE --
`cockroachMaster_C::addRoach`/`deleteRoach`, `serverBox_C::check`, `trigger_alarm_C::runTrigger`,
`trigger_eventer_C::runEvent`/`runSpecialEvent`, `mainGamemode_C::wakeup`,
`analogDScreenTest_C::updComp`, each declared on its own class.

**Carried forward (latent, not live): three shipping sites are SILENT on a null resolve -- but they are
NOT one item.** Same mechanism, incomparable consequences, so they are split into two ledger rows and
must not be deferred together:

- **`spawn_menu.cpp:130` + `:165` -- LOAD-BEARING.** They gate the input-mode restore the file's own
  comment at `:158` calls "THE LOAD-BEARING UN-STICK". A silent no-op there leaves the player's input
  trapped in `GameAndUI` -- the player loses control of the game. This is the highest-priority latent
  item produced by the audit.
- **`save_browser.cpp:188` -- low priority.** Deliberate fail-open ("list rather than hide"); worst
  case is a mis-listed save name.

Grouping them by "silent on null" would let a future session read "three latent, safe today" and
shelve all three at the same weight. Split by CONSEQUENCE OF A RECOOK, not by mechanism.

**Scope of this audit, stated so it is not over-read:** it verifies DECLARATION, i.e. that the resolve
returns non-null. It does NOT verify that the call lands -- `ParamFrame` validity, parameter-name
spelling, and `EX_*`-invisible dispatch are all separate questions.

---

## 3. Hygiene state at the time of writing

- **HEAD** `ad2c199e`, **one** commit ahead of `origin/main` (`c6a212c7`). Everything earlier is pushed.
- **DLL** `multivoid-0.9.0n-125.dll`, SHA-256 `05aff7799a707858`, MATCH x4, proto 125.
- **Held WIP, never to be committed** (explicit paths only, never `git add -A`):
  `tools/mp.py`, `src/votv-coop/src/coop/props/trash_collect_sync.cpp`,
  `research/puppet_shots/host_puppet_nameplate.png`.
- `container_contents_sync.cpp` is **853 LOC**, over the 800 soft cap. Extraction proposal on record:
  move host arbitration (`HostAcceptsClientWrite`, the host branch, `RelayToOthers`, `g_publishedHash`,
  `g_localChangeMs`, `g_conflictRejects`) into `container_contents_authority.cpp/.h`.
- Pre-push discipline: leak-audit the WHOLE unpushed stack, not just the newest commit.
