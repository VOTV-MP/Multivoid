# Phase 1 — save-loaded identity via in-memory index->eid map — BUILD PLAN (2026-06-25)

> **SUPERSEDED (2026-06-26) — point-in-time. BUILT + VERIFIED (mechanism), but two specifics here changed:**
> (1) the 1B "host map = replay `GetAllActorsWithInterface` gather ordinal" was FALSIFIED (live gather order !=
> saved objectsData order) and replaced by **PATH A** (read the live `saveSlot.objectsData`+`primitivesData`
> arrays in load order — see `docs/COOP_STABLE_ID_SIDECAR.md` §6 + `[[lesson-chippile-saved-in-primitivesData-not-objectsData]]`);
> (2) the spawn-time bind is verified but the bound chipPiles die to the host-proxy `TryDestroyTwin`, so the
> end-to-end fix is the **(X) native-authoritative** design
> (`research/findings/phase1-X-native-authoritative-chippile-DESIGN-2026-06-25.md`). Read those for current state.

**ON REVIEW, ZERO code.** Build-plan-level detail for `docs/COOP_STABLE_ID_SIDECAR.md` §2.1/§3/§3.6
(design greenlit-in-principle). Covers BOTH keyless save-loaded families — chipPiles AND off-prop kerfurs
(`prop_kerfurOmega_C`) — with ONE mechanism. Phase 0 (the completeness floor + the purge-aware timing gate)
is PROVEN + pushed (`9e2f4d2f`); this is the identity layer on top.

The position reconcile layer (`save_time_retire_util.h`, `g_blobPileXforms`, `g_blobKerfurXforms`,
`SweepReconcileSaveTimeTwins`, `kerfur_reconcile` scope-A for save-loaded) STAYS as a parallel safety-net
through this whole plan; it is retired ONLY in a later Phase 4 after ~a dozen hands-on sessions (RULE 2,
proof-gated). Nothing here regresses the 3 working instances or instant-world.

---

## 0. What ships, in order (the probe is the first gate)

| Step | Deliverable | Gate before next |
|---|---|---|
| **1A** | §3.2 spawn-order PROBE (read-only, ini-gated, RULE-2-exempt) — **DONE 2026-06-25, PASS: spawn-order is the deterministic PRIMARY** | ✓ thunk catches every keyless load-spawn (see 1A RESULT below) |
| **1B** | Host map-build at save-capture (`eidByIndex`), logged, NO wire | host eids match `prop_element_tracker`; counts == captured keyless count |
| **2** | Transport (ride `save_transfer`) + client bind pass at quiescence | client binds 100% of keyless natives; eids agree host<->client |
| **3** | Reconcile/sweep match by eid for save-loaded keyless | L1 pile + kerfur + instant-world unregressed; jUuC + chipPile tests pass |
| **4** | (LATER, separate, proof-gated) retire the position layer | ~a dozen clean hands-on sessions |

If 1A fails (thunk doesn't fire on load, or order diverges), the bind falls to §3.3 (exact-transform
bijection) — proven-deterministic, same-class, same-blob, at load time. 1A decides which primary; everything
downstream is identical.

### 1A RESULT (2026-06-25, autonomous, binary `E4B7B3C3`, `coop/dev/spawn_order_probe.{h,cpp}`)
The probe records every keyless-family `BeginDeferredActorSpawnFromClass` the client thunk sees (continuous,
not bracket-gated — the first run armed too late and read a false fired=0; the boot load precedes the
bracket), then at load quiescence walks the surviving keyless natives and checks each was recorded. Three
autonomous runs, consistent:
- **chipPile: fired=870** every run — the thunk catches the ENTIRE pile field's load-spawns (870 == the full
  field). (`survivors=0` all three runs: the autonomous join consistently purged the client piles before
  quiescence and did NOT reload them — a SEPARATE reload-race observation, see note — so chipPile
  caught-vs-survivors was unmeasured, but fired=870 proves every pile spawned through the caught thunk.)
