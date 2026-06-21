# Piles — the complete knowledge base (ambient trash pile coop sync)

> Single source of truth for the `actorChipPile_C` ("ambient trash pile") multiplayer
> sync problem. Assembled 2026-06-20 (session 32) after the save-strip attempt FAILED
> and the user pointed back to a working June-12/13 scheme. Everything we know about
> piles lives here.

## TL;DR — read this first

- **A pile is `actorChipPile_C`.** It is KEYLESS and has **NO cross-peer identity** of any
  kind. Bytecode-verified on the cooked pak (see `re-artifacts/`): no Key field, `setKey()`
  is a no-op the BP discards, `gatherDataFromKey` hardcodes `gather=false`, and a pile
  persists as a `primitivesData` record (class+transform+chipType) that loads by
  **destroy-all-int_primitive + fresh POSITIONAL respawn**. The only handle a pile can have
  across peers is a HOST-MINTED eid the coop layer assigns and streams.
- **The recurring bug is a JOIN DUPE** (the client ends with 2 of each pile) **+ an
  interaction dupe** (the client spamming E accumulates local-only clumps).
- **A working scheme existed (~June 12-13, committed in the June-10 batch `77225106`):
  "stale save → reconcile".** The client loaded the host's stale `.sav` (so it had its own
  copy of every pile), the host streamed a keyless eid per pile, and a **reconcile**
  (adopt-by-position at the snapshot bracket + a divergence sweep for residue) bound the
  client's OWN pile actor onto the host eid — one actor, no dupe. See [01-HISTORY.md](01-HISTORY.md).
- **What broke it:** a long chain of "improvements" (live-save switch → position-bind seed →
  death-watch → reaper → thin-client doom → save-strip) progressively replaced the
  adopt-by-position reconcile with fresh-spawn-mirror + destroy-the-original mechanisms that
  never converge. The latest (save-strip) was REVERTED 2026-06-20.
- **The plan:** restore the adopt-by-position reconcile (it is intact at committed HEAD
  `1272b0a3`'s `EnsurePileBindIndex`) and fix the interaction dupe at its real root (the
  freed-mirror re-stream trigger). See [03-RESTORATION-PLAN.md](03-RESTORATION-PLAN.md).

## What is a pile, exactly (the engine facts)

| Fact | Detail | Source |
|---|---|---|
| Class | `AactorChipPile_C` (+ subclasses `_erie`/`_wetConcrete`/`_leaves`) | `re-artifacts/chipPile.json`, CXXHeaderDump |
| Interfaces | `int_objects_C` + `int_save_C` + `int_primitive_C` | re-artifacts |
| Identity | **NONE.** No Key field; `setKey()` writes a transient frame nothing reads | `re-artifacts/chipPile.json` (getData/getKey hardcode "None") |
| Save (objectsData) | `getData` writes a key="None" struct_save record @ objectsData | `re-artifacts/saveObjects_dump.txt` |
| Save (primitivesData) | `getPrimitiveData` writes class+transform+chipType-json @ primitivesData (the LOAD-authoritative array) | `re-artifacts/mainGamemode.json` (Save Primitives) |
| Load | `loadObjects` spawns from objectsData, then "Load Primitives" DESTROYS all int_primitive and RESPAWNS positionally from primitivesData | `re-artifacts/loadObjects_dump.txt` |
| Grab/morph | grabbing a pile runs the BP `toClump()` → spawns a `prop_garbageClump_C` (the carried "ball") | `findings/votv-chippile-clump-morph-RE-2026-05-27.md` |
| Why no save-strip | the save-loaded pile IS what the coop layer adopts as the mirror; stripping it leaves the client with NO piles | session 32 hands-on (this folder) |

## The two bugs, precisely

1. **JOIN DUPE** — on join the client loads the host save (its own ~870 piles) AND the host
   streams a mirror eid per pile. If the mirror **adopts onto** the client's pile → 1 actor,
   correct. If it **fresh-spawns a separate mirror** (or the doom that should delete the
   original never fires) → 2 actors = the dupe. The recent stack fresh-spawns + relies on a
   doom that wedges (`liveMirrors < expected`, never converges). Full RCA:
   [02-CURRENT-DIAGNOSIS.md](02-CURRENT-DIAGNOSIS.md) §1.
2. **INTERACTION DUPE (E-spam)** — the join's same-world "shadow-drain" (a mass actor purge)
   FREES the host pile-mirror actors; the catalog isn't decremented and the re-stream that
   should re-materialize them is gated behind a purge-episode-EXIT edge that never fires under
   churn → the piles the player walks up to are mirror-UNBOUND → grabbing one mints a
   permanent local-only `prop_garbageClump_C` (eid=0) that no relay suppresses and no doom
   reaps. Full RCA: [02-CURRENT-DIAGNOSIS.md](02-CURRENT-DIAGNOSIS.md) §2.

## Cross-cutting truth FIRST (read these before the pile-specific docs)

