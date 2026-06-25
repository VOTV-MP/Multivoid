# COOP_STABLE_ID — save-loaded identity via a stable ID, not position

**Status: BUILDING (2026-06-26). Phase 0 PROVEN+PUSHED. Phase 2a sidecar transport VERIFIED (874 entries
arrive intact). Phase 2b client eid-range bind VERIFIED at the MECHANISM level — autonomous re-smoke binds
874/874 (870 chipPile + 4 kerfurOff), family tripwire SILENT, all case(i) fresh-mirror — BUT NOT end-to-end:
the bound chipPile natives are then destroyed by the existing host-proxy reconcile (`TryDestroyTwin`), so
`totalLiveNatives=0`. The fix is the (X) NATIVE-AUTHORITATIVE design (keep the native as the host-eid mirror,
suppress the proxy per-eid) — `research/findings/phase1-X-native-authoritative-chippile-DESIGN-2026-06-25.md`,
DESIGN ON REVIEW, every mechanism RE/probe-proven, ZERO code. Off-kerfurs are already native-authoritative
(no proxy). The 1B "gather-ordinal == objectsData order" assumption was FALSIFIED (2b smoke k=0 desync) and
replaced by PATH A (parse the live `saveSlot.objectsData`+`primitivesData` arrays in load order). See §6.**

> Filename keeps the user-chosen `COOP_STABLE_ID_SIDECAR` slug, but the
> **RECOMMENDED** design is the **in-memory index->eid map** (zero disk
> surface). The on-disk "side-car" is documented as a FALLBACK only — needed
> *only if* we later want client-side persistence across restarts (we do not
> now). See §2.

This doc closes the architecture the verdict converged on (4-agent design
sweep + 2 IDA read-only RE passes, 2026-06-25): the heavy join-window bugs
(L1 pile-dup, kerfur forward, pile-move `docs/piles/09`, pile×kerfur
over-destroy `docs/piles/10`) are mostly ONE class — *an object changed in the
join window and its cross-peer identity got confused because we match identity
by save-time POSITION*. Source/Quake don't have this: they key on a stable
network ID, never position. We already HAVE that primitive — the `eid`
(ElementId). The gap is that **save-loaded keyless forms** (chipPiles, off-prop
kerfurs; `Key == None`) never get a cross-peer-stable eid, so the mod falls
back to position. This design gives them one.

---

## 0. The decision in one paragraph

`eid` (ElementId) is ALREADY a stable, position-independent, host-minted
cross-peer ID that rides every channel (`elementId` field) and survives the
grab morph via explicit `RebindE`. Keyed props already have a stable identity
(their `Aprop_Key`, byte-identical on both peers because both load the SAME
transferred blob). The ONLY forms left on position are KEYLESS save-loaded
forms. Fix: at join, the host sends the client a map **{ objectsData index ->
eid }**; the client binds each save-loaded actor to the host's eid by that
index; thereafter ALL reconcile is by eid, never position. The move/collision
bugs disappear because identity no longer depends on where the object sits.
A separate, independent **per-class completeness floor** fixes the 11:16
over-destroy (an expression/claim failure that a stable ID does NOT address).

---

## 1. RE result (what we are building on — DO NOT re-derive)

### 1.1 The save format is STOCK UE4.27 GVAS (IDA read-only, 2026-06-25)
- `saveSlot_C` / `save_main_C` are Blueprint `USaveGame` subclasses of stock
  `/Script/Engine.SaveGame`. **No native `Serialize` override anywhere in the
  save path.** All save structs (`struct_save`, `struct_byteImage`, photo
  structs, signals) are `UserDefinedStruct` — constructively incapable of a
  custom serializer. Everything serializes via reflection
  `SerializeTaggedProperties`.
- On disk: standard GVAS header (`"GVAS"` magic confirmed in image at
  `0x142b5997d`) -> SavegameFileVersion -> PackageFileUE4Version ->
  FEngineVersion -> CustomVersions[{FGuid,int}] -> SaveGameClassName(FString) ->
  tagged-property body. Photos (~19 MB) are plain `TArray<uint8>`
  (`struct_byteImage.bytes`), verbatim-copyable.