- **kerfurOff: fired=4, survivors=4, caught=4, missed=0 -> CAUGHT-ALL** every run — the clean survivors>0
  proof: every surviving off-kerfur native fired the thunk.
**VERDICT: the BeginDeferred thunk catches every keyless load-spawn -> spawn-order is the deterministic
PRIMARY (§4.1).** The §3.3 exact-transform bijection stays as the backstop, NOT needed as primary. (Order vs
index is not separately measured but follows from coverage + the synchronous per-index loadObjects loop,
loadObjects_dump.txt [223-291]; both peers walk the SAME index-ordered objectsData.)

**SEPARATE OBSERVATION (not a 1A blocker, worth a look before Phase 2 hands-on):** in all 3 probe runs the
client's 870 piles LOADED (fired=870) then were PURGED and did NOT reload -> 0 chipPile survivors at
quiescence (the client ends with NO piles). PATH B (also autonomous, but host `force_chippile_unclaim=1`)
showed 870 SURVIVING. The difference (host expresses piles vs host-skips) flipping client pile SURVIVAL is
unexplained and is the join-reload race surfacing again; the timing fix + floor concern the SWEEP, not this
PURGE-side loss. Flag for a hands-on check (a real progressed-save join likely persists the piles); it does
not change the 1A verdict.

---

## 1. Data structures

New file `coop/save_identity_map.{h,cpp}` (~300-400 LOC, principle-7 `coop/` = gameplay/network). One owner
of the map on each side.

```cpp
namespace coop::save_identity_map {

// One entry per KEYLESS save object (chipPile | off-prop kerfur). Keyed forms are NOT here -- they bind by
// key (§3.1) and already carry a stable cross-peer identity. Index = position in saveSlot_C.objectsData.
struct IdEntry {
    uint32_t index;       // objectsData ordinal (byte-identical cross-peer: same blob, loadObjects in order)
    uint32_t eid;         // host-minted ElementId for this object
    uint8_t  family;      // 0 = chipPile, 1 = kerfurOff  (class-scopes the spawn-order counter + the bijection)
    // NO position. NO key. The index IS the anchor; the eid IS the identity.
};

// HOST: built at save-capture, sent with the blob. CLIENT: received, consumed at load quiescence.
using IdMap = std::vector<IdEntry>;   // sparse over objectsData (keyless subset only) -> a few hundred entries

}  // namespace
```

