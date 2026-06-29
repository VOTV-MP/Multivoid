# Hands-on runbook — kerfur turn-off DOUBLE fix + 5-of-6 probe (2026-06-29)

Deployed: **`72C7B19F`** (was 7BCE41C4; rebuilt clean through the reorg + authority pass + sync->element merge -- kerfur behaviour identical) (hash-verified host + client + dev). HEAD `06e693b0`. Build GREEN.
Launch via the named-window bats (`mp_host_game.bat` + `mp_client_connect.bat`). Fresh New Game on the
host (or a recent save with a few kerfurs); client joins.

## What changed
- **ROOT 1 FIX (the turn-off DOUBLE):** `kerfur_convert.cpp` — the convert's fallback no longer arms the
  join-window fuzzy adopter (the arg-slot bug). A steady-state turn-off should now produce **ONE** off-prop.
- **ROOT 2 PROBE (the 5-of-6):** a `[KERFUR-R2]` WARN that fires iff an off-prop is retired while its
  replacement NPC isn't a live mirror. Diagnostic only — no behaviour change.

## TEST 1 — turn-off is no longer a double (the headline)
1. Both peers in the world, kerfur(s) present (ON / NPC form).
2. On the HOST, turn a kerfur OFF (radial menu). Watch the CLIENT.
3. **PASS:** exactly **ONE** off-prop kerfur appears on the client, and it **falls + rests** on the floor
   (not frozen in the air, not two copies).
   **FAIL:** two off-prop kerfurs (a double), or one hangs in the air.
4. Repeat 3-4 times, and also turn one OFF on the CLIENT side (client-initiated), watching the host.

## TEST 2 — join count + the 5-of-6 probe
1. On the HOST, before the client joins: turn at least one kerfur ON (so a kerfur is in NPC form, the
   case that diverged at 10:30).
2. Client joins. Once the world settles, **count the kerfurs on the client vs the host** (walk the base).
3. **PASS:** same count on both peers; no kerfur missing.
   **FAIL (the 5-of-6):** a kerfur the host has is absent on the client until you re-toggle it.
4. Either way, tell me — I grep both logs for **`[KERFUR-R2]`**:
   - If a `[KERFUR-R2] retire-WITHOUT-replacement` line is present AND a kerfur was missing → ROOT 2 is a
     fuzzy NPC-adopt miss; I build the deterministic fix (gate the retire on the NPC being live / make the
     materialize deterministic — folds into the Part-2 authority work).
   - If NO `[KERFUR-R2]` line and counts matched → the 10:30 "5-of-6" was the off-prop⇄NPC dedup flicker
     (one kerfur briefly shown twice during the join window), not a real loss.

## What I read in the logs afterward
- Client: `kerfur_convert[client]: adopted parked turn-off ghost as PROP mirror` (clean, by-eid) and the
  ABSENCE of `kerfur-prop-adopt: ... no local twin -> fresh-spawning` for a turn-off (that fresh-spawn was
  the double).
- `[KERFUR-R2]` (host + client) for TEST 2.
- No new `[Error]`/SEH from our DLL; RSS stable.

## Honest status
ROOT 1 fix = a 1-line arg-slot correction, identical to a fix already proven at the sibling call site —
**not yet hands-on-re-verified** (this run is the verification). ROOT 2 = **not reproduced** in the 10:30
log (host & client both = 5 distinct); the probe pins it if it's real. No blind replay was built.
</content>
