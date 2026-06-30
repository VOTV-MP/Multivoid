# Hands-on runbook 2026-06-30 -- kerfur DELIVERY axis (the 13:30 5-vs-6) + the OWNER BOUNDARY

**Deployed:** `votv-coop.dll` SHA256 `7026AE4E...` (short `7026AE4E`), proto **92** (UNCHANGED -- the fix
reuses the existing `PropSpawn` ReliableKind; no wire-format change). Hash-verified MATCH on all 4 folders
(host / copy / copy2 / dev). Build GREEN (Release).

**What changed (3 code files + 3 boundary guards):**
- `prop_snapshot.cpp/.h`: NEW `DeliverLateRegisteredProps` (the named late-registration deliver-missing
  owner) + `ExpressIncrementalKerfurOffProp` (delivers a re-seed-NEW = un-converted kerfur off-prop via the
  dupe-safe deferKerfur path). `ExpressIncrementalSpawn`'s :568 kerfur exclusion is UNTOUCHED. Shared
  `BroadcastIncrementalPropSpawn_` (RULE 2, one builder).
- `net_pump.cpp`: the re-seed loop now calls `DeliverLateRegisteredProps(newProps)` (was `ExpressIncrementalSpawn`).
- Boundary guards: (1) runtime self-check in `ExpressIncrementalKerfurOffProp` (WARN+skip a convert-owned
  eid via `GetKerfurIdForEid`); (2) docs (`COOP_MIRROR_IDENTITY_WINDOW_RACE.md` delivery-ownership facet +
  `COOP_SYNC_MAP.md`); (3) memory `[[feedback-deliver-missing-owner-delivery-axis]]`; (4) executable:
  `tools/kerfur-delivery-assert.ps1`.

## The bug being fixed (13:30 hands-on)
Host turned OFF a kerfur DURING the client's join window -> off-prop `ARD_i857g` reached the client via ZERO
channels (post-dated the snapshot; KerfurConvert never fired -- its source NPC was never a watched live Npc
Element, the host registers world NPCs once at join and the turn-off raced that scan; the generic incremental
express excludes kerfurs). Result: host 6 kerfurs, client 5. The destroy the user saw was a DIFFERENT kerfur
(`Nrby`) turning ON -- correct. The missing one is the turned-OFF form.

## Ini setup (already set; confirm)
In `votv-coop.ini` `[dev]` on BOTH host + client: `kerfur_census=1`, `save_identity_bind=1`.

## Launch
You (user) launch hands-on: `mp_host_game.bat` (host), then `mp_client_connect.bat` (client). I do NOT launch.

---

## SCENARIO A -- the bug side (join-window turn-off -> owner MUST fire)
1. Host: start a fresh save (or the current test save) with at least 2 ACTIVE kerfurs + a couple of off-props.
2. Client: connect. **WHILE the client is joining (loading screen / first ~2-3 s after), the HOST turns OFF
   one active kerfur** (radial menu -> turn_off). This recreates the race.
3. Let both settle ~20 s. Read the census in both logs.

**PASS (Scenario A):**
- `[KERFUR CENSUS][HOST] TOTAL N` == `[KERFUR CENSUS][CLIENT] TOTAL N` (the turned-off kerfur now appears on
  the client).
- Host log shows `incremental PropSpawn for runtime-adopted kerfur-off prop ... (eid=...)` (the owner fired).
- Client log shows `kerfur-prop-adopt: ... fresh-spawning a mirror` for that eid at quiescence (materialized).

## SCENARIO B -- the boundary side (steady-state turn-off -> convert PRIMARY, owner SILENT)
1. After the client is fully settled (census stable, >=30 s post-join), the HOST turns OFF a kerfur (a normal
   steady-state turn-off, NO join in progress).
2. Let settle ~15 s. Read both logs.

**PASS (Scenario B):**
- `[KERFUR CENSUS][HOST] TOTAL` == `[KERFUR CENSUS][CLIENT] TOTAL` (still equal).
- Host log shows `kerfur_convert: POLL turn_off ... host broadcasts destroy+prop` + the KerfurConvert (the
  convert path delivered it -- the steady-state primary).
- Host log shows NO `kerfur-off deliver-missing owner SKIPPED a convert-owned kerfur` WARN (the boundary held;
  the converted off-prop was marked known and never reached the owner).

---

## One-command verdict (run after BOTH scenarios, or per scenario)
```
# after a run that played BOTH Scenario A and Scenario B in one session:
pwsh tools/kerfur-delivery-assert.ps1 -Scenario both-sides -BoundaryTest
# or, per single-scenario run (no both-sides requirement):
pwsh tools/kerfur-delivery-assert.ps1 -Scenario join-turnoff
```
`-BoundaryTest` REQUIRES both sides were actually exercised this run -- side A owner-fired>=1 (PROVES the
join-window race was reproduced: a convert-delivered off-prop is marked-known and can NEVER reach the owner,
so a race-MISS shows as owner=0 = FAIL, never a false green) AND side B convert-fired>=1 (the steady-state
primary path). Run it only on a session that did BOTH turn-offs.
- CRITICAL invariants (exit 0 iff all hold): `kerfur-census-parity` (host TOTAL == client TOTAL),
  `owner-boundary-no-converted` (0 boundary WARNs), `boundary-discriminator` (no eid both converted AND
  owner-delivered), `no-stray-divergence-rearm` (the late-delivery did NOT re-arm the divergence sweep:
  client ARM <= host connect-replay -- this catches a bracket regression that count-parity alone would miss
  if it wiped non-kerfur join-churn).
- INFO: `deliver-missing-owner-fired` (>=1 after Scenario A), `convert-path-active` (>=1 after Scenario B),
  `client-late-kerfur-materialized`.

**Pre-fix proof the harness catches the bug:** run against the OLD 13:30 logs -> `kerfur-census-parity` FAILs
(host 6, client 5), `deliver-missing-owner-fired` 0. Post-fix Scenario A -> parity equal, owner fired >=1.

## If it FAILs
Paste the harness output + the two census tails + grep `incremental PropSpawn for runtime-adopted kerfur-off`
(host) and `kerfur-prop-adopt` (client). The likely suspects: the re-seed didn't run (NumObjects flat -- the
high-water gate), or the PropSpawn raced peer-readiness (reliable, should still arrive -- check the client got
the eid).