**Wire format** (little-endian, mirrors `snapshot_census`'s explicit layout):
`uint16 version` (=1), `uint16 count`, then `count` * `{ uint32 index, uint32 eid, uint8 family }` = 9 B/entry.
~870 chipPiles + a handful of off-kerfurs ≈ ~8 KB. **This EXCEEDS the 228 B reliable payload cap**, so it does
NOT ride a SnapshotComplete tail (that is why the census could and this cannot). It rides the **chunked
`save_transfer` pipeline** (§3) — same transport as the ~19 MB blob, so the map and the blob arrive as one
atomic unit before `loadObjects` runs.

---

## 2. HOST side — build the map at save-capture

**Where:** `save_capture.cpp`, immediately AFTER the game's `saveObjects` has rebuilt `objectsData`
(`save_capture.cpp:~90`, where the live array Num is already read). One new call:
`save_identity_map::BuildFromCapture(objectsDataActor, outMap)`.

**How (deterministic, no GVAS parse — the array is live in memory):**
1. Replay the exact `saveObjects` filtered walk in the SAME frame (deterministic within one call,
   `COOP_STABLE_ID_SIDECAR.md §1.3`): `GetAllActorsWithInterface(int_save_C)` -> for each, in order, the ones
   that pass `!ignoreSave()` are the producers of `objectsData[0..k]` in order. So walking that filtered list
   gives `index k -> actor` for free, by ordinal.
2. For each index k whose actor is a KEYLESS family (`IsChipPile(actor)` -> family 0; `IsKerfurPropClass` ->
   family 1; everything else SKIP — keyed forms bind by key), resolve `actor -> eid` via
   `prop_element_tracker::GetPropElementIdForActor(actor)`.
   - If the actor has NO eid yet (a save-loaded native never seen by a seed-walk), **self-seed it here** —
     the EXACT idempotent register-only mint `CollectTrackedPileTransforms` already does
     (`prop_element_tracker.cpp:727`, `MarkPropElement(obj,L"",cls)`). This is the same self-seed-eid
     FOUNDATION (§5 of the design) — it ESTABLISHES the identity at capture, the host's authoritative instant.
   - Emit `IdEntry{ index=k, eid, family }`.
3. Return the map. The host now OWNS an eid for every keyless object it is about to ship.

**Phase 1B stops here** (build + log + assert eids match the tracker; NO wire). Validation: the count of
emitted entries == the count of keyless actors in `objectsData`; every eid resolves in `prop_element_tracker`.

**Cost:** one filtered-actor walk at capture (cold, once per join, the host already walks for the blob). No
per-frame work. Self-seed mints are register-only (no broadcast), idempotent.

---

## 3. TRANSPORT — ride the chunked save_transfer (no new ReliableKind)

**Where:** `save_transfer.cpp` (the host->client blob pipeline). The map is DERIVED from the same
`objectsData` and must arrive BEFORE `loadObjects`, so it is a **sidecar of the blob transfer**, not a
separate message:
- Host: after `BuildFromCapture`, serialize the map (§1 wire format) and PREPEND its length + bytes to the
  transfer payload (or send it as the transfer's first framed segment). The map (~8 KB) is negligible beside
  the blob; the existing chunker carries it.
- Client: the transfer receiver deserializes the map FIRST (it is small + fixed-layout), stashes it via
  `save_identity_map::SetFromWire(...)`, THEN writes the blob to the scratch slot and triggers
  `loadObjects`. So the map is resident before the first load-spawn fires.

**Why ride save_transfer, not a new ReliableKind:** (a) atomic pairing — the map is meaningless without its
blob and vice-versa; coupling them removes an ordering race; (b) the [[feedback-reliablekind-router-checklist]]
3-place wiring (enum/payload + family dispatcher + event_feed master-router) is AVOIDED — the map is a field
of the existing save-transfer kind, not a new kind; (c) the chunker already handles >cap payloads. If
save_transfer cannot carry a sidecar cleanly, the fallback is ONE new chunked ReliableKind `SaveIdMap` wired
per the checklist — but the sidecar is preferred.

---

## 4. CLIENT side — correlate index->actor, then bind eid

Two mechanisms, in priority order (§3 of the design). The probe (1A) decides whether §4.1 is trusted.

### 4.1 PRIMARY — spawn-order via the existing BeginDeferred thunk (§3.2)
**Where:** the Func-thunk already installed at `trash_collect_sync.cpp:76`
(`OnBeginDeferredSpawnObserve(srcObj, newActor)`), which fires at every `BeginDeferredActorSpawnFromClass`
POST below the BP VM (EX_CallMath-invisible to ProcessEvent, but this native thunk catches it). During a
CLIENT `loadObjects`, `newActor` is each spawned save actor, in objectsData order.

**How (new, client + load-window only):**
- A client-side, load-window-gated spawn counter in `save_identity_map`: when a load is in progress (gate:
  the save_transfer just completed + `loadObjects` running — the same window the join uses), each thunk fire
  whose `newActor` is a keyless family increments a per-family ordinal and binds that ordinal to the matching
  `IdMap` entry of the same family, in order.
  - Concretely: maintain `nextChipPileK`, `nextKerfurK`. On a chipPile `newActor`, take the next `IdMap`
    entry with `family==0` (entries are in index order) -> `MarkPropElement`/rebind that actor to its
    `entry.eid`. Same for kerfurOff.
  - Account for `loadObjects`'s pre-spawn dedup ([229]) and post-spawn `K2_DestroyActor` dedup ([277]): a
    destroyed spawn never reaches a stable bind — if the thunk fired for it, the K2_DestroyActor observer
    (already hooked) un-binds it, so the ordinal stays aligned. The probe (1A) MUST confirm this alignment.
- The bind assigns the HOST eid to the client native via the canonical owner path
  (`prop_element_tracker::MarkPropElement(actor, key="" , cls)` then a rebind of that local element's eid to
  `entry.eid`, OR a dedicated `BindLocalActorToHostEid(actor, eid)` that installs the host-range eid directly
  — see §6 eid-range note). After this, the native carries the host's cross-peer eid.

### 4.2 FALLBACK — exact-transform bijection (§3.3), if 1A's probe fails
**Where:** a client bind pass at load quiescence (the `HasLoadTailQuiesced` gate the sweep already uses).
**How:** for each unbound keyless native, match to the `IdMap` entry of the SAME family whose
`objectsData[index].transform` equals the actor's transform BIT-FOR-BIT (same blob set both). Class-scoped,
exact-value, single-blob -> unambiguous (two piles have distinct exact positions). This is the ONE residual
position-touch, and it is a deterministic bijection at bind time, NOT the fuzzy 1cm/30cm/500cm reconcile
match that caused the bugs. Everything downstream is eid-only.

### 4.3 KEYED forms — by key (§3.1), unchanged
Keyed props/NPCs already bind by `getKey` (byte-identical cross-peer). NOT in the map. No change.

---

## 5. The match migration — position -> eid, for BOTH keyless families

Once §4 has bound the client native to the host eid, the reconcile/sweep match key changes from save-time
POSITION to eid (design §3, Phase 3):

- **chipPile:** the join-window dup (`docs/piles/09` move-dup, L1) matched the host's moved pile to the
  client native by save-time position (`save_time_retire_util.h`, 1cm). Now the host expresses the pile by
  `entry.eid`; the client native ALREADY carries `entry.eid` (bound at load) -> the proxy/convert resolves
  by eid, the in-window move is irrelevant (identity does not depend on where it sits). The self-seed-at-grab
  (`trash_collect_sync.cpp:417`) stays as the grab-edge identity establishment (design §5).
- **off-prop kerfur (the jUuC class):** today an untracked-at-join off-kerfur (`key=jUuC...`) has NO client
  mirror (host never expressed it) AND is not key-resolvable in the remote_prop maps, so a later host grab
  mints a late eid the client can't match (`no local match (key or eid)` -> stays missing). With the map: jUuC
  IS in `objectsData` (every save object is) -> the host assigns it an eid at capture (self-seed) -> the
  client binds its local jUuC to that eid at load -> when the host grabs/poses jUuC by that eid, the client's
  PropPose matches by eid -> **the off-kerfur appears + poses.** This is the case §3.6 was written for; it is
  unreachable by the key path (untracked) or scope-A (position) — only the index->eid map gives the UNTRACKED
  ones an identity.

---

## 6. Coexistence with scope-A (kerfur off<->active) — explicit boundary

`kerfur_reconcile` scope-A (the off->active dup retire, `CollectTrackedKerfurTransforms` +
`SweepReconcileSaveTimeKerfurs`) and the class+pose fuzzy adoption handle TWO different things; the map takes
ONLY the save-loaded IDENTITY half:

- **Save-loaded IDENTITY (off-kerfur exists in the blob at join):** MOVES to the map (§5). The map supersedes
  scope-A's position-retire FOR THIS — a save-loaded off-kerfur is now identified by its index-bound eid, not
  by a save-time position match. scope-A's save-loaded position path becomes dead for bound kerfurs.
- **Runtime off<->active TRANSITION (a kerfur turned on/off DURING the session):** STAYS on its existing seam —
  the npc channel + the `kerfur_convert` poll + scope-A's runtime trigger. This is NOT a save-loaded identity
  question; it is a live state transition of an already-identified entity (it has its eid). The map does not
  touch it.

Boundary rule for the build: scope-A's save-loaded branch is GATED behind "the kerfur has no bound eid" — once
the map binds it, scope-A skips it (the eid path owns it); scope-A still runs for runtime transitions and for
any kerfur the map did not bind (belt-and-suspenders until Phase 4). The eid-bound check is the single
discriminator. No parallel duplicate logic — scope-A simply yields to a bound eid.

---

## 7. Phasing & the safety-net (RULE 2 timing)

- Phases 1B/2/3 ADD the eid path; the position layer (`g_blobPileXforms`, `g_blobKerfurXforms`,
  `SweepReconcileSaveTimeTwins`, `SweepReconcileSaveTimeKerfurs`, `save_time_retire_util.h`,
  the mirror-identity reconcile) STAYS, running in PARALLEL as a safety-net. A native that gets a bound eid
  is owned by the eid path; an unbound one still falls to the position net. They cannot fight: the eid match
  runs FIRST (at load), so by reconcile/sweep time a bound native is already converged and the position net
  finds nothing to do for it.
- **Phase 4 (separate, later, proof-gated):** delete the position layer fully (RULE 2) ONLY after ~a dozen
  hands-on sessions show the eid path covers every case. Not in this build.

---

## 8. Tests (MUST include both keyless families + the regressions)

Autonomous (log-truth) where possible; the two NEW identity cases need a real interaction:
1. **off-kerfur untracked-at-join (the jUuC case):** a save with an off-prop kerfur the host does NOT express
   at join. CLIENT loads it; host GRABS it. EXPECT: client log `bound jUuC-class off-kerfur to host eid E at
   load` then on grab `PropPose eid=E -> local match` (NOT `no local match`); the off-kerfur APPEARS + poses
   on the client. (This is the exact 13:21 symptom; it must flip from missing to present.)
2. **chipPile keyless save-loaded:** a save with the ~870 pile field. EXPECT: every native binds to a host
   eid at load (`bound K chipPiles to host eids`); the L1 join-window move-dup does NOT occur (the moved pile
   resolves by eid, no second proxy); census 0 unclaimed for chipPile.
3. **Regressions (must stay green):** L1 pile take-4 (no dup), kerfur fuzzy-gate + forward, instant-world
   (curtain/deferred-spawn), and the Phase-0 floor A/B (still WIPE-off / KEEP-on). The completeness floor +
   timing gate are UNCHANGED by Phase 1.
4. **Spawn-order probe (1A, gating):** the read-only probe log shows the BeginDeferred thunk fires for every
   keyless load-spawn, in objectsData order, dedup-accounted. PASS -> §4.1 primary; FAIL -> §4.2 fallback.

---

## 9. Open items / decisions for review
1. **Transport:** save_transfer sidecar (recommended — atomic pairing, no new ReliableKind) vs a new chunked
   `SaveIdMap` ReliableKind. Confirm save_transfer can carry a small framed sidecar before the blob.
2. **eid range on the client bind:** the host eid is HOST-RANGE; binding it to a client LOCAL native means the
   native's element must hold a host-range eid (like a mirror) yet stay a real local actor. Confirm the
   cleanest install: a `BindLocalActorToHostEid` that registers the actor in `prop_element_tracker` with the
   host-range eid (NOT the client peer-range AllocLocalId path), so a later host PropPose(eid) resolves it.
   This is the one non-trivial element-registry interaction; it wants its own mini-design + the §9.3 eid-
   lifetime trace from the design doc.
3. **Self-seed at capture vs at seed-walk:** Phase 1B self-seeds keyless actors at capture if unseeded
   (reusing the `CollectTrackedPileTransforms` idempotent mint). Confirm this does not double-mint vs the
   normal seed-walk (it is idempotent, but confirm the ordering at the capture frame).
4. **Probe FIRST:** build 1A and run it (read-only) BEFORE 1B/2 — it is the only unproven assumption. If the
   user wants, 1A can ship + run autonomously (it is read-only, ini-gated) to settle §4.1-vs-§4.2 before any
   identity code is written.

Code is ZERO until the user reviews this plan and greenlights step 1A (the probe) — then phase by phase.
