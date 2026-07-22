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
| **Q-STACK: the v125 client->host container decrement** | BUILT (proto 125) | **gate:user** -- runbook step (c) | `container_contents_sync.cpp` |
| **Q-PROP / C5: does the extracted item reach the host's world** | code says NO; runtime UNCONFIRMED | **gate:user** -- runbook steps (a)+(b), positive control first | runbook `handson_runbook_2026-07-22_container_v125.md` |
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

## 2. Threads outside the container cluster

| Thread | Status | Gate | Note |
|---|---|---|---|
| **`reflection.cpp` `FindFunction` does not walk the superclass chain** | 1 confirmed dead call (ours, fixed); 19 `ClassOf(instance)` sites NOT audited; `door_probe.cpp:81` a second candidate | gate:none -- **own audit** | Possible RED inside lanes currently labelled verified. `reflection.cpp:427` -- `if (OuterOf(obj) != owningClass) continue;` |
| **SACK-PHYS** -- our `SuppressTick` silences the AnimBP feed | OPEN | gate:none -- own fix | from take-4 |
| **KERFUR-BLUE** | NOT RE'd | gate:none -- own approach | from take-4 |
| **droneConsole** -- a client pressing E to send the drone back is a no-op | NOT RE'd | gate:none -- own hook | from take-4 |

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
