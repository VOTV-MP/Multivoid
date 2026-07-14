# Hands-on runbook -- kerfur turn_off SOLE-EXPRESS dupe fix (the 19:09 dupe)

**Date:** 2026-07-14 eve. **Take:** sole-express dupe fix (behavior change on host-own turn_off).
**Deployed DLL SHA256 (first 16):** `23DC7D54A603DF49` -- byte-identical on HOST, CLIENT_1, CLIENT_2, DEV.
**Flags:** `vm_dispatch_log=1` on HOST + CLIENT_1. Commit `de3dccb5` (on top of `44f8c69b` captured-B).

## What this take verifies

The 19:09 take (44f8c69b) fixed bug1/bug2 but REGRESSED host-own turn_off into a **client dupe** (host
census 6, client 8, 2 UNCLAIMED orphan props). RCA: a host-own turn_off double-EXPRESSED the successor prop
-- the keyed pipeline broadcast a real-key PropSpawn AND KerfurConvert broadcast a synthetic one; the client
kept both. The Init POST suppressor that was meant to prevent this (`TryAdoptFreshKerfurProp`) gated on a
racy PROXY ("a dead kerfur NPC is nearby"), which the verb's spawn-vs-destroy order defeats on some toggles.

This build REPLACES that proxy with the DETERMINISTIC fact: **is this prop the successor the conversion just
captured?** (`IsCapturedForm`). A captured conversion successor is TRACKED but its generic PropSpawn is
SUPPRESSED -- KerfurConvert is its sole wire signal (redesign 10.3). This is a **pre-existing 6-month race**
that 44f8c69b merely made visible (before: silent orphan NPC; after 44f8c69b: visible orphan prop; now:
neither).

## Autonomous pre-checks already done

- Release build clean; deployed x4 byte-identical (`23DC7D54`).
- LAN join smoke PASS: no crash/hang/OOM.
- Log-diff POSITIVE (the over-suppression risk, ruled out): **0** spurious `SUPPRESSED` lines in a
  conversion-free join, and **2 save-loaded kerfur off-props STILL broadcasting** via Init POST (they're not
  captured -> not suppressed). So the fix does NOT swallow save-loaded kerfurs.
- **NOT verified:** the conversion behavior (the smoke can't toggle kerfurs). That is this take.

## PRECONDITIONS

1. No VOTV running. HOST copy + CLIENT_1 copy (both `vm_dispatch_log=1`).
2. Fresh save -- New Game on the host. At least one kerfur (spawn one if the world has none).

## STEPS -- all four cells, ~2 s between toggles

**CELL 1 -- HOST-OWN turn-OFF (THE dupe cell -- watch the client count)**
1. On the HOST, turn a live kerfur **OFF**. Repeat 4-5x (interleave on/off a few times; the race showed on
   SOME toggles, not all). After each OFF, glance at the CLIENT: exactly ONE prop should appear where the
   kerfur was -- never two, never a stray floating/duplicate prop.

**CELL 2 -- HOST-OWN turn-ON (bug2 -- must still converge)**
2. On the HOST, turn a kerfur (a prop) **ON**. Repeat 3-4x. It must become an NPC on BOTH peers.

**CELL 3 -- CLIENT turn-ON (bug1)** and **CELL 4 -- CLIENT turn-OFF**
3-4. On the CLIENT, toggle a kerfur ON then OFF, 3-4x. No drop, no dupe, right form on both peers.

**END:** disconnect the client, quit both. Leave the games closed and tell me -- I'll copy both
`votv-coop.log` files (they truncate at boot) and read them. Or copy them yourself to the scratchpad.

## WHAT I READ AFTER (the pass/fail signals)

**GREEN = the census diff is ZERO.** The host prints `[KERFUR CENSUS][HOST] TOTAL N ... = N kerfur(s)`; the
client prints `[KERFUR CENSUS][CLIENT] TOTAL ... = M kerfur(s)`. **N must equal M**, and the client must show
**0 UNCLAIMED** props and **0 UNTRACKED** NPCs (those lines are the dup halves). Last session: host 6 / client
8 with 2 UNCLAIMED = the dupe. This take: they must match.

Per-cell lines:
- **CELL 1 (host turn-OFF):** each OFF should log on the host `grab_hook[Aprop.Init POST]: kerfur conversion
  successor ... generic PropSpawn SUPPRESSED (KerfurConvert is the sole express)`, and there must be NO
  `grab_hook[Aprop.Init POST]: HOST broadcasting SPAWN cls='prop_kerfurOmega_C'` for that same conversion
  prop. If you see the `broadcasting SPAWN` line for a conversion prop (not a save-load), the suppression
  missed -> the dupe is back (tell me which toggle).
- **The census is the truth.** Even if the fire-lines look right, the client census `= M` matching the host
  `= N` with 0 UNCLAIMED/UNTRACKED is the definitive pass.

**Failure signatures in-game:** two props where one kerfur was; a prop that never despawns after you turn the
kerfur back on; a kerfur that flips on one peer but not the other. Note the cell + rough toggle count.

## HONEST STATUS

- Build clean, deployed x4, join smoke + log-diff clean (over-suppression ruled out) -- but the dupe fix
  itself is UNVERIFIED until this take. GREEN = client census equals host census across all four cells.
- If GREEN: the follow-ups are (1) retire the staged proximity fallback in TryCapture (separate commit),
  (2) extract the destroy-edge/converge machinery out of `kerfur_convert.cpp` (moves-only commit).
- If any cell dupes: hand me the logs; the `SUPPRESSED` vs `broadcasting SPAWN` lines + the census diff
  localize which route leaked.