- **Consequence:** a byte-exact read->write->diff roundtrip is ACHIEVABLE with
  a stock UE4.27 GVAS lib (e.g. `uesave-rs`) — NO bespoke parser. BUT (see §2)
  the recommended in-memory design needs **neither a GVAS reader NOR a writer**:
  the host reads `objectsData` LIVE in memory (already does — `save_capture.cpp:64`
  reads its `Num`), and the client never serializes anything. The roundtrip
  harness becomes a contingency, not a dependency.
- **We do not touch the vanilla save format. Ever.** RE was read-only; the mod
  only (a) calls the game's own `SaveGameToSlot` (stock serializer) into a
  scratch slot, and (b) reads the live `objectsData` array. The canonical
  `.sav` is never named in a write call (`save_capture.cpp` uses
  `kHostXferSlot`).

### 1.2 The per-object anchor in the save
- `struct_save`: `class`@0x00, `transform`@0x10, **`key` (FName)@0x40**, then
  typed property arrays. The `key` is ALWAYS serialized (tagged property).
- Keyed forms carry a real FName (stable cross-peer on the shared blob).
- **Keyless chipPiles carry `key == "None"`** (`actorChipPile_C` does not
  override `getKey`). For them the only stable handle is the **objectsData
  array INDEX** (ordinal position in `saveSlot_C.objectsData@0x300`), which is
  byte-identical on both peers (same blob, `loadObjects` iterates in order).

### 1.3 Host write order = `saveObjects` (bytecode `saveObjects_dump.txt`)
`GetAllActorsWithInterface(int_save_C) -> objs` ([32]); loop by index ([34-63]):
cast to `int_save` ([41]); skip if `ignoreSave()` ([44] — the coop-mirror
opt-out, `votv-save-path-RE-2026-05-30.md:64`); `getData(obj) -> data` ([53]);
`Array_AddUnique(copy1, data)` ([54]); finally `objectsData = copy1` ([93]).
So `objectsData[k]` is produced by the k-th actor that passes the filter, in
`GetAllActorsWithInterface` order. The host can recover **index k -> actor** by
replaying that exact filtered walk in the same frame (deterministic within one
call), then **actor -> eid** via `prop_element_tracker::GetPropElementIdForActor`.

### 1.4 Client spawn order = `loadObjects` (bytecode `loadObjects_dump.txt`)
- `data = saveSlot.objectsData` ([150]); load loop by index ([223-291]):
  `Array_Get(data, idx)`; **spawn via `BeginDeferredActorSpawnFromClass` +
  `FinishSpawningActor`** (`FinishSpawningActor` at [139]); `loadData(data[k])`
  populates the actor ([265]); post-spawn dedup by `getKey`+transform destroys
  duplicates ([269-277]).
- **The spawn is per-index, in objectsData order, through the deferred-spawn
  UFunction the mod ALREADY native-hooks** (`trash_collect_sync.cpp:76`
  Func-thunk on `BeginDeferredActorSpawnFromClass`, below the BP VM — fires
  regardless of the `EX_CallMath` invisibility that blinds the ProcessEvent
  detour). This is the seam that makes a **deterministic, position-free**
  client correlation possible (§3).

---

## 2. Architecture: IN-MEMORY map (recommended) vs on-disk side-car (fallback)

### 2.1 RECOMMENDED — in-memory `{ index -> eid }` map, sent at join
The persistence of identity does NOT need a disk file. The mod already
transfers the full save blob host->client at join. Add ONE reliable packet:
a vector `eidByIndex[]` parallel to `objectsData` (or a sparse list of
`{index, eid}` for the keyless subset).

- **Host (at save-capture):** after `saveObjects` rebuilds `objectsData`
  (`save_capture.cpp:90`), replay the filtered `int_save` walk to map each
  surviving `objectsData[k]` -> live actor -> `eid`; emit `eidByIndex[k]`. No
  GVAS parse (the array is live in memory). No disk write.
- **Client (at load quiescence):** bind each save-loaded actor to
  `eidByIndex[k]` for its index k (§3), assigning the host's eid to the local
  native. Every join re-seeds + re-sends a fresh map; nothing persists.

**Why this is strictly best (not just cheapest):** the dangerous CLASS of
actions — writing a file, a backward-compat matrix, CRC anchoring, stale
detection, orphan handling — is ABSENT, not merely small. "Don't break saves /
mod-removed-plays-vanilla" holds **by construction**: the mod writes nothing to
disk for identity (it already never writes the vanilla `.sav`). The safest
design is the one where the failure mode cannot occur because the operation
does not exist. Disk surface = 0.

