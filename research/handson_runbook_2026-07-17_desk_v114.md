# Hands-on runbook — v112 desk INPUT + v113 L4 DISHES + v114 L7 CADDY/TASK (batched), take 1

DEPLOYED: `votv-coop.dll 13874C48D3D7F220` x4 (HOST + CLIENT_1/2/3), hash-verified.
kProtocolVersion **114** (a 113-or-older peer HARD-CLOSEs at the gate). HEAD `ba8ce297`
(committed; unpushed). Autonomous LAN smoke PASS (both L7 lanes resolved on both peers:
reelBig=0x288/reelSmall=0x28C/Progress=0x364; install latched once; zero lane WARN/ERROR;
RSS stable). Perf audit PASS 0 CRITICAL (both recommendations applied pre-commit);
correctness audit CRITICAL 1 (parked-place Progress reset) FIXED pre-deploy.
**NOTHING below is hands-on verified yet.** This take BATCHES THREE unverified proto layers
(v112 + v113 + v114 — flagged in the L7 pass, round 7); per-lane log prefixes keep
attribution: `desk_input:`/`desk_sim:` = v112, `dish_sync:`/`[dish]` = v113,
`[reel]`/`[task]` = v114. The v112+v113 steps live in
`handson_runbook_2026-07-16_desk_v113.md` (still current for those lanes — run its STEPS
first or interleave); THIS file adds only the v114 half.

## What changed in v114 (L7, all BUILT — design `votv-tape-caddy-L7-impl-DESIGN-2026-07-17.md`)
1. The STOLAS tape caddy's reel slots sync: insert/eject on either peer mirrors (slot meshes
   + hover progress); the recording accrual converges to the HOST's value (1 Hz corrector —
   both peers keep ticking natively, the corrector snaps; sawtooth <= 1%/s is by design).
2. A CLIENT-ejected reel now EXISTS on the host/others (it used to be a local-only ghost):
   the host authors it at the eject moment; it follows the client's hands.
3. Reel props carry their Progress % cross-peer at BIRTH (eject, container, join window,
   pocket->place) — hover shows the same % everywhere.
4. saveSlot.taskNew (the daily task: requirements, sigCompleted, reel grades, reward fields)
   mirrors host->client a second after any change (drone sell / day rollover).
5. The caddy ON/OFF toggle already synced (appliance lane, v45) — regression-check only.

## STEPS (v114 half; do the v112+v113 runbook first)
1. Both peers walk to the STOLAS caddy (base interior wall unit).
2. HOST ejects the BIG reel (E on the slot; refuse-while-active means toggle OFF first —
   the toggle itself must mirror, both peers see the unit stop). CLIENT watch: the slot mesh
   empties <= 0.5 s; the reel appears in the host's hands and its hover shows the SAME %.
3. HOST re-inserts it. CLIENT watch: slot mesh returns; hover Progress resumes accruing
   (toggle back ON) and the % matches host's within ~1%.
4. CLIENT ejects the SMALL reel. HOST watch: slot empties; a reel EXISTS in the client's
   hands on the host's screen (expect 2-4 one-shot `no local match` WARN lines in the host
   log at that moment — bounded, by design L7-R3). Client DROPS it on the desk: it lands on
   both peers at the same spot with the same hover %.
5. CLIENT picks it back up (E), pockets it (hold-R), then PLACES it from the hotbar: the
   placed reel keeps its Progress % on BOTH peers (the correctness-audit CRITICAL 1 case —
   a blank-tape reset here is the bug we fixed; verify it stays).
6. CLIENT throws a reel INTO the caddy funnel (route 2 insert): the slot fills on both peers
   with that reel's %, the thrown prop vanishes on both.
7. Let the unit record ~2 min with both reels in: hover % identical on both peers (+-1%).
8. Day flow (if in the session's plan anyway): drone-sell both reels HOST-side loaded ->
   CLIENT's tablet task page shows reel grades update <= 1-2 s ([task] line in the client
   log). KNOWN residual L7-R1: a sack loaded BY THE CLIENT does not grade (container
   inventory unsynced until the sack-contents lane) — not a v114 regression.

## WHAT TO READ IN THE LOGS
- `[reel] local INSERT/EJECT edge ... -- broadcast` on the presser; `[reel] wire INSERT/
  EJECT applied` on the mirror. NO repeating [reel] lines while nobody touches the caddy
  (a >=1 Hz repeat = a poll-echo bug — flag it).
- `[reel] wire INSERT onto OCCUPIED slot ... HOST keeps own` = the R5 tiebreak firing —
  expected ONLY on a genuine simultaneous insert; if it repeats, flag.
- `[task] taskNew changed -- broadcast` host-side at sell/rollover; `[task] taskNew
  mirrored` client-side. More than a few per game-day = the change-hash is unstable — flag.
- `[PROP-DROP] CLIENT authored REEL-EJECT intent ... +savedScalar` at a client eject;
  `[PROP-DROP] HOST spawned client-placed prop` right after.
- The v113 watch-fors (cue-reconciler repeat line, ARM pre-clear WARN) still apply.

## HONEST STATUS
v114 = BUILT + audited + smoke PASS. NOT hands-on. v112 and v113 are ALSO not hands-on —
three layers stack in this take; if a desk/dish regression appears, attribute by prefix
before assuming v114.
