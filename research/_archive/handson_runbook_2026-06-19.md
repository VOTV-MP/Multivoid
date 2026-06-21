# Hands-on runbook — 2026-06-19 (take 11): grab-path SELF-HEAL (the churn-rot root)

Deployed DLL `7CF1298AB76E` (HOST + CLIENT + CLIENT2 + DEV verified SHA256 MATCH). Proto **v87** (wire
UNCHANGED — this is a pure client-side fix; still restart both to load the new DLL). Logs cleared (prior test
archived `research/handson_2026-06-19_churn_untrack/`).

## What this build fixes (and what it doesn't — read this)

Your last test: mash E → works ~10s → fully breaks. The v87 mass-purge fix DID hold (verified: no false
re-init during the mash). The break was a *different* mechanism, and I found the real root with the workflow:

**The grab was the only pile-identity path in the whole system that didn't self-heal.** When a pile's
internal id binding goes stale under churn (the engine replaces the pile actor in a reload + a "self-heal"
cleanup pass drains the tracking), the destroy path already *rebinds and retries* — but the grab path just
gave up and refused to relay. That's the cascade you hit: once a pile's id rotted, every grab no-oped.

**The fix:** the grab now rebinds + retries exactly like the destroy path does — so a static pile whose id
rotted under churn gets its id restored and the grab relays precisely. New log line on a heal:
`pile_handle: grab self-heal -- rebound rotted pile eid=N ... precise eid relay restored`.

**Honest scope — what this does NOT cover:** this fixes the *static save piles* (the dominant case, and what
broke in your v87 test — those piles were all the same variant in a tight cluster). It does **not** yet make
a *thrown* pile that lost its sync (a throw landing in a dense cluster where the match grabbed the wrong
neighbor) grab-syncable — that pile would stay a benign client-local hold (the host just doesn't mirror it; no
cascade, no crash). I deliberately did **not** bolt on a position-based fallback for that case in this build,
because position can't tell apart identical same-variant piles sitting ~30cm apart (it'd grab the wrong
neighbor and quietly desync) — the right fix there is precise throw-landed-pile capture, which is a separate,
larger change I'll do next if you still see thrown piles not syncing.

So: this should stop the **cascade** (mash E → everything stops syncing). If after this you still see an
*occasional single* thrown pile not mirror, that's the known smaller follow-on, not the cascade.

## Please test (host + 1 client, fresh save / New Game)

1. **CLIENT: mash E on piles** — grab/throw rapidly, the exact thing that broke it. The cascade should be
   gone — grabs should keep relaying even after sustained mashing.
2. Watch for the heal in the client log: `grab self-heal -- rebound rotted pile eid=N`.
3. Host grab/throw should stay good (take-9/10).

## Log markers

- Client grabs should keep logging `relay=sent (eid=N)` with N host-range (< 32768), NOT a wall of
  `relay=no-op`. The self-heal line appears when it rescues a rotted pile.
- If you STILL get a wall of `relay=no-op (eid=4294967295)` after mashing → the self-heal isn't rescuing
  (tell me; I'll need to see whether RebindPileSeedsAfterWorldChange is finding the seeds).

Send **both** `votv-coop.log` either way.

## Why I scoped it this way (you've earned the honesty)

This pile feature has turned out to be a stack of separate fragilities, and I've been clearing them one at a
time. I chose the precise, low-risk self-heal over a big position-fallback rewrite because the rewrite has a
real wrong-pile hazard in exactly the dense same-variant clusters your test created — I'd rather ship the
correct narrow fix than a broad one that quietly grabs the wrong pile. If the cascade is gone but thrown
piles still nag you, the next step is precise throw-pile capture (hooking the pile's spawn), which eliminates
the orphan at the source.