The pile dupe was, at root, a **"which seam expresses this entity?" / "will my hook fire?"** problem.
Those questions are now answered canonically (code-verified, confidence-tagged) in:
- **[../COOP_ENTITY_EXPRESSION_MAP.md](../COOP_ENTITY_EXPRESSION_MAP.md)** — every entity's spawn→catch→
  identity→destroy, incl. the chipPile/clump dupe matrix.
- **[../COOP_DISPATCH_VISIBILITY.md](../COOP_DISPATCH_VISIBILITY.md)** — VISIBLE vs INVISIBLE dispatch +
  which of our seams can/can't see what (the `init()`-is-`EX_LocalVirtualFunction` fact that killed the
  take-18 Init-POST bet).

These supersede the cross-cutting parts of the pile docs below; 01-04 remain the pile-specific
history/diagnosis/design, and **08 is the CURRENT design** (the host-authoritative trash channel);
**07 is SUPERSEDED + RETIRED** (the morph -- a real hands-on refuted its smoke "VERIFIED" 2026-06-21).

## Folder contents

- **[01-HISTORY.md](01-HISTORY.md)** — the full per-stage evolution of pile sync (every
  scheme, when it worked, what each change did and why), incl. the June-12/13 working
  "stale save → reconcile" the user remembers, and the exact regression chain.
- **[02-CURRENT-DIAGNOSIS.md](02-CURRENT-DIAGNOSIS.md)** — the two bugs' root causes, from the
  bytecode RE + the user's real logs (the 758/868 doom wedge + the E-spam local-clump
  accumulation).
- **[03-RESTORATION-PLAN.md](03-RESTORATION-PLAN.md)** — the plan: restore the adopt-by-position
  reconcile + fix the interaction dupe; the revert/keep/delete inventory across the uncommitted
  stack; what to preserve (kerfur/email/harness). (EXECUTED — see Status.)
- **[04-ROBUST-DESIGN.md](04-ROBUST-DESIGN.md)** — what good looks like: the 5 pillars of robust
  native-looking pile sync + the anti-patterns we did wrong (the deterministic-placement insight).
- **[08-HOST-AUTH-TRASH-CHANNEL.md](08-HOST-AUTH-TRASH-CHANNEL.md)** — **THE CURRENT DESIGN + AS-BUILT
  (2026-06-21 session 38, HEAD `fea04c26`, deployed `BA79E705`, proto v82).** Host-authoritative trash-entity
  identity (eid = logical entity), MTA single-syncer + sync-time-context byte. **GRAB (pile→clump) = VERIFIED
  hands-on** via the `InpActEvt_use` PRE seam + the held-edge adopt. **RE-PILE (clump→pile) = the
  DETERMINISTIC `UFunction::Func` thunk converter** (commit `d19ae4d4`, `ue_wrap/ufunction_hook`): catches the
  clump's `EX_CallMath BeginDeferred` and converts E onto the EXACT spawned pile the same tick — zero
  proximity, no reaper race (the ~5s vanish-return is gone by construction). The thunk DETECTION is VERIFIED
  (read-only pass agreed ptr-for-ptr with the old death-watch); the CONVERT + the triple-grab-cue fix
  (`fea04c26`) are deployed-pending-hands-on. The proximity death-watch is RETIRED (RULE 2). **The s35
  "BeginDeferred-POST is observable" link was DISPROVEN** (EX_CallMath → invisible; 0 host_spawn_watcher
  fires). **OPEN — the client mirror-staleness dup** (a client join-mirror goes NOT-LIVE within ~10s →
  fresh-clump spawn + original lingers; the ROBUSTNESS track, separate from the thunk fix:
  `research/findings/votv-pile-mirror-staleness-robustness-DESIGN-2026-06-21.md`). Increment 2 (client
  suppress-native + GrabIntent → host executes on puppet-N; the PILED/HELD/FLYING state machine) is still
  DESIGN. **Read this for the pile sync.**
- **`_archive/07-MORPH-V2-held-object-channel.md`** — **SUPERSEDED + RETIRED 2026-06-21, archived** (the
  morph: held-object adopt + PROXIMITY land-watch). Its smoke "VERIFIED" was a FALSE POSITIVE; the real
  hands-on refuted it (proximity false-fires in clusters; client grab never armed). History only — see 08.
- `_archive/05-MORPH-SYNC-DESIGN.md` + `_archive/06-AS-BUILT-sync-mirror.md` — **SUPERSEDED** (the FAILED
  take-18 that bet on the clump Init-POST observer firing; it does NOT). Kept for the failure analysis.
- `_archive/session-log/` — the day-by-day failed-iteration log (s21..s33). Historical.
- **`findings/`** — every pile/trash/clump/snapshot/save-transfer RE + design doc (20 files),
  copied here verbatim from `research/findings/`.
- **`re-artifacts/`** — the cooked-pak Blueprint RE that proves the pile-identity facts:
  - kismet JSON + extracted dumps: `chipPile.json`, `mainGamemode.json` (saveObjects /
    Save Primitives / Load Primitives), `saveSlot.json`, `int_save.json`, `int_primitive.json`,
    `int_objects.json`, `prop_base.json`, `saveObjects_dump.txt`, `loadObjects_dump.txt`.
  - **`bp_reflection/`** (19 files) — the BP function-reflection dump for the WHOLE pile
    ecosystem: `actorChipPile`, `prop_garbageClump` (+ `_clump_uber_full.txt`), `trashBitsPile`,
    `event_trashPiles`, `garbagePileSpawner`, `undergroundGarbageSpawner`, `baseCleaner_clumps`,
    `baseCleaner_trashBits`, `arirTrasher` — each as `.functions.txt` (readable function list) +
    `.json` (full reflection).
