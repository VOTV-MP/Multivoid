# Live host-world save on join (root-cause kerfur save-transfer dupe fix) -- RE + AS BUILT (2026-06-15)

> **REVERTED (`68767d25`) THEN RE-ENABLED WITH THE FIX (`145dab9e` re-apply + `f5397a4e` fix;
> deployed SHA `66DBFF0F`).** First hands-on lost ~2979 props. Root cause (logs): the divergence
> sweep destroys props the connect-snapshot doesn't claim, but it fires on the CLIENT's load-tail
> quiescence while the HOST streams the snapshot in chunks over many ticks -- AND the host sent a
> PARTIAL first bracket (88 props, `SnapshotBegin.propTotal=88`) then re-seeded. The sweep ran with
> 88 claimed and ate the other 2979 just-loaded props (`claim sweep -- 3229 live, 88 claimed, 2979
> destroyed`). The stale build got lucky on timing (3050 claimed first, only 1020 swept). The host
> capture itself was PERFECT (`objectsData 3287 -> 3319`, 19MB, every prop converged `d=0.00cm`).
>
> **THE FIX (`f5397a4e`), deeper than timing:** the divergence sweep was built for the FRESH-BOOT
> join model (client New-Games -> has its own litter to remove). For a SAVE-TRANSFER join the client
> loads the host's world via the game's own loadObjects -- it has NO litter; with a LIVE save it
> loaded the host's EXACT current world, so there is ZERO divergent baseline and the sweep is
> reconciling nothing. So: a `liveCaptured` flag on SaveTransferBegin (reuses a pad byte, no size
> change); on a live-capture join the client SKIPS claim tracking + the prop divergence sweep + the
> NPC ghost sweep (`npc_adoption::OnSnapshotComplete(skipGhostSweep=true)` latches the sweep done --
> ADOPTION still binds the kerfur twin). The host's ongoing PropDestroy/PropPose streams keep it in
> sync; convergence still runs. PLUS a SAFETY VALVE in the sweep for every OTHER path (stale/fresh):
> abort if it would destroy >50% of in-universe props (a legit divergence is a small delta; >50% =
> an incomplete/racing snapshot, not divergence). Section 5/6 below describe the live capture itself
> (unchanged, correct). HANDS-ON PENDING on SHA 66DBFF0F.

Session 18 (post-compact). Shipped commit `de10514c`, deployed SHA `806699DB`, REVERTED `68767d25`.
Supersedes the session-18 client-side reconcile (deferred divergence sweep, commit `52b1d94d`)
as the PRIMARY fix for the kerfur save-transfer dupe -- that reconcile layer now survives as the
fallback/safety-net, not the load-bearing fix. See also
`votv-host-world-snapshot-RE-and-design-2026-06-04.md` (the host-authoritative-world direction
this completes) and `votv-kerfur-savetransfer-ghost-prop-RCA-2026-06-15.md` (the prior RCA).

User direction that drove this:
- "are agents just less smart versions of you" + "you're gonna burn tokens again and not achieve
  anything" -> stop the agent swarm; read the log + code directly (I had the evidence in hand).
- "we have IDA, we have BLUEPRINTS LOGIC, can we just fix this properly per rule 1".
- "Can we build our own save constructor/deconstructor to live construct on client connection".
- "we can save the host's world upon new client's connection, but make sure the save is not REAL
  in terms of overwriting the original host's slot."
- (answer to the mechanism question) "the dupe can be fixed simply by our existing logic when
  syncing-mirroring objects ... the kerfur object is a non-existent object which needs a sweep on
  the client side (we have functionality for that)."

---

## 1. THE LOG-PROVEN DIAGNOSIS (client log, Game_0.9.0n_copy, the failed 14:13 test)

The session-18 deferred sweep DID fire (`divergence sweep FIRING (load tail quiesced; 2161ms
after arm)`), but the dupe was actually TWO coupled failures, both visible in one log:

