# Thin-client pile sync — IMPLEMENTATION SPEC (gathered code facts) — 2026-06-20

> **`[corr 2026-09-04: docs census sweep]` DURABLE RECORD, SUPERSEDED IN OUTCOME.** Its measurements
> stand as of its date. `[V]` What it was written to inform has since been replaced: the pile mirror
> went PROXY -> NATIVE on 2026-06-30 (`docs/piles/11`, "nativization AS-BUILT + VERIFIED (hands-on)
> ... a rooted real `actorChipPile_C` native"), and the arc then ran on into crutch **C2** and the
> 2026-09-01 rejoin-crash fix. `[V]` Its file paths are pre-refactor -- `prop_adoption.cpp`,
> `pile_handle.cpp`, `pile_claim.cpp`, `pile_seed.cpp`, `prop_spawn_core.cpp` and `freeze_probe.cpp`
> are gone from the tree. Read it as evidence from June 2026, never as current design.

Companion to `research/findings/_archive/thin-client-pile-sync-redesign-2026-06-20.md` (the
ARCHITECTURE — read it first; §8 delete-list, §5 doom-gate, §6 world-wipe proof,
§10 file-by-file, §12 audit-focus are authoritative). This file pins the exact
code locations + wire values gathered from the live tree so the implementation is
unambiguous. Repo root: `d:/Projects/Programming/VOTV_MP`.

## ALREADY DONE — do NOT redo
- **P1 mint-gate** is APPLIED in `src/votv-coop/src/coop/prop_element_tracker.cpp`
  `MarkPropElement` (~line 329): `const bool isChipPile =
  ue_wrap::prop::IsChipPile(actor);` added beside `isKerfur`; the gate now reads
  `if ((isKerfur || isChipPile) && s != nullptr && s->role() ==
  coop::net::Role::Client) return;`. Verified; leave it.

## Wire facts (the 3-place ReliableKind wiring + proto bump)
- **Proto version:** `protocol.h:697` `inline constexpr uint16_t kProtocolVersion
  = 87;` → bump to **88** with a comment: "v88: thin-client pile sync — adds
  PileResyncRequest=80 (client→host) + PileResyncComplete=81 (host→client); a new
  drain-edge round-trip, so old↔new peers must not interop. WIRE: new
  PileResyncCompletePayload{uint32 pileCount}."
- **Enum** `ReliableKind` (`protocol.h`, ends at `PileThrowRequest = 79`, ~1745):
  add `PileResyncRequest = 80,` and `PileResyncComplete = 81,` with doc comments.
  80 and 81 are the next free values (76/77 = WorldActorSpawn/Destroy, 78/79 =
  Pile{Grab,Throw}Request).
- **Payload** (`protocol.h`, beside `PileGrabRequestPayload` ~2135): add
  `struct PileResyncCompletePayload { uint32_t pileCount; };` +
  `static_assert(sizeof(PileResyncCompletePayload) == 4, ...)`. PileResyncRequest
  has NO payload (sent `nullptr, 0`).
- **Family-router list** (`event_feed.cpp:451-454`): add
  `case net::ReliableKind::PileResyncRequest:` and
  `case net::ReliableKind::PileResyncComplete:` to the case list that falls into
  `HandleStateEvent(...)` (alongside PileGrabRequest/PileThrowRequest). This is the
  "silent third place" of `[[feedback-reliablekind-router-checklist]]`.
- **Handler switch** (`event_dispatch_state.cpp:713`, the PileGrabRequest case is
  the exact template):
  - `case PileResyncRequest:` — HOST-ONLY (drop on client with a WARN, like
    PileGrabRequest). No payload. Call the host re-stream (below) for
    `msg.senderPeerSlot`.
  - `case PileResyncComplete:` — CLIENT-ONLY (drop on host). memcpy
    `PileResyncCompletePayload`; call
    `coop::remote_prop_spawn::OnPileResyncComplete(p.pileCount)`.
- **Sending** uses the EXISTING `session.SendReliableToSlot(slot, kind, ptr, len)`
  (empty payload = `nullptr, 0`, e.g. net_pump.cpp:655 ClientWorldReady) and
  `s->SendReliable(kind, &p, sizeof(p))` (broadcast) — NO new Session methods.

## Host re-stream (P4-2) — lives in `prop_snapshot.cpp` (it needs the file-local
`BuildPropSpawnPayload_`, line 239; it is NOT exported)
- Add `int ReStreamPilesToSlot(coop::net::Session& s, int slot)` (declare in
  `prop_snapshot.h`): host-only; ONE GUObjectArray walk for live `IsChipPile(obj)
  && !Default__`; resolve `eid = prop_element_tracker::GetPropElementIdForActor(obj)`
  (host minted it in SeedWalk_); `BuildPropSpawnPayload_(obj, eid, -1, p)` then
  `s.SendReliableToSlot(slot, ReliableKind::PropSpawn, &p, sizeof(p))`; count + return.
  **CRITICAL: bracket-free — NO SnapshotBegin/Complete** (re-bracketing re-arms the
  client divergence sweep = the R1 churn regression; this is audit-focus §12.3).
  The event_dispatch PileResyncRequest handler calls this, then sends
  `PileResyncCompletePayload{count}` back via `SendReliableToSlot(senderSlot,
  PileResyncComplete, &cp, sizeof(cp))` (the explicit-complete marker §6 needs;
  send even when count==0 so the client distinguishes "host has zero" from "no reply").
  Mirror the existing per-slot send shape (prop_snapshot.cpp:494-499 DrainChunk).

