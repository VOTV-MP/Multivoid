# 07 — active-at-blob -> OFF collision (the 14:05 5-vs-4) + the anti-collision fuzzy-gate (fix#1)

**Status: FIX#1 (fuzzy-gate) + OBS-2 arg-fix VERIFIED hands-on (clean-bracket run 14:59-15:00, all 4 axes
confirmed by log) -> COMMITTED. Probe removed. Deployed MD5 `F419F594` (probe-free). Push HELD. FIX#2 nuance
(body via fresh-spawn+sweep vs convert) = backlog, NOT blocking -- the clean-bracket run already yields the
correct 5-off/1-active end-state.**

> **VERIFIED (clean-bracket hands-on, client log 14:59:44-15:00, MD5 `2E250812` then re-deployed probe-free
> `F419F594`):** host AND client both 5-off + 1-active. All 4 axes log-confirmed:
> 1. **Armed bracket:** `BeginSnapshot -- receiving world (3086)` + `claim tracking ARMED` + `Complete applied
>    3084/3086` + `divergence sweep ARMED` -> `FIRING (load tail quiesced; 2284ms)`.
> 2. **Collision GONE:** 5 kerfur-prop mirrors on DISTINCT actors (4343/4344/4345/4346/3147 ->
>    `7CC20BE0`/`7CC20400`/`7CC2FC10`/Nrby `7CC2F430`/fresh `FFF323340`). Nrby kept its own actor; no 2 eids
>    share one (vs 14:05's `BBC91C0` collision).
> 3. **Ghost sweep fired (the flicker):** `destroyed untracked ghost NPC kerfurOmega_C` + `post-snapshot ghost
>    sweep complete (1 untracked orphan destroyed)` -- the window-turned-off kerfur's active twin reduced ->
>    1 active.
> 4. **Body ON QUIESCENCE (not the 60s timeout):** `[OBS-2 PROBE] eid=3147: TWIN ABSENT` -> `no local twin
>    (load tail quiesced)` -> `spawned ... prop_kerfurOmega_C` -- fresh body at ~2.3s quiescence.
> The earlier 14:42 run (4-off/2-active) was CONFOUNDED by a lost `SnapshotBegin` (bracket never armed ->
> sweeps gated off + 60s timeout); the clean run armed the bracket and yields the correct end-state.

## What the 14:05 test actually was (NOT OBS-2, NOT doc-03 scope A)
The user ran "host turn_off a kerfur in the join window" -> host 5 off-objects + 1 active, client **4** + 1
active. This is **active-at-blob -> OFF**: the kerfur was ACTIVE (an NPC) in the transferred save; the host
turned it OFF post-blob in the window. This is the direction **doc 03 explicitly SCOPED OUT** (doc 03 KEY FACT
+ edge 1: scope A is off-at-blob->ON; active-at-blob touches the npc channel, deferred). It is also NOT OBS-2
(the OBS-2 fresh-spawn arg path was never reached -- zero `[OBS-2 PROBE]` lines this run).

## Root (proven by the RegisterPropMirror actor addresses)
Two host eids bound to ONE client actor:

| host eid | key | bind path | client actor |
|---|---|---|---|
| 3147 | xXPHX (runtime-turned-off, FRESH key) | `kerfur_prop_adoption` class+pose (500cm) | `…BBC91C0` |
| 4346 | Nrby (save-off, persisted key) | OnSpawn exact-key | **`…BBC91C0`** (same) |

5 host off-props -> **4 distinct client actors** -> 4 visible; the missing one = xXPHX (its "mirror" points at
Nrby's actor 206cm away).

**Mechanism:** xXPHX (turned off at runtime on the host) has a FRESH per-load `Aprop_Key` with NO cross-peer
twin, and its client-side twin is an ACTIVE NPC (different class -- `BindAsMirror` only scans prop-form
kerfurs). So xXPHX's deferred class+pose poll found no own twin and **fuzzy-grabbed the nearest same-class
kerfur (Nrby, 206cm, within the 500cm radius)** -- and it ran BEFORE Nrby's exact-key bind claimed Nrby
(ordering race). Then Nrby's exact-key resolve reused the same actor -> collision. (Save-off kerfur keys ARE
cross-peer-stable -- log-proven: the 4 neighbors all `resolves to live actor` by key. Only the runtime-turned-
off one has a fresh key.)

## The two faces of one gap (the decomposition)
A host-turned-off kerfur with no correct local off-twin is mishandled two ways:
- **fuzzy false-match (this collision)** -> steals a neighbor.
- **OBS-2 fresh-spawn drop** (doc 06) -> when no neighbor is in range, the (pre-arg-fix) fresh-spawn dropped it.
-> Fix split into **fix#1 (fuzzy-gate, direction-agnostic anti-collision)** and **fix#2 (active-at-blob->off
   body)**. scope A (doc 03, exact-key for the save-off cluster) is a COMPANION (protects the 4 neighbors) but
   does NOT close 14:05 (xXPHX is active-at-blob -> not in the off-prop capture map; no save-time pos for it).

