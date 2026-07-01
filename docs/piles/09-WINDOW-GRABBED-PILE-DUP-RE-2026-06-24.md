# 09 -- window-GRABBED/moved pile dup (eid 5283, 17:23) -- ROOT RE

> **2026-07-01 UPDATE: this doc covers the SINGLE moved-pile dup. The MASS version (host clears a whole
> cluster in-window → ~5 stale piles @old) is a distinct class — see [12-WINDOW-MASSMOVE-DUP-2026-07-01.md](12-WINDOW-MASSMOVE-DUP-2026-07-01.md).
> Note: `SweepReconcileSaveTimeTwins` (referenced below) was REWRITTEN 2026-07-01 (`46e35edd`) to a per-eid
> CONFIRMED-move retire with the `>50%` cap as a fallback + unconfirmed twins kept pending — the old aggregate
> "matched-then-capped-then-cleared" description is superseded.**

**Status (2026-06-26): the window-moved-pile dup is FIXED + HANDS-ON VERIFIED, but by a DIFFERENT mechanism
than this doc's original f837fbad approach.** The `matchPos`/`ArmPendingSaveTimeTwin` machinery this doc
designed DID land (commit `08e35d77`, in the pushed stack — the host still stamps `+saveTimeKey docs/piles/09`
on the in-window kToPile LAND). But the actual DUP cure is now the **(X) native-authoritative** model +
**#2 proxy-wins** (`save_identity_bind.cpp`, commit `acc416eb`): when a convert touched the eid in-window
(`CtxForEid>0`), the bind keeps the host-authored proxy and retires the redundant save-loaded native, instead
of binding native@old. **VERIFIED hands-on 2026-06-26 15:42** (`PROXY-WINS ... case(ii)-converted`, chipPile
overflow=0, no dup). The remaining edge was positional only (the moved pile rendered at the old spot until
interaction) — tracked as **b2/b3** (AS-BUILT commit `2829ce6d`) — **now ROOT-FIXED + HANDS-ON VERIFIED
2026-06-30** (commit `a6242cfe`, deployed `C726D533`): the b3 quiescence pos-correction fired but its
`SetActorLocation` silently NO-OP'd because a save-loaded `actorChipPile_C` native is **STATIC mobility** (the
`SetActorRootMovable`-before-teleport fix — see the 2026-06-30 section at the bottom of this doc). The earlier
b2 `2829ce6d` log-attempt was observability, not the cure; the mobility no-op was the actual root.
Original b2 RE: `research/findings/coop-grab-throw-and-join-window-bind-RE-2026-06-26.md`. The `f837fbad` MD5 below is a dead
ancestor (its content re-landed via `08e35d77`); read the RE below for the original diagnosis, but the cure is
#2. The 4TH mirror-identity window-race instance; same CLASS as L1.

> **FIX AS-BUILT 2026-06-25 (uncommitted->committed this session; NOT deployed; HELD pending the 11:16
> over-destroy root — see `docs/piles/10`).** Six changes implement the design in §Fix direction:
> (1) `protocol.h` PropConvertPayload 112->124 (+`hasMatchPos`+`matchX/Y/Z`), proto 88->**89**;
> (2) `trash_collect_sync.cpp::OnPileGrabPre` self-seeds the eid (`MarkPropElement`, take-4 pattern) +
> `RecordGrabTimePileXform(preGrabLoc)` when the aimed pile is UNTRACKED; (3) `save_transfer.{h,cpp}`
> `RecordGrabTimePileXform` + `TryGetSaveTimePileXformAnySlot`; (4) `trash_channel.cpp::BroadcastConvert`
> stamps the save-time key on the kToPile LAND; (5) `remote_prop.cpp::OnConvert` arms a pending twin
> when a kToPile carries `hasMatchPos`; (6) `pile_reconcile.{h,cpp}` `ArmPendingSaveTimeTwin` (feeds the
> bracket-independent `SweepReconcileSaveTimeTwins`, the shared kernel). It COMPILES (f837fbad); it is
> NOT verified (no deploy, no hands-on). **Why held:** the 2026-06-25 11:16 hands-on surfaced a WORSE,
> SEPARATE bug — pile×kerfur in-window -> ALL piles UNCLAIMED -> a valve-free claim sweep mass-destroyed
> all 870 (`docs/piles/10`). That over-destroy is orthogonal to this fix (this fix adds a key to the
> convert; the over-destroy is the piles never being EXPRESSED/claimed at all), so this fix neither
> causes nor cures it — but the over-destroy is more dangerous and is diagnosed first. A blueprint agent
> (feature-dev:code-explorer) mapped the change; its "PropSpawn is the primary carrier, skip the client
> OnConvert" conclusion was CORRECTED here — the connect-snapshot is built PRE-grab, so the CONVERT is
> load-bearing and the client OnConvert MUST arm the twin (instrumented so a mid-window reliable-deliver
> failure shows in the log, the kerfur-scope-A lesson). VERIFY GATE before trusting it:
> `nearestNative_d` is no longer `NONE`, `[PILE-09] CLIENT armed pending save-time twin`, then
> `[PILE-1C] sweep-reconcile` removes the moved native; AND L1 unregressed; AND NO over-destroy.

