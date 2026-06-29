# Hands-on runbook -- kerfur retire eid made CROSS-PEER-STABLE via KEY-bind (sidecar v3, 2026-06-29)

Deployed: **`0B702E26`** (votv-coop.dll, hash-verified build = host = client = client2 = dev). **Proto 92**
(wire changed: sidecar v2->v3 -> BOTH peers MUST run this DLL; an old peer is cleanly rejected on version
mismatch). Build GREEN. Launch via the named-window bats (`mp_host_game.bat` + `mp_client_connect.bat`). Fresh
New Game on the host (or a recent save with a few kerfurs); client joins.

## What changed (the 15:55 regression ROOT fix -- key, not cursor)

The 15:55 hands-on REGRESSED: turning a kerfur ON in the join window retired the WRONG off-kerfur on BOTH
peers. ROOT: the off-kerfur's host eid was bound by a **load-order CURSOR** (`save_identity_bind`) that ASSUMES
the client's load order == the save-array order. Under async-load / GC churn (worst when the host toggles
kerfurs during the join) that assumption fails -> the SAME physical kerfur bound DIFFERENT eids across peers ->
`retireOffEid` resolved a different kerfur on each peer -> the destroy propagated and killed a legit one on
both. [[lesson-eid-not-cross-peer-stable-loadorder-bind]]

FIX (RULE 1, root): the off-kerfur carries a **PORTABLE save key** (the `+0x40` FName the host already reads;
byte-identical on both peers -- same save blob). The sidecar now ships it (v3: variable-length entries,
`+u16 keyLen + ASCII key`), and the client pairs each off-kerfur native<->eid **BY KEY**, never by cursor:
- **seam fast-path** (`OnSaveLoadSpawn`): if the key is readable at the `BeginDeferred` seam, bind immediately.
- **quiescence backstop** (`BindUnboundReCreates`): any kerfur whose key wasn't readable yet at the seam binds
  by key post-FinishSpawning (guaranteed readable). NEVER position, NEVER cursor.

-> the bound eid is now intrinsic + **cross-peer-stable** -> `retireOffEid` (proto 91, unchanged) resolves the
SAME kerfur on both peers -> the retire is correct. **chipPiles are untouched** (genuinely keyless -> they stay
ordinal-cursor at the seam + position-rebind at quiescence; the pile master-fix is a SEPARATE later change).

HOLE-TOLERANT by construction: an off-kerfur untracked on the host (no map entry -- the jUuC case) finds no
key-match -> it stays a benign LOCAL without shifting any OTHER kerfur's pairing. A rank/cursor scheme would
shift every entry after the hole -> mis-bind. Key-match cannot.

## The bind gate (CLIENT ini)

`save_identity_bind=1` must be in the CLIENT's `votv-coop.ini` `[dev]` section (the bind gates on it). It was
already on for the prior runs. For the HOLE test (TEST 4) add `force_kerfur_unmap=1` to the CLIENT too.
(Per [[feedback-test-flags-in-ini-not-bats-or-env]] -- flags live in the ini, not the bats/env.)

---

## TEST 1 -- turn-off is still a single off-prop (ROOT 1 regression check, do-nothing-in-window)
1. Both peers in-world, kerfur(s) present (ON / NPC form). DO NOTHING during the join window.
2. On the HOST, turn a kerfur OFF (radial). Watch the CLIENT.
3. **PASS:** exactly ONE off-prop appears on the client and it falls + rests (no double, no hang). Repeat 3-4x;
   also turn one OFF on the CLIENT (watch the host).

## TEST 2 -- THE 15:55 REGRESSION REPRO (act DURING the join window) **<- the important one**
This is the EXACT case that regressed: the user does actions in the connection window. The hole that bit us is
produced by window churn, so this test reproduces the conditions, not just the happy path.
1. Host in-world with several kerfurs (mix of ON/NPC and OFF). Start the CLIENT connecting.
2. **DURING the client's connect/load window**, on the HOST: turn 1-2 kerfurs ON and 1 OFF (toggle a few).
3. Let the world settle. **Walk the base and COUNT kerfurs on the client vs the host.**
4. **PASS:** same count both peers; **NO kerfur wrongly deleted on EITHER peer**; no kerfur floating in the
   air; the ones the host toggled converged to the same form on both. This is the direct anti-regression check.
   **FAIL:** any kerfur vanishes on both peers, a floating off-prop, or a count mismatch.