## FIX#1 -- the anti-collision fuzzy-gate (BUILT, HANDS-ON PENDING)
**Principle:** a candidate carrying its OWN real, cross-peer-stable `Aprop_Key` that DIFFERS from the pending
kerfur's broadcast key exact-BELONGS to a different host kerfur -> never steal it via fuzzy. A keyless/
mismatched-key kerfur with no valid candidate falls through to fresh-spawn/defer instead of stealing.
Ordering-independent (gates on the candidate's identity key, not its current bind state).
- `kerfur_prop_adoption.cpp` ResolvePending class+pose (500cm): skip candidate where `key` non-empty/non-None
  AND `!= pendKey`. (The 14:05 culprit path.)
- `remote_prop_spawn.cpp` OnSpawn Gap-I-1 (30cm): same skip, **kerfur-gated only** (mushroom/garbage spawners
  keep their intended divergent-key dedup -- RULE 1, no regression).
Covers the user's 3 parts: (i) skip exact-belonging actors; (ii) no-exact-match kerfur never fuzzy-binds;
(iii) ordering race closed (identity-keyed, not bind-state).

**Interaction with OBS-2 (why this run validates both):** with the gate, xXPHX skips every keyed neighbor ->
no class+pose match -> it now REACHES the fresh-spawn fallback (where the OBS-2 arg-fix + probe live, build
`75A92E57`, carried forward). So the probe should FIRE this run, and the arg-fixed fresh-spawn should give
xXPHX a DISTINCT fresh off-prop body -> potentially a full 5-object fix. If the body doesn't appear, fix#1
still succeeds: collision -> clean "no body" (symptom 2), which fix#2 closes.

## FORWARD off->active dup (15:11 / 15:41 / 15:43) -- CENSUS-PINNED to identity-key (scope A)

A SEPARATE forward case (host turns an OFF kerfur ON in the window): the client shows the turned-on kerfur as
active + object (dup), OR (15:41) the off-prop collapses onto the WRONG position (under an active) instead of
its save-time position. The 15:11 log could not show the dup's stale half (a "silent" un-reconciled local
actor). A one-shot client **census probe** (`coop/dev/kerfur_census.cpp`, diagnostic-only, fires at quiescence
after both sweeps, enumerates every live kerfur form with bound status) NAMED it.