## The scenario
During the client's join-load window, the **HOST** grabs a chipPile, moves it near the kerfurs, drops
it -> it re-piles. On the client the pile DUPS. Host log: `[PILE] HOST RE-PILE(thunk) eid=5283` (a
HIGH/late eid = created mid-session, post-blob). Client log: `[PILE-DELTA] eid=5283 ...
nearestNative_d=NONE (no live native in the index)`.

## Root (lifecycle trace, file:line)

1. **Host grabs a pile that is UNTRACKED at grab time.** `OnPileGrabPre` (`trash_collect_sync.cpp:339-411`)
   resolves the aimed pile's eid (`fwdEid = GetPropElementIdForActor`, `mirEid = ResolveMirrorEidByActor`,
   :396-397) and arms `NotePendingGrab` ONLY if `grabEid != kInvalidId` (:408-411). The grabbed pile had
   **no eid** (freshly settled / lost in the menu->game prop-element mass-purge the connect window
   straddles -- the same purge-vs-reseed gap as the take-3/4 RE) -> `NotePendingGrab` SKIPPED. **This is the
   upstream defect: a grabbable pile was untracked.**

2. **The clump rides with eid 0.** BP morphs the pile to `prop_garbageClump_C`. The held-edge
   `AdoptPendingGrabClump` (`local_streams.cpp:313-333` -> `trash_channel.cpp:234`) finds no pending grab
   -> returns `kInvalidId`; `EnsureHeldItemBroadcast` early-returns for any garbageClump
   (`trash_collect_sync.cpp:156`). So the host never PropSpawns the clump -- **no cross-peer identity yet.**

3. **Host drops -> re-pile mints a NEW eid.** The clump's `BeginDeferredActorSpawnFromClass(chipPile)`
   fires `OnBeginDeferredSpawnObserve` (`trash_collect_sync.cpp:75-99`), but `E = GetPropElementIdForActor`
   on the eid-less clump is `kInvalidId` -> early-returns at :82 ("UNTRACKED clump, eid=0 gap -> skip"). **No
   convert path for the new pile here.**

4. **The new pile is announced by the periodic RE-SEED, not the convert.** The new live `actorChipPile_C`
   trips the steady-world re-seed (`net_pump.cpp:617-626`): `ReSeedKnownKeyedProps` mints it a fresh host
   eid = **5283** (the high value) and `ExpressIncrementalSpawn` broadcasts ONE bracket-free `PropSpawn`
   (`prop_snapshot.cpp:528-560`). Mid-game incremental -> `matchSlot = -1` -> `p.hasMatchPos = 0`
   (`prop_snapshot.cpp:325-332, 548-550`). **No save-time key on the wire.** (The `HOST RE-PILE(thunk)
   eid=5283` line is a LATER convert, once 5283 is tracked and re-grabbed.)

## The dup mechanism (client side)
- Client receives PropSpawn eid 5283 -> `remote_prop_spawn::OnSpawn` trash-proxy branch (:308-313) ->
  `SpawnProxy(5283)` = one proxy @NEW position.
- In-bracket it calls `TryDestroyTwin(payload, twinMatchPos, isSaveTimeKey=false, ...)`
  (`remote_prop_spawn.cpp:340-353`). With `hasMatchPos==0`, `twinMatchPos = loc@new`.
- `TryDestroyTwin` searches `g_pileBindIndex` (save-loaded natives, indexed @bracket-open) for a native
  within 1cm of `loc@NEW` (`pile_reconcile.cpp:127-135`). The client's own copy is the save-loaded
  **native @OLD** position -> far from @new -> `matchCount == 0`.
- Because `isSaveTimeKey == false`, the `matchCount==0` branch does NOT record a `g_pendingSaveTimeTwin`
  for the post-quiescence sweep (`pile_reconcile.cpp:164-166` gates the record on `isSaveTimeKey`). So
  `SweepReconcileSaveTimeTwins` never retries it either.
- **Result: proxy@new (host) + native@old (client save copy) both survive = persistent dup.** Identical
  shape to L1, but the reconcile is STRUCTURALLY DISARMED because a post-blob pile has no save-time key.

## Why the existing fixes miss it
- **L1 save-time key:** eid 5283 did not exist at the blob instant -> no `g_blobPileXforms` entry ->
  `hasMatchPos=0` -> both the world-ready twin-destroy (matches the wrong @new pos) and the sweep (never
  armed for it) are blind.
- **30cm `FindAndConsumeAdoptCandidate`:** runs only on the `eidOnly && !fromConvert` adopt branch
  (`remote_prop_spawn.cpp:531-539`); the trash-proxy branch returns before it (:359), and the native is
  @old (>30cm) anyway.

