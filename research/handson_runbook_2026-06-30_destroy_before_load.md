# Hands-on runbook -- DESTROY-BEFORE-LOAD fix + ANTI-SMEAR refactor + census (2026-06-30)

Deployed: **`DFABFBF4`** (votv-coop.dll, hash-verified host = client = client2 = dev). **Proto 92** (unchanged
wire). Build GREEN, audits CLEAN. HEAD **`6a9dd7af`** on main. Launch via the named-window bats
(`mp_host_game.bat` + `mp_client_connect.bat`).

**THIS ONE RUN GATES TWO THINGS** (the anti-smear refactor merged the destroy logic into the new order owner,
so verifying the join also verifies the refactor): (1) the **destroy-before-load fix** (`211eec5e`, below);
(2) the **ANTI-SMEAR refactor** (`quiescence_drain` order owner + `pile_spawn_bind` + `join_membership_sweep`
+ `remote_prop_destroy`; the kerfur retire folded into the one sequence). A clean join with matching census
TOTALs + the kerfur/pile reconcile still working = both verified. See [[project-anti-smear-refactor-2026-06-30]].

## What changed (the 11:24 client=7 host=6 root)

The census (now real on BOTH peers) pinned it: the client's extra 7th kerfur was the **Nrby off-prop (eid
3472)** the host no longer had. The host's PropDestroy of Nrby arrived at **11:24:06**, but the client didn't
load+bind its Nrby off-prop until **11:24:15** (9s later). `remote_prop::OnDestroy` found "no local actor" and
**DROPPED** the destroy -> Nrby then loaded **unopposed** -> a client-side dup. A **destroy-before-load** race
(the ORDER axis).

**FIX (RULE 1, one order owner):** on a miss, `OnDestroy` now ARMS a deferred destroy on the drain-edge order
owner (`pile_reconcile::ArmPendingDestroy`) instead of dropping it. The quiescence reconcile sequence applies it
AFTER the rebind (`identity_reconcile`: `SweepReconcileSaveTimeTwins -> BindUnboundReCreates ->
ApplyPendingDestroys -> ApplyPendingPosCorrections`) via `remote_prop::TryApplyDestroy`: destroy if the target
has now loaded, else stay queued. Delivery order can no longer leak a dup. The event handler only CAPTURES;
the drain-edge SEQUENCES + applies. [[feedback-one-owner-order-axis]]

The kerfur KEY-BIND (`59c3fdf7`, sidecar v3) is unchanged + proven sound; this is a separate, additive fix.

## Inis (already set -- you touch nothing)

Both host + client `[dev]`: `save_identity_bind=1`, `kerfur_census=1`. (`force_kerfur_unmap=0`.)

## TEST -- the 11:24 repro (act DURING the join window)
1. Host in-world with several kerfurs (mix of ON/NPC + OFF). Start the CLIENT connecting.
2. **DURING the client's connect/load window**, on the HOST: grab + re-pile a clump AND toggle 1-2 kerfurs
   ON/OFF (turn an off-kerfur ON, turn an NPC OFF) -- the exact churn that produced the 11:24 dup.
3. Let the world settle ~30s.
4. **PASS:** walk the base -- the client and host show the **same kerfur count**; no client-only extra off-prop
   sitting where the host has none. The census confirms it (below).
5. **FAIL:** the client has MORE kerfurs than the host (a leftover off-prop the host destroyed).

## What I read in the logs (tell me when done; I grep)
- The decisive pair: **`[KERFUR CENSUS][HOST] TOTAL N ...`** and **`[KERFUR CENSUS][CLIENT] TOTAL N ...`** (the
  LAST block of each, ~every 10s) -- the two TOTALs must **MATCH**. Diff the per-form lines by position if not.
- The fix firing: **`[DESTROY-DEFER] CLIENT armed deferred destroy key='...' eid=...`** (a destroy that raced
  ahead of the load) followed by **`[DESTROY-DEFER] CLIENT applied deferred destroy eid=... -- target finally
  loaded -> destroyed`** (the order owner reconciled it post-bind). If you see the `armed` without an `applied`,
  the target genuinely never loaded (benign, dropped at Reset) -- I confirm from the census.
- ABSENCE of any client-only `UNCLAIMED PROP` / `UNTRACKED NPC` census line; no `[Warn]`/SEH from our DLL; RSS
  stable.

## Honest status
Destroy-before-load fix in code + build GREEN + audit CLEAN + hash-verified deploy (`21CDC33A`). NOT yet
hands-on re-verified -- this run is the verification (the two census TOTALs matching is the PASS gate). The
10:54 desync (a join-window CONVERT half-deliver -- `SendReliable(KerfurConvert) failed` + silent NPC register,
the DELIVERY axis) is a SEPARATE open item, not addressed here. NEXT after this verifies: the anti-smear
refactor (merge `identity_reconcile` + `pile_reconcile` into one concept-named order-owner module + extract
`remote_prop_destroy.cpp`), per [[feedback-one-owner-order-axis]]. Detail:
[[project-kerfur-identity-authority-refactor-2026-06-29]].
