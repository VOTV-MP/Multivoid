# Thin-client pile sync — AUDIT-FIX spec (2026-06-20)

> **`[corr 2026-09-04: docs census sweep]` DURABLE RECORD, SUPERSEDED IN OUTCOME.** Its measurements
> stand as of its date. `[V]` What it was written to inform has since been replaced: the pile mirror
> went PROXY -> NATIVE on 2026-06-30 (`docs/piles/11`, "nativization AS-BUILT + VERIFIED (hands-on)
> ... a rooted real `actorChipPile_C` native"), and the arc then ran on into crutch **C2** and the
> 2026-09-01 rejoin-crash fix. `[V]` Its file paths are pre-refactor -- `prop_adoption.cpp`,
> `pile_handle.cpp`, `pile_claim.cpp`, `pile_seed.cpp`, `prop_spawn_core.cpp` and `freeze_probe.cpp`
> are gone from the tree. Read it as evidence from June 2026, never as current design.

Apply on top of the implemented + audited change. Authoritative source of WHAT to
fix: `thin-client-pile-sync-AUDIT-2026-06-20.md` (the GO-WITH-FIXES verdict, §
"Recommended action order"). This file pins the EXACT mechanics for the subtle
safety-critical pieces (CR-1/CR-2 gate, H-2) so they are unambiguous; for the rest
follow the audit doc's per-finding "Minimal fix".

## THE KEY INSIGHT (why the unified denominator fix is correct)
The pile doom has TWO independent gates: (a) the QUIESCENCE clock
(`CountLoadTailUnsettled_` stable / deadline) decides the TIMING — it already
holds the fire until the host re-stream AND the save-pile load tail have settled;
(b) the MIRROR-PRESENCE gate decides SAFETY — don't doom while host mirrors are
missing. The bug (CR-1/CR-2) is that (b)'s denominator was a FROZEN latch
(`g_pileExpectedMirrors`) minus a LEAKY global set. The fix: drive (b) off the
LIVE `g_hostPileCatalog` (the set of host pile eids the client currently believes
the host has — self-tracking: +CataloguePileMirror on spawn, -ForgetPileCatalogEntry
on every host destroy). Because the catalog grows WITH liveMirrors during a clean
stream (so `liveMirrors >= expected` holds and quiescence times the fire) but stays
STALE-HIGH across a same-world drain (the drain frees actors without OnDestroy, so
the catalog is NOT forgotten → `liveMirrors < catalog.size()` → defer until the
re-stream restores them), the catalog is the correct denominator for BOTH the
clean-single-load case AND the drain case. `g_pileResyncCompleteSeen` is still a
REQUIRED gate (distinguishes "catalog==0 because host has zero" from "catalog==0
because the stream has not arrived"); without it the connect cycle could fire
before any pile PropSpawn arrives = a wipe.

## CR-1 + CR-2 (UNIFIED) — `prop_adoption.cpp` TickPileReconcile gate + DoomNonMirrorChipPiles_
1. **New public API in remote_prop** to snapshot the mirror-actor set ONCE (H-2 too):
   - `remote_prop.h`: `void SnapshotReceivedMirrorActors(std::unordered_set<void*>& out);`
   - `remote_prop.cpp`: ONE `PropMirrors().Snapshot(snap)`; for each `p`, if
     `p->IsMirror() && p->GetActor() && R::IsLiveByIndex(p->GetActor(), p->GetInternalIdx())`
     → `out.insert(p->GetActor())`. (Liveness filter prevents a GC-recycled freed
     mirror pointer from masking a live non-mirror save pile.) Add `<unordered_set>`.
2. **DoomNonMirrorChipPiles_(const std::unordered_set<void*>& mirrorActors)** — take
   the set as a param; replace the per-pile `IsReceivedPropMirror(obj)` with
   `mirrorActors.count(obj)` (H-2: kills the per-pile mutex-locked Snapshot).
3. **TickPileReconcile gate** (replace the block from `if (!g_pileResyncCompleteSeen) return;`
   through the fire): 
   - keep `if (!g_pileResyncCompleteSeen) return;` (await this cycle's marker).
   - build the mirror set ONCE: `std::unordered_set<void*> mirrorActors;
     coop::remote_prop::SnapshotReceivedMirrorActors(mirrorActors);`
   - count `liveMirrors` via the GUObjectArray walk using `mirrorActors.count(obj)`
     (NOT IsReceivedPropMirror).
   - **HONEST denominator (CR-1/CR-2):** `int expected = 0; for (ElementId e :
     g_hostPileCatalog) if (!g_pendingRemoveEids.count(e)) ++expected;` — the live
     catalog minus this-cycle pending-removes. DELETE the
     `g_pileExpectedMirrors - g_pendingRemoveEids.size()` line.
   - `if (liveMirrors < expected) { defer-log; return; }` — the implicit `>=1` floor
     (expected>0 ⇒ needs liveMirrors≥expected≥1; expected==0 + completeSeen ⇒ host
     genuinely zero ⇒ fire ⇒ doom the client's stale save piles).
   - on fire: `g_pileDoomPending = false; g_pendingRemoveEids.clear();` (cycle
     converged — drop stale pending-removes, closes the #9 leak) then
     `DoomNonMirrorChipPiles_(mirrorActors);`.
4. **EnqueuePendingRemove pile-gate (CR-2/#16):** at the top add
   `if (!g_hostPileCatalog.count(eid)) return;` (only a KNOWN host pile eid may
   enter the pending set — auto-excludes clump / non-pile keyless eids that never
   drain). REQUIRES: in `remote_prop.cpp` OnDestroy, call `EnqueuePendingRemove(eid)`
   BEFORE `ForgetPileCatalogEntry(eid)` (else the catalog check fails). Order:
   resolve-no-actor → `EnqueuePendingRemove(eid)` (gated) → `ForgetPileCatalogEntry(eid)`.
5. **ArmPileDoomForConnect:** it may keep setting `g_pileExpectedMirrors =
   g_hostPileCatalog.size()` for the LOG line only, but that field is NO LONGER the
   gate denominator (the catalog is). Keep `g_pileResyncCompleteSeen = true` (the
   connect marker). Update the comment.
6. **OnPileResyncComplete:** `g_pileExpectedMirrors = count` becomes informational
   (log only); the gate reads the catalog. Keep setting `g_pileResyncCompleteSeen`.
7. **#3 cross-bracket reset:** in `BeginClaimTracking` add
   `g_pileResyncCompleteSeen = false;` (+ `g_pileExpectedMirrors = 0;`) so a 2nd
   connect bracket re-awaits this bracket's authoritative state.

## #12/#14 — `prop_adoption.cpp` CountLoadTailUnsettled_ chipPile short-circuit
A chipPile is always keyless/load-tail — short-circuit it BEFORE the
`ClassNameOf`/`GetInteractableKeyString` reads: add, right after the IsLive/CDO
checks in the keyless-prop branch, `if (ue_wrap::prop::IsChipPile(obj)) { ++unsettled; continue; }`
(the agent removed the old `if(IsChipPile)continue;`; this restores pile-inclusion
CHEAPLY — counts the pile as unsettled without the per-pile key ProcessEvent).

## H-1 + H-3 — `prop_snapshot.cpp` ReStreamPilesToSlot chunking + host debounce
Chunk the re-stream like `DrainChunk` (≤100 piles/tick): a per-slot pending
re-stream cursor (the live chipPile list captured once at request time + an index),
drained from the host's per-tick path (the same Tick that drives DrainChunk), and
send `PileResyncComplete{count}` ONLY after the last chunk drains (so the marker
still means "stream complete"). Host debounce (H-3): drop / coalesce a
PileResyncRequest from a slot already being re-streamed or serviced within a short
window (mirror TriggerForSlot busy/dedup). Add a freeze_probe Scope around the
per-tick re-stream drain and confirm no >threshold stall in the smoke (#13 open risk).
NOTE: this changes WHEN PileResyncComplete is sent (after the last chunk) — that is
COMPATIBLE with the gate (completeSeen still gates correctly; the catalog fills as
chunks arrive). event_dispatch_state.cpp PileResyncRequest handler calls the new
enqueue (not the synchronous ReStreamPilesToSlot) and does NOT send the marker
inline (the drain sends it).

## H-4 — interaction smoke (force a shadow-drain across a grab)
`autotest.cpp`: a new env-gated smoke that (1) joins a client, (2) forces a
same-world shadow-drain (2nd client reload / small travel), (3) issues a pile grab
during the drain window, and asserts the live markers: `client shadow-drain
complete -> PileResyncRequest sent + pile doom armed`, `re-streamed N live pile(s)
+ PileResyncComplete{N}`, `PILE DOOM FIRING ... liveMirrors=N expected=N`, and the
grab resolving. Add those markers to the `tools/mp.py` joinchurn verdict
(~1644-1681). This is the [[feedback-interaction-smoke-not-join-smoke]] gap.

## Cheap hardening (same pass)
- **#7** pin `PileResyncComplete` to `Lane::Bulk` (LaneForKind / session_lanes) —
  matches the SnapshotComplete precedent so the marker can't overtake its PropSpawn
  stream.
- **#17** drop the dead v84 `locX/locY/locZ` from `PropDestroyPayload` (protocol.h)
  + the writer at `pile_handle.cpp:199` (ConfirmGrabCandidate_ host branch) + the
  v84 comment; update the static_assert size. proto is already v88 (no interop), so
  the wire shrink is free.
- **#20** gate the small-travel `PileResyncRequest`/`ArmPileDoom` (net_pump.cpp:549
  branch) on the SAME-WORLD condition (`reapWorld == g_announcedWorld`) so a genuine
  UWorld swap relies on the fresh bracket's `ArmPileDoomForConnect` (no double-arm /
  double re-stream).
- **#5** make the pending-remove consume robust: it is correct today (piles only
  reach fresh-spawn), but add a one-line comment at the OnSpawn `existing`/fuzzy
  early-returns noting a chipPile cannot reach them with a pending-remove eid (or
  move the consume into the create primitive). Documentation-only.

## DO-NOT regress (the 8 landmines still hold)
Pile clock independent of g_sweepFired; no RunDivergenceSweep_ re-arm / g_claimedActors;
host re-stream bracket-free (chunking stays bracket-free — NO SnapshotBegin/Complete);
the doom never fires while liveMirrors < expected (now catalog-honest + the implicit
floor); npc_adoption.cpp UNTOUCHED, g_ghostSwept NOT cleared (P6 out of scope).

## After fixes: build Release clean, then the parent re-verifies the gate + deploys.