**Cost:** one reliable packet per join (≤ a few KB for the keyless subset;
≤ ~16 KB even if dense). Negligible beside the ~19 MB blob.

### 2.2 FALLBACK — on-disk side-car `<slot>.coopids` (NOT now)
A sibling file mapping anchor -> eid, written atomically
(`.tmp`+fsync+`MoveFileEx`, the `player_inventory_sync.cpp` pattern already in
this codebase), transferred alongside the blob, CRC-cross-validated. This is
ONLY needed if we later want **client-side persistence across restarts** (a
client that keeps its own save and rejoins without a fresh transfer). We do not
want that now (the model is: client loads the host's fresh capture each join).
If/when we do, §7 has its full backward-compat matrix. Until then this is
migration baggage (RULE 2) and is NOT built.

### 2.3 Why parallel MP-save was REJECTED (closed, do not revisit)
A fully-own MP serialization format (stable-ID baked in) + an MP->SP converter
was considered as plan B if no anchor existed. It is rejected with cause:
- The anchor EXISTS (§1.2), and the format is stock GVAS (§1.1) — a stock GVAS
  reader is itself resilient to content patches (new properties appear as new
  tagged properties), so the "decoupling from patches" argument (the only real
  advantage of a parallel format) is NEUTRALIZED.
- The MP->SP converter would still need a full GVAS WRITE — the same work the
  parallel path claimed to avoid — PLUS a from-scratch serializer of all
  save semantics. Months, and a throw-away of working code.
- Justified ONLY for a true thin-client (client with no local save at all),
  which is explicitly out of scope. Closed.

---

## 3. The client actor<->index correlation (the one real unknown — solved deterministically)

The client must bind each loaded actor to its objectsData index to apply
`eidByIndex[k]`. We solve this DETERMINISTICALLY, not heuristically. Three
populations, three deterministic mechanisms — in priority order:

### 3.1 KEYED forms -> by key (fully position-free, robust)
`loadObjects` reads `getKey(spawnedObj)` ([269]); the key is byte-identical on
both peers (shared blob). The host's `eidByIndex` for a keyed entry travels
with its key; the client matches the loaded actor by `getKey`. No position, no
order dependence. This already covers every keyed prop and NPC.

### 3.2 KEYLESS forms -> native deferred-spawn ORDER (position-free; PROVE before trusting)
`loadObjects` spawns keyless forms per-index, in objectsData order, through
`BeginDeferredActorSpawnFromClass` — the UFunction the mod already native-hooks
(`trash_collect_sync.cpp:76`, a Func-thunk below the BP VM). The thunk fires
for each load-spawn in order, so the mod can count spawns and bind spawn #k to
objectsData[k] -> `eidByIndex[k]`. Position never enters.

**This is a claim that MUST be probe-confirmed before it is trusted**
(`[[feedback-probe-dont-guess-rule]]`). The probe (read-only, ini-gated,
diagnostic — RULE-2-exempt): during a client load, log each BeginDeferred
thunk fire with its spawn ordinal + the class+transform it carries, and the
final objectsData. Acceptance: (a) the thunk fires for EVERY keyless save-load
spawn; (b) the fire order matches objectsData index order (accounting for the
pre-spawn dedup at `loadObjects`[229] and the post-spawn `K2_DestroyActor`
dedup at [277] — destroyed spawns simply don't bind). If both hold ->
spawn-order is the deterministic primary. If the thunk does NOT fire during
load, or the order diverges -> fall to §3.3.

### 3.3 KEYLESS fallback -> EXACT (bit-identical) transform bijection (deterministic, NOT fuzzy)
If spawn-order proves untrustworthy, correlate by transform — but note this is
NOT the fuzzy, collision-prone match that caused the bugs. At BeginDeferred /
post-load, the loaded actor sits at `objectsData[k].transform` **bit-for-bit**
(the same blob set both the host's `getData` transform and the client's
`loadData` transform). So the correlation is an EXACT-VALUE bijection,
**class-scoped** (only chipPile entries <-> chipPile actors), within ONE blob.
Two distinct piles have distinct exact positions in that blob -> unambiguous.
Contrast the bug class: today's match is FUZZY (1cm/30cm/500cm radii) at
RECONCILE time ACROSS classes (pile near kerfur -> collision). This bind is
EXACT, same-class, same-blob, at LOAD time, and **everything downstream is
eid-only**. The one residual position-touch is the bind itself; it is a
deterministic bijection, not a guess.

**Honest statement (no corner cut):** the keyless path is not literally
"zero position-touch." It is "position-touch only as a deterministic
bit-exact, same-class, same-blob BIJECTION at the bind step, then eid forever."
That is strictly better than the status quo and is the best achievable given
the engine seams (BP-internal spawns are ProcessEvent-invisible; keyless forms
have no key). §3.2 (spawn-order) removes even that residual touch IF the probe
passes — so we pursue §3.2 first and keep §3.3 as the proven-deterministic
backstop.

---

## 3.6 Kerfur off-prop forms — the SAME keyless class, the SAME map covers them

Phase 1 must cover BOTH keyless save-loaded form families with ONE mechanism: chipPiles AND **off-prop
kerfurs** (`prop_kerfurOmega_C`, the inanimate object form). The 2026-06-25 13:21 hands-on proved they
are the same root (live-log evidence, client `Game_0.9.0n_copy`):

- A save off-kerfur `key='jUuC_tdW...'` was **UNTRACKED on the host at join** -- the snapshot enumerated
  3084 Prop Elements but jUuC was not one (only 3 off-kerfurs reached the client: eids 3471/3472/3473).
  So the host never expressed it -> **no client mirror**.
- The client DID load jUuC from the transferred blob, but its native off-kerfur is **not key-indexed in
  the remote_prop maps** (only host-expressed mirrors are indexed) -- the same "local save off-prop NOT
  key-resolvable" fact the kerfur scope-A work already hit.
- When the host later GRABBED jUuC (13:21:56) it minted a late eid 5278 and streamed `PropPose key=jUuC
  eid=5278`; the client logged `no local match (key or eid)` repeatedly -> the pose drove nothing ->
  **the kerfur stayed missing even on grab** (the user's exact symptom).

This is the identical disease to the chipPile eid-0: an untracked-at-join, save-loaded, keyless form
with no stable cross-peer identity. The in-memory `objectsData-index -> eid` map (§2.1) fixes BOTH,
because it is built from the SAVE BLOB's `objectsData` -- which contains EVERY save object regardless of
the runtime Prop-registry tracking. jUuC is in `objectsData`; the map assigns it an eid; the client
binds its local jUuC to that eid at load; the host expresses/poses jUuC by that eid; pose streams match
by eid. The off-kerfur appears + syncs. This SUPERSEDES the kerfur scope-A position retire and the
class+pose fuzzy adoption for save-loaded off-kerfurs (those stay only for runtime off<->active
transitions, not save-loaded identity).

**Coverage note:** the off-kerfur correlation uses §3.1 (by key) when the host DOES express it, and the
§3.2/§3.3 untracked path when it does not -- but crucially the index->eid map gives even the UNTRACKED
ones (jUuC) an identity, which neither the key path nor scope-A could. The kerfur active<->object
transition (off-prop <-> NPC) keeps its own seam (npc channel + scope-A trigger); only the save-loaded
IDENTITY moves to the map. Phase 1 design + tests MUST include an off-kerfur untracked-at-join case
(grab the missing one -> it appears + poses) alongside the chipPile case.

## 4. The 11:16 over-destroy fix — per-class completeness floor (SEPARATE, ships FIRST)

`docs/piles/10`: all 870 keyless piles vanished because the host failed to
EXPRESS/claim them and the claim sweep (`remote_prop_spawn.cpp:1071`) dooms
every unclaimed chipPile UNCONDITIONALLY; the `>50%` abort valve (`:1117`) is
GLOBAL (953/3083 = 31% < 50% -> no abort, even at 100% of piles). A stable ID
does NOT fix this: if the host never expresses the pile, no ID (stable or
otherwise) reaches the client. This is an expression/claim COMPLETENESS failure
and needs its own guard, independent of everything else, shipped FIRST.

**Do NOT use a crude percentage floor** ("never destroy >N% of a class") — it
would block a LEGITIMATE mass-clear (the player really deleted every pile).
The right signal is a **positive per-class completeness handshake**:

- At snapshot finalize, the host walks its live world and sends a per-class
  **census manifest**: `{ class C : liveCount }` (a GUObjectArray count,
  computed INDEPENDENTLY of the per-actor expression path — so a broken
  expression path cannot also corrupt the census).
- The client's claim sweep consults the manifest per class. For class C, doom
  unclaimed locals **only if** `claimedCount[C] >= manifest[C]` (the client has
  accounted for everything the host says exists -> the rest are genuine
  deletions). If `claimedCount[C] < manifest[C]` (the host says there should be
  more than the client matched) -> the snapshot for C is INCOMPLETE -> KEEP the
  unclaimed locals (the missing expressions are in flight or failed; dooming
  would destroy genuine objects). Log the incomplete verdict.
- This distinguishes the two cases the crude floor cannot:
  - **Incomplete (the bug):** manifest says chipPile=870, client claimed 31 ->
    839 unmatched -> KEEP (don't wipe). 11:16 is caught.
  - **Legit clear:** the player deleted all piles -> the host CAPTURED 0 piles
    into the blob -> the client LOADS 0 piles -> nothing to doom anyway
    (manifest=0, loaded=0). No false guard. (In the transfer model the client's
    loaded count always equals the host's captured count — same blob — so
    "client has 870, host genuinely has 0" cannot arise.)

**Invariant this enforces:** every form in the transferred blob must be
accounted for (claimed) before the sweep may doom its class. The over-destroy
was the sweep running ahead of an incomplete expression; the manifest gates it.

Ships as Phase 0 (below), independent of the stable-ID work — it is the
catastrophe guard and must land regardless of how the rest proceeds.

---

## 5. self-seed-eid stays the FOUNDATION (not retired)

The `docs/piles/09` self-seed-at-grab (`trash_collect_sync.cpp:417-423`,
`OnPileGrabPre`) is NOT a position crutch and is NOT retired. The `eid` IS the
stable ID; the self-seed ESTABLISHES that identity at the grab edge — the last
instant the pre-grab pile is alive — so the eid threads through the
pile->clump->repile morph via `RebindE`. A save-loaded pile reaching the grab
edge without an eid has no other window to be identified (its birth predates the
session; only the seed-walk sees it retroactively). What this design CHANGES is
the reconcile **match key**: from save-time POSITION (1cm,
`save_time_retire_util.h`) to **eid**. The pile-09 fix (self-seed + carry the
key on the convert) remains correct as the identity-establishment layer; once
the in-memory map gives the client native its eid, the convert/proxy match by
eid and the in-window move is irrelevant.

---

## 6. Phased migration (do not regress the 3 working instances + instant-world)

- **Phase 0 — per-class completeness floor (§4) + the purge-aware sweep TIMING gate.**
  **STATUS 2026-06-25: PROVEN + PUSHED (origin/main `f83f9635`).** The 11:16 over-destroy
  was RE-DIAGNOSED as a RACE, not a host under-express: the claim sweep fired against a
  MID-PURGE registry (`docs/piles/10`). So Phase 0 is TWO complementary layers, both shipped:
  (1) the **purge-aware quiescence gate** (commit `5e91519a`, `remote_prop_spawn.cpp`
  `kSweepHardCapMs`/`g_sweepLastProgressAt` + the `!HasSeededOnce()||InPurgeEpisode()` hold)
  = the RULE-1 root, so the sweep only adjudicates the fully-reloaded world; (2) the per-class
  **completeness floor** (commit `88a50890`, `snapshot_census` + the `claimed<census` KEEP) =
  the NET for a genuine under-express at true quiescence. **PROVEN by a deterministic A/B**
  (binary `BCDD46DA`, dev `force_chippile_unclaim` injection, one var = floor toggle): floor OFF
  -> 870 piles WIPED; floor ON -> `completeness FLOOR kept 870` + piles survive; BOTH fired at
  full-world (3082/3106 in-universe) via `load tail quiesced`. (The 14:11 PATH-B-INCONCLUSIVE
  attempt + the "871->88 collapse OPEN ROOT" are RESOLVED -- it was the mid-purge race the timing
  fix now closes; see `docs/piles/10` "FIX PROVEN".) Caveat: autonomous=rendering-blind (floor
  KEEP is engine-truth from the re-seed pile count); a hands-on visual confirm is optional.
- **Phase 1 — host map build. STATUS 2026-06-26: BUILT+VERIFIED, but the build approach CHANGED (1B
  gather → PATH A parse).** Probes (read-only, all PASS): **1A** (`spawn_order_probe`) thunk catches every
  keyless load-spawn; **§8.2** (`eid_lifetime_trace`) capture-eid == wire-eid (874/874). **1B FALSIFIED in
  practice:** `BuildHostMap` first replayed a LIVE `GetAllActorsWithInterface(int_save_C)` re-gather and used
  the gather ordinal as the index — the 2b smoke proved the live GUObjectArray order (chip-first) != the
  saved objectsData order (kerfur-first), so the bind's family tripwire fired at k=0 (`save_identity_map.cpp`
  header documents this). **PATH A (the fix, commit `6e0ede3b`, binary `F0D8951F`):** `BuildHostMap` reads the
  LIVE save arrays in load order — `saveSlot.objectsData` (off-kerfurs, `Fstruct_save` stride 0x100) THEN
  `saveSlot.primitivesData` (chipPiles, `Fstruct_primitiveSave` stride 0x60 — chipPile.ignoreSave==TRUE so it
  persists via the dedicated primitives path, NOT objectsData), filters keyless chip/kerfur by class, joins
  each to its host eid by a host-LOCAL exact class+location match (`CollectTracked{Pile,Kerfur}Transforms`).
  VERIFIED: host map 874, 0 unmatched/undrained/ambiguous, correct order.
- **Phase 2a — sidecar transport. VERIFIED (commit `c9f8babd`, binary `4E7552AF`).** The IdMap rides the
  `save_transfer` blob SIDECAR (decision (a), PREPENDED in-band: one stream, one CRC `0xB9971117` — cannot
  desync; `SaveTransferBeginPayload.sidecarBytes`). Smoke: client received 874 entries byte-identical,
  `.sav` stripped + loaded clean. Gated `[dev] save_identity_map_log=1`.
- **Phase 2b — client eid-range bind. VERIFIED at MECHANISM level, NOT end-to-end (commit `06cc2c6a`).**
  `coop/save_identity_bind.{h,cpp}`: the k-th keyless BeginDeferred load-spawn binds to the host map's k-th
  entry (`UnmarkKnownKeyedProp` retire local + `RegisterPropMirror` host-eid + `MarkBoundMirrorNative` guard so
  the re-seed won't re-localize). Re-smoke: **874/874 bound, tripwire SILENT, all case(i) E-free**, free-win
  confirmed (sweep doomed zero chipPiles). **END-TO-END BLOCKED:** the bound natives are then destroyed by the
  host-proxy reconcile (`[PILE] DESTROY native twin eid=2275 … proxy is the sole mirror`) → `totalLiveNatives=0`.
  Gated `[dev] save_identity_bind=1`. Root RE'd: the autonomous purge is VOTV's intrinsic `loadObjects` rebuild
  (host SP too); the real killer is `TryDestroyTwin`, not a pre-quiescence race (bound eids == destroyed eids).
- **(X) NATIVE-AUTHORITATIVE — the end-to-end fix. DESIGN ON REVIEW (zero code), all mechanisms RE/probe-proven.**
  `research/findings/phase1-X-native-authoritative-chippile-DESIGN-2026-06-25.md`. Keep the native as the
  host-eid mirror; **per-eid SUPPRESS the proxy** (2 guards on `IsBoundMirrorNative`: skip `SpawnProxy` + skip
  `TryDestroyTwin`; + `RetireProxy` for the race) — NOT a wholesale delete (runtime/host-only piles + clumps
  still need the proxy). This RESTORES the native interaction window the proxy lacks (proxy = bare
  `AStaticMeshActor`, no `lookAtActor`/collision/occlusion — through-wall grab, walk-through). Grab routes via
  `GrabIntent` (host-auth); the native local `playerGrabbed` is suppressed by **clearing `lookAtActor` on the
  `OnPileGrabPre` edge** (probe-proven clean: read fresh after the PRE observer, self-heals next trace tick).
  KEEP Path-A (quiescence-pivot DROPPED — evidence: the bind survives the rebuild). Blast radius NIL (per-eid).
  Pulls Phase 4 (retire proxy for save-loaded piles) onto the critical path because of the INTERACTION gap.
- **Phase 3 — reconcile matches by eid.** After (X), the bound native IS the eid mirror; eid-equality reconcile.
- **Phase 4 — retire the position layer (RULE 2).** Only after the eid path is hands-on-verified across ~a
  dozen sessions. (X) already retires the camera-cone + proxy/`TryDestroyTwin` for bound save-loaded piles.

The 3 verified instances (L1 pile, kerfur fuzzy-gate, kerfur forward) and
instant-world are NOT touched until Phase 4, and only then on proof.

---

## 7. Backward-compat — TRIVIAL for the in-memory design

For the RECOMMENDED in-memory map there is **nothing to test for save
compatibility**: the mod writes nothing to disk for identity, the vanilla
`.sav` is byte-untouched (already true), mod-removed loads vanilla by
construction, there is no file to orphan, no CRC to go stale. The matrix is
empty by design.

The disk side-car matrix (mod->vanilla loads; orphan harmless; vanilla-save
falls back; mod->vanilla->mod re-picks-up; CRC stale-detect) is retained in the
git history of this section ONLY for the day client-side persistence is wanted
(§2.2). It is NOT in scope now.

---

## 8. Scope

EVOLUTION (weeks, phased), not a rewrite. Reused: `save_transfer` pipeline,
the element registry + `prop_element_tracker`, the quiescence gate
(`HasLoadTailQuiesced`), the existing BeginDeferred native Func-thunk, the
`elementId` field already on every channel. New: one reliable packet
(`eidByIndex`), the host map-build at capture, the client bind pass, the
per-class manifest + floor. One new file per the modular rule
(`coop/save_identity_map.{h,cpp}` or similar), ~300-400 LOC, soft-cap clean.

---

## 9. Open items before any build (greenlight gates)

1. **§3.2 spawn-order probe** — **DONE 2026-06-25, PASS.** The read-only probe
   (`coop/dev/spawn_order_probe.{h,cpp}`, binary `E4B7B3C3`, 3 autonomous runs)
   showed the BeginDeferred thunk catches EVERY keyless load-spawn: chipPile
   fired=870 (full field), kerfurOff 4/4 CAUGHT-ALL (missed=0). Spawn-order is
   the deterministic PRIMARY; §3.3 bijection stays as the backstop. (Detail +
   the separate "client piles purge-not-reload" observation:
   `research/findings/phase1-save-identity-map-BUILD-PLAN-2026-06-25.md` 1A RESULT.)
2. **§4 manifest source** — confirm a host-side per-class GUObjectArray census
   at snapshot-finalize is cheap + independent of the expression path (it must
   not share the failure mode it guards).
3. **eid lifetime across the bracket** — confirm the host eid assigned at
   capture is the SAME eid the host later expresses on the wire (so the client's
   index-bound eid matches the proxy/convert eid). Trace `elementId` continuity.

Code is ZERO until the user greenlights a specific phase. This doc fixes the
plan; it does not authorize a build.

---

## Source map
`save_capture.cpp:35-145` (live capture, reads objectsData Num) ·
`docs/piles/re-artifacts/saveObjects_dump.txt` ([32] gather, [44] ignoreSave,
[53-54] getData+AddUnique, [93] objectsData=copy1) ·
`docs/piles/re-artifacts/loadObjects_dump.txt` ([139] FinishSpawningActor,
[150] data=objectsData, [223-291] per-index load loop, [265] loadData,
[269-277] key+transform dedup) · `trash_collect_sync.cpp:76` (BeginDeferred
Func-thunk) · `:417-423` (grab-edge self-seed, docs/piles/09) ·
`remote_prop_spawn.cpp:1071/1117` (claim sweep unconditional doom + GLOBAL
valve, docs/piles/10) · `prop_element_tracker` (GetPropElementIdForActor,
the eid<->actor maps) · `save_time_retire_util.h` (the position kernel being
superseded) · `docs/COOP_MIRROR_IDENTITY_WINDOW_RACE.md` (the class) ·
`docs/piles/09`, `docs/piles/10` · `votv-save-path-RE-2026-05-30.md` (save path,
ignoreSave opt-out, GVAS on-disk) · IDA read-only RE 2026-06-25 (stock GVAS,
no custom serializer, FName key@0x40 anchor).
