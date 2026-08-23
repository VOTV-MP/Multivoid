# Hands-on runbook — the v134 batch (2026-08-23; NOTHING in it is hands-on yet)

**Deployed:** `multivoid-0.9.0n-134.dll` sha256 **`1DAC87AF6BD98A34`** ×4 (post R-3/R-2b/R-4a-end, 2026-08-23 late eve) (HOST/CLIENT_1/2/3),
proto **134**. (Supersedes the `588960CF…`/`7099255A…` bytes this runbook earlier named — the R-2 scan-hub
arc landed after them, same day.) **RELAUNCH BOTH PEERS** (every copy already carries the
final bytes via deploy-all).

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
5. **R-2 — the shared scan hub**: the once-per-1-2 s stutter's root (13 independent full
   GUObjectArray walks) replaced by ONE sliced shared pass; per-frame walk cost capped ~1-2 ms
   (was 66-305 ms spikes); dead-world index window closed family-wide. FEEL check: the
   once-per-second stutter should be gone in steady play.
6. **R-2b — the steady reseed census as hub consumer #14 (2026-08-23 eve)**: the residual ~20 s
   periodic hitch (13-18 ms single-frame; field 120/1,880 ms) is ALSO retired -- sliced pass +
   ~1 ms/tick budget drain. FEEL check: NO periodic hitch should remain in steady play at all;
   spawn-menu/toolgun props must still appear on the other peer within ~2-20 s.
7. **R-4a-end — the reconcile window (2026-08-23 late eve)**: the joining client no longer
   broadcasts its own rebuild-churn destroys after the load episode closes (the field's 1,629
   junk broadcasts). FEEL check: nothing visible -- verify by grepping the client log for
   "reconcile window RAISED/LOWERED" around the join and ZERO "CLIENT broadcasting DESTROY"
   bursts in the join minute.

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
