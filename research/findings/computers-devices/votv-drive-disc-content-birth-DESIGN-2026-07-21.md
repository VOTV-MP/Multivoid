# Drive-disc content data-loss — the per-class birth-content channel (DESIGN, 2026-07-21)

**Status: DESIGN, not built. Not hands-on.** Fixes take-4 R14/R15/R16 (the `prop_drive_C` disc losing
its exported-signal content on the HOST when a client interacts with it). Converged over an 8-round `/qf`
(the user drove the critic side); this doc is the record of what the measurements settled, including two
reframes that RETRACT earlier positions.

Read alongside: `[[lesson-saved-scalar-birth-channel-covers-every-birth-path]]` (the discipline this
generalizes), `docs/COOP_ENTITY_EXPRESSION_MAP.md` (birth/destroy seams),
`research/findings/computers-devices/votv-take4-hands-on-bugs-2026-07-21.md` (R14/15/16 evidence).

---

## 1. The bug (measured, take-4 hands-on)

A `prop_drive_C` disc holding an exported signal (green lamp) loses its content **on the HOST** when a
CLIENT picks it up and drops it; the client keeps showing green + the signal ID. The host owns the save,
so the signal is lost on persist. Symptoms #18/#19/#20.

## 2. Root — after two reframes (measured)

The `/qf` twice changed shape as the substrate was measured. Recording both retractions so they are not
re-derived:

- **Reframe 1 (rejected: "identity churn / custody").** Round 2 concluded the root was a client grab
  destroying the host's authoritative copy, and proposed a host-custody actor (never destroy+respawn).
  **RETRACTED** by the inventory measurement (§4): while held/collected the disc is per-player INVENTORY
  data, custodied separately from the world save. A hidden host world-custody actor would be the disc a
  SECOND time (world save via `getData` **and** the per-player blob) → double-write → two discs on reload.
  The pickup-destroy is CORRECT (the disc left the world into inventory).
- **Reframe 2 (rejected: "generalize the whole SaveRecord / retire drive_sync content lane").** Measuring
  `prop_base::getData` showed the base groups (`transform`, the 4 bools `static/frozen/sleep/
  removeWOrespawn`, the lifespan float) are ALREADY carried by `PropSpawn.physFlags` / re-derived — so
  there is NO broader base-state loss, and the record must NOT re-author them. And `drive_sync`'s
  `DrivePayload` is a STEADY-STATE lane (a disc mutating while sitting in a rack under `saveSignal`), a
  DIFFERENT concept from birth-state that a birth channel cannot carry. So the fix does not touch base
  fields and does not retire the steady-state lane.

**Root (settled):** a client dropping a pre-existing keyed prop back into the world is a genuine
inventory→world BIRTH (native `simulateDrop` spawns a fresh actor, `loadData`'ing content from the
inventory record → client green). The client cannot author a keyed SPAWN, so it sends `PropDropIntent`
(host `HostSpawnPlacedProp`). `PropDropIntent` carries only base fields + a single `savedScalar` **float**
— **not** the disc's per-class signal content. So the host's dropped actor is born empty. The disc's
content had a birth channel for exactly one shape (a reel's Progress float); it has NONE for a signal
struct. `NoteLocalDriveBirth` (the only client→host drive-content emitter) is gated `freshBirth`-only, so
the parked-place drop never re-broadcasts it either.

This is the SAME class as the v114 correctness-audit CRITICAL-1 (`savedScalar` was built to stop a
reel pocket→place resetting to CDO). The level was right; the birth channel just carries only a float.

## 3. Measured facts the design rests on

- **`prop_drive_C::getData`** ([research/bp_reflection/prop_drive.json]): calls `Super::getData` (base),
  then `EX_SetArray data_0 → EX_Let signals` — copies the runtime `Data_0` into the save's `signals`
  group. **The content IS saved** (so it persists on a plain SP save-load; ruled out "runtime-only").
- **`prop_drive_C::loadData`**: reads `data.signals[0]` → writes instance `data_0` → one refresh virtual.
  **No construction, no BeginPlay dependence, no component spawn, no transform touch.** Post-Finish safe,
  same safety class as `ApplySavedScalarForClass`.
- **`prop_base::getData`** ([research/pak_re/prop_base.json], 21 stmts): the 4 bools =
  `static/removeWOrespawn/frozen/sleep` (== `PropSpawn.physFlags`, already replicated); the float =
  `GetLifeSpan()` (engine bookkeeping, re-derived on a fresh spawn). **No player-observable base loss.**
- **Content is variable but small:** the wire `Row` deliberately OMITS the `image` (Fstruct_byteImage PNG
  @0x58, laptop-photo-only) — same scope boundary `DrivePayload` and `inventory_wire` already hold. So a
  drive's synced content is a `Row` (name/id/object/signal caps + ints/floats/date), a few hundred bytes.
- **Inventory custody exists:** `player_inventory_sync` streams the client's inventory (incl. held/
  collected discs as `SaveRecord`s with `key` + `signals`) to the host and persists it per-GUID, separate
  from the world save. So content is NOT lost while held/collected — only at the drop-birth.