## Pile-reconcile core (P2 catalog, P3 doom, P5 pending-remove, P4 clock) — in
`prop_adoption.cpp` (namespace `coop::remote_prop_spawn`; shares
`CountLoadTailUnsettled_` + the quiescence constants `kSweepScanIntervalMs` /
`kSweepQuiesceScans` / `kSweepDeadlineMs`). Declarations go in
`remote_prop_spawn.h` (replacing the deleted RebindPileSeeds/RecordMirrorPileSeed/
ForgetPileSeed decls).
- **Catalog:** `std::unordered_set<ElementId> g_hostPileCatalog;` (client-gated).
  `CataloguePileMirror(eid)` (insert; client + nonzero only),
  `ForgetPileCatalogEntry(eid)` (erase). Plain bookkeeping — NEVER a spawn source.
- **Pending-remove:** `std::unordered_set<ElementId> g_pendingRemoveEids;`.
  `EnqueuePendingRemove(eid)` (insert), `ConsumePendingRemove(eid)` → bool (erase +
  return whether present). Consult it on EVERY pile-create path (OnSpawn fresh-spawn,
  the re-stream OnSpawn, OnConvert pile spawn): if the just-created mirror's eid is
  pending-remove, immediately drop the mirror + DestroyLocalProp it (audit §12.5).
