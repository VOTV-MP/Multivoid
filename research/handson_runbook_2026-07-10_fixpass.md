# Hands-on runbook — 2026-07-10 fix-pass + reorg + camera-front hand item (take 2)

**Supersedes take 1 (same day, morning).** Take 1's DLL predates the afternoon series.

**Deployed:** DLL `EDFED54A` (hash-verified all 4 installs) = the full 2026-07-10 day:
fix pass `db6ecd0b` + probe `7109efd1` + hand-item camera-front `be98beb6` + placement series
#5-#13 + 7 soft-cap extractions + residue fixes `9becc5e3`/`89bb24e0` + email census gate
`606fda3b` + the evening block `dab12a2e..fb490e36` (3 more extractions + dead-residue
retirement -- behavior-preserving; adds NOTHING new to test). Proto unchanged (107).
Post-deploy smoke PASS (LAN, 0 ERROR both peers, client slot 1 + puppet, RAM stable; the only
WARNs are the known transient connect-burst TimeSync/SkyState send fails).
**Probe note:** `[dev] rng_roll_census=1` stays ON in HOST + CLIENT_1 — every session you play
accumulates the T1 census (the fork call is formally DEFERRED at 7/16 until more client-side
exposure lands; your organic sessions are the accumulation path).

## Already VERIFIED by a real log this day (no action needed)
- **serverbox breaker restore on session end** — the census client's 14:15:00 death teardown
  printed `serverbox_sync: restored 1 ticker_serverBreaker on session end`; event_fire
  (allEvents 0->69) + time_sync (TimeScale->1) restores fired on the same path.

## Test steps (client = you, host = second box/window unless noted)
1. **Hand item, camera-front (the new look, `be98beb6`)**: hold-R pick up any item; the OTHER
   peer looks at your puppet. EXPECT: the item floats in FRONT of the puppet's face (camera
   height, slight right bias, follows your look up/down) — the SP hold look from third person;
   NOT at the right shoulder (the old placement). If it needs to sit closer / lower / more
   centered, say it in those words — it's a 3-constant retune.
2. **F2 rock (the headline, unchanged from take 1)**: as CLIENT, hold-R pick up a keyed rock ->
   walk -> hold-R place. EXPECT: host sees vanish at pickup + appear at place; NO duplicate, NO
   mid-air rock. Log: `[PROP-DROP] CLIENT authored drop intent key=...` -> `HOST spawned placed
   prop`. Also try quick-slot switching while carrying, and pick-up-never-place.
3. **serverbox mirror**: host waits for / forces a server break. EXPECT client sees the same
   server down (map + box skin), no false "server down" otherwise. (The disconnect-restore half
   is already real-log-verified.)
4. **email host-only + the census gate**: play until a story email triggers. EXPECT both inboxes
   match the host's; host inbox receives ZERO client-authored emails.
5. **peer-action chat**: either peer deletes an email. EXPECT "<nick> deleted an email: <subject>"
   on all peers ("You ..." on the deleter); F1 > Cosmetics > Chat toggle kills it; survives
   relaunch.
6. **(passive) census**: just play — a longer CLIENT life is what the fork call needs (the
   armed spawners fire on 30 min-3 h cycles; the autonomous client only survives ~18 min idle).

## Honest status
Everything above except the serverbox restore is AS-BUILT + smoke-clean ONLY (LAN connect,
0 ERROR, RAM stable). The positive paths (a real place, a real break, a real delete, the new
hand-item look) have not run under human eyes — this runbook IS that verification.