- **(A) ghost off-prop:** `sweep SKIPPED 161 keyless unclaimed actor(s) (top: 109 x 'prop_C')`.
  My session-18 comment claimed "every load-tail straggler has its key by quiescence" -- the log
  proves 161 props are STILL keyless at quiescence, so the keyless-skip (the original adoption
  hole) still spares the stale off-kerfur.
- **(B) missing alive NPC (NEW):** `npc-adopt eid=5255 kerfurOmega_C -- no local twin (save load
  tail quiesced); fresh-spawning a mirror` -> `BeginDeferred returned null for 'kerfurOmega_C'
  ... suppressor swallowed it? bypass slot not consumed?`. The client's fresh-spawn of the alive
  NPC mirror is EATEN by our own npc suppressor (kerfurOmega_C is on the allowlist). So the client
  can't show the alive kerfur even when adoption decides correctly.

Host had 2 alive kerfurOmega NPCs (eid 5255/5256) + 3 OFF props (replayed as prop_kerfurOmega_C
mirrors). The client saw an off-prop where the host had an NPC = the user's "client sees object
kerfur, host has alive kerfur."

## 2. ROOT CAUSE (one defect, both failures): the snapshot is STALE, not the reconcile

`save_transfer.cpp:105` (old TryCaptureBlob_) shipped the host's STALE on-disk `<g_hostSlot>.sav`
from disk. The kerfur the host turned ON after its last autosave is in that file as a turned-OFF
prop. So the client materialized a stale off-prop (A) AND the host streamed the live NPC, which
the client couldn't spawn (B). The whole reconcile layer (sweep + adoption + suppressor bypass)
exists ONLY to paper over that staleness.

The game's OWN save/load already round-trips a turned-on kerfur perfectly -- it's an `int_save_C`
actor (kerfurOmega.json: int_save_C + loadData + getKey + assignKey + type), and SP
save->quit->reload keeps it on. The ONLY defect is that coop snapshots a stale file instead of
live state.

## 3. THE FIX: ship a LIVE host-world save (game's own serializer, throwaway slot)

On a client's `SaveTransferRequest`, the host now serializes its LIVE world and ships THAT.

`ue_wrap/save_capture.{h,cpp}` -- `CaptureLiveWorldToScratchSlot(scratchName)` (game thread):
1. `mainGamemode::saveObjects(false)` + `saveTriggers()` -- repopulate the in-memory
   `mainGamemode.saveSlot` (@0x4B0) from LIVE actors. saveObjects walks every int_save_C world
   actor (props + NPCs incl. the on-kerfur, captured as its live NPC state). We deliberately SKIP
   the player-state populates (playerTransform/inventory/heldObj): the joiner overrides those and
   each has its own live coop sync channel (balance/time/signals/inventory).
2. `GameplayStatics::SaveGameToSlot(saveSlot, scratchName="zcoop_hostxfer", 0)` -- the slot NAME
   is OUR parameter, so the host's canonical slot is never named, opened, or written.
3. `save_transfer::OnRequest` reads the scratch .sav, deletes it, streams the bytes (new
   `BeginStreamFromBlob_` helper, shared with the canonical fallback).

**No client-side change.** With a live save, the client's native loadObjects() spawns the
on-kerfur as a live NPC via EX_CallMath (bypasses the suppressor -- that's the v75 save-loaded-NPC
path). The host's connect-replay EntitySpawn(savePersisted=1) then routes to
`npc_adoption::ArmAdoption` -> the deferred class-match poll BINDS that local twin -> no
fresh-spawn (kills B), no ghost off-prop in the save (kills A). The existing reconcile layer just
works because the base is finally accurate.

## 4. HOST-SAVE SAFETY (the critical invariant) -- agent-audited PASS, 3 independent proofs

1. The scratch name never aliases the canonical slot: `SaveGameToSlot` takes the name as a param,
   and only `"zcoop_hostxfer"` is ever passed; `g_hostSlot` is never handed to it.
