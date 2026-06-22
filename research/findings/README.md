# research/findings — point-in-time RE + design log (read this first)

This folder is the project's **append-only, point-in-time** reverse-engineering and design log
(per `docs/ARCHITECTURE.md`: living docs stay current in `docs/`; the dated history lives here). ~150
files. **It is NOT a description of the current state** — each file is a snapshot from its date.

## How to read it (so you don't mistake an old doc for the truth)

1. **For the CURRENT cross-cutting truth, start in `docs/`, NOT here:**
   - [../../docs/COOP_ENTITY_EXPRESSION_MAP.md](../../docs/COOP_ENTITY_EXPRESSION_MAP.md) — how every
     synced entity gets identity/expression/destroy (code-verified, confidence-tagged).
   - [../../docs/COOP_DISPATCH_VISIBILITY.md](../../docs/COOP_DISPATCH_VISIBILITY.md) — will my hook fire?
     (VISIBLE vs INVISIBLE dispatch). **These two supersede the cross-cutting parts of the RE docs here.**
   - [../../docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md), [COOP_SCOPE](../../docs/COOP_SCOPE.md),
     [ROADMAP](../../docs/ROADMAP.md), and the auto-memory (`MEMORY.md` index) for the running state.
2. **`*-RE-*` docs are DURABLE** — bytecode/struct/dispatch facts that stay true until the GAME updates
   (e.g. `votv-pile-grab-observable-hook-RE`, `votv-clump-pile-dupe-DECISIVE-RE` — both cited by the
   COOP_* maps as the evidence base). Trust them, but still verify the offset/field against the current
   CXXHeaderDump (WP18: memory decays, the dump is authority).
3. **`*-DESIGN-*` / `*-PLAN-*` / `*-roadmap-*` docs are POINT-IN-TIME** — the design rationale for a
   feature as of its date. Most describe SHIPPED features (the **code is the as-built truth**, not the
   design doc). Some describe APPROACHES THAT WERE ABANDONED — those live in `_archive/`.
4. **`*-AUDIT-*` / `*-RCA-*` docs** are post-mortems of a specific bug/state; the bug is usually long-fixed.

## `_archive/` — definitively superseded / abandoned approaches
Moved out of the active log so they can't be mistaken for a current plan (see `_archive/README.md`). As
of 2026-06-20: the failed pile save-strip + thin-client-sync approaches. **The CURRENT pile/trash design
is [docs/piles/08-HOST-AUTH-TRASH-CHANNEL.md](../../docs/piles/08-HOST-AUTH-TRASH-CHANNEL.md)** — the
morph (07) was retired 2026-06-21 (its smoke "VERIFIED" was refuted by a real hands-on); the
`*-clump-*`/`*-pile-*` RE docs here are durable bytecode facts, but their design conclusions defer to 08
+ the COOP_* maps. **Correction (2026-06-21):** the `...-pass2` RE's "BeginDeferred (from the chipPile/clump
ubergraph) is `EX_CallMath` → unobservable" is **TRUE** (verified by a live hands-on: 0 host_spawn_watcher
fires, commit `0e56ca39`); it was the s35/**08** "observability reversal" (claiming that POST is observable)
that was FALSE — now corrected in 08 + the COOP_* maps. New durable RE:
`votv-chippile-dispatch-and-thunk-hook-RE-2026-06-21.md`. **Correction (2026-06-22):** the CARRY mirror's
**FREEZE is now FIXED, hands-on VERIFIED** (the `!carrying` release-edge gate in `local_streams.cpp` — the
client clump updates through the carry). The `*-staleness-*-2026-06-21` finding's "carry MIRRORS on a settled
join / it was the JOIN RACE" was FALSE, and so was the interim "contact-re-pile churn" diagnosis: the actual
release-edge cause was `updateHold` PUPPET RECREATION (the `heldActor` ptr changing with `pendingSettle=0`),
NOT a re-pile. Option 2 (the `holdPlayer` convert/ctx gate) is **DISPROVEN by bytecode** — `holdPlayer` is set
once on grab and never cleared in any BP. STILL OPEN [?] (root-found, fix pending): carry JANK (keyless-pose
fix), proxy SCALE (no `SetActorScale`), an intermittent ORPHAN dup (eid-resolve race), the throw-velocity flip.
The canonical carry root + the open-item fixes: `votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md`.

## Note on duplication
The pile/trash/clump/snapshot/save-transfer RE docs are ALSO copied verbatim under
`docs/piles/findings/` (the consolidated pile knowledge base). The originals here are the canonical
copies; `docs/piles/` is the curated subset.

> Sweep note (2026-06-20): this index was added during the "fix ourselves" pass. A full per-file
> staleness audit of all ~150 point-in-time docs was NOT done (most are durable RE or shipped-feature
> design — not misleading); only the definitively-dead approaches were archived. If a specific topic's
> docs look contradictory, the `docs/` canonical doc + the code win.