- **Dedicated pile clock (independent of g_sweepFired):**
  `bool g_pileDoomPending; steady_clock::time_point g_pileDoomArmedAt,
  g_pileDoomLastScan; int g_pileDoomLastUnsettled = -1; int g_pileDoomStableScans;
  int g_pileExpectedMirrors;`.
  - `ArmPileDoom()`: client-only; sets `g_pileDoomPending=true`, fresh
    `g_pileDoomArmedAt = now`, resets stableScans/lastUnsettled, leaves
    `g_pileExpectedMirrors` as set by OnPileResyncComplete (or the connect count).
    Idempotent / debounced (re-arm while pending just refreshes the window).
    MUST NOT touch `g_sweepFired` / `g_sweepPending` / `OnClientWorldReadyResetSweep`
    (audit §12.1).
  - `OnPileResyncComplete(uint32 count)`: client-only; `g_pileExpectedMirrors =
    count` (the host's this-cycle ground truth) and ensure the doom is armed.
  - `TickPileReconcile()`: zero-cost early-return when `!g_pileDoomPending`. While
    pending: throttle 5 Hz; deadline = `kSweepDeadlineMs`; quiescence via
    `CountLoadTailUnsettled_()` (now chipPile-inclusive — see below) stable ×
    `kSweepQuiesceScans`. When (quiesced OR (deadline AND no local grab/throw in
    flight)) AND the §5 gate holds → run `DoomNonMirrorChipPiles_()`, then clear
    `g_pileDoomPending`. If the gate's `liveMirrors < expectedMirrors` → DO NOT
    clear pending; retry next tick (abort this pass).
- **§5 doom gate (exact):** count `liveMirrors` = live chipPiles with
  `IsReceivedPropMirror == true`. Compute `expected = g_pileExpectedMirrors` minus
  any of this cycle's eids already pending-removed. Gate:
  `(a) quiesced-or-guarded-deadline AND (b) liveMirrors >= expected`. The
  zero-pile case: if `expected == 0` AND a PileResyncComplete WAS received this
  cycle (an explicit-complete marker, NOT inferred from silence), proceed (host
  genuinely has zero piles → doom the client's un-mirrored save piles). If no
  complete marker yet, defer (don't world-wipe on "stream not arrived"). Do NOT
  use the `>50%` valve for piles (it mis-aborts — doomed save piles ≈ 50% by design).
- **`DoomNonMirrorChipPiles_()`:** ONE GUObjectArray walk; destroy each
  `IsChipPile(obj) && IsLiveByIndex && !NameStartsWith("Default__") &&
  !IsReceivedPropMirror(obj) && !IsPendingGrabOrThrow(obj)` via the existing
  echo-suppressed teardown (`ClearAnyDriveFor` + `ReleaseMainPlayerGrabIfHolding`
  + `prop_lifecycle::DestroyLocalProp(deferred=false)`), then
  `ForceGarbageCollection`. `IsPendingGrabOrThrow(obj)` consults pile_handle's
  pending grab/throw (add a small `coop::pile_handle::IsPileInActiveRelay(void*)`
  that checks `g_grabCandidate` actor + `g_throwWatch` membership + the pending
  grab/throw clump/pile pointers — audit §12.8). Re-resolve the local player via
  `R::FindObjectByClass(P::name::MainPlayerClass)` like RunDivergenceSweep_'s tick.

## CountLoadTailUnsettled_ change (P4)
- `prop_adoption.cpp:729`: REMOVE the `if (ue_wrap::prop::IsChipPile(obj)) continue;`
  line so the load-tail quiescence count waits for the pile tail too. (Used by BOTH
  the keyed sweep clock and the new pile clock — both want piles settled now.)

## net_pump.cpp drain-edge change (P4-1, P4-4)
- The `InPurgeEpisode`-exit branch (~505-521) currently calls
  `RebindPileSeedsAfterWorldChange(true)`. Replace with: `if (client) {
  session.SendReliableToSlot(0, ReliableKind::PileResyncRequest, nullptr, 0);
  coop::remote_prop_spawn::ArmPileDoom(); }`. Same for the small-travel branch
  (~542). The host-side PileResyncRequest handler (event_dispatch) does the re-stream.
- Drive `coop::remote_prop_spawn::TickPileReconcile();` next to
  `coop::remote_prop_spawn::TickClientReconcile();` at `npc_mirror.cpp:641`.

## OnSpawn / OnDestroy / OnConvert / pile_handle (P2, P5, deletions)
- `remote_prop_spawn.cpp` OnSpawn: DELETE the `TryClaimKeylessPileAtBracket`
  call+return (~448-451); keyless piles always fresh-spawn. After
  `RegisterPropMirror` (~793): `CataloguePileMirror(payload.elementId)`; then if
  `ConsumePendingRemove(payload.elementId)` → drop+destroy this just-created mirror
  (a host PropDestroy landed while it was absent). DELETE the `RecordMirrorPileSeed`
  call (~799-800).
- `remote_prop.cpp` OnDestroy: DELETE the self-heal retry (894-901) and the
  POSITION-fallback block (912-943). When a keyless `PropDestroy(eid)` resolves NO
  live actor → `coop::remote_prop_spawn::EnqueuePendingRemove(payload.elementId)`.
  Replace the `ForgetPileSeed` call (~911) with `ForgetPileCatalogEntry`. Simplify
  `ResolveMirrorEidByActor` to single-pass (no peer-range pile locals exist post-P1,
  so the prefer-mirror two-pass is dead — but KEEP correctness for non-pile actors;
  if unsure leave it two-pass, it is not load-bearing). OnConvert: `ForgetPileCatalogEntry`
  where `ForgetPileSeed` was (~1004).
- `pile_handle.cpp` SetGrabCandidate client branch (228-248): DELETE the self-heal;
  reduce to `eid = coop::remote_prop::ResolveMirrorEidByActor(pileActor);` (kInvalidId
  → the existing no-op relay path is correct). Add `bool IsPileInActiveRelay(void*)`
  (+ header decl) for the doom collateral guard.

## RULE-2 deletions (§8 of the architecture doc — FULL removal, not rename)
In `prop_adoption.cpp`: `struct PileSeed` + `g_pileSeeds`; `struct PileBindCandidate`
+ `g_pileBindIndex` + `g_pileBindIndexBuilt` + `g_pileBindCount` + `ResetPileBindIndex`
+ `EnsurePileBindIndex` + `kSeedBindRadiusCm`; `g_joinedAsClient` (fold its client
gate into the catalog write-guard); `detail::TryClaimKeylessPileAtBracket`;
`FindCoLocatedSaveTwin_`; `BindPileSeeds_`; `RebindPileSeedsAfterWorldChange`;
`RecordMirrorPileSeed`; `ForgetPileSeed`; the `BindPileSeeds_()` call in
RunDivergenceSweep_ (516); the in-`propPairs` chipPile doom branch (582-586). Clear
`g_hostPileCatalog` + `g_pendingRemoveEids` + the pile clock in `ResetClaimTracking`
+ `BeginClaimTracking` + disconnect. Remove the now-unused `g_pileSeeds.clear()`
references in BeginClaimTracking/ResetClaimTracking.
In `remote_prop_spawn_internal.h`: remove `detail::TryClaimKeylessPileAtBracket`
(likely empties the header — if so delete the file + its includes).
In `remote_prop_spawn.h`: remove decls of RebindPileSeedsAfterWorldChange,
RecordMirrorPileSeed, ForgetPileSeed; add decls for CataloguePileMirror,
ForgetPileCatalogEntry, EnqueuePendingRemove, ConsumePendingRemove, ArmPileDoom,
OnPileResyncComplete, TickPileReconcile.

## Build order (each step compiles)
1. P1 mint-gate — DONE.
2. Wire: protocol.h enum/payload/version + event_feed family list + event_dispatch
   handlers (stub the host re-stream + OnPileResyncComplete first so it links).
3. Host re-stream `ReStreamPilesToSlot` (prop_snapshot).
4. Pile-reconcile core (catalog + pending-remove + clock + doom + gate) in
   prop_adoption; CountLoadTailUnsettled_ chipPile-inclusive; wire TickPileReconcile.
5. OnSpawn/OnDestroy/OnConvert/pile_handle edits (P2/P5).
6. net_pump drain-edge edit (P4-1/P4-4).
7. RULE-2 deletions LAST (so the tree compiles at each prior step).

## DO-NOT (the safety landmines — also the audit-focus list)
- Do NOT reset/read `g_sweepFired` / call `OnClientWorldReadyResetSweep` from the
  pile clock (cross-subsystem kerfur re-dupe).
- Do NOT re-arm `RunDivergenceSweep_` / touch `g_claimedActors` from the pile path
  (empty-claim keyed-prop wipe).
- Host re-stream is bracket-free (no SnapshotBegin/Complete) — no sweep re-arm.
- Consume `g_pendingRemoveEids` on EVERY pile-create path.
- The doom must NOT fire while `liveMirrors < expectedMirrors` (world-wipe guard);
  the zero-pile branch needs the explicit PileResyncComplete marker, never silence.
- The doom GUObjectArray walk + ~870 SpawnLocalTrashActor per resync are COLD
  (drain edges only) — TickPileReconcile early-returns at zero cost when not pending.
- P6 (coupled kerfur drain-bridge) is OUT OF SCOPE this pass; do NOT clear
  `g_ghostSwept` (would world-wipe NPCs). Leave npc_adoption untouched.