2. On the host, `SaveGameToSlot` is the STOCK engine serializer -- `save_block::Install`
   early-returns for `role()!=Client` ("Host: never install", save_block.cpp:106). No detour.
3. The saveObjects/saveTriggers populate can't corrupt the host's NEXT autosave: it writes the
   same in-memory saveSlot a normal autosave repopulates immediately before serializing, and the
   next autosave rebuilds it from scratch. Even if saveObjects APPENDED, the damage is contained
   to the scratch blob (the host's slot is still never written).

**saveObjects rebuild-vs-append** is the one assumption: it must rebuild objectsData, not append,
for a standalone call to be correct. Evidence it rebuilds: it builds a local `copy1`
TArray<struct_save> then assigns to the member (build-local-then-replace pattern,
UE4SS_ObjectDump_GAMEPLAY_SAVE.txt:275639); and SP save/reload never duplicates the world. A
live before/after `objectsData.Num` probe (save_capture.cpp) confirms it at runtime and WARNs on
>1.5x growth. Failure mode if wrong = a doubled SCRATCH blob (over-populates the joiner only;
host-safe; obvious; one-line fix to add an explicit clear).

## 5. AS BUILT -- the fix is conditional (honest framing)

- **Live-capture succeeds (the normal path):** the dupe is eliminated AT THE SOURCE -- the kerfur
  arrives as an NPC on both sides, no off-prop ghost, adoption binds the local twin.
- **Live-capture fails (gamemode not live / UFunctions unresolved / scratch unreadable -- a
  should-never-happen while a client is mid-join):** `OnRequest` falls back to the canonical
  stale slot via the torn-read guard, and the client relies on the prior reconcile-based
  mitigation (deferred sweep + adoption). NOT a regression -- it's the pre-fix behavior, logged
  loudly (UE_LOGW). So: "live-capture path eliminates the dupe; stale fallback retains the prior
  reconcile mitigation," not an unconditional fix.

Perf: one-shot per join (saveObjects over ~3000 actors + ~17MB SaveGameToSlot + 17MB read+CRC on
the game thread = ~one autosave's cost). Audited NOT a per-frame path (`SaveTransferRequest` is
sent once, `g_cliRequested`-latched). Thread-safe (net_pump::Tick asserts game thread; no
concurrent-join scratch collision -- OnRequest runs to completion in the sequential reliable
drain). Leak-free. No protocol change.

## 6. RUNBOOK (user hands-on, deployed SHA 806699DB x4) -- no ini change, automatic

1. Host: load a save (or New Game), turn ONE kerfur ON, do NOT re-save.
2. Client: join (save-transfer is automatic on connect).
3. Expect: one active kerfur on the client matching the host, ZERO ghost off-prop, turn-off works.

Host log markers:
- `save_capture: objectsData repopulated N -> M live world object(s)` -- **M should be ~N**
  (clean rebuild). M ~ 2*N => saveObjects appends => add an explicit clear (the one open risk).
- `save_transfer: slot X streaming LIVE host world (...bytes...)`.
- (failure) `save_transfer: slot X -- LIVE capture unavailable; falling back to canonical slot` =>
  capture failed; diagnose why (gamemode not live? UFunction unresolved?).

Client: the kerfur loads as an NPC and npc_adoption ADOPTS it (no `BeginDeferred returned null`,
no divergence-sweep ghost).

## 7. OPEN / DEFERRED
- Confirm the objectsData.Num probe reads M~N on the first hands-on (the rebuild assumption).
- If a fully-live transfer is wanted later, the player-state populates (day/time/signals) are
  currently left at last-autosave and corrected by their live channels -- fine for now.
- The reconcile layer (remote_prop_spawn divergence sweep 1176 LOC >800, npc_adoption) stays as
  the fallback; the session-18 prop_divergence_sweep.{h,cpp} extraction flag still stands.
