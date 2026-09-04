# Thin-client ambient-pile sync — definitive redesign (2026-06-20)

> **`[corr 2026-09-04: docs census sweep]` DURABLE RECORD, SUPERSEDED IN OUTCOME.** Its measurements
> stand as of its date. `[V]` What it was written to inform has since been replaced: the pile mirror
> went PROXY -> NATIVE on 2026-06-30 (`docs/piles/11`, "nativization AS-BUILT + VERIFIED (hands-on)
> ... a rooted real `actorChipPile_C` native"), and the arc then ran on into crutch **C2** and the
> 2026-09-01 rejoin-crash fix. `[V]` Its file paths are pre-refactor -- `prop_adoption.cpp`,
> `pile_handle.cpp`, `pile_claim.cpp`, `pile_seed.cpp`, `prop_spawn_core.cpp` and `freeze_probe.cpp`
> are gone from the tree. Read it as evidence from June 2026, never as current design.

Lead-architect reconciliation of the 5 candidate designs, 3 judge panels, and 2
adversary reports, verified line-by-line against the code under
`d:/Projects/Programming/VOTV_MP/src/votv-coop/`.

---

## 0. TL;DR

Thin-client (Family A) is the correct root fix and is kept. But **every one of
the 5 designs is unsafe as written**, and the two adversary reports each break
their target. The decisive, code-verified findings that force the synthesis:

- **`HasLoadTailQuiesced()` IS `g_sweepFired`** (prop_adoption.cpp:833) — one
  sticky bit, shared by the pile sweep **and** npc_adoption's fresh-spawn gate
  (npc_adoption.cpp:146), kerfur_convert.cpp:590, trash_collect_sync.cpp:230.
  Resetting it mid-session to "re-arm the pile doom" (Designs 1 & 4) re-opens
  NPC/kerfur fresh-spawn mid-drain = a kerfur re-dupe. **Reusing `g_sweepFired`
  for a re-armable pile doom is a cross-subsystem landmine.**
- **`RunDivergenceSweep_` hard-returns on `!g_claimTrackingActive`** (line 507)
  and clears that flag at line 676. The shadow-drain has **no** claim bracket
  (maybeReAnnounce suppresses re-announce on a same-world drain,
  net_pump.cpp:478-487). So re-arming the *full* sweep on the drain edge
  (Designs 1/2/4) either no-ops or runs the keyed-prop doom against an empty
  claim set = a keyed-prop world-wipe.
- **Deleting the OnDestroy position-fallback (remote_prop.cpp:912-943)** — which
  every design does — makes a host grab that lands *during* the drain window a
  silent no-op (the mirror is freed, the fallback is gone) = the exact
  "host-grab-doesn't-remove-client-pile" bug regresses. (Adversary attack,
  confirmed.)
- **Client re-spawning a pile mirror from a persisted seed** (Designs 1-5'
  `RespawnHostPileMirrors_`/catalog respawn) is the client **predicting host
  state** from a cached record — an MTA violation, and a stale-position ghost
  dupe if the host moved/grabbed that pile during the drain. (Adversary attack,
  confirmed: it re-introduces the same rot under a new name.)

The chosen approach therefore keeps the strongest *core* (thin-client identity,
the mint-gate extension, the standalone catalog-driven doom of Designs 3/5) but
replaces the two broken bridges with **host-authoritative re-materialization**:
on the same-world shadow-drain, the **client requests a pile re-stream and the
HOST re-sends `PropSpawn` per live pile** (the true MTA EntityAdd-on-rescope
shape). The client never re-spawns from a cached seed; the doom is a dedicated,
quiescence-gated, re-runnable pile-only path that does **not** touch
`g_sweepFired`. A durable **pending-remove-by-id set** closes the host-grab
in-drain window MTA-faithfully (remove-by-id queued until the entity exists),
replacing the deleted position-fallback.

---

## 1. Chosen approach + why

**Approach: Thin-client piles with a HOST-RE-STREAM drain bridge + a dedicated,
self-clocked, re-runnable pile doom + a pending-remove-by-id queue.**

Graft map (what comes from where):

| Element | Source design | Why |
|---|---|---|
| Thin-client identity; client never mints/binds a save pile; host eid never position | All 5 (consensus) | The proven MTA + kerfur K-5 root fix |
| Mint-gate extension to `IsChipPile` | All 5 (consensus) | Kills the peer-range shadow Element at the source (the v86 root) |
| Standalone pile-only doom (NOT a re-arm of `RunDivergenceSweep_`) | Designs 3 & 5 | Avoids the empty-claim keyed-wipe + the RULE-2 mode-flag |
| `reArmsDivergenceSweep = false` | Designs 3 & 5 | The full keyed sweep stays one-shot-per-world; piles get their own clock |
| **Dedicated pile quiescence clock (own armedAt/stableScans, NOT `g_sweepFired`)** | **New (forced by the shared-bit finding)** | Re-runnable per drain without re-opening NPC/kerfur fresh-spawn |
| **HOST re-stream on the drain edge (PileResyncRequest → host PropSpawn per live pile)** | **New (forced by the adversary's "client predicts host state" attack)** | True MTA EntityAdd-on-rescope; the client never predicts a mirror |
| **Pending-remove-by-id queue replacing the position-fallback** | **New (forced by the host-grab-in-drain attack)** | MTA Packet_EntityRemove queued until the entity exists; no position identity |
| Pile-inclusive quiescence | All 5 (consensus) | Closes the 870→0→870 churn-window mis-fire |
| Coupled kerfur g_ghostSwept drain re-arm | Designs 2 & 4 (correct) — **with the catalog-respawn-before-sweep ordering the adversary demands** | The symmetric NPC dupe is real and must not ship a half-fix |

**Why thin-client (not host-re-stream-only-no-doom, not robust-bind):** the
client loads the *same save* → it independently creates ~870 chipPile actors
that the host's stream knows nothing about. No keyless-pile identity survives a
cross-process reload, so any bind rots (proven across sessions 22-30). The only
crutch-free shape is: the client owns *zero* save piles; every pile it shows is
a host mirror (host eid identity); a reconcile dooms the save originals. That is
the MTA invariant. The doom is the one non-MTA-literal piece, and it exists
solely to bootstrap VOTV's "both peers load the save" reality into the MTA
invariant; once converged it is idempotent.

**Why host re-stream on the drain (the key divergence from Designs 3/5):** the
adversary proved that a client re-spawning a mirror from a persisted seed is
prediction — if the host grabbed/moved that pile during the ~50s drain, the
client re-materializes a ghost at a stale position that the doom then *spares*
(it is a registered mirror) = a permanent un-killable dupe. The MTA-correct
answer is: the client lost its runtime mirrors, so it asks the host to re-send
them, and the host sends only the piles it *actually still has*. Identity stays
the host eid; nothing is predicted.

---

## 2. The invariant

> **On the client, after load-tail quiescence, EXACTLY ONE live
> `actorChipPile_C` actor exists per host pile eid — and it is a host mirror
> (`IsReceivedPropMirror == true`, host-range eid via `RegisterPropMirror`). NO
> live chip-pile actor is un-mirrored. The client never mints a local Prop
> Element for a chipPile, never binds its save pile to a host eid, and never
> instantiates a pile mirror except from a host `PropSpawn`. Identity is the
> host eid, always; position is never runtime identity.**

Hold this and:
- a host grab → `PropDestroy(eid)` resolves the one mirror directly (no
  co-located save twin survives → no host-grab dupe);
- a client grab → `ResolveMirrorEidByActor` returns the host-range eid (the
  *only* eid the actor has) → the relay always has a valid host-range id;
- the client never accumulates its save piles (the doom destroys them, and
  re-runs after every drain that re-creates them).

---

## 3. Mechanism steps

**(P1) Mint-gate — the client never owns a save pile.**
`prop_element_tracker.cpp` MarkPropElement (~329-336): add
`const bool isChipPile = ue_wrap::prop::IsChipPile(actor);` beside the existing
`isKerfur` (read outside the lock, reflection-only) and change the gate to
`if ((isKerfur || isChipPile) && s && s->role() == Role::Client) return;`. The
host path is unchanged (host still mints keyless pile eids in `SeedWalk_`:588
for its snapshot). This kills the peer-range LOCAL shadow Element the 0.25 Hz
re-seed stamps on every save pile — the v86 root that defeated
`ResolveMirrorEidByActor`.

**(P2) Receive — the client fresh-spawns host mirrors, only from a host
PropSpawn.** `remote_prop_spawn.cpp` OnSpawn (~448-451): delete the
`TryClaimKeylessPileAtBracket` call + early return; a keyless `eidOnly` pile
always falls through to the existing fresh-spawn path
(`SpawnLocalTrashActor` + `RegisterPropMirror`, ~793). Record the pile into a
**plain bookkeeping catalog** `g_hostPileCatalog` keyed by host eid (eid only —
NOT a re-spawn source; see P4). This replaces `RecordMirrorPileSeed`.

**(P3) The doom — destroy every save original; spare host mirrors.** A
dedicated `DoomNonMirrorChipPiles_()`: one GUObjectArray walk collecting
`IsChipPile(obj) && IsLiveByIndex && !NameStartsWith("Default__") &&
!IsReceivedPropMirror(obj) && !IsPendingGrabOrThrow(obj)` → destroy with the
existing echo-suppressed teardown (`ClearAnyDriveFor` +
`ReleaseMainPlayerGrabIfHolding` + `DestroyLocalProp(deferred=false)`),
followed by `ForceGarbageCollection`. Because P1 stops the client
MarkPropElement-ing save piles, they are NOT in `propPairs`, so the existing
in-`propPairs` chipPile doom (prop_adoption.cpp:582-586) is **structurally
dead** and is deleted (RULE 2). The `>50%` valve (629-638) is **kept for the
keyed-prop sweep** and **not used by the pile doom** (the pile doom uses the
mirror-presence gate, §5).

**(P4) The drain bridge — host re-streams, client never predicts.** On the
`InPurgeEpisode`-exit edge (net_pump.cpp:505-521 — the only same-world bridge),
replace the `RebindPileSeedsAfterWorldChange(true)` call (and the small-travel
one at :542) with:
1. CLIENT sends a new reliable **`PileResyncRequest`** (the client lost its
   runtime mirrors; ask the host to re-send its live pile set).
2. HOST, on receiving it, walks its tracked chipPile Prop Elements and re-sends
   one `PropSpawn` per live pile to that slot (the existing per-pile
   `BuildPropSpawnPayload_`/wire path; bracket-free, MTA EntityAdd shape — never
   a re-bracket, so the keyed divergence sweep is **not** re-armed).
3. CLIENT receives them through the normal OnSpawn fresh-spawn path (P2) →
   mirrors re-materialize on their host eids from the host's authority.
4. CLIENT **arms the dedicated pile doom** (`ArmPileDoom()`, §4) so the doom
   re-runs once the re-created save piles + re-streamed mirrors have quiesced.

**(P5) Pending-remove-by-id — close the host-grab-in-drain window.**
`remote_prop.cpp` OnDestroy: if a keyless `PropDestroy(eid)` resolves no live
actor, enqueue the eid into `g_pendingRemoveEids` (instead of the deleted
position-fallback). The pile doom and the OnSpawn fresh-spawn path consult it:
a re-streamed/re-created pile whose eid is in the set is immediately destroyed /
its mirror dropped. This is MTA Packet_EntityRemove queued until the entity
exists — by id, never position.

**(P6) Coupled kerfur fix.** Apply the same drain bridge to NPCs: on the
`InPurgeEpisode`-exit edge, the client sends an NPC resync request (or reuse the
existing NPC snapshot path), the host re-sends its NPC set, and only THEN clear
`g_ghostSwept` + re-arm the NPC ghost sweep. Shipping the bare `g_ghostSwept`
clear without the host NPC re-send is the symmetric NPC world-wipe the adversary
flagged — do not ship it half.

---

## 4. Shadow-drain handling (the hard problem)

**Verified facts (cited):**
- The reaper is 4 s-throttled, caps eviction at 256/scan
  (net_pump.cpp:336-340). A full drain of ~3300 dead elements takes ~13 scans ≈
  **~52 s**, during which `InPurgeEpisode()` stays TRUE (entered net_pump.cpp:
  499-504 on `reaped>=64 && keyedReaped>0`; exited only at 505-521 when the
  reaper catches up).
- `maybeReAnnounce` (net_pump.cpp:478-487) suppresses re-announce on a
  same-world drain (`reapWorld == g_announcedWorld`) → `OnClientWorldReady` /
  `OnClientWorldReadyResetSweep` never fire → no fresh snapshot bracket.
- The host's steady re-seed sends only NEWLY-tracked props
  (net_pump.cpp:574-581) — existing piles are never re-sent **automatically**.
- `RebindPileSeedsAfterWorldChange` (net_pump.cpp:521) is today the only
  same-world bridge — and it is a position-rebind crutch.

**Why the timing matters (the judge-decisive point):** the
`InPurgeEpisode`-exit edge fires on **deletion-drain-complete**, not on
**save-pile re-creation-settled** (the async `loadObjects` over ~12-15 s). The
two timelines are decoupled. A *synchronous* doom at that single edge (the
`reArms=false` synchronous variant of Designs 3/5) can fire before the engine
finishes re-creating the ~870 save piles → late arrivals are un-doomed = the
re-dupe. So the doom **must** be clocked on pile-inclusive quiescence measured
*from the drain instant*, not run synchronously at the edge.

**Our handling:**
1. At the `InPurgeEpisode`-exit edge: CLIENT sends `PileResyncRequest` (P4-1)
   and calls `ArmPileDoom()` (below). Nothing destructive happens at the edge
   itself.
2. HOST re-streams its live pile set → mirrors re-materialize via OnSpawn (P4-2,
   P4-3). This runs concurrently with the engine re-creating the save piles.
3. `TickPileReconcile()` (driven from the same client tick that drives
   `TickClientReconcile`) waits on a **dedicated** pile quiescence clock:
   - its own `g_pileDoomArmedAt`, `g_pileDoomStableScans`,
     `g_pileDoomLastUnsettled` — **independent of `g_sweepFired`**;
   - `CountLoadTailUnsettled_` made chipPile-inclusive (remove the
     `if (IsChipPile) continue;` at prop_adoption.cpp:729) so the clock waits
     for the pile load tail to settle;
   - same 5 Hz / `kSweepQuiesceScans` / 45 s-deadline cadence as the keyed
     sweep, but a separate set of counters.
4. When the pile clock quiesces (or the deadline hits AND no grab/throw is in
   flight, §6 attack-4 mitigation), `DoomNonMirrorChipPiles_()` runs under the
   mirror-presence gate (§5). The re-streamed mirrors are present (host
   authority), the re-created save originals are doomed.

**Multi-drain / re-entrant join (2-3 reloads):** `ArmPileDoom()` is idempotent
— a new `InPurgeEpisode`-exit while a pile doom is still pending just refreshes
the clock window (debounce), and re-sends the `PileResyncRequest`. The catalog
is plain eid bookkeeping, never a spawn source, so a second drain freeing the
re-streamed mirrors simply triggers another host re-stream. Coalesced: the
respawn+doom converges once after the LAST drain settles.

---

## 5. The doom gate (exact)

The `>50%` valve is **not used** for the pile doom (it mis-aborts: client save
piles ≈ host mirror count ≈ ~50% of the pile universe BY DESIGN —
prop_adoption.cpp:361-362 confirms "the save pile IS the entity"). The pile doom
uses a **mirror-presence gate against the host's authoritative re-streamed
set**:

```
DOOM iff:
  (a) the dedicated pile quiescence clock has quiesced
      (chipPile-inclusive CountLoadTailUnsettled_ stable x kSweepQuiesceScans),
      OR the 45s deadline hit AND no local grab/throw is in flight; AND
  (b) liveMirrors >= 1                      // a positive floor: the host stream materialized at all
      AND liveMirrors >= expectedMirrors    // every pile the host RE-STREAMED this cycle is present
```

`expectedMirrors` = the count of `PropSpawn`s the host sent in response to *this
cycle's* `PileResyncRequest` (a wire-counted value, carried in the resync reply
header or counted as PropSpawns arrive), **minus** any eid the host has since
`PropDestroy`'d (pending-remove set). It is the host's ground truth for *this
cycle*, not a stale catalog ratio — so it cannot starve from catalog rot (the
adversary's attack-1 starvation). If a mirror failed/raced to spawn,
`liveMirrors < expectedMirrors` → **abort this pass, retry next tick**. The doom
never destroys a save original while the host's pile set is not fully present as
mirrors.

For the connect-time first doom (no resync cycle yet), `expectedMirrors` = the
count of pile PropSpawns delivered in the connect bracket; the same equality
holds, layered under the keyed sweep's existing quiescence wait.

---

## 6. World-wipe safety proof (step by step)

The 2979/3229 incident ([[feedback-join-reconcile-sweep-safety]]) was a doom
firing against a majority-unclaimed, still-loading world. Each guard below is
independent; the wipe needs ALL to fail simultaneously, which the construction
prevents:

1. **No synchronous edge doom.** The pile doom never runs at the
   `InPurgeEpisode`-exit edge directly. It is clocked on a *fresh*
   pile-inclusive quiescence measured from the drain instant — so it cannot fire
   during the 870→0→870 transient (the exact mis-fire that bound 0/870 in
   session 30). The drain-edge work is only "ask the host to re-stream + arm the
   clock".
2. **Mirrors before doom, by host authority.** The mirrors are re-materialized
   from the HOST's re-stream (P4), not predicted from a cached seed. By the time
   the clock quiesces, every pile the host still has is present as a mirror
   (`liveMirrors >= expectedMirrors`, §5b). Destroying the co-located save
   originals leaves the mirrors — the world converges to host, never empties.
3. **The equality gate aborts on incompleteness.** If the host re-stream raced
   or a spawn was refused mid-transition, `liveMirrors < expectedMirrors` →
   abort + retry. A partial mirror set can never trigger a mass kill (a few
   stale dupes beat an empty world — the documented trade at
   prop_adoption.cpp:625-628, now keyed to the RIGHT denominator).
4. **No `g_sweepFired` reuse.** The pile clock is independent, so arming it never
   resets the keyed sweep / NPC fresh-spawn gate → no cross-subsystem mis-fire,
   no keyed-prop wipe.
5. **No empty-claim keyed wipe.** `RunDivergenceSweep_` is NOT re-armed on the
   drain edge (it stays one-shot, claim-bracket-scoped). The pile doom is a
   separate function that never touches `g_claimedActors`, so it cannot doom
   keyed Prop Elements against an empty claim set.
6. **Deadline guarded against live interaction.** The 45 s deadline path fires
   only if NO local grab/throw is in flight (`pile_handle` `g_grabCandidate` /
   `g_throwWatch`); during active mashing it keeps deferring — a few dupe ghosts
   beat vanishing the player's thrown pile (the "appears-then-disappears" the
   user rejected).

**Pathological case:** host genuinely has zero piles (or the resync reply is
empty) → `expectedMirrors == 0`, `liveMirrors == 0`. Then the floor `liveMirrors
>= 1` fails AND the equality `0 >= 0` holds. Resolve by: if the host's resync
reply explicitly says "0 piles" (a delivered-and-complete signal, not absence),
the doom proceeds and destroys the client's un-mirrored save piles (correct —
the host has none). The floor is only a guard against "stream not yet arrived";
the explicit-complete signal distinguishes "host has zero" from "host hasn't
answered". **This is why the resync reply must carry an explicit count/complete
marker, not be inferred from silence.**