## 4. Design — a per-class BIRTH-CONTENT channel

Generalize the birth channel from a single `savedScalar` float to **per-class content**, carried at the
same birth sites, applied the same post-Finish way. Reuse each class's EXISTING content accessor + codec;
no new content logic and no touch of base fields.

- **Wire:** `PropSpawnPayload` / `PropDropIntentPayload` grow a variable **content trailer** (present iff
  a `kHasContent` flag), replacing the 4-byte `savedScalar`. The trailer is the per-class content blob:
  a reel → its Progress float; a drive → `signal_wire::Serialize(Row)` (image-omitted, the proven
  `DrivePayload` codec). Variable size ⇒ the payload becomes header + trailer (chunk via `blob_chunks`
  only if a class's content ever exceeds one datagram; a single drive `Row` does not).
- **Read (author side):** `ReadSaveRecordForClass(actor)` generalizes `ReadSavedScalarForClass` — a
  per-class switch reusing the proven accessors: reel → Progress read; drive → `DC::ReadDriveRow`. Returns
  the content blob + the flag.
- **Apply (birth side):** `ApplySaveRecordForClass(actor, blob)` generalizes `ApplySavedScalarForClass`
  (which already writes ONLY the scalar, not base) — reel → write Progress; drive → `DC::WriteDriveRow` +
  `DC::CallDriveUpd`. **Writes ONLY the per-class content. Never base.**
- **INLINE, not a follow-up message** (settled): the content rides the SAME reliable birth message as the
  spawn, so the actor is born WITH content — no separate eid-keyed message that can arrive after the
  receiver ticks the fresh actor (that is the late-`DrivePayload` shape that produced R15's red→green
  flash) and no dead-eid race. Born-with-content is true by construction — the entire point of a birth
  channel (why `savedScalar` was inline).

### 4.1 Author of base vs content (Q2 — no fact on two lanes)

- **Base** (transform, `physFlags`, lifespan): `PropSpawn`'s existing explicit fields stay the SOLE
  author. **UNCHANGED.** The wire content trailer carries CONTENT groups only — it does NOT include base.
- **Content** (the per-class trailer): the SOLE author. `ApplySaveRecordForClass` writes only content,
  exactly as `ApplySavedScalarForClass` writes only Progress today.

No field is carried by two lanes. We do NOT call the game's full `loadData` (it calls `Super`, touching
base) — we use the targeted per-class accessors, consistent with `savedScalar`.

### 4.2 Every birth path (Q3 — enumerated, one shared reader/applier)

The gap that started this (`NoteLocalDriveBirth` freshBirth-only) can only be prevented from recurring by
routing EVERY birth through ONE reader + ONE applier. The current `savedScalar` fill sites (measured) are
the authoritative list — the channel fills at exactly these, via `ReadSaveRecordForClass`:

| Birth path | Fill site (today, savedScalar) | Apply site |
|---|---|---|
| host live express | `prop_lifecycle.cpp:321` | `prop_fresh_spawn.cpp:304` (PropSpawn apply) |
| join snapshot | `prop_snapshot.cpp:372` | `prop_fresh_spawn.cpp:304` |
| container extract | `prop_container_extract.cpp:140` | `prop_fresh_spawn.cpp:304` |
| PropDropIntent parked-place | `prop_drop_intent.cpp:299` | `prop_drop_intent.cpp:196` |
| ReelEjectIntent fresh | `prop_drop_intent.cpp` (freshBirth block) | `prop_drop_intent.cpp:196` |
| rack `getDrive` | (drive_sync birth emission — see §4.3) | (via PropSpawn adopt) |

Each site calls `ReadSaveRecordForClass` (replacing its `ReadSavedScalarForClass` line); the two apply
sites call `ApplySaveRecordForClass`. A NEW birth path is one call, not a re-implementation — the
freshBirth-only gap cannot recur.

### 4.3 What is retired vs kept (RULE 2)

