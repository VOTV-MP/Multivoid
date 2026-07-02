# Hands-on runbook — residual mass-move dup PROBE (2026-07-01, take 2)

> **UPDATE (2026-07-02): the probe did NOT fire on the 20:17 run -- root-caused + fixed.** The ini
> reader's exact-equality match silently read every inline-`;`-commented flag as ABSENT, so
> `pile_dup_probe=1   ; comment` never enabled it. Root fix `f81256e4` (strip inline comments before
> matching). **The probe is ARMED now** (flag in all client inis; current deploy `a142e2bc` carries
> it) -- the next mass-move run yields the [DUP-PROBE] verdict lines described below. 20:17 aside:
> 10 twins DID retire that run (8+1+1 confirmed), so twin-retire works when FindExactMatch hits.

**Deployed DLL** `votv-coop.dll` sha256 `7856bac32378cf74` (all 4 folders hash-verified). Built Release.
`[dev] pile_dup_probe=1` set in the client ini (Game_0.9.0n, _copy, _copy2). Read-only diagnostic — no
behavior change with the flag off; the owner + grabbed-clump gate are UNTOUCHED.

## Why this build exists
The mass-move dup class is VERIFIED-closed to **ONE residual** local dup (19:06 "всё на своих местах" cleared
the corner; one pile still dupes). The 19:41 client log proves the failure MODE but not the CAUSE:
- 8 `identity key UPDATED` (retrack @old->@new) + 11 `HOST-VACATE twin` arms + 1 `DUP-RETIRE` arm = the owner
  fired correctly.
- BUT the sweep retired **0 of 21** pending twins: every `FindExactMatch` returned MISS on every pass, then
  all 21 dropped after kMaxTwinPasses **without retiring**. (`0 confirmed-moved retired, 0 unconfirmed retired`.)
- `FindExactMatch` MISSes for THREE different reasons, indistinguishable in the aggregate log:
  (0) no unbound native@old = CLEAN (host moved the single actor, no dup — correct),
  (>1) two co-located unbound natives = AMBIGUOUS-skip,
  (BOUND) the orphan@old is bound to the WRONG eid -> excluded from the unbound-only candidate set -> invisible
  to every retire path = **the predicted GC-pointer-reuse tail**.
- Fingerprint pointing at BOUND: the DUP-RETIRE arm FOUND an unbound native@old at 19:40:48, yet the sweep 7s
  later found none -> the orphan went unbound->bound (a racing RE-BIND grabbed it) or was GC'd.

We do NOT guess which. This probe measures it, per [[feedback-probe-dont-guess-rule]], before we touch the
VERIFIED owner.

## What the probe logs
On each sweep pass, for EVERY pending twin whose `FindExactMatch` MISSes, one line:

```
[DUP-PROBE] eid=E [host-vacate|event-twin] @old=(x,y,z) FindExactMatch=MISS pass=N |
  UNBOUND@old: a<1cm b<30cm | BOUND@old<30cm: w wrong-eid (first eid=W d=Dcm) + s same-eid |
  E.bound=PTR d_from_old=Dcm  --> <VERDICT>
```

Verdict decodes the MISS:
- **`BOUND-TO-WRONG-EID residual (the predicted GC tail)`** — `w>0`: a chipPile sits at @old bound to eid `W`
  (!= this twin's eid). THIS is the residual dup: a native@old re-bound to the wrong eid, invisible to the
  unbound-only sweep + HOST-VACATE. -> the fix is in the RE-BIND ordering / a bound-to-wrong-eid detector.
- **`unbound near but >1cm off (fuzzy)`** — `b>0,a=0`: an orphan is there but the 1cm exact match can't claim
  it -> the fix is a relaxed/tie-broken match for the host-vacate case.
- **`unbound-present-but-unmatched(?)`** — `a>0` but still MISS: means the ambiguous(>1) guard tripped or a
  chipType mismatch -> the fix is the ambiguity tie-break.
- **`CLEAN (no orphan @old)`** — `a=b=w=0`: no dup for THIS eid (host moved cleanly). If ALL twins say CLEAN
  yet you still see a dup on screen, the dup is NOT a save-time twin at all (different track).

## Steps
1. Client: `[dev] pile_dup_probe=1` is already set. Launch host + client, join.
2. Reproduce the mass-move: host mass-grabs a chip cluster during/around the client's join and throws them
   (incl. LATE moves after world-ready). Get the client into the state where ONE residual dup remains.
3. On the client, walk up to the residual dup pile; note roughly where it is.
4. Quit. Send me `Game_0.9.0n_copy/WindowsNoEditor/VotV/Binaries/Win64/votv-coop.log` (or whichever folder the
   client ran in).

## What I'll read
- `grep [DUP-PROBE]` -> the per-eid verdict for the residual. One `BOUND-TO-WRONG-EID` line names the exact eid
  `E`, the wrong eid `W`, and the actor -> that IS the root, measured not guessed.
- Cross-ref: the twin's eid `E` should be one of the 8 moved eids (key UPDATED / HOST-VACATE armed).

## Also flagged this build (NOT fixed here — reported honestly)
**FPS fix `70e0d899` did NOT land.** `sync:npc_client` still spiked 20-51ms (mean 23.5ms, 60/69 samples >20ms)
across the whole 17s drain window in the 19:41 run — even the last pass, re-binding only 1 chip candidate, took
20ms. The NameOf-reorder shaved allocs but the DOMINANT cost is the raw ~330k-object iteration run by MULTIPLE
walks per drain pass (BindUnbound + sweep + join reconcile) at 4Hz, kept hot the whole window because the 21
never-retiring twins pinned `HasPendingWork`. So the dup and the FPS hitch share a root: twins that never
resolve pin the reconcile hot. Fixing twin-resolution (this probe's target) shortens the pin; the deeper FPS
fix is merging the per-pass walks into ONE shared scan. Deferred until the probe pins the dup cause.

## Honest status
Probe built + deployed + hash-verified 4/4 (`7856bac32378cf74`); NOT yet run. Owner UNCHANGED (dup stays
VERIFIED). 10 feature commits still held local (unpushed). No fix applied — this run is measurement only.
