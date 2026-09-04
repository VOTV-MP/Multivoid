# Thin-client pile-sync — consolidated go/no-go audit (2026-06-20)

> **`[corr 2026-09-04: docs census sweep]` DURABLE RECORD, SUPERSEDED IN OUTCOME.** Its measurements
> stand as of its date. `[V]` What it was written to inform has since been replaced: the pile mirror
> went PROXY -> NATIVE on 2026-06-30 (`docs/piles/11`, "nativization AS-BUILT + VERIFIED (hands-on)
> ... a rooted real `actorChipPile_C` native"), and the arc then ran on into crutch **C2** and the
> 2026-09-01 rejoin-crash fix. `[V]` Its file paths are pre-refactor -- `prop_adoption.cpp`,
> `pile_handle.cpp`, `pile_claim.cpp`, `pile_seed.cpp`, `prop_spawn_core.cpp` and `freeze_probe.cpp`
> are gone from the tree. Read it as evidence from June 2026, never as current design.

Lead-reviewer collation + dedup of the verified audit findings for the
UNCOMMITTED thin-client ambient-pile sync refactor (proto v88) on `1272b0a3`.
Source design: `thin-client-pile-sync-redesign-2026-06-20.md` (§5 doom gate,
§6 world-wipe proof) + `...IMPL-SPEC-2026-06-20.md`. All findings re-verified
against the live working tree; line cites confirmed.

---

## VERDICT: **GO-WITH-FIXES**

The architecture is sound and faithfully implemented — thin-client identity,
the dedicated `g_sweepFired`-independent pile clock, the bracket-free host
re-stream, the explicit `PileResyncComplete` marker, and the pending-remove-by-id
queue are all present and correct in shape. The 8 hard safety landmines are
respected at the structural level: the pile clock does NOT touch `g_sweepFired`
(landmine 1, verified line 698-744), the pile path never re-arms
`RunDivergenceSweep_`/`g_claimedActors` (landmine 2), the re-stream is
bracket-free (landmine 3), `npc_adoption.cpp` is untouched / `g_ghostSwept`
not cleared (P6 scope), and the doom is COLD (early-returns at line 656).

But the change MUST NOT deploy as-is. There is **one CONFIRMED-mechanism dupe
that defeats the entire feature on a near-routine sequence** (CR-1) and **one
world-wipe-guard defeat** (CR-2). Both live in the same `expected`-denominator
accounting and share a single minimal fix. They are exactly the bug class this
redesign exists to kill (CR-1) and the 2979/3229 mass-wipe guard the redesign
was built to enforce (CR-2). Apply the 2 CRITICAL + 2 HIGH fixes, run the
interaction-smoke that drives a shadow-drain across a grab (HIGH-4 / §12.10),
then deploy.

The accounting cluster (CR-1, CR-2, plus the contributing leaks #9/#16) is the
decisive blocker: the doom denominator is computed from a frozen latch
(`g_pileExpectedMirrors`, never decremented) minus a leak-prone GLOBAL
pending-remove set (`g_pendingRemoveEids`, never bounded to the cycle, polluted
by non-pile eids). That single denominator drives BOTH a permanent-defer dupe
(too high) AND a guard-defeat wipe (driven to 0). Fixing the denominator to be
honest (live catalog size; this-cycle, pile-only pending-removes; an explicit
`liveMirrors >= 1` floor) closes all four at once.

---

## CRITICAL (block deploy)

### CR-1 — Connect-cycle pile doom permanently defers → the ~870-pile client dupe the redesign exists to kill (was #1, HIGH/CONFIRMED → elevated)
- **File:** `src/votv-coop/src/coop/prop_adoption.cpp:715` (gate) / `:648` (latch) / `remote_prop.cpp:907` (resolved-destroy path).
- **Why CRITICAL:** `ArmPileDoomForConnect` latches `g_pileExpectedMirrors = g_hostPileCatalog.size()` (~870) at connect SnapshotComplete and is the ONLY arm in the clean single-load case. The §5 gate fires only if `liveMirrors >= expected`, `expected = g_pileExpectedMirrors - g_pendingRemoveEids.size()`. A NORMAL host grab/collect of one ambient pile during the 2-15 s client load-tail broadcasts `PropDestroy(eid)` that resolves the live mirror and destroys it (`remote_prop.cpp` OnDestroy actor-present branch) → `liveMirrors` drops to 869, but `g_pileExpectedMirrors` is NEVER decremented (set only at lines 622/648) and pendingRemove is NOT incremented (the actor resolved, so the `!actor` enqueue branch is skipped). Result: `869 < 870` → the gate defers every tick FOREVER, the connect doom never fires, and the client's ~870 save-original chipPiles are never destroyed = the exact persistent client-local dupe the change exists to eliminate. Trigger = a single host ambient-pile interaction during the client load tail — near-routine for host-authoritative pile play, not an exotic edge.
- **Minimal fix:** drive the connect doom's denominator off the LIVE `g_hostPileCatalog.size()` at gate time, not the frozen `g_pileExpectedMirrors`. `ForgetPileCatalogEntry` (prop_adoption.cpp:548-551) already keeps the catalog current — every resolved host destroy calls it (via remote_prop.cpp OnDestroy line 907), so `g_hostPileCatalog.size()` self-tracks. Concretely, in `TickPileReconcile` at prop_adoption.cpp:715, for the connect cycle compute `expected` from `g_hostPileCatalog.size()` rather than the latch (or, equivalently, decrement `g_pileExpectedMirrors` in OnDestroy/UnregisterPropMirror whenever a catalogued connect-pile mirror is destroyed). This makes `liveMirrors >= expected` self-track host destroys. See CR-2 for the unified denominator fix that subsumes this.

### CR-2 — World-wipe `expected` floor computed from the whole leak-prone pending-remove set → can drive `expected` to 0 and defeat the mirror-presence guard (was #10, HIGH/PLAUSIBLE; subsumes #9, #16)
- **File:** `src/votv-coop/src/coop/prop_adoption.cpp:715` (denominator) / leak sources `remote_prop.cpp:919-920`, `trash_collect_sync.cpp:364`, `pile_handle.cpp:309`.
- **Why CRITICAL:** the §5 safety gate computes `expected = g_pileExpectedMirrors - g_pendingRemoveEids.size()` clamped to 0 (lines 715-716), subtracting the ENTIRE GLOBAL pending-remove set. That set (a) leaks unbounded within a connection — entries are consumed only when that exact eid is re-created, cleared only at SnapshotBegin/disconnect, NOT at the same-world drain arm (#9), and (b) is polluted by NON-pile keyless eids — `EnqueuePendingRemove` fires for ANY keyless `PropDestroy` resolving no actor (garbageClump despawns, holder-disconnect clumps), but `ConsumePendingRemove` is gated `if (IsChipPile(spawned))`, so clump eids never drain (#16). As the set grows toward `g_pileExpectedMirrors`, `expected` clamps to 0, the guard `liveMirrors < expected` becomes `liveMirrors < 0` (always false), and the doom fires regardless of how few host mirrors are actually present → it destroys the client's save piles before the host mirrors re-materialize = a partial pile world-wipe. Critically, the design's stated positive floor `liveMirrors >= 1` (§5b, §6) is **NOT implemented** — the only fire-guards are `g_pileResyncCompleteSeen` (698) and `liveMirrors >= expected` (717). This is precisely the 2979/3229 mass-wipe guard being silently disabled.
- **Minimal fix (unified — closes CR-1, CR-2, #9, #16):**
  1. **Honest denominator:** subtract only the intersection of `g_pendingRemoveEids` with the pile set, i.e. `g_hostPileCatalog`, not the whole set size — `expected = liveCatalogSize - |pendingRemove ∩ catalog|`. Drive `liveCatalogSize` off the current `g_hostPileCatalog.size()` (self-tracking via `ForgetPileCatalogEntry`), which also fixes CR-1. (Alternatively, gate `EnqueuePendingRemove` on a chipPile-class hint at remote_prop.cpp:919 so only pile eids ever enter the set.)
  2. **Bound the queue to the cycle:** clear `g_pendingRemoveEids` when a `PileResyncComplete` is fully processed (host has re-asserted ground truth — any unmatched remove is now stale), and/or attach a short TTL matching the pile_handle grab/relay TTLs. This stops the unbounded leak (#9) and keeps the denominator scoped to the in-flight cycle.
  3. **Implement the design's explicit positive floor:** require `liveMirrors >= 1` UNLESS (`g_pileResyncCompleteSeen` AND `expected == 0` by an explicit zero-count marker). A corrupted/zero denominator can then never license a fire against an empty/partial mirror set. This is the §5b/§6 floor the spec mandates and the code omits.
- **Verify against an interaction smoke** (HIGH-4): connect → host-grab during load-tail (CR-1 path) AND a long-session drain after clump churn (CR-2 path).

---

## HIGH (fix before hands-on)

### H-1 — `ReStreamPilesToSlot` sends ~870 PropSpawns in one synchronous game-thread call → host frame hitch 2-3x/join (was #6, MEDIUM/PLAUSIBLE → elevated for the §12.6 / freeze_probe scar)
- **File:** `src/votv-coop/src/coop/prop_snapshot.cpp:544-579` (loop) / called at `event_dispatch_state.cpp:760`.
- **Why HIGH:** `ReStreamPilesToSlot` walks the full GUObjectArray building one PropSpawn per live chipPile (~870) with three reflection transform reads each (GetActorLocation/Rotation/Scale3D), in ONE synchronous call, no chunking. The sibling `DrainChunk` deliberately caps at 100/tick because prop_snapshot.cpp:86-90 estimates ~5-15 ms per 100 under the 16.6 ms frame budget — this does ~870 in one frame, directly contradicting that budget logic, 2-3x per join. The redesign §13 + §12.6 explicitly required a freeze_probe frame-hitch measurement before handoff; no such instrumentation was added. This is the exact join-window-hitch class the project has scars for (2.5 fps reaper, 19 GB OOM).
- **Minimal fix:** chunk the re-stream like `DrainChunk` into a per-tick drain (100/tick), send `PileResyncComplete{count}` only after the last chunk drains, and profile the host send burst with freeze_probe before handoff (close the §13 open risk). Combine with H-3 host-side rate limit (#8) since both touch the same handler.

### H-2 — Pile-doom gate runs O(P×M) with a per-pile mutex-locked allocation, repeated at 5 Hz across the join window (was #11, MEDIUM/CONFIRMED → elevated; 2.5fps-reaper-class pattern)
- **File:** `src/votv-coop/src/coop/prop_adoption.cpp:705-712` (gate count) + `:574-602` (`DoomNonMirrorChipPiles_`); `remote_prop.cpp:788-796` (`IsReceivedPropMirror`).
- **Why HIGH:** both the §5 `liveMirrors` count loop AND `DoomNonMirrorChipPiles_` walk the whole GUObjectArray (~150k) and call `IsReceivedPropMirror(obj)` per chipPile (~870). That function takes the PropMirrors mutex, reserves+copies the ENTIRE mirror map (~870 ptrs) into a fresh `std::vector`, then linear-scans it — so one gate tick = ~870 mutex-locked vector allocations + ~870×870 ≈ 750k compares. Because the host re-stream is async, the gate defers and re-runs every 5 Hz (~200 ms) across the quiescence/deadline window (up to 45 s), a sustained O(P×M)+heap-churn loop on the exact join path that already stresses RSS. This is the per-iteration-allocation O(N²) shape CLAUDE.md's audit rule and the 2.5 fps reaper scar warn against. (Partially mitigated: the full walk runs only after `g_pileResyncCompleteSeen` is true, but the sustained-loop characterization holds while the re-stream trickles in.)
- **Minimal fix:** hoist the mirror set out of both per-pile loops — call `PropMirrors().Snapshot(snap)` ONCE per gate tick, build a `std::unordered_set<void*>` of mirror actors (`IsMirror()==true`), then test set membership inside the GUObjectArray walk → O(P+M) with a single allocation. Share the same single-snapshot set between the `liveMirrors` count (705-712) and `DoomNonMirrorChipPiles_` (574-602). Cold-edge semantics preserved; removes the per-pile lock+alloc+scan. (This also closes #15, the same pattern in the doom walk.)

### H-3 — No host-side rate limit on `PileResyncRequest` → a churning client can force repeated full ~870-pile re-streams (was #8, LOW/PLAUSIBLE → folded HIGH with H-1)
- **File:** `src/votv-coop/src/coop/event_dispatch_state.cpp:760`.
- **Why grouped HIGH:** the handler unconditionally calls `ReStreamPilesToSlot` (the H-1 ~870-pile synchronous burst) on every request with no debounce/coalescing. A multi-reload client sending several `PileResyncRequest` in quick succession stacks H-1's host hitch into back-to-back stalls. Low on its own, but it multiplies H-1's severity, so fix together.
- **Minimal fix:** debounce host-side (skip a request serviced within a short window, e.g. the reaper's 4 s throttle) or fold the re-stream into the per-tick drain queue (the same chunking from H-1), mirroring `TriggerForSlot` busy/dedup.

### H-4 — Interaction smoke does not force a shadow-drain across grab/throw → the highest-risk new code is unexercised (was #21, LOW/CONFIRMED → elevated; §12.10 + the 3-broken-pile-fixes scar)
- **File:** `src/votv-coop/src/harness/autotest.cpp` (PILEGRAB_TEST / PILEGRAB_REACH_TEST) + `tools/mp.py:1644-1681` (joinchurn verdict).
- **Why HIGH:** the two new env-gated smokes exercise STEADY-STATE grab via the real InpActEvt_use path — good — but NEITHER forces a same-world shadow-drain (InPurgeEpisode entry/exit) and verifies `PileResyncRequest → ReStreamPilesToSlot → PileResyncComplete → pile-doom`, nor issues a grab/throw landing during the drain window. The drain bridge + pending-remove-in-drain + the doom denominator (the CR-1/CR-2 surface) are the highest-risk new code and are exercised only INCIDENTALLY by the multi-reload join, never asserted. The joinchurn verdict asserts sweep/incr/destroyed/kerfur metrics but NONE of the pile-resync/doom markers. This is the exact coverage gap that shipped 3 broken pile fixes. Given CR-1/CR-2 are denominator-race bugs, an asserting smoke is the only thing that will catch a regression of the fix.
- **Minimal fix:** add a smoke that triggers a shadow-drain (2nd client reload / small travel) and asserts the live log markers: `client shadow-drain complete -> PileResyncRequest sent + pile doom armed` (net_pump.cpp:527), `re-streamed N live pile(s) + PileResyncComplete{N}` (event_dispatch_state.cpp:760), `PILE DOOM FIRING ... liveMirrors=N expected=N` (prop_adoption.cpp:727), and a grab issued during the drain window resolving correctly afterward. Add these markers to the `mp.py` verdict. This is the §12.10 "force a shadow-drain AND exercise grab/throw across it" check — run it to validate the CR-1/CR-2 fixes.

---

## OTHER (LOW / nits — fix opportunistically, not blocking)

- **#17 [MEDIUM/CONFIRMED] RULE-2: dead v84 `locX/locY/locZ` in `PropDestroyPayload`.** The OnDestroy position-fallback was deleted but `PropDestroyPayload` still declares `float locX, locY, locZ` (protocol.h:2371, sizeof locked at 48 via static_assert) and `pile_handle.cpp:199` still WRITES them; no receiver reads them for piles. Migration baggage RULE 2 forbids. Since proto is already bumping to v88 (no interop), the wire shrink is free: drop the 3 fields + the writer + the v84 comment, update the static_assert. *Recommend fixing in this pass* — it is a true RULE-2 violation and trivially free, even if not a runtime blocker.
- **#16 / #9 accounting leaks** — subsumed by CR-2's unified fix (queue-scoping + pile-only enqueue). Listed here for traceability.
- **#15 [LOW/CONFIRMED]** `DoomNonMirrorChipPiles_` per-pile `IsReceivedPropMirror` Snapshot — subsumed by H-2's single-snapshot fix.
- **#12 / #14 [LOW/CONFIRMED] `CountLoadTailUnsettled_` chipPile-inclusion adds ~870×2 wstring (and a ProcessEvent dispatch per pile via `GetInteractableKeyString`) allocations per 5 Hz scan to BOTH quiescence clocks during the join window.** Cold relative to steady state (clocks early-return when idle), but a measurable per-scan cost in the exact join window with prior hitch scars. Fix: short-circuit a chipPile before the key read — `if (IsChipPile(obj)) { ++unsettled; continue; }` (a pile is always keyless/load-tail, no ClassNameOf/GetInteractableKeyString needed). Preserves the pile-inclusive settle semantics cheaply.
- **#4 / #19 [LOW/CONFIRMED] RULE-2/3: TEMP `freeze_probe` dev diagnostic compiled unconditionally into the shipping DLL** (CMakeLists.txt:313, called at net_pump.cpp:261 + 4 other sites). Self-labeled "remove once diagnosed", no ini/compile gate (less gated than its siblings perf/leak/heap). Negligible cost (one QPC+compare per call). Before committing "the whole stack": either env-gate it like the sibling probes or remove the file + its 5 call sites + the CMakeLists entry per its own note. *Note:* H-1 wants freeze_probe to measure the host send burst — keep it until that measurement is done, then remove.
- **#13 [LOW/CONFIRMED] `remote_prop.cpp` grew 1024 → 1128 LOC, past the 800 soft cap, no extraction proposal.** Under the 1500 hard cap (not a rejection) but the modular rule requires the flag now. Proposed extraction: split the mirror-registry/resolve layer (`RegisterPropMirror` / `ResolveMirrorEidByActor` / `IsReceivedPropMirror` / `UnregisterPropMirror`) into `coop/prop_mirror_registry.cpp` in a separate prior commit.
- **#18 [LOW/CONFIRMED] `remote_prop_spawn.cpp` at 829 LOC exceeds the 800 soft cap** (net-shrank from ~1455 via RULE-2 deletions but the new `SpawnLocalTrashActor` + OnSpawn catalog block keep it over). Flag-with-proposal: extract `SpawnLocalTrashActor` (+ deferred-spawn helpers) into `coop/trash_actor_spawn.cpp`, or move the OnSpawn catalog/consume block into prop_adoption.cpp.
- **#7 [LOW/CONFIRMED] `PileResyncComplete` rides Lane Normal while its PropSpawns ride Lane Bulk** (session_lanes.h:92 falls through to default Normal; PropSpawn is Bulk at line 42). GNS orders only within a lane, so the marker can overtake its own stream under backpressure — the identical hazard `SnapshotComplete` was pinned to Bulk to prevent. Safe today only because the `liveMirrors >= expected` gate defers, but it erodes the explicit-complete-means-stream-landed semantics. Fix: add a `LaneForKind` case mapping `PileResyncComplete` to `Lane::Bulk`, matching the `SnapshotComplete` precedent. *Cheap and worth doing* — it hardens the §6 explicit-marker invariant.
- **#5 [LOW/PLAUSIBLE] OnSpawn `existing`/fuzzy early-return paths do not `ConsumePendingRemove`** (remote_prop_spawn.cpp:338/355/476/491/557). Provably unreachable for piles today (a pending-remove eid has no live mirror by construction, so a keyless pile can never hit the `existing`/fuzzy branch and always reaches fresh-spawn where consume fires). Latent only — a future change letting a chipPile resolve to an `existing` mirror with a pending-remove eid would silently skip the queued remove. Fix: make consume local to the create primitive (after each pile-bearing `RegisterPropMirror`, `if IsChipPile && ConsumePendingRemove(eid)` → teardown), or add a one-line assert/comment documenting the unreachability.
- **#3 [LOW/PLAUSIBLE] cross-bracket staleness of `g_pileResyncCompleteSeen`/`g_pileExpectedMirrors`** — `BeginClaimTracking` clears the catalog + pending-remove but does NOT reset the completeSeen/expected state on a 2nd connect bracket. Safety holds only incidentally via the `liveMirrors >= expected` arithmetic (stale-high defers; stale-low only over-dooms save originals, the safe direction). No wipe constructible. Fix: reset `g_pileResyncCompleteSeen=false` (+ optionally `g_pileExpectedMirrors=0`) in `BeginClaimTracking` so a fresh bracket forces the doom to re-await this bracket's authoritative count (makes the invariant explicit). *Fold into CR-2's queue-scoping work.*
- **#20 [LOW/CONFIRMED] small-travel world-change branch double-arms the doom + sends a redundant `PileResyncRequest`** (net_pump.cpp:549) — on a genuine UWorld swap, `ArmPileDoom` from the edge AND `ArmPileDoomForConnect` from the re-announce bracket both fire, streaming the full pile set twice. ArmPileDoom is idempotent and both counts are valid host truths → not a wipe, just redundant wire + clock churn under the join cover. Fix: gate the small-travel `PileResyncRequest`/`ArmPileDoom` on the SAME-WORLD condition (`reapWorld == g_announcedWorld`) so a genuine swap relies on the fresh bracket's `ArmPileDoomForConnect`.

---

## Safety-landmine compliance summary (the 8 hard checks)

| # | Landmine | Status |
|---|---|---|
| 1 | Pile clock independent of `g_sweepFired` | PASS — TickPileReconcile uses only `g_pileDoom*`; never touches g_sweepFired / OnClientWorldReadyResetSweep |
| 2 | Pile path never re-arms `RunDivergenceSweep_` / `g_claimedActors` | PASS |
| 3 | Host re-stream bracket-free | PASS — `ReStreamPilesToSlot` sends raw PropSpawns, no SnapshotBegin/Complete |
| 4 | Doom never fires while `liveMirrors < expected`; zero-pile branch needs explicit marker | **PARTIAL FAIL — CR-2** (no `>= 1` floor; `expected` corruptible to 0) |
| 5 | Consume pending-remove on every pile-create path | PARTIAL — fresh-spawn only; latent gap #5 (unreachable today) |
| 6 | Doom spares an active grab/throw relay | PASS — `IsPendingGrabOrThrow` / `IsLocalRelayInFlight` guards present |
| 7 | TickPileReconcile zero-cost idle; doom + re-stream COLD | PASS idle (656); **PERF debt H-1/H-2** when active |
| 8 | New ReliableKinds wired in all 3 places + proto bump | PASS (enum/payload + family router + dispatch handlers; v88); **lane gap #7** |

P6 (coupled kerfur) correctly OUT OF SCOPE — `npc_adoption.cpp` untouched, `g_ghostSwept` not cleared. The known pre-existing NPC dupe is acceptable; no NEW NPC regression introduced.

---

## Recommended action order

1. **CR-2 unified denominator fix** (honest pile-only this-cycle `expected` off live `g_hostPileCatalog.size()` + queue-scoping clear on PileResyncComplete + explicit `liveMirrors >= 1` floor) — closes CR-1, CR-2, #9, #16, #3.
2. **#17** drop the dead v84 PropDestroy fields (free RULE-2 cleanup, same pass).
3. **#7** pin PileResyncComplete to Lane::Bulk.
4. **H-1 + H-3** chunk `ReStreamPilesToSlot` + host-side debounce; profile with freeze_probe.
5. **H-2** single mirror-snapshot per gate tick (closes #15); **#12/#14** chipPile short-circuit in `CountLoadTailUnsettled_`.
6. **H-4** build the shadow-drain-across-grab interaction smoke; add markers to the mp.py verdict; run it to validate 1-5.
7. Remove `freeze_probe` (#4/#19) AFTER the H-1 measurement; flag #13/#18 file extractions for a follow-up commit.

Then run the pre-deploy checklist (30 s LAN smoke + log diff) and commit the whole stack.