---

## 7. Dupe safety proof

- **Client-local dupe (re-accumulate save piles):** the doom is re-runnable on
  every `InPurgeEpisode`-exit (P4-4) clocked on quiescence (§4) — every drain
  that re-creates the save piles re-arms + re-runs the doom after they settle.
  Not a one-shot latch.
- **Host-grab dupe (host grabs, client pile stays):** post-doom there is exactly
  ONE actor (the host mirror) per host eid; `PropDestroy(eid)` resolves it
  directly. A grab landing *during* the drain window (mirror freed) is caught by
  the pending-remove-by-id queue (P5) — the eid is destroyed the moment the
  re-streamed pile arrives. No co-located save twin survives.
- **Stale-position ghost dupe (the adversary's prediction attack):** impossible
  — the client never re-spawns from a cached seed. Mirrors come only from a host
  `PropSpawn`, so a pile the host grabbed during the drain is simply not in the
  re-stream → no ghost is created.
- **Peer-range shadow eid (the v86 root):** impossible — P1 stops the client
  minting any local eid for a chipPile, so `IsReceivedPropMirror` is an
  unambiguous keep/doom discriminator and `ResolveMirrorEidByActor` returns the
  host-range eid for the only Element the actor has.

---

## 8. Delete list (RULE 2 — full deletion, not rename)

In `prop_adoption.cpp` (despite the file, the `remote_prop_spawn` namespace):
- `struct PileSeed` + `g_pileSeeds` (88-108) — the save-twin/respawn seed.
- `struct PileBindCandidate` + `g_pileBindIndex` + `g_pileBindIndexBuilt` +
  `g_pileBindCount` + `ResetPileBindIndex` + `EnsurePileBindIndex` (163-208) —
  the position-bind index.
- `g_joinedAsClient` (113) — fold its client-gate into the catalog write-guard.
- `detail::TryClaimKeylessPileAtBracket` (223-295) — the save-twin claim.
- `FindCoLocatedSaveTwin_` (336-356) — position re-resolution.
- `BindPileSeeds_` (368-451) — the whole adopt/respawn/dedup bind engine.
- `RebindPileSeedsAfterWorldChange` (457-466) — the rotting position-rebind
  bridge.
- `RecordMirrorPileSeed` (468-490) — the re-spawnable seed writer (replaced by
  the plain eid catalog).
- `ForgetPileSeed` (492-504) — replaced by `ForgetPileCatalogEntry` +
  pending-remove handling.
- the in-`propPairs` chipPile doom branch (582-586) — structurally dead after
  P1.

In `remote_prop_spawn.cpp`:
- the `TryClaimKeylessPileAtBracket` call + early return in OnSpawn (448-451).
- the `RecordMirrorPileSeed` call (799-800) — replaced by `CataloguePileMirror`.

In `remote_prop.cpp`:
- OnDestroy self-heal `RebindPileSeedsAfterWorldChange` retry (894-901).
- OnDestroy **POSITION-fallback** block (912-943) — replaced by the
  pending-remove-by-id queue (P5). (Resolves the RULE-2 dual-identity-path the
  adversary flagged: an eid path AND a position path cannot coexist under
  "identity by ID".)

In `pile_handle.cpp`:
- SetGrabCandidate client self-heal `RebindPileSeedsAfterWorldChange` retry
  (230-247) — the host-range mirror eid is stable post-fix; reduce to a plain
  `ResolveMirrorEidByActor`.

In `net_pump.cpp`:
- both `RebindPileSeedsAfterWorldChange` calls (521, 542) — replaced by the
  `PileResyncRequest` + `ArmPileDoom()` drain bridge.

In the headers (`remote_prop_spawn.h`, `remote_prop_spawn_internal.h`):
- declarations of `RebindPileSeedsAfterWorldChange`, `RecordMirrorPileSeed`,
  `ForgetPileSeed`, `TryClaimKeylessPileAtBracket` (internal header likely
  empties — audit + remove).

---

## 9. Keep list

- `RegisterPropMirror` / `ResolveMirrorEidByActor` / `IsReceivedPropMirror`
  (remote_prop.cpp) — the host-eid identity layer; `IsReceivedPropMirror` is now
  the SOLE keep/doom discriminator. `ResolveMirrorEidByActor` can be simplified
  to single-pass once peer-range pile locals can no longer exist (P1).
- `SpawnLocalTrashActor` (remote_prop_spawn.cpp) — the fresh-spawn primitive for
  host mirrors.
- The pile_handle relay: confirm-by-clump grab signal, throw death-watch,
  `RelayClientGrab`/`OnPileGrabRequest`/`OnPileThrowRequest`/
  `TryAdoptOrDeferConvert` — real un-hookable BP-morph bridges, NOT rot. Keep
  (minus the self-heal).
- The client-never-authors-shared-trash invariant (prop_lifecycle.cpp:205,
  trash_collect_sync.cpp:186) — complements P1.
- The keyed divergence sweep machinery: `RunDivergenceSweep_`,
  `ArmDivergenceSweep`, `TickClientReconcile`, the `>50%` valve (FOR KEYED PROPS
  ONLY), `OnClientWorldReadyResetSweep`, `g_sweepFired`/`HasLoadTailQuiesced` as
  the one-shot-per-world NPC/kerfur signal — UNCHANGED. The pile doom does NOT
  reuse them.
- `CountLoadTailUnsettled_` — KEPT, with the `if (IsChipPile) continue;` at line
  729 REMOVED so the pile clock waits for the pile load tail.
- `SeedWalk_` host-side keyless chipPile minting (prop_element_tracker.cpp:588)
  — the host still mints pile eids for its snapshot.
- The net_pump reaper, `InPurgeEpisode` detection + `keyedReaped>0`
  co-condition, host death-watch `PropDestroy(eid)` (439-454) — the drain
  detector + per-entity remove. UNCHANGED.

---

## 10. File-by-file edits

### `src/votv-coop/src/coop/prop_element_tracker.cpp`
MarkPropElement (~329-336): add `const bool isChipPile =
ue_wrap::prop::IsChipPile(actor);` beside `isKerfur` (outside the lock); change
the gate to `if ((isKerfur || isChipPile) && s && s->role() ==
coop::net::Role::Client) return;`. Host path unchanged.

### `src/votv-coop/src/coop/remote_prop_spawn.cpp`
OnSpawn (448-451): delete the `TryClaimKeylessPileAtBracket` call+return →
keyless piles always fresh-spawn. (799-800): replace `RecordMirrorPileSeed` with
`CataloguePileMirror(eid)` (plain eid bookkeeping, client-gated). In the OnSpawn
fresh-spawn path, after `RegisterPropMirror`: if `eid ∈ g_pendingRemoveEids`,
immediately drop the mirror + destroy the actor (P5 consume).

### `src/votv-coop/src/coop/prop_adoption.cpp`
Delete the save-twin machinery (§8). Add: `g_hostPileCatalog`
(`std::unordered_set<eid>`, client-gated) + `CataloguePileMirror` +
`ForgetPileCatalogEntry`; `g_pendingRemoveEids` (set) + `EnqueuePendingRemove` /
`ConsumePendingRemove`; the dedicated pile clock state (`g_pileDoomPending`,
`g_pileDoomArmedAt`, `g_pileDoomStableScans`, `g_pileDoomLastUnsettled`,
`g_pileExpectedMirrors`); `ArmPileDoom()` (sets pending + fresh clock; does NOT
touch `g_sweepFired`); `TickPileReconcile()` (the 5 Hz pile clock → calls
`DoomNonMirrorChipPiles_` under the §5 gate); `DoomNonMirrorChipPiles_()` (the
GUObjectArray walk + gate + echo-suppressed teardown + GC). In
`CountLoadTailUnsettled_`: remove `if (IsChipPile(obj)) continue;` (729). In
`RunDivergenceSweep_`: remove the `BindPileSeeds_()` call (516) and the
in-`propPairs` chipPile doom (582-586); the `>50%` valve stays for keyed props.
Clear the catalog + pending-remove + pile clock in `ResetClaimTracking` /
disconnect.

### `src/votv-coop/src/coop/net_pump.cpp`
`InPurgeEpisode`-exit edge (521) + small-travel (542): replace
`RebindPileSeedsAfterWorldChange(true)` with `if (client) {
session.SendPileResyncRequest(); coop::remote_prop_spawn::ArmPileDoom(); }`.
Add the host-side `PileResyncRequest` handler: walk tracked chipPile Prop
Elements, send one `PropSpawn` per live pile to the requesting slot, then send a
`PileResyncComplete{count}` marker. Drive `TickPileReconcile()` from the existing
client tick (alongside `TickClientReconcile`, e.g. via npc_mirror.cpp:641's
reconcile pump). Reaper / death-watch / `maybeReAnnounce` unchanged.

### `src/votv-coop/src/coop/remote_prop.cpp`
OnDestroy: delete the self-heal retry (894-901) and the POSITION-fallback
(912-943). If a keyless `PropDestroy(eid)` resolves no live actor, call
`EnqueuePendingRemove(eid)` (P5). Keep the `ForgetPileCatalogEntry(eid)` call
where `ForgetPileSeed` was (911). Simplify `ResolveMirrorEidByActor` to
single-pass. OnConvert: `ForgetPileCatalogEntry` where `ForgetPileSeed` was
(1004).

### `src/votv-coop/src/coop/pile_handle.cpp`
SetGrabCandidate client branch (228-248): delete the self-heal; reduce to
`eid = coop::remote_prop::ResolveMirrorEidByActor(pileActor);`. If `kInvalidId`
(a genuinely un-streamed pile mid-drain), the existing no-op relay is correct.

### `src/votv-coop/src/coop/npc_adoption.cpp` (coupled kerfur fix, P6)
Add an NPC drain bridge: on the `InPurgeEpisode`-exit edge (called from
net_pump), the client requests an NPC resync; the host re-sends its NPC set;
only THEN clear `g_ghostSwept` + `g_snapshotDelivered` and re-arm. Do NOT clear
`g_ghostSwept` without the host NPC re-send (symmetric world-wipe).

### `src/votv-coop/include/coop/net/protocol.h`
Add `PileResyncRequest` (client→host, empty payload) + `PileResyncComplete`
(host→client, `uint32 count`) reliable kinds; bump proto version. (RULE from
[[feedback-reliablekind-router-checklist]]: wire each new ReliableKind in THREE
places — enum/payload, the family dispatcher case, and the event_feed.cpp
master-router list. Add an interaction smoke that exercises the resync wire.)

### `src/votv-coop/include/coop/remote_prop_spawn.h` / `..._internal.h`
Remove the deleted declarations; add `CataloguePileMirror`,
`ForgetPileCatalogEntry`, `ArmPileDoom`, `TickPileReconcile`,
`EnqueuePendingRemove`.

---

## 11. Build-order note

Land as one audited pass (the user's directive: no more band-aids, commit the
whole stack after), but build in this internal order so each step compiles and
is individually reasoned:

1. **P1 mint-gate** (prop_element_tracker) — smallest, kills the shadow eid at
   the source; everything else assumes it.
2. **P5 pending-remove queue + P3 standalone doom** (prop_adoption,
   remote_prop) — the doom + remove-by-id, independent of the wire.
3. **P2 catalog + OnSpawn fresh-spawn** (remote_prop_spawn) — receive path.
4. **Wire: `PileResyncRequest`/`Complete`** (protocol, net_pump host handler,
   event_feed router) — the three-place ReliableKind wiring.
5. **P4 drain bridge + pile clock** (net_pump edge, `ArmPileDoom`/
   `TickPileReconcile`) — ties P2+P3+wire to the drain edge.
6. **Delete the save-twin machinery** (§8) — RULE-2 removal, last so the tree
   compiles at each prior step.
7. **P6 coupled kerfur drain bridge** (npc_adoption) — the symmetric fix.

Then `wc -l` every touched file (§12 modularity); prop_adoption.cpp +
remote_prop_spawn.cpp net-shrink from the deletions. If the new pile-clock /
catalog / doom code pushes prop_adoption past the 800 soft cap, extract
`coop/pile_reconcile.cpp` in a SEPARATE prior commit.

---

## 12. Audit focus list (for the post-build audit agents)

1. **`g_sweepFired` isolation:** prove `ArmPileDoom` / `TickPileReconcile` never
   read or write `g_sweepFired` / call `OnClientWorldReadyResetSweep` — the pile
   clock must be fully independent of the NPC/kerfur quiescence bit. (The
   landmine that breaks Designs 1/4.)
2. **No empty-claim keyed wipe:** prove the pile doom never touches
   `g_claimedActors` and `RunDivergenceSweep_` is not re-armed on the drain
   edge.
3. **Host re-stream is not a re-bracket:** the `PileResyncRequest` reply sends
   per-pile `PropSpawn`s WITHOUT `SnapshotBegin/Complete` — confirm it does not
   re-arm the divergence sweep (the R1 churn regression).
4. **`expectedMirrors` honesty:** the gate denominator is the host's
   *this-cycle* re-streamed count minus pending-removes — confirm it cannot
   starve (permanent defer = re-dupe) and the explicit `PileResyncComplete{count}`
   marker distinguishes "host has zero" from "no reply yet".
5. **Pending-remove consume on every create path:** OnSpawn fresh-spawn AND the
   re-stream path AND any throw/convert pile-create must consult
   `g_pendingRemoveEids`.
6. **Hot-path / re-entry table** per `audit-prompt-perf-template.md`: the
   GUObjectArray doom walk + ~870 SpawnLocalTrashActor per resync must be COLD
   (drain edges only, never per-tick); measure the worst-case 3-drain join RSS +
   frame hitch with freeze_probe (the 2.5 fps reaper scar + the 19 GB OOM scar).
   Confirm `TickPileReconcile` early-returns at zero cost when not pending.
7. **Coupled kerfur ordering:** prove the NPC `g_ghostSwept` clear happens only
   AFTER the host NPC re-send, never before (symmetric world-wipe).
8. **Doom collateral:** `DoomNonMirrorChipPiles_` spares a pile that is the
   subject of a pending grab/throw relay (`IsPendingGrabOrThrow` consults
   pile_handle's `g_grabCandidate`/`g_throwWatch`).
9. **Modularity:** `wc -l` every touched file; flag any past 800 with an
   extraction proposal (prop_adoption / remote_prop_spawn should net-shrink).
10. **Interaction smoke (NOT a join smoke):** a smoke that forces a shadow-drain
    AND exercises grab/throw across it — the gap that shipped 3 broken pile
    fixes ([[feedback-interaction-smoke-not-join-smoke]]). Build it before
    handoff.

---

## 13. Open risks

- **New wire round-trip on the drain edge.** `PileResyncRequest` → host walk →
  N `PropSpawn` adds a host-side per-request cost (~870 PropSpawns under the join
  cover, 2-3x/join). It is cold + MTA-faithful, but profile the host send burst
  + client receive/spawn burst (freeze_probe + RSS) before handoff. This is the
  principled cost of NOT predicting on the client.
- **Pile clock late vs deadline.** chipPile-inclusive quiescence waits for ~870
  piles to settle on a ~19 MB save; confirm it settles well under the 45 s
  deadline so the deadline path (guarded by no-interaction) is the rare backstop,
  not the norm.
- **`PileResyncComplete` reliability.** The gate's "host has zero piles" branch
  depends on the complete-marker arriving; it rides the reliable ARQ channel, so
  a drop retries — confirm it cannot be silently lost (else the doom defers
  forever = re-dupe). An ARQ-acked reliable kind, not fire-and-forget.
- **Coupled kerfur scope.** The NPC drain bridge (P6) doubles the wire + clock
  work; if it risks destabilizing the pile pass, land the pile fix first behind
  the same mechanism and the NPC fix as an immediate follow-up commit — but do
  NOT ship the bare `g_ghostSwept` clear alone (world-wipe).
- **Modularity:** prop_lifecycle.cpp is already ~922 LOC (pre-existing, this
  pass does not grow it); the new pile-reconcile code may push prop_adoption.cpp
  past 800 → extract `coop/pile_reconcile.cpp` if so.

---

## 14. MTA fidelity

- Host is the sole id-minter + broadcaster of create (`PropSpawn`) / destroy
  (`PropDestroy`) / convert (`PropConvert`); the client instantiates a pile ONLY
  from a host `PropSpawn` (EntityAdd). Identity is the host eid (`RegisterPropMirror`),
  never position. (CStaticFunctionDefinitions.cpp one CEntityAddPacket/entity;
  Packet_EntityRemove by id.)
- The drain re-stream is **MTA EntityAdd-on-rescope**: the client's runtime
  mirrors left scope (engine freed them), so the host re-sends them on request —
  exactly MTA re-streaming an entity that left and re-entered scope, identity
  preserved. This is the principled replacement for the client-predicts-from-seed
  divergence the adversary killed.
- The doom = MTA deletion-by-tracked-membership inverted (the client dooms what
  is NOT a host mirror); it has no MTA analogue because MTA clients never load
  shared state — it exists solely to bootstrap VOTV's same-save reality into the
  MTA invariant, idempotent once converged.
- The pending-remove-by-id queue = MTA Packet_EntityRemove queued until the
  entity materializes — by id, never position (replacing the position-fallback,
  which violated identity-by-id).
- **Documented divergence (RULE 2026-05-28 comment in net_pump):** the
  same-world shadow-drain has no MTA analogue (MTA never frees a streamed entity
  behind the server's back); our client-requests-resync + host-re-streams bridge
  is the VOTV-specific necessity, cited against the EntityAdd / re-scope precedent.