**[KERFUR CENSUS] 15:43 run (preserved `research/kerfur_forward_census_1543/`):**
```
PROP gPXK... pos=(1675.5,-997.4) BOUND eid=4349
PROP CZvy... pos=(1826.4,-1130.3) BOUND eid=4350
PROP p0KP... pos=(1636.5,-891.7) BOUND eid=4351
PROP Nrby-tq-... pos=(1670.4,-772.2) *** UNCLAIMED (no host mirror -- stale local off-prop) ***
NPC  kerfurOmega_C pos=(1729.1,-527.2) BOUND eid=3145
NPC  kerfurOmega_C pos=(1732.1,-745.5) BOUND eid=3147
TOTAL 2 live NPC (0 UNTRACKED) + 4 live PROP (1 UNCLAIMED)
```
**VERDICT: identity-key root. The silent extra form is an UNCLAIMED `prop_kerfurOmega_C` (Nrby), NOT an
untracked NPC (0 untracked -> retire-side RULED OUT).** Nrby was OFF in the save (client has a local off-prop)
and the host turned it ON in the window, so the host never broadcasts a Nrby off-prop -> the client's local
Nrby off-prop has no host mirror to claim/retire it -> it survives (15:43 dup) or, lacking a stable save-time
key, fuzzy-collapses onto a neighbour active's position (15:41 position-skew). Both faces = the SAME missing
stable cross-peer key. **This IS doc 03 scope A's designed scenario (off-at-blob -> host turns ON);** the
save-time exact-position key links the client's local off-prop to the host's now-active kerfur so the client
RETIRES the stale off-prop (and never mis-positions it). Scope A fixes BOTH the dup (15:43) and the
position-skew (15:41).

**NON-DETERMINISM:** 15:41 (count matched, position skewed) vs 15:43 (dup) = the same stale Nrby off-prop
reconciling differently -- consistent with the SnapshotBegin-lost / bracket-arming flake (arms -> off-prop
partially reconciles; doesn't -> survives whole). The census for 15:41 was truncated on the 15:43 relaunch
(only 15:43 preserved), but the 15:43 census is unambiguous and the 15:41 symptom corroborates.

**Audit-map (generalize):** forward-dup = the **THIRD mirror-identity window-race instance** (collapse-by-stable
-key), with L1 pile dup + the kerfur fuzzy-gate collision. Rule-of-three MET -> the mirror-identity layer
(save-time exact key + post-quiescence reconcile + uniqueness anti-collision) is now a clean
generalize candidate (`docs/COOP_MIRROR_IDENTITY_WINDOW_RACE.md`). NOT a retire-authority instance (census
ruled out the untracked-NPC path).

**NEXT: build scope A** (doc 03, design ready) -- it closes the forward dup + the position-skew. Census probe
STAYS (new rule 2026-06-24: RULE 2 does not apply to probes/diagnostics/tools).

## FIX#2 -- active-at-blob -> OFF body (NOT built; SEPARATE design pass)
The host-turned-off kerfur's client twin is an ACTIVE NPC; giving it the correct off-prop body means either
(a) converting the client's active NPC -> off-prop at join (the KerfurConvert path applied in-window), or
(b) fresh-spawning the off-prop + sweeping the active NPC (the reverse ghost sweep already destroys the active
twin -- so the arg-fixed fresh-spawn in fix#1 MAY already deliver (b); the hands-on tells us). This is doc 03's
scoped-out active-at-blob work + overlaps doc 04 (the body). Design + edge-vet before building, as usual.

## Refactor note
The fuzzy-gate ("keyless/mismatched-key never steals") is a GENERIC anti-collision primitive -- a candidate for
the future shared mirror-identity layer (`docs/COOP_MIRROR_IDENTITY_WINDOW_RACE.md`). L1 piles got
anti-collision via save-time exact-key uniqueness; the kerfur fuzzy-gate is the other face of the same
property. Possible COMMON with the pile anti-collision at a future N>=3 refactor -- do NOT generalize on N=2.

## NEXT
Hands-on the fuzzy-gate (runbook). On result: collision-gone + 5 -> commit fuzzy-gate + OBS-2 arg-fix (remove
probe); collision-gone + 4-clean-absence -> commit fuzzy-gate, open fix#2 design; collision-still -> re-diagnose.
Then fix#2 (active-at-blob->off body) design -> symptom 2 camera (doc 04). scope A (doc 03) deferred (companion,
not the 14:05 root).
