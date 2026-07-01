# Hands-on runbook — pile PICKUP + LAND sounds (2026-07-01)

**Deployed DLL:** `votv-coop.dll` sha256 `8b0d4576dc8c…` (all 4 folders hash-verified:
Game_0.9.0n, _copy, _copy2, _dev). Built Release. HEAD after commit will be the sound-events
increment (1 commit ahead of the pushed `de492af8`).

## What changed
Receiver-side pile sounds, per rule 1. RE (`research/findings/votv-pile-pickup-land-sound-RE-2026-07-01.md`):
the chipPile/clump BP has NO dedicated pickup/land sound — the native sounds are the physics-material
`physSound` table (`.soft` = grab cue, `.impact` = land thud) + the `use` click.

1. **Client's own grab now sounds.** When YOU (on the client) grab a pile, the native grab is
   suppressed + routed to the host, which used to leave the grab silent for you. Now the `use` click +
   material pickup cue play locally at the pile.
2. **Pile LAND now sounds.** When a pile lands / re-piles (host- OR client-thrown), the client plays
   the material IMPACT thud at the landed pile (`prop_sound::PlayLandSound`, on the `kToPile` convert).

## Test (2 peers; you run hands-on, Claude does NOT launch)
Fresh New Game or a recent save. Host + client both with the new DLL.

### T1 — client grabs a pile (own-grab pickup sound)
1. Client: walk to any resting trash pile, aim, press **E** to grab it.
2. **Expect:** you (client) hear the pickup click + a soft material cue AS you grab (previously silent).
3. Host: should still hear the grab natively (it runs the grab on your puppet) — unchanged.

### T2 — pile LAND thud (both directions)
1. Client carries the clump, press **E** (drop) or **LMB** (throw); it flies + re-piles on landing.
2. **Expect:** BOTH peers hear an impact "thud" at the spot the pile lands.
3. Host grabs + throws its OWN pile; **expect** the CLIENT hears the land thud when it re-piles.

### T3 — no double / no spurious sound
1. Do several grabs/throws in a row; join a client while the host holds/moves a pile.
2. **Expect:** exactly ONE land thud per re-pile (no double), and NO land thud on a plain grab
   (ToClump) or on a join-window echo convert.

## What to read in the logs (host + client)
- `prop_sound: pile land thud at (x,y,z)` — the LAND sound fired. Should appear once per re-pile.
- `prop_sound: land thud SKIP -- no impact cue (root-mat miss; pile=..)` — the material has no
  `impact` row → silent land. If you HEAR nothing on T2 and see this line, the asset row needs a
  fallback (report it; the wiring is correct, only the chosen row missed).
- `[GRAB-INTENT] CLIENT E-PRESS on BOUND native pile` / `... aimed at pile proxy` — the client grab
  seam that now also plays the local pickup cue.
- Confirm NO land thud line on a ToClump (grab) convert and NO second thud on an idempotent-echo
  (`already pile, idempotent no-op`) convert.

## Honest status
Built + deployed + hash-verified 4/4; NOT smoke-run (needs your hands-on — sound is not observable in
the autonomous log-only smoke). The land asset is `physSound.impact` (the game's own material impact
cue); if T2 is silent with a `SKIP` log line, the row missed and we pick a fallback asset.
