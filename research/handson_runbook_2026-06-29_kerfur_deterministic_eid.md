> **SUPERSEDED 2026-06-29 evening.** The eid-RETIRE half (`retireOffEid`) below REGRESSED at 15:55 (the eid was
> bound by a load-order cursor -> not cross-peer-stable). The ROOT FIX (bind kerfurOff BY KEY, sidecar v3,
> `0B702E26` proto 92) + its hands-on plan live in **`handson_runbook_2026-06-29_kerfur_keybind.md`**. This file
> is kept only for the RESULT/regression-root section at the bottom (the diagnosis that drove the key-bind fix).

# Hands-on runbook — kerfur fuzzy-position -> DETERMINISTIC eid (2026-06-29)

Deployed: **`DE089F66`** (votv-coop.dll, hash-verified build = host = client = dev). **Proto 91** (wire
changed -> BOTH peers MUST run this DLL; an old peer is cleanly rejected on version mismatch). HEAD
`b6169ba7`. Build GREEN, audit clean (no CRITICAL/WARN). Launch via the named-window bats
(`mp_host_game.bat` + `mp_client_connect.bat`). Fresh New Game on the host (or a recent save with a few
kerfurs); client joins.

## What changed (two kerfur fuzzy-position mechanisms -> deterministic, RULE 1)
1. **Off->active dup RETIRE** was a position-FUZZY 1cm match (a GUObjectArray walk). Now the host carries
   the off-prop's HOST EID (`EntitySpawnPayload.retireOffEid`); the client retires the off-prop MIRROR
   bound at that EXACT eid (it already binds save-loaded off-props to host eids via save_identity_bind).
2. **Turn-on ghost ADOPT** was a 500cm `FindParkedGhostNpcNear` position match. Now the host carries the
   converting eid (`convertFromEid`); the client adopts its parked turn-on ghost by EXACT eid
   (`TakeParkedGhostByEid`). `FindParkedGhostNpcNear` is DELETED.

The visible behaviour should be IDENTICAL when things go right -- the win is robustness (no fuzzy
miss/collision -> no dup, no missing kerfur, no respawn-pop) + the logs now say exactly which kerfur was
retired/adopted and WHY (by eid).

## TEST 1 -- turn-off is still a single off-prop (ROOT 1 regression check)
1. Both peers in-world, kerfur(s) present (ON / NPC form).
2. On the HOST, turn a kerfur OFF (radial). Watch the CLIENT.
3. **PASS:** exactly ONE off-prop appears on the client and it falls + rests (no double, no hang).
4. Repeat 3-4x; also turn one OFF on the CLIENT (watch the host).

## TEST 2 -- join count with a kerfur turned ON before join (the 5-of-6 surface, now eid-deterministic)
1. On the HOST, BEFORE the client joins: turn at least one kerfur ON (NPC form) -- the case that diverged.
2. Client joins. Once the world settles, **count kerfurs on the client vs the host** (walk the base).
3. **PASS:** same count both peers; no kerfur missing, no duplicate.
4. Tell me -- I grep for the deterministic retire line:
   `kerfur_reconcile: retired off-prop eid=N actor=... (DETERMINISTIC eid-keyed -- host turned this kerfur ON in-window...)`
   and confirm N matches the kerfur the host turned on. (No more `[KERFUR-R2]` -- that probe is gone.)

## TEST 3 -- rapid on/off cycling (the ghost-adopt path, both peers)
1. Both in-world. On the CLIENT, turn a kerfur ON, then OFF, then ON... several times quickly.
2. Repeat on the HOST for a different kerfur.
3. **PASS:** each toggle = one clean form swap on BOTH peers -- no second kerfur appearing beside it, no
   destroy/respawn "pop", no off-prop hanging in the air.
   **FAIL:** a transient or persistent DOUBLE, a pop, or a kerfur that desyncs form between peers.

## What I read in the logs afterward (client)
- Turn-on: `kerfur_convert[client]: adopted parked turn-on ghost as NPC mirror eid=N (by eid)` and/or
  `npc-mirror[adopt]: bound EXISTING local actor ... (kerfur turn-on -- no respawn, no ghost)`.
- Turn-off: `kerfur_convert[client]: adopted parked turn-off ghost as PROP mirror eid=N (by eid -- deterministic...)`.
- Join: the `[KERFUR CENSUS]` line (TOTAL N live) + the deterministic `kerfur_reconcile: retired off-prop eid=...`.
- ABSENCE of any `fresh-spawning` / second-prop-beside-ghost for a conversion, and no `[Warn]`/SEH from our DLL; RSS stable.

## Honest status
Both fuzzy mechanisms are eid-deterministic in code + build GREEN + audit clean, but NOT yet hands-on
re-verified (this run is the verification). The 13:21 run already proved TEST 1 (no turn-off double) and
that the count converges; this run confirms the deterministic paths behave identically + the cycling
(TEST 3) has no respawn-pop. Remaining kerfur fuzzy sites (host-side find-new-form 500cm,
kerfur_prop_adoption 500cm) are DEFERRED with documented rationale (see topic memory); piles are next.

---

## RESULT -- 15:55 hands-on: the eid RETIRE **REGRESSED** (do NOT trust the eid-retire half)

User did actions DURING the join window. Symptoms: client desync, kerfurs floating in the air, **one
kerfur deleted on BOTH host and client**, piles don't snap when host moved them in-window. ("Doing
NOTHING in the window = perfect.")

**ROOT (logs, 15:55, both peers):** the off->active RETIRE-by-eid is WRONG because **`eid` is NOT a
cross-peer-stable identity for save-loaded off-props.** The Build-3 sidecar binds each off-prop to its
host eid by **load-order CURSOR** (host map log: *"index = keyless ordinal (== client loadObjects spawn
order)"*) -- it ASSUMES the client's load order equals the save-array order. Under async-load / GC-churn
(esp. when the host toggles kerfurs in the window) that assumption fails, so the client binds the WRONG
off-prop to an eid. Proven: the same physical kerfur (key `gPXK`) got eid 3469 (13:21 run) vs 3470 (15:55
run). The host turned ON eid 3472 = key `Nrby`; the client's eid 3472 = key `p0KP`; `retired off-prop
eid=3472` destroyed `p0KP` (a legit kerfur) and the teardown's destroy propagated to the host (host log:
`OnDestroy key 'p0KPwI...'`) -> deleted on BOTH peers; the real stale `Nrby` survived UNCLAIMED (floating).

The OLD position-match was robust here because save-position is INTRINSIC to the kerfur (same save on both
peers). The eid is load-order-dependent. **Neither eid nor position survives heavy window churn** (eid
diverges; position drifts when the off-prop physics-settles >1cm). The intrinsic stable identity is the
off-prop's **save KEY** (proven portable: `p0KP` byte-identical on both peers; `[4 non-None key]` in the
host map). The ghost-ADOPT half (convertFromEid) is NOT affected (mid-session eids are host-assigned, not
cursor-bound).

**FIX PENDING (user choosing depth):** (1) narrow = retire by KEY (host carries the off-prop's key; client
destroys its local off-prop with that exact key); (2) deep = bind kerfurOff by KEY in `save_identity_bind`
so eid becomes cross-peer-stable (fixes the root + all eid logic; chipPiles stay keyless ordinal). Claude
recommends (2). The pile-window-move symptom is the SAME class (keyless cursor bind) -> the pile master fix.
Detail + lesson: [[project-kerfur-identity-authority-refactor-2026-06-29]], [[lesson-eid-not-cross-peer-stable-loadorder-bind]].