5. Tell me -- I grep:
   - `save_identity_bind: BOUND ... kerfurOff ... by eid` / `RE-BIND kerfurOff by KEY -- native=... key='...' -> host eid=N`
   - `kerfur_reconcile: retired off-prop eid=N ...` and confirm N is the kerfur the host actually turned ON
     (now the SAME eid on both peers), with NO collateral destroy of another key.

## TEST 3 -- rapid on/off cycling (the convert/adopt path, both peers)
1. Both in-world. On the CLIENT, turn a kerfur ON, then OFF, then ON... several times quickly. Repeat on the
   HOST for a different kerfur.
2. **PASS:** each toggle = one clean form swap on BOTH peers -- no second kerfur beside it, no destroy/respawn
   "pop", no off-prop hanging. **FAIL:** a transient/persistent DOUBLE, a pop, or a form desync between peers.

## TEST 4 -- DETERMINISTIC HOLE test ([dev] force_kerfur_unmap=1 on the CLIENT) **<- proves hole-tolerance, not luck**
A natural jUuC hole is non-deterministic. This flag injects one deterministically: it drops ONE off-kerfur's
map entry at arm time, so that kerfur is GUARANTEED to have no host entry (== untracked-on-host).
1. Add `force_kerfur_unmap=1` to the CLIENT `[dev]` ini (keep `save_identity_bind=1`). Need >= 1 off-kerfur in
   the save (turn a couple OFF on the host before saving, or have them off at join).
2. Client joins. Let it settle.
3. **PASS (grep the CLIENT log):**
   `save_identity_bind: [force_kerfur_unmap] HOLE VERDICT=PASS -- dropped kerfurOff key='...' eid=N boundActor=0x0 ...`
   -> the unmapped kerfur stayed UNBOUND and **no OTHER kerfur stole its eid** (a rank/cursor shift would have
   mis-bound a wrong native onto eid N -> the regression class). The dropped kerfur shows as a benign local on
   the client (it may not mirror the host's -- expected; the host never mapped it). Every OTHER kerfur binds
   normally.
   **FAIL:** `HOLE VERDICT=FAIL ... boundActor=0x<nonzero>` -> hole-tolerance broken (a shift mis-bound).
4. Remove `force_kerfur_unmap=1` after this test (it deliberately unmaps a kerfur -- not for normal play).

## What I read in the logs afterward (client)
- ARM: `save_identity_bind: ARMED with N-entry host eid map (... + K kerfurOff [KEY bind, sidecar v3])`.
- Map rx: `save_identity_map: ... rx[j] index=.. eid=.. family=kerfurOff key='....'` (key non-empty for kerfurs).
- Bind: the seam `BOUND ... kerfurOff` or the quiescence `RE-BIND kerfurOff by KEY ... key='...' -> host eid=N`.
- Summary: `BIND SUMMARY -- bound .../... (... + K/K kerfurOff [KEY, sidecar v3]) ...`.
- Retire (on a host turn-ON): `kerfur_reconcile: retired off-prop eid=N ...` -- N now identical on both peers.
- ABSENCE of any collateral `OnDestroy key '...'` for a kerfur the host did NOT turn on; no `[Warn]`/SEH from
  our DLL; RSS stable.

## Honest status
Key-match bind is in code + build GREEN + hash-verified deploy (`0B702E26`, proto 92). NOT yet hands-on
re-verified -- this run is the verification. The fix is decoupled from the ordering axis (key-match is
timing-independent), so the kerfur fix ships + verifies ALONE first (minimal regression surface); the pile
master-fix (position-as-primary-at-quiescence) is a SEPARATE later change with its own smoke. The
`force_kerfur_unmap` HOLE test (TEST 4) is the deterministic hole-tolerance proof; TEST 2 is the end-to-end
two-peer regression repro. Detail + lesson: [[project-kerfur-identity-authority-refactor-2026-06-29]],
[[lesson-eid-not-cross-peer-stable-loadorder-bind]].
