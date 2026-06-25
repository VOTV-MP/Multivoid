# Hands-on runbook -- Phase 0 completeness FLOOR vs the 11:16 over-destroy (2026-06-25)

**Deployed: floor-on-baseline MD5 `68633342` (proto v88, NO pile-09) on host + client + client2 + dev.**
This binary = the deployed baseline (head probe + SAVED detect) + ONLY the Phase 0 per-class
completeness floor. pile-09 was reverted out of the build for CLEAN ATTRIBUTION (if piles hold, it is
the FLOOR, not pile-09's self-seed). Git HEAD `88a50890` keeps pile-09 in the tree; push HELD.

## What Phase 0 does (docs/COOP_STABLE_ID_SIDECAR.md S4, docs/piles/10)
The host sends an INDEPENDENT per-class live census of chipPiles (a raw GUObjectArray count, NOT the
registry the snapshot used). The client's claim sweep KEEPs unclaimed piles of a class it claimed
FEWER of than the host's census -- an incomplete snapshot is not a divergence. Stops the 11:16 wipe
(ALL 870 piles vanished) without blocking a legitimate clear.

## THE REPRO (the 11:16 scenario)
1. Host: fresh recent save (current game version). Have a pile of chipPiles near some kerfurs.
2. Host: during/just before a client connects, THROW piles near the kerfurs AND toggle those kerfurs
   off/on IN THE JOIN WINDOW (the pile x kerfur in-window interaction that triggered 11:16).
3. Client: connect.
4. WATCH the client world after it loads (~quiescence, a few seconds after world-ready): **do the piles
   STAY, or do they all vanish?**
   - EXPECTED (floor working): the piles HOLD. They do NOT mass-vanish.
   - FAIL (floor not catching it): piles disappear like 11:16.

## GREP the CLIENT log after the repro (say "ready" -- I grep, per SAM-GREP)
Decisive lines (client `Game_0.9.0n_copy/.../votv-coop.log`):
- `snapshot_census: client received host completeness census -- N class(es) (top: X x 'actorChipPile_C')`
  -> the census arrived (X should be the host's true pile count, e.g. ~870).
- `remote_prop_spawn: completeness FLOOR kept K unclaimed 'actorChipPile_C' -- host census X, claimed only C`
  -> THE GUARD FIRED: it kept K piles the host under-expressed. This is the 11:16 save.
- `remote_prop_spawn: completeness floor KEPT M of N doomed actor(s)` -> summary of the rescue.
- Contrast the OLD 11:16 line that must NOT now wipe everything:
  `claim sweep -- ... unclaimed locals destroyed` should show a SMALL number, not ~870/953.

HOST log (`Game_0.9.0n/.../votv-coop.log`):
- `snapshot_census: host completeness census built -- N chipPile class(es) total, M emitted (top: X x '...')`
  -> the host counted X piles independently of expression.

## Acceptance
- PASS: piles HOLD on the client + the FLOOR-kept lines appear with census X ~= the host's real count
  and claimed << X (the incomplete-snapshot case caught). -> Phase 0 verified; greenlight Phase 1.
- If the floor did NOT fire but piles held anyway: the host expressed fully this run (the 11:16
  non-determinism); retry to reproduce the under-expression.
- If piles vanished: grep -- was a census received? was claimed >= census (then it was a legit-looking
  complete snapshot, different root)? Report the lines; do NOT assume the floor is wrong without them.

## NOTE
- This is YOUR hands-on. I do NOT run smokes while you play (mp.py kill would overwrite your host log).
- pile-09 is NOT in this binary (reverted for the test). The move-dup it fixes is a SEPARATE issue;
  if you also throw-and-move a single pile expecting the dup fix, that is pile-09 (not deployed here).
