# Pile sync — SAVE-STRIP + single-source host stream — DESIGN (2026-06-20, session 32)

> **`[corr 2026-09-04: docs census sweep]` DURABLE RECORD, SUPERSEDED IN OUTCOME.** Its measurements
> stand as of its date. `[V]` What it was written to inform has since been replaced: the pile mirror
> went PROXY -> NATIVE on 2026-06-30 (`docs/piles/11`, "nativization AS-BUILT + VERIFIED (hands-on)
> ... a rooted real `actorChipPile_C` native"), and the arc then ran on into crutch **C2** and the
> 2026-09-01 rejoin-crash fix. `[V]` Its file paths are pre-refactor -- `prop_adoption.cpp`,
> `pile_handle.cpp`, `pile_claim.cpp`, `pile_seed.cpp`, `prop_spawn_core.cpp` and `freeze_probe.cpp`
> are gone from the tree. Read it as evidence from June 2026, never as current design.

Supersedes the thin-client doom (sessions 30-31, DEPLOYED + FAILED in the user's real
hands-on: doom wedged `liveMirrors=758 < expected=868`, never fired, dupes persisted).
Root cause + direction were RE-decided this session; see
`pile-dupe-rootcause-RE-2026-06-20.md` (the 6-agent memo) THEN the two decisive RE
agents that OVERTURNED its A' recommendation.

## Why this, in one paragraph

The joiner loads the host's LIVE-captured save, which already contains every ambient pile
(`actorChipPile_C`) at correct positions ("nothing to reconcile" was the original intent,
`save_capture.h:14-16`). The dupe is STRUCTURAL: the coop layer ALSO streams those piles
as host mirrors -> save-loaded pile + streamed mirror = 2 piles. Every fix since s22 tried
to reconcile the two sources; the latest (destroy the save-originals via a count-gated
"doom") wedged because it needs the host mirror set to fully arrive, which it never does
after the join shadow-drain. **Bytecode RE proved piles have NO game-level identity** (no
Key field; `setKey` is a no-op the BP discards; persisted as `primitivesData` records of
class+transform+chipType; loaded by destroy-all-int_primitive + fresh POSITIONAL respawn).
So you cannot give a kept save-pile a stable shared id (the only candidate is position,
the rotting key that already failed s23-24). The ONLY stable identity a pile can have is a
HOST-MINTED eid carried over the coop wire. Therefore: make the host stream the SOLE source
(MTA `CEntityAddPacket`+`ElementID` model) and remove the second source by STRIPPING piles
from the transferred save. No save-original -> no doom -> no positional matching -> no dupe;
host grabs/morphs address the one instance cleanly ("old piles don't morph" fixed).

## Verified RE facts (do NOT re-derive)

- `actorChipPile_C` implements BOTH `int_save_C` (-> `objectsData`@0x300, key="None" record)
  AND `int_primitive_C` (-> `primitivesData`@0xE30, class+transform+chipType-json record).
- LOAD: `loadObjects` spawns a pile per objectsData record; then (at its own `[211]`) calls
  "Load Primitives" which DESTROYS every live int_primitive actor and RESPAWNS positionally
  from `primitivesData`. **=> the surviving piles come from `primitivesData`. Stripping
  `primitivesData` alone makes the joiner load ZERO piles** (the transient objectsData
  spawns are destroyed by Load Primitives). We strip BOTH arrays for cleanliness, but
  primitivesData is the load-decisive one.
- `setIgnoreSave(true)` gates ONLY the int_save walk (objectsData). The Save-Primitives walk
  has NO skip gate -> setIgnoreSave alone is INSUFFICIENT.
- `saveObjects` internally calls "Save Primitives" + "saveHoldObj" -> save_capture's single
  `saveObjects` call DOES repopulate primitivesData fresh (NOT stale).
- Struct layouts (two-source verified): `struct_save` stride **0x100**, class(ClassProperty)
  @0x00, transform@0x10, key(FName)@0x40. `struct_primitiveSave` stride **0x60**, class@0x00,
  transform@0x10, key@0x40, json(FString)@0x48. `saveSlot_C`: objectsData TArray<struct_save>
  @0x300, primitivesData TArray<struct_primitiveSave> @0xE30 (TArray = {Data@0,Num@8,Max@C}).
- int_primitive implementers: `actorChipPile_C` (+ erie/wetConcrete/leaves SUBCLASSES),
  `prop_garbageClump_C` (+ 3 subclasses), `grime_C`. **Filter by `IsChildOf(actorChipPile_C)`,
  NOT exact equality.** Leave grime_C. (garbageClump = the CARRIED form; see scope note.)

## The strip mechanism (host side, leak-free, NON-destructive to live saveSlot)

In `ue_wrap::save_capture`, AFTER `saveObjects` populates and BEFORE `SaveGameToSlot`:
for each target array (primitivesData@0xE30 stride 0x60; objectsData@0x300 stride 0x100):
1. Read the live TArray header `{Data, Num, Max}`.
2. Build a NEW contiguous buffer (std::vector<uint8_t>, kept alive on the stack) holding a
   SHALLOW memcpy of only the elements whose `class`@0x00 is NOT `IsChildOf(actorChipPile_C)`.
   (Shallow = FString/TArray inner pointers are COPIED, ownership SHARED with the live array.)
3. Save the original header; temporarily overwrite the live TArray header to point at the
   filtered buffer: `{buf.data(), keptCount, keptCount}`.
4. Serialize (the engine reads the filtered elements; never frees them).
5. RESTORE the original `{Data, Num, Max}` -> the live saveSlot is byte-for-byte unchanged
   (every pile + its FStrings still owned by the original array). Free only the stack buffer.
This never mutates/destructs a live element -> no leak, no double-free, no corruption of the
host's real save state. (Contrast: in-place compaction would leak ~870 `json` FStrings.)
- Class identity read: `cls = *(void**)(elem + 0x00)`; `R::IsChildOf(cls, chipPileCls)`.
  chipPileCls resolved once via `R::FindClass("actorChipPile_C")`.
- Policy vs mechanism (principle 7): the FILTER mechanism lives in ue_wrap/save_capture; the
  WHICH-classes policy is passed in by the coop caller (save_transfer) as a class-name list.
  So `CaptureLiveWorldToScratchSlot(slot, {L"actorChipPile_C"})` (extensible to kerfurs later).
- Sanity: after the strip, log `primitivesData N -> M (stripped K piles)`; assert M < N when
  the host had piles. Restore is in a scope-guard so an early return can't leave the live
  array pointing at a freed buffer.

## SCOPE: piles now; clumps + kerfurs noted

- **In scope:** ambient `actorChipPile_C` (+ subclasses) -- the ~870-pile dupe. STRIP these.
- **Carried clumps (`prop_garbageClump_C`):** a resting clump in the save is rare; it is
  Aprop_C-derived and MAY be keyed. Decide separately -- do NOT strip in v1 unless RE shows
  it dupes. (The pile_handle relay already host-authoritatively handles a peer carrying a
  clump.)
- **Kerfurs: NOT a strip.** Stripping kerfurs regresses the camera-safe local-twin adoption
  (v74 floating-camera on fresh-spawn). The kerfur dupe is a staleness/form-mismatch race;
  its fix is form-agnostic adopt-then-`KerfurConvert` (separate workstream). DO NOT touch
  npc_adoption / kerfur_* here. g_ghostSwept stays.

## RULE-2 DELETE (the reconcile apparatus -- nothing to reconcile with one source)

- PILE DOOM: `TickPileReconcile`, `DoomNonMirrorChipPiles_`, `ArmPileDoom`,
  `ArmPileDoomForConnect`, the `liveMirrors<expected` gate, `CountLoadTailUnsettled_`
  pile-inclusion (prop_adoption.cpp). [KEEP the keyed divergence sweep + its quiescence, but
  re-derive a pile-independent quiescence -- see KEEP note.]
- `g_hostPileCatalog` + `CataloguePileMirror` / `ForgetPileCatalogEntry`.
- `g_pendingRemoveEids` + `EnqueuePendingRemove` / `ConsumePendingRemove`.
- OnSpawn pile-catalog + pending-remove block (remote_prop_spawn.cpp ~797-818).
- OnDestroy pending-remove ordering (remote_prop.cpp ~932-949) -> revert to plain destroy.
- net_pump drain-edge `ArmPileDoom` + connect `ArmPileDoomForConnect` calls; event_feed arm.
- P1 mint-gate CHIPPILE half (prop_element_tracker.cpp) -- with strip the client has NO save
  pile to mint; the kerfur half STAYS. (`SnapshotReceivedMirrorActors` -> delete if only the
  doom used it.)

## KEEP (the single-source stream + relay + drain recovery)

- save-transfer + native `loadObjects` (now strips piles at capture).
- Host->client pile mirror STREAM (PropSpawn/RegisterPropMirror) -- the SOLE pile source.
- `pile_handle.cpp` grab/throw RELAY -- peer interactions; its identity resolver
  (`ResolveMirrorEidByActor`/host eid) is now UNAMBIGUOUS (no save-original shadows a mirror).
- PileResyncRequest/PileResyncComplete + the host chunked re-stream -- the DRAIN RECOVERY
  (the join shadow-drain's Load Primitives wipes the mirror piles -> client re-requests ->
  host re-streams). Now race-free: there is no save-original to dupe against, so the gate is
  just "request -> receive", NO doom, NO count gate.
- mass-purge drain-edge DETECTION (net_pump).
- KEYED divergence sweep + quiescence: it consumed pile-inclusive `CountLoadTailUnsettled_`.
  After deleting the pile doom, ensure the keyed sweep's quiescence does NOT depend on piles
  (piles are no longer client-loaded) -- re-derive a keyed-only quiescence or confirm the
  pile term is now a constant 0. MUST verify so the keyed sweep doesn't fire early.

## Drain recovery, restated (why the bridge stays + is now clean)

On the join, a same-world mass-purge (Load Primitives re-run) destroys ALL int_primitive
actors. After strip, the joiner's only piles are host MIRRORS (also int_primitive) -> they
get wiped, and primitivesData has no piles to respawn -> joiner shows 0 piles briefly ->
client detects the drain edge -> sends PileResyncRequest -> host re-streams its live piles
-> mirrors restored. No save-original ever races this. Transient "piles blink then refill"
on the drain is acceptable; a dupe is not.

## Verification (user hands-on; Claude prepares ground, never launches while user is on PC)

Markers to add/keep: `save_capture: stripped K chipPile record(s) from primitivesData
(N->M) + objectsData`; host `re-streamed N live pile(s)`; client census `piles: mirrors=M
save-origins=0`. Pass = client pile count == host, NO duplicates accumulating, host grab
removes the client pile, FPS normal, no doom log lines at all (the doom is deleted).

## Open risks (audit must check)

1. struct strides 0x100/0x60 must be used for element walking (raw 0xF8/0x58 = AV on elem>=2)
   -- the `tarray-stride-aligned-not-raw-size` rule. Verified by saveSlot member sizes.
2. The non-destructive copy-swap MUST restore the header on EVERY exit path (scope guard).
3. Confirm the keyed divergence-sweep quiescence no longer needs pile-inclusion.
4. Confirm no game system spawns ambient piles on the CLIENT (trash gen is host-authoritative
   / client-never-authors -- P2 already shipped).
5. trashPilesData@0x818 is ALSO TArray<struct_save> -- confirm saveObjects does NOT route
   piles there (RE showed objectsData+primitivesData only); if it does, strip it too.