- **RETIRE (subsumed by the birth-content channel):** `savedScalar` / `kHasSavedScalar` /
  `ReadSavedScalarForClass` / `ApplySavedScalarForClass`; and `drive_sync`'s BIRTH-time content emission
  `NoteLocalDriveBirth` + its `g_notedBirths` broadcast-at-adoption path.
- **KEEP (different concept — steady-state, not birth):** `drive_sync`'s `SweepPayloads` /
  `DrivePayload` lane that syncs a disc's content MUTATING in place (a disc in a rack under
  `saveSignal`/`deleteSignal`/`comp_uploadData`). A birth channel cannot carry an in-place mutation;
  folding it in would lose that sync. `DC::ReadDriveRow`/`WriteDriveRow` are shared by both (birth reader/
  applier AND the steady-state lane) — same accessor, different trigger.

### 4.4 Retirement is a PRECONDITION, not a plan (Q1)

`savedScalar` and `NoteLocalDriveBirth` stay COMPILED AND LIVE until a verifying hands-on take shows,
on BOTH peers: a **reel pocket→place keeps its Progress** AND a **disc drop stays green on the host**.
The deletions-only commit lands in the SAME session as that take. A proven load-bearing fix
(`savedScalar` for reels) is not retired against an untested replacement
(`[[feedback-verify-before-retiring-a-fix]]`). If reels cannot be hands-on-tested that session, the
retirement WAITS — the add ships, the deletion does not.

## 5. Build sequence

1. Add `ReadSaveRecordForClass`/`ApplySaveRecordForClass` (generalize the two `savedScalar` funcs; drive
   case reuses `DC::ReadDriveRow`/`WriteDriveRow`).
2. Grow `PropSpawnPayload`/`PropDropIntentPayload` to the content trailer + `kHasContent`; bump
   `kProtocolVersion` (`[[feedback-wire-format-change-bumps-protocol-version]]`).
3. Route every §4.2 site through the new reader/applier (drop the freshBirth-only `NoteLocalDriveBirth`
   call from `prop_drop_intent.cpp:311`).
4. Build + audit (perf: the join snapshot now carries a content blob per NON-DEFAULT prop only — reuse
   the `RowIsDefault` omit-if-default already in `drive_sync::QueueConnectBroadcastForSlot`, so cost is
   proportional to props that differ, not the thousand-prop total).
5. **Verify hands-on (both peers): disc drop stays green on the host AND reel pocket→place keeps Progress.**
6. ONLY THEN, same session: deletions-only commit (§4.3 retirements).

## 6. Verification (per the checklists)

Smoke first (build + 30 s LAN, log clean). But the CLOSING evidence is HANDS-ON on both peers:
- Disc: client picks up a green (content) disc, drops it → host disc stays GREEN with the signal ID
  (no red flash, no empty). Client and host agree.
- Reel (retirement gate): client pocket→place a reel → its Progress survives on the host.
- Re-run the disc-churn repro (rapid pickup/drop) and read §7.

## 7. Open residuals (NOT closed by this design)

- **The churn re-fire (thread Q2, UNEXPLAINED).** During rapid pickup/drop the client adopts the host's
  fresh SPAWN and immediately re-fires `grab_hook[destroy-seam]`, ~1 Hz, ~9 respawns. No measurement
  explains WHY the adopted mirror is destroyed again. This design fixes the DATA LOSS (content on the
  birth) but is NOT asserted to dissolve the churn. **After it lands, re-run the disc-churn repro and
  measure whether the re-fire dissolves (plausible: it was the destroy+respawn cycle) or survives as its
  own bug (then its own `/qf`).** Do not close this silently.
- **R-Hold / R-tap not verb-isolated.** Only the E-grab-into-hand verb was cleanly isolated (single-slice
  13:49:40). Pure R-Hold (physics carry — would keep the world actor and should NOT lose content) and
  pure R-tap are inferred by the same mechanism, not measured per-verb. Confirm in the verifying take.
- **Base lifespan float** — re-derived on a fresh spawn (measured); assumed irrelevant. If any prop's
  lifespan is player-meaningful, revisit (not observed).
- **Content authority (integrity, not data-loss).** The host trusts the client's content wholesale — the
  SAME trust level as the existing client-streamed inventory blob (already unvalidated), not a new hole.
  A cheap correctness guard belongs here: accept the content only for the parked key being respawned
  (key-match), not content-blind. Value-validation is the arbiter/syncer arc (`docs/COOP_SYNCER_MODEL.md`
  + `docs/security`, all OPEN), NOT this fix.