- **`test-evidence/`** (11 logs) — the ground-truth behavior logs: the user's hands-on dupe runs
  (`handson-s31-doom-*`, `handson-s32-strip-*`) the diagnosis is built on, plus the autonomous
  pile smoke/grab logs (`pilesmoke_*`, `pilegrab_*`, `pilefix_run.log`).
- **`session-log/`** — the day-by-day attempt log copied from session memory
  (`project_session21..32` + the MTA-divergence roadmap + the UAF-crash note). 13 files; each is
  one iteration of the saga. Read these for the full blow-by-blow rationale behind every
  scheme/abandonment.

### NOT copied here (referenced — bulky binary/visual, live in `research/`)
- Raw cooked assets `research/pak_re/extracted/VotV/Content/objects/{actorChipPile,prop_garbageClump,
  trashBitsPile,event_trashPiles,...}.uasset/.uexp` — the BINARY input the `re-artifacts/` JSON was
  decompiled from. The JSON IS the readable RE; the raw assets are kept only to re-derive.
- Pile screenshots: `research/clumpshot_shots/`, `research/clumpvis_shots/`,
  `research/smoke_shots/pile*.png` — visual evidence (PNGs).

## Cross-references (not copied — live elsewhere)

- **Session memory** (`memory/project_session21..32_*.md`): the day-by-day attempt log
  (s21 join-churn → s22 death-watch → s23 invisible-handle → s24 position-keyed → s25 eid-bind
  timing → s26 take2 → s28 static-object → s29 confirm-by-clump → s30 reaper → s31 thin-client
  doom → s32 save-strip). Each is one failed/partial iteration; read for the full blow-by-blow.
- **MTA precedent**: `reference/mtasa-blue/` — `CEntityAddPacket` + server-stamped `ElementID`
  (one instance per entity, id injected at stream). The north star for "host is the sole owner."
- **`research/findings/`** — the originals of everything in `findings/` here (plus the kerfur
  docs, which are a sibling problem: `votv-kerfur-savetransfer-ghost-prop-RCA-2026-06-15.md`,
  `votv-kerfur-prop-join-adoption-RCA-AND-DESIGN-2026-06-16.md`).

## Status (2026-06-21, session 38) — DESIGN → AS-BUILT → VERIFIED

The whole saga below (01–04 + the s21–s33 session-log) converged on **08 — the host-authoritative trash
channel**, which is the CURRENT design + as-built. The day-to-day live state is in the auto-memory
(`MEMORY.md` index + the top `project_session*` entry); this is the durable summary:

1. **GRAB (pile→clump) — VERIFIED hands-on** (`[SYNC-MIRROR OK]`) via the `InpActEvt_use` PRE seam + the
   held-edge adopt. Identity is the host-minted eid end-to-end, NO proximity.
2. **RE-PILE (clump→pile) — the DETERMINISTIC `UFunction::Func` thunk converter** (commit `d19ae4d4`,
   `ue_wrap/ufunction_hook`): the thunk catches the clump's `EX_CallMath BeginDeferred` and converts E onto
   the EXACT spawned pile the same tick — zero proximity, no reaper race. The thunk DETECTION is **VERIFIED**
   (a read-only pass agreed ptr-for-ptr with the prior proximity death-watch, which is now RETIRED, RULE 2);
   the CONVERT flip + the triple-grab-cue fix (`fea04c26`, the ctx-gate requireCurrentGen split) are
   **AS-BUILT, deployed `BA79E705`, hands-on-PENDING** (the next hands-on confirms a single grab cue + no
   vanish-return). Runbook: `research/handson_runbook_2026-06-21_repile_thunk.md` (take-22).
3. **OPEN — the client mirror-staleness dup (ROBUSTNESS track):** a client's join-mirror of a pile goes
   NOT-LIVE on its own within ~10s → `OnConvert`'s `ResolveLiveActorByEid` returns null → a fresh clump is
   spawned + the original lingers = a dup. This is NOT the host-side mis-bind the thunk fixes. Design:
   `research/findings/votv-pile-mirror-staleness-robustness-DESIGN-2026-06-21.md`.
4. **NEXT (DESIGN, NOT built):** grab-via-thunk (closes the eid=0 adopt-miss gap) + Increment 2 (the
   client-grab direction — suppress-native + GrabIntent → host executes on puppet-N; proto v83).

**DEPLOYED: `BA79E705` (proto v82) = HEAD `fea04c26`** — the thunk re-pile + the sound fix, pending the
hands-on confirmation. The earlier `C7030D00` adopt-bind baseline + the FAILED s05/06 morph + the s07
morph-V2 are all superseded by 08 (the 07 doc is archived).
