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
  (2026-06-21, commit `0e56ca39`, proto v82).** Host-authoritative trash-entity identity (eid = logical
  entity), MTA single-syncer + sync-time-context byte. **GRAB (pile→clump) = VERIFIED hands-on** via the
  `InpActEvt_use` PRE seam + the held-edge adopt. **RE-PILE (clump→pile) = AS-BUILT, hands-on no-dupe** via
  a clump death-watch convert onto the fresh UNTRACKED pile (one ~5s known-minor glitch). **The s35
  "BeginDeferred-POST is observable" link was DISPROVEN** (EX_CallMath → invisible; 0 host_spawn_watcher
  fires; that POST link was removed). The deterministic zero-proximity catch needs a `UFunction::Func`
  thunk patch (DESIGN, IDA-gated — `research/findings/votv-chippile-dispatch-and-thunk-hook-RE-2026-06-21.md`).
  Increment 2 (client suppress-native + GrabIntent → host executes the verb on puppet-N; the
  PILED/HELD/FLYING state machine) is still DESIGN. **Read this for the pile sync.**
- **[07-MORPH-V2-held-object-channel.md](07-MORPH-V2-held-object-channel.md)** — **SUPERSEDED + RETIRED
  2026-06-21** (the morph: held-object adopt + PROXIMITY land-watch). Its smoke "VERIFIED" was a FALSE
  POSITIVE; the real hands-on refuted it (proximity false-fires in clusters; client grab never armed).
  History only — see 08.
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

## Status (2026-06-20, session 32) — DESIGN → AS-BUILT → VERIFIED

1. Save-strip REVERTED + the entire s23–s32 thin-client churn REVERTED → restored the
   **adopt-by-position bind** (HEAD `1272b0a3`). Deployed `C7030D00`.
2. **Baseline hands-on (user-confirmed): the bind WORKS** — 872 chipPile mirrors bound, no
   "no piles", no dupes; host-grab → client-pile-vanishes proven. BUT the **morph/held layer**
   didn't sync (grabbed pile → keyless clump `eid=0`; 0 `PropConvert` morphs) → "no sync-mirror".
3. **Morph sync ([05](05-MORPH-SYNC-DESIGN.md) DESIGN + [06](06-AS-BUILT-sync-mirror.md) AS-BUILT)
   was implemented (proto v81), audited+fixed, deployed — and FAILED the hands-on ("nothing works")
   as a REGRESSION → REVERTED.** Root cause (logs): the clump-adopt correlation keyed on
   `clump.holdPlayer`, which is NULL at the clump's Init-POST (set later by `toClump()`), so the
   adopt never fired → no convert; and it had REPLACED the reliable grab `PropDestroy`, so a missed
   morph = no host-grab sync. See `06` for the full RCA + what the next attempt must MEASURE first.

**DEPLOYED BASELINE: `EFD869CBF2D8788F` (proto v80) = the take-17 adopt-BIND** (piles mirror,
host-grab→client-vanish works; the clump carry / re-pile morph is NOT synced — the open gap).

4. **MORPH V2 — the carry+re-pile done RIGHT ([07](07-MORPH-V2-held-object-channel.md), session 33).**
   Built on the bind model, anchored on the PROVEN held-object channel (NOT the un-hookable clump
   Init-POST / holdPlayer the take-18 bet failed on), with a deferred-destroy fallback so a missed morph
   never regresses the grab. RETIRES the v52 fresh-eid clump death-watch (RULE 2). Re-skins eid E in
   place across pile→clump→pile (oldEid==newEid==E). Land via a death-watch poll (re-seed-race-safe).
   DLL `2f0970276799478c` (proto v81), build clean, **2 adversarial reviews + a 4-agent trace applied**.
   **STATUS: BUILT + REVIEWED, NOT deployed, NOT verified** (the one unverified link = does the clump
   become `holding_actor`; needs the `garbage_pickup_probe` + 1 grab) — see `07` AS-BUILT.

Then (separate): the kerfur window-dupe (host-authoritative client-relays-turn-on + form-agnostic adopt).
