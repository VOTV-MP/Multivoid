# Hands-on runbook — the WHOLE assembled sync module (2026-06-28)

> WHY NOW: the sync-consolidation refactor is ASSEMBLED to its RULE-1 end (identity owner unified,
> CreateOrAdopt keystone, ElementDeleter destroy funnel, bool-chain router; residue-fold rejected as a
> perf regression; authority model named). The earlier D1/D2 runbook was cancelled as premature (half-
> assembled). This runs the WHOLE module in one session: D1 + D2 + the 3 open symptoms + a regression
> sweep. On a clean result -> push the assembled module as the sync ETALON. On a fail -> the symptom is
> REAL (not transitional noise) and we fix it on the whole path before push.

Deployed DLL: `0E04B197923E9BDDAEF8F2F4F50704AD3F2FA68DDE27B2721359FBF51A5A7D6F` (votv-coop.dll, built at
HEAD `3cf745d0` = keystone `ecdc527c`/`6aeaf55c` + router `fcf5b1b1` + comment/doc fixes). Hash-verified
MATCH on host (`Game_0.9.0n`) + client (`Game_0.9.0n_copy`). [dev] ini both peers: `save_identity_bind=1`,
`save_identity_map_log=1`, `force_save_churn=0`; client `pile_delta_probe=1` + `garbage_pickup_probe=1` ON;
host `puppet_head_probe` turned OFF (closed investigation, log-noise). NO rebuild/redeploy needed.

## Steps (ONE session covers everything)
1. HOST: launch `mp_host_game.bat`, load the recent fresh save (the standard 6-kerfur / chip-pile slot).
2. CLIENT: launch `mp_client_connect.bat`, join. Let the world finish loading (~15-25 s; you've seen the
   clean census in past runs).
3. **REGRESSION / JOIN census** — just let it settle ~30 s after load. (Claude greps the join census:
   KERFUR n/n, 0 UNCLAIMED, 0 orphans, the RE-BIND-by-position count on the valve-abort path.)
4. **D1 -- steady grab of a SAVE-loaded chip-pile.** Wait ~1 min after settle (steady state, NOT the join
   window). On the CLIENT, walk to a save-loaded chip-pile and GRAB it (carry a moment). Watch: NO second
   copy / ghost left behind. One pile, picked up clean.
5. **D2 + symptom #1/#2 -- toggle a CLIENT kerfur.** On the client, find a kerfurOmega and TOGGLE it off,
   then on. Watch: (a) it should NOT flash a frozen mid-air copy [old D2]; (b) it should NOT twitch /
   jitter as if facing two directions [symptom #1]; (c) when turned OFF it should fall/rest, NOT hang in
   the air at standing height [symptom #2].
6. **Symptom #3 -- piles present on both peers.** Glance at a few chip-piles on the CLIENT that exist on
   the host: they should be VISIBLE on the client, not absent.
7. Quit both. Tell Claude "logs ready" -- Claude greps (you don't read logs).

## What Claude greps (PASS / FAIL)
**Regression / join (host + client):**
- PASS: `KERFUR n/n`, `0 UNCLAIMED`, `0 orphans`; `RE-BIND by position` on the abort path (fix #1);
  `post-purge window` reconcile fires (fix #2); no `[Warn]`/`[Error]` from our DLL, no SEH dumps.
- Also: NO `event_feed: unknown ReliableKind` spam (the router bool-chain: a spurious unknown-kind line
  for a known family kind = a missed-break in the SyncRouter conversion -> a real router bug to fix).

**D1 (steady grab of a bound save-native):**
- PASS: `[PILE] CLIENT convert ... bound save-loaded NATIVE pile GRABBED -> handed to runtime clump ...
  (native retired; native-authoritative hand-off)` with the native retired -> no ghost.
- FAIL: the else-branch `proxy SPAWNED ... (convert beat its spawn)` with native NOT retired, AND/OR a
  later `[PILE-CENSUS]` orphan / a ghost dup at that spot.

**D2 + symptoms #1/#2 (client kerfur toggle):**
- adopt PASS (already proven 16:24): `kerfur_convert[client]: adopted parked ... ghost as PROP/NPC mirror
  eid=N (by eid ...)` -- adopted by the host's authoritative eid, no flash.
- symptom #1 (twitch) / #2 (hang-in-air): these are DOWNSTREAM of the adopt. Claude correlates the
  adopted off-prop's eid against the held-pose stream + any host PropPose for it. If the adopted prop gets
  NO host pose after adopt (only the client's local held-pose stream) -> CONFIRMS the deferred D2
  corrective-pose gap (the SyncAuthority item) = a REAL fix to build, not noise. If a host pose DOES land
  and it still hangs -> a different root, re-derive.

**Symptom #3 (pile host-only):**
- Claude diffs host `[PILE-CENSUS]` vs client `[PILE-CENSUS]` for the same eids: present-host/absent-client
  = CONFIRMS a real client-apply gap (grep the client for a dropped/failed PropSpawn or a sweep that ate
  it). Absent on host too = the pile was consumed (not a bug).

## After
- All clean -> the assembled module is verified on the whole -> push `ee19ec8c`+`fcf5b1b1`+`3cf745d0`
  (the assembled arc, ~46 commits ahead) to origin/main as the sync ETALON.
- Any symptom CONFIRMED real -> it is NOT transitional noise; fix it on the whole path (D2 -> the
  SyncAuthority corrective-pose; #3 -> the client-apply gap) BEFORE push, then re-run this runbook.