## Mirror-identity verdict -- the 4TH instance, and the MOVE-scenario the class doc anticipated
It IS the same class (`docs/COOP_MIRROR_IDENTITY_WINDOW_RACE.md`): two channels (the transferred save =
native@old; the host's connect-window broadcast = proxy@new) with no stable cross-peer key, mutated in
the window. The twist the other 3 don't have: **the entity's identity (eid) CHANGES mid-window** (grab
destroys the old native, re-pile mints a new eid), so neither eid NOR the frozen save-time position of
*this* eid can tie the two channels. This is exactly the MOVE-scenario headroom at
`COOP_MIRROR_IDENTITY_WINDOW_RACE.md:79-87`: "the identity key must survive a position change in-window
-- a form that moves post-blob needs a re-capture or an identity that isn't its current position."

## Fix direction (NOT built -- RULE 1, root-first)
Close the **eid-0-at-grab gap** so the existing convert + save-time-stamp machinery applies UNCHANGED:
when the host grabs a pile in-window, force-seed/mint its eid at the InpActEvt PRE grab edge (the take-4
self-seed pattern -- `MarkPropElement`, register-only/idempotent) so `NotePendingGrab` arms with a real
eid. Then the grab captures the pile's pre-move position (= its save/native position, since it hasn't
moved yet) and threads it through the clump->re-pile chain, so the re-pile's PropSpawn/PropConvert
carries `hasMatchPos=1, matchX/Y/Z = pre-grab position`. The client's `TryDestroyTwin` then matches its
native@old by that frozen key (the L1 mechanism, unchanged) and records the sweep retry. The save-time
key is frozen at the GRAB edge instead of the blob -- the same cure, extended to survive the in-window
identity change. This is where the just-extracted `mirror_identity` kernel earns its keep (the 4th caller).

## Open point for the hands-on probe (the one thing static code can't settle)
Confirm the grabbed pile was genuinely UNTRACKED (eid 0) at grab time vs tracked-but-eid-changed: the
decisive marker is the `[PILE] HOST E-PRESS ... [UNTRACKED -> grab will NOT sync]` line
(`trash_collect_sync.cpp:402-407`) plus the `net_pump: steady-world re-seed adopted N NEW ... (pile)`
that minted 5283. The 17:23 host log was verbal (not captured in `research/`; grep for `5283` found no
file). A `pile_delta_probe=1` census at quiescence would also show the @old native as a `gt30` true-orphan
and confirm the client holds proxy@new + native@old (two distinct actors).

## Source map (cited)
`trash_collect_sync.cpp:75-99,156,339-411` (grab seam, re-pile thunk, eid-0 gap) ·
`local_streams.cpp:313-333` (held-edge adopt) · `trash_channel.cpp:146-242` (OnHostConvert /
AdoptPendingGrabClump) · `net_pump.cpp:617-626` (re-seed minting 5283) · `prop_snapshot.cpp:325-332,
528-560` (incremental PropSpawn, hasMatchPos=0) · `remote_prop_spawn.cpp:308-359,531-539` (proxy spawn +
TryDestroyTwin) · `pile_reconcile.cpp:120-197` (twin-destroy + the NONE probe) ·
`research/findings/votv-pile-dup-join-window-two-channel-RE-2026-06-23.md` (L1 root + purge/re-seed gap) ·
`memory/project_prop_appearance_delay_backlog_2026-06-23.md:18-22` (symptom-1 held-pile-in-window).

## 2026-06-30 -- b3 pos-correction never snapped the native (STATIC-mobility no-op) -- ROOT-FIXED + VERIFIED
The b3 PropSnapPos path (host flushes a save-authoritative position for a pile it moved in the join window;
client arms `quiescence_drain::ArmPendingPosCorrection`, applies at the quiescence sweep) FIRED correctly but
the snap **never took**: client log eid 4436 showed `[PILE-B3] APPLIED ... applied=(1699) host=(1556)
drift=150cm` -- the native read back STILL at the save pos right after `SetActorLocation`. ROOT: a save-loaded
`actorChipPile_C` native rests at **STATIC mobility**, and a Static component silently no-ops
`SetActorLocation` (the K2 call still returns true). So a host-moved-in-window pile stayed at the old spot on
the client unless the host RE-interacted (re-grab/drop -> a convert morphs a Movable clump). Kerfurs reconciled
because an NPC is Movable -- the user's exact "moved piles don't snap like kerfurs" report. FIX (commit
`a6242cfe`, deployed `C726D533`, **HANDS-ON VERIFIED 2026-06-30**): new `ue_wrap::engine::SetActorRootMovable`
(RootComponentOf + SetComponentMobility Movable=2, the `trash_proxy.cpp:183` precedent) called in
`ApplyPendingPosCorrections` BEFORE the teleport; the existing APPLIED drift log self-verifies (drift~0 = the
snap took). Same Static-no-op trap as the proxy lesson, now confirmed for a NATIVE game pile.
[[lesson-runtime-staticmeshactor-must-be-movable]]
