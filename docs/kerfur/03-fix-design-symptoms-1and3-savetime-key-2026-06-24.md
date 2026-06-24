# 03 — FIX DESIGN (DRAFT): symptoms 1+3 shared save-time EXACT-key reconcile

**Status: DESIGN READY + GREENLIT TO BUILD (2026-06-24). Root CENSUS-PINNED to identity-key; edges vetted; the
forward off->active dup (doc 07) is EXACTLY this scenario. NOT YET built -- this is the NEXT build.**
Covers symptom 1 (window dup) + symptom 3 (identity-collision). Symptom 2 (camera) = a SEPARATE design (doc 04, later).

> **CENSUS PIN (2026-06-24, doc 07 + `research/kerfur_forward_census_1543/`):** the forward off->active dup's
> silent half = an **UNCLAIMED `prop_kerfurOmega_C`** (Nrby, a stale local off-prop the host no longer has as
> off because it turned the kerfur ON), **0 UNTRACKED NPC** -> identity-key, retire-side ruled out. Two faces of
> one missing stable key: 15:43 the off-prop SURVIVES (dup); 15:41 it fuzzy-collapses onto a neighbour active's
> position (skew). Scope A's save-time exact key fixes BOTH.
>
> **VERIFIED RETIRE REQUIREMENT (critical, 2026-06-24):** the existing divergence sweep does NOT close this. In
> the 15:43 run the bracket armed CLEANLY (`claim tracking ARMED` -> `divergence sweep FIRING 2439ms after arm`),
> the sweep fired and doomed 80 trashBitsPile + 1 swinger + 1 mushroom -- but **NOT the unclaimed Nrby off-prop.**
> So scope A must do more than DEDUP: it must **RETIRE** the stale local off-prop, keyed by save-time position,
> when the host now has that kerfur ACTIVE. The retire is **identity-key-driven, NOT convert-driven** (the window
> conversions do NOT sync as KerfurConvert -- they fold into the snapshot; the client sees the host's active
> kerfur via the npc channel + its own stale off-prop via the prop channel, with no convert linking them). The
> save-time key is what links "my local off-prop was Nrby at P_save" to "the host's active kerfur carries P_save"
> -> retire the off-prop. **A pure dedup-by-key leaves the dup; the build MUST include the save-time-keyed RETIRE
> of an unclaimed local off-prop whose kerfur the host expresses ACTIVE.** (Open build sub-question: WHY the
> divergence sweep skipped Nrby -- not-tracked-element vs claimed -- determines whether `kerfur_reconcile` reuses
> the sweep's propPairs or walks kerfur props itself; settle at build time.)

## Root (confirmed, doc 02): kerfur object identity falls to POSITION-FUZZY

The kerfur object channel binds save-object <-> host-kerfur by **30 cm + class+pose fuzzy** (`remote_prop::OnSpawn`
Gap-I-1, `kerfur-prop-adopt class+pose match`), because the convert broadcasts a SYNTHETIC key
(`coopkerfur#<eid>`) that the local save-object (deterministic `Aprop_Key`, e.g. `gPXK...`) cannot match ->
fuzzy fallback. In a cluster the nearest BODY wins -> collision (#3). And kerfur objects are NOT chipPiles,
so the L1 `pile_reconcile` sweep skips them (`[PILE-CENSUS] totalLiveNatives=0`) -> the dup never auto-resolves
(#1), persisting until a manual turn/grab fires a convert whose 30 cm match happens to de-dup.

## The fix shape: L1 Path 1c, generalized to the kerfur object channel

Bind the client's save-loaded kerfur object to the host's kerfur by an **EXACT cross-peer-stable key**, retried
at **post-quiescence** -- exactly L1 piles. The 5 gates the user raised, answered:

### Q1 -- WHERE does the save-time exact key come from?
**The kerfur's SAVE-TIME POSITION, captured at the blob instant** -- same source + moment as L1's
`g_blobPileXforms` (`save_transfer::OnRequest`, `CollectTrackedPileTransforms`). Kerfur OBJECTS are keyed
props (`prop_kerfurOmega_C : Aprop_C` passes `IsClassKeyedInteractable`), so they are in the prop element
tracker and enumerable at the SAME blob instant the piles are. ADD a parallel capture
`g_blobKerfurXforms[slot]` keyed by the **host PROP-EID** (`map<host prop-eid -> save-time FVector>`) filled by
a `CollectTrackedKerfurTransforms` (mirror of the pile collector, class-gated to `prop_kerfurOmega_C`).
**CODE-VERIFIED capture key (2026-06-24, corrects an earlier "KerfurId-keyed" imprecision):** at the blob
instant an untouched save-loaded off-prop kerfur has a host-range **prop eid** but **NO KerfurId** -- the host
registers its kerfur props normally for the snapshot ([prop_element_tracker.cpp:325-327](../../src/votv-coop/src/coop/prop_element_tracker.cpp#L325-L327))
but `BindFormActor` **lazily allocates the KerfurId only at the first conversion**
([kerfur_entity.cpp:241-243](../../src/votv-coop/src/coop/kerfur_entity.cpp#L241-L243)). So the capture MUST key
by the prop eid, not K. (Already-ACTIVE kerfurs at save-time go via the npc channel and DO have a K + host NPC
eid at registration -- npc_world_enum/npc_sync `AllocKerfurId` -- captured by a parallel NPC-eid-keyed entry; see
edge 1.)
**Why position, not the `Aprop_Key`:** the key IS deterministic+cross-peer (both load the same save), BUT (a)
it is restored LATE (the load-tail race) and (b) the ACTIVE form (`kerfurOmega_C` NPC) has NO `Aprop_Key` at
all -> the key cannot bridge the active<->object transition (Q2). The save-time POSITION bridges it.

### Q2 -- EID/CLASS survival (the decisive one: kerfur CHANGES class, pile did not)
**TWO DIFFERENT survivals -- do not conflate (the user caught a doc imprecision here 2026-06-24):**
- **KerfurId survives the active<->object class FLIP at runtime, host-side, same session** -- `BindFormActor`
  rebinds the SAME K in place ([kerfur_entity.cpp:261-269](../../src/votv-coop/src/coop/kerfur_entity.cpp#L261-L269),
  "K preserved across the conversion, never re-minted"; log-proven K=3146 through eid 3145->5278->5279).
- **KerfurId does NOT survive save load and is NOT cross-peer** -- it is host-RAM-only (AllocHostId, never
  persisted; [kerfur_entity.h:16-22](../../src/votv-coop/include/coop/kerfur_entity.h#L16-L22),
  [kerfur_entity.h:88-90](../../src/votv-coop/include/coop/kerfur_entity.h#L88-L90) "the CLIENT is eid-based and
  does NOT track KerfurIds"; the game's own `Aprop_Key` is random per peer per load,
  [kerfur_entity.h:5-8](../../src/votv-coop/include/coop/kerfur_entity.h#L5-L8)).

The design uses ONLY the first axis, and ONLY host-side. **It does NOT match anything by KerfurId cross-peer.**
The three roles, correctly separated:
- **Capture key (host, blob instant): the host PROP-EID** -- the off-prop has one, has no K yet (Q1).
- **Carry handle (host, post-conversion): the KerfurId** -- when the host turns the off-prop ON in the window,
  `BindFormActor(oldEid=propEid, ...)` looks up `g_blobKerfurXforms[oldEid]` and STAMPS the save-time position
  into the freshly-allocated `KerfurRecord`; from there K carries it through any further flips (edge 3). K is a
  pure host-side durable handle, never sent as an identity key.
- **Cross-peer identity key (client): the save-time POSITION** -- both peers derive it from the one transferred
  save; the client matches its save-loaded native/object to the host's broadcast by EXACT position (Q3). The
  client never needs a KerfurId.

So the apparent contradiction dissolves: "host-RAM-only" (true) and "survives the class flip" (true) are
different axes; the cross-peer link is the save-time position, never K.

### Q3 -- LINK the diverged broadcast-key vs save-key
Today: broadcast `coopkerfur#<eid>`, save-actor `gPXK...` -> no key match -> fuzzy. **Fix: every kerfur
broadcast (KerfurConvert AND the connect-snapshot EntitySpawn/PropSpawn) CARRIES the kerfur's save-time
position** (the L1 `matchX/Y/Z` fields). The client matches its save-loaded object/native to the host kerfur by
**EXACT save-time position** (both peers derive it from the same transferred save -> a guaranteed common key),
NOT `coopkerfur#<eid>`, NOT 30 cm. This is L1's "both peers compute the same key from one save" applied to
kerfur. The synthetic eid-key stays only as the post-bind wire handle (PropPose routing); identity resolution
is the exact save-time position.

### Q4 -- CLUSTER tie-break (the #3 core)
Exact save-time position is UNIQUE per kerfur -- two kerfurs cannot occupy one identical save position (L1
proved this kills dense-cluster mis-match for stacked piles). So a clustered/additional kerfur has a DIFFERENT
save-time position -> it CANNOT claim the hopeless kerfur's binding. Collision eliminated by construction,
exactly as L1's save-time-exact key killed the session-35 pile dense-cluster mis-bind. (No chipType-analog
tie-break needed; position uniqueness suffices, same as L1 -- chipType there was a secondary guard, not the
discriminator.)

### Q5 -- INDEPENDENCE from symptom 2 (camera) + order
The 1+3 key-fix is INDEPENDENT of #2. It (a) collapses the dup at join automatically (exact-match the
save-object -> twin-destroy, like L1), and (b) gives every kerfur its CORRECT identity so a neighbor cannot
steal it. **But it does NOT give the hopeless kerfur a body** -- that body never spawned (the #2 fresh-spawn
floating-camera failure). So: **ORDER = 1+3 key-fix FIRST** (kills dup + collision; the hopeless kerfur now
keeps its own identity instead of losing it to a neighbor) **THEN #2 camera-fix** (gives it a body). Both are
required for the complete repair; they are independent and do not share code.

## Timing (inherited from L1): retry at POST-QUIESCENCE
The `Aprop_Key`/native load is a LATE Init pass not quiesced before SnapshotComplete (RCA RC-A) -- the SAME
load-tail race L1 take-1 hit. So the exact-save-time-position twin-destroy must run BOTH at world-ready AND
retry at the post-quiescence sweep (`HasLoadTailQuiesced`), with the >50% valve, mirroring
`pile_reconcile::SweepReconcileSaveTimeTwins`. Likely a sibling `kerfur_reconcile::SweepReconcileSaveTimeKerfurs`
(NOT folded into pile_reconcile -- different class/channel; one feature per file, RULE 2026-05-25).

## Edge points for the user to VET before code -- VETTED 2026-06-24 (code-cited verdicts inline)

**KEY FACT that frames all of these (save_transfer.cpp:101-102 + RCA line 126):** the transferred save IS the
LIVE host world serialized at the blob instant ("the blob was just serialized from them"), and "a turned-ON
kerfur round-trips as an ON NPC." So **`P_save == P_live` for every kerfur by construction** -- there is NO
separate "saved position", and a kerfur's **blob-instant FORM** determines BOTH its capture key AND the form of
the client's save-native. The CONFIRMED window-race repro (doc 02, symptom-2 evidence) is **off-prop-at-blob**:
the kerfurs were OFF when the client connected; the host ACTIVATED them POST-blob, in the load window. This is
the build scope.

1. **Capture set -- VET: PASS. ONE map (not two), the active case is OUT of scope.** Your two-map "migration
   gap" worry dissolves because the right structure is **ONE host-eid-keyed map + the durable KerfurRecord**,
   not two parallel maps:
   - **Capture (off-prop-at-blob):** `g_blobKerfurXforms[propEid] = P_live`, filled by a
     `CollectTrackedKerfurTransforms` at the blob instant. Off-prop kerfurs are tracked keyed props with a
     host-range prop eid ([prop_element_tracker.cpp:325-327](../../src/votv-coop/src/coop/prop_element_tracker.cpp#L325-L327)).
     **Take-4 self-seed:** if a live off-prop kerfur has eid==0 at capture, MINT it inline (idempotent
     register-only, as `CollectTrackedPileTransforms` does) -- the capture owns its precondition
     ([[feedback-snapshot-before-state-ready]]).
   - **No migration gap, because BindFormActor is the SINGLE choke-point for BOTH directions**
     ([kerfur_convert.cpp:220](../../src/votv-coop/src/coop/kerfur_convert.cpp#L220) prop /
     [kerfur_convert.cpp:254](../../src/votv-coop/src/coop/kerfur_convert.cpp#L254) npc). At the FIRST conversion
     it promotes `g_blobKerfurXforms[oldEid]` -> the K-keyed `KerfurRecord`; every SUBSEQUENT flip
     (off->on->off->...) carries from the record. A kerfur changing state in the window never "falls between
     sources" -- there is one map (bootstrap) + one record (durable carry), and the choke-point bridges them.
     This is exactly the active->off transition you flagged: it is covered because the record (not a second map)
     holds the anchor across the flip.
   - **ACTIVE-AT-BLOB is OUT of scope for this build (honest boundary).** A kerfur already ON when the client
     connects serializes into the save as an ON NPC (RCA line 126) -> the client loads an NPC save-native ->
     retiring it on a window turn_off touches the **npc_adoption / npc_mirror channel**, a SEPARATE reconcile
     path from the prop-channel 1+3 fix, AND this case is NOT in the confirmed repro. Building it now = guessing
     across an unreproduced npc-channel interaction (violates RULE 1 / verify-game-domain-facts). DEFER it: a
     clean hook (stamp `P_live` into the K-having record at capture for the anti-collision anchor) is cheap and
     can land later, but the npc-native-retire-on-convert resolution waits until the case is reproduced. The
     prop-channel fix below fully closes the CONFIRMED repro.
2. **Position precision -- VET: PASS, same assumption as L1.** Capture reads the live actor transform at the
   blob instant; since the save IS that live world, `P_live == P_save` exactly (no settle gap for a resting
   kerfur), and the client's freshly-loaded native is at the same value -> the ~1 cm epsilon holds. RESIDUAL
   RISK (identical to L1): a kerfur physically displaced on the host AT the blob instant captures that displaced
   pos -- but the save serializes the SAME displaced pos, so the client native still matches. The only true
   miss would be a position change BETWEEN serialize and capture; they are the same instant in OnRequest, so
   this is moot. No frozen-save-value read needed.
3. **KerfurRecord carry + lifetime -- VET: PASS, with an EXPLICIT teardown (your (b)).** Add `saveTimePos`
   (FVector) + `hasSaveTimePos` (bool) to `KerfurRecord`
   ([kerfur_entity.cpp:36-44](../../src/votv-coop/src/coop/kerfur_entity.cpp#L36-L44)); stamp in `BindFormActor`
   via `g_blobKerfurXforms[oldEid]` at the first conversion, then it rides the K-keyed record across every flip
   ([kerfur_entity.cpp:255-269](../../src/votv-coop/src/coop/kerfur_entity.cpp#L255-L269)). **Lifetime (the (b)
   teardown -- corrected, NOT "while pending"):** `g_blobKerfurXforms[slot]` DOES outlive the snapshot (piles
   clear at snapshot-send; kerfur window-conversions fire AFTER that, during the client load-tail, and need the
   map) -- so it gets an **EXPLICIT bounded teardown**, cleared at the FIRST of:
   (i) a hard per-slot deadline (parity with the divergence-sweep `kSweepDeadlineMs`, ~8 s -- by then every
   window conversion has fired and promoted into its record); (ii) the per-slot stream reset / cancel / rejoin
   (the same call sites as `g_blobPileXforms`, save_transfer.cpp:363/425/568); (iii) `OnDisconnect`. There is NO
   unbounded "while something is pending" branch -- the deadline is the hard upper bound, so the map cannot leak
   for the session, and a post-teardown late conversion falls back to the normal eid path (no stale-key bind).
4. **Post-save-purchased kerfur skip -- VET: PASS, symmetric with L1.** A kerfur bought/spawned AFTER the blob
   instant has no `g_blobKerfurXforms` entry -> `kerfur_reconcile` finds no save-time twin -> no false
   twin-destroy -> normal broadcast spawn, exactly L1's "new post-save pile" case (and the L5 arcade late-buy
   invariant). Skip is by ABSENCE from the map, no special-case needed.

### Your two pre-greenlight points -- CLOSED
- **(a) Two-source continuity / active->off gap:** RESOLVED by structure -- it is ONE map (off-prop bootstrap)
  + the durable K-keyed `KerfurRecord`, bridged at the single `BindFormActor` choke-point, so no migration gap
  exists for the confirmed off-prop-at-blob repro across ANY conversion sequence. The active-AT-blob "second
  source" is scoped OUT (npc channel + unreproduced); not built blind. (NPC-eid strict-key anti-collision is the
  deferred hook, not needed for the confirmed repro where all kerfurs are off-props at the blob instant.)
- **(b) `g_blobKerfurXforms` teardown:** EXPLICIT + BOUNDED -- hard per-slot deadline (~8 s, sweep parity) OR
  per-slot stream-reset/cancel/rejoin OR OnDisconnect, whichever first. NOT "while pending"; the deadline caps
  it. The durable carry is the record (promoted at the choke-point), so the map's job ends when the window's
  conversions have fired.

## REFACTOR MAP -- build kerfur NOW, generalize AFTER (N=2, user direction 2026-06-24)

The user spotted the PATTERN: pile + kerfur + any future save+broadcast mirror object hit the SAME class
(two-channel window with no stable cross-peer key -> window-dup + cluster collision). See the cross-cutting
doc `docs/COOP_MIRROR_IDENTITY_WINDOW_RACE.md`. ORDER: make kerfur WORK first, refactor into a shared
mirror-identity layer SECOND -- do NOT generalize on N=2 (pile + kerfur already diverge; a 2-example
abstraction risks not fitting the 3rd). So build `kerfur_reconcile` now, but design it so the shared layer
extracts NATURALLY later. Map of what's common vs kerfur-specific (the refactor's seam):

**COMMON with `pile_reconcile` (candidates for the future shared layer -- REUSE PARAMETERIZED, don't copy):**
- Save-time position capture at the blob instant (`g_blob*Xforms`, `CollectTracked*Transforms`) -- identical
  mechanism, only the class filter differs (chipPile vs prop_kerfurOmega). -> parameter: the class predicate.
- Carrying the save-time position on the join broadcast (`matchX/Y/Z`) -- identical wire shape.
- The exact-position twin-destroy at world-ready + the **post-quiescence sweep with the >50% valve**
  (`SweepReconcileSaveTimeTwins`) -- identical gating/timing. The strongest shared candidate.
- The exact-position UNIQUENESS anti-collision (two objects cannot share a save position) -- identical proof.

**KERFUR-SPECIFIC (does NOT fit `pile_reconcile` without violence -> keep in `kerfur_reconcile`; the SIGNAL
that the abstraction is not yet ripe -- generalize on the 3rd, not now):**
- **CLASS-SURVIVAL (the big one):** kerfur changes class active<->object (`kerfurOmega_C` <-> `prop_kerfurOmega_C`);
  a pile stays a pile. `pile_reconcile` is single-class; the kerfur reconcile must bridge "the save-object's
  twin may now be an ACTIVE NPC of a different class" via the KerfurId. This is the divergence most likely to
  NOT fit a single-class abstraction -- record it as the seam.
- **SECONDARY tie-break field:** piles use `chipType` as a secondary guard; kerfurs have none (identity is
  pure position). -> a future shared layer needs the tie-break as a parameter (chipType / none / class).
- **BP-KEY shape:** pile is KEYLESS; kerfur has a random per-peer `Aprop_Key` AND the synthetic `coopkerfur#eid`
  (the current fuzzy-fallback cause). The fix DROPS the eid-key dependence for identity -> save-position only.

**Discipline:** REUSE `pile_reconcile`'s sweep/valve/capture mechanism parameterized where it fits cleanly; do
NOT add kerfur hacks that block the future extraction; do NOT pre-abstract now. If kerfur fits with params ->
the shared layer falls out trivially later. If the class-survival forces violence -> that is the recorded
signal: fix kerfur in its own file, extract the shared layer when a 3rd mirror object arrives.

## NEXT
User vets the 4 edge points -> if clean, build: `g_blobKerfurXforms` capture + carry the save-time pos on the
kerfur broadcasts + `kerfur_reconcile` exact-position twin-destroy at world-ready + post-quiescence sweep
(reusing `pile_reconcile`'s sweep/valve mechanism parameterized where it fits; class-survival kerfur-specific).
Then symptom 2 (camera) design (doc 04). Do NOT build before the edge-vet (L1 Path 1c discipline).
