# Hands-on runbook — the v134 batch (2026-08-23; NOTHING in it is hands-on yet)

**Deployed:** `multivoid-0.9.0n-134.dll` sha256 `588960CF15944F17` ×4 (HOST/CLIENT_1/2/3),
proto **134**. HEAD at write time: `0676e5a8` + the documentize commit. **RELAUNCH BOTH PEERS**
(every copy already carries the final bytes via deploy-all).

## What this batch contains (all autonomous-verified only)

1. **R-1/R-1b/R-1e + R-4a** (2026-08-22/23): the world-stamp on `CachedObjRef` (the 44 s
   dead-pawn AV storm), the `!keyless` destroy-seam scoping (871→0 client world-load destroy
   broadcasts), the absorbed-AV rate latch.
2. **R-4b** — the reliable-delivery guarantee (`coop/net/send_backlog`): no silent rc=-25 loss;
   container parks bracket-anchored; 4 MB send buffer; client inbox pause-not-drop.
3. **SendRate**: decided NO change (the "256 KB/s clamp" premise was false — the binary has
   shipped 1/25 MB/s global since June). Overdrive drill-proven: a 4× thin uplink degrades
   gracefully, never dies. New testing knob `net.fakelink_kbs`.
4. **Seeds arc** — emails/saved-signals authored during a joiner's load window now DELIVERED
   (ready-edge seed); the solo-author email DUP (live since v64) retired; apply parks replace
   the "row lost" drops; per-slot assembler teardown.

## Suggested hands-on checks (any subset; prefixes attribute)

- **Normal 2-player join** onto a lived-in save: joins should feel unchanged or faster; watch
  for containers that used to arrive EMPTY on the joiner — they should now fill.
- **The seeds case (the star of the batch):** while the client sits on the loading screen, the
  HOST catches a signal that produces an email (or any event mail lands). After the client
  loads: the email must be on the client's laptop, exactly once. The reverse dup check: host
  plays SOLO a while (emails accumulate), then a client joins — every email exactly once
  (previously the first joiner could get duplicates).
- **Feel check for R-2's absence**: the once-per-second stutter row (R-2) is NOT in this batch —
  if you feel it, that's expected and queued.

## What to grab if anything looks wrong

Both `multivoid.log` files (host + client). Grep-keys worth knowing:
`send_backlog: EPISODE` (delivery episodes), `seed slot=` (the join seed firing),
`applied email from slot` (wire email arrivals), `apply park` (backpressure), `rc=-25` (must
stay absent), `[SEED-DRILL]` (only if the drill env was on — not in normal play).

## Honest status

Everything above: smoke + purpose-built drills + agent audits (0 CRITICAL), **zero hands-on**.
The "4-peer relay gap" that FAILed the smoke's cross-peer verdict turned out to be the SMOKE'S
OWN WINDOW, not a relay bug (resolved 2026-08-23 same day: with the window anchored on every
client's world-ready edge, all four peers see each other via relay — the relay is healthy in
both directions). If a 3-4 player hands-on still shows "can't see the newest player" after
everyone has fully loaded in, that IS news — grab both logs.
