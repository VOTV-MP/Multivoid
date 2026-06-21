# Hands-on runbook — trash-proxy PHASE 1 — take-24 (CARRY mechanism SMOKE-PROVEN on a settled join; confirm the VISUAL)

**Deployed:** `votv-coop.dll` SHA `70f1f04b` to all 4 copies (host / copy / copy2 / dev). Proto **v82**
(UNCHANGED). Code HEAD `245148c6` + this session's instrumentation + autotest fixes. Build CLEAN (Release).

**STATUS — read before testing:**
- **DUP FIX + VISIBILITY — hands-on VERIFIED** (unchanged from take-23): no doubled piles; resting + landed
  piles mirror correctly and VISIBLY.
- **LIVE CLUMP CARRY — MECHANISM PROVEN by smoke on a SETTLED join (2026-06-22).** Two clean instrumented
  LAN smokes show the full carry mirroring: the host's ToClump convert is adopted on the client (`known`
  0->1), the clump mesh resolves (`dirtball`), `GRAB-IN` fires, the pose-drive advances `#1..#540 [proxy]`
  tracking the host's walk 1:1, and the LAND convert re-skins back to a pile. **The earlier take-23
  "carry does NOT mirror / 2 fps / stays a pile" was the JOIN RACE** — the autotest (and most likely your
  hands-on) grabbed BEFORE the client finished expressing its proxy snapshot, so there was no live proxy to
  drive. Fixed in the test by a puppet-live settle gate.
- **What this hands-on confirms:** the smoke is RENDER-BLIND (it proves the drive computes the right target
  positions, not that the pixels actually follow on screen). So YOU confirm the on-screen VISUAL: does the
  carried clump visibly re-skin + follow smoothly on the client? And does it still "2 fps" if you grab on a
  FULLY SETTLED join (it should NOT)?

## The KEY to a valid test: grab only AFTER the join is FULLY settled
The carry mirrors when the grab lands on a settled world. The client expresses its mirror of every host pile
(could be ~875 of them) as a batch a few seconds AFTER it enters the world. If you grab during that window,
the convert can beat the proxy and the carry won't mirror (the take-23 symptom). So:
1. Host loads a recent save WITH chipPiles (the garbage litter). Client joins.
2. **WAIT** for the client's join to FULLY complete: the loading screen clears AND the world is populated
   (you see the host's piles appear on the client) AND give it another ~10 s of quiet. THEN test.

## The test
1. **JOIN — dup + visibility (confirmed; re-confirm).** Look at a host pile cluster on BOTH screens: each
   host pile = exactly ONE pile on the client, VISIBLE, right place. No doubles, no "won't disappear."
2. **GRAB + CARRY (the part to confirm).** On the HOST, grab a pile (it becomes a clump in your hand) and
   WALK with it — a long walk, around corners, far from the origin.
   - EXPECT (the proven mechanism, now confirm visually): on the CLIENT the proxy **re-skins to a clump in
     the host's hand and FOLLOWS SMOOTHLY** (interpolated), the original rest-spot pile is gone (it IS the
     same entity, re-skinned + moved), and it tracks however far you walk. A brief network hitch FREEZES it
     (not a drop), then it resumes.
   - If instead it stays a pile at the rest spot / only jumps occasionally ("2 fps"): note whether the join
     had FULLY settled before you grabbed (see above). If it was fully settled and it STILL fails, that is
     new data — grab the client log (markers below) so we can pin it.
3. **THROW / DROP.** Host throws the clump. Client: the clump freezes briefly at release, then SNAPS to the
   host's authoritative landed pile (one pile, right place).
4. **RE-PILE (no throw).** Host grabs + drops in place. Client re-skins clump->pile cleanly, no dup, no
   ~5 s vanish-return.
5. **Reverse / disconnect.** Client can't grab the host's mirrored pile in phase 1 (NoCollision — expected).
   Dropping a peer cleanly retires the proxy (no floating ball, no crash).

## What to read in the CLIENT log (the carry markers — all present in build `70f1f04b`)
A correct carry, in order, for the grabbed eid:
- `[PILE] CLIENT recv convert GRAB(pile->clump) eid=N ctx=1 known=0 isProxy=1 -- RECEPTION` — the ToClump
  convert arrived. (Its ABSENCE = the convert never reached the client — a race or a wire drop.)
- `[PILE] trash_proxy: SkinProxy CLUMP ... mesh-src=dirtball` — the clump mesh resolved. (`mesh-src=
  PILE-FALLBACK` would mean the clump renders identical to a pile — report it; it did NOT happen in smoke.)
- `[PILE] ... PROXY re-skinned IN PLACE to CLUMP` — the re-skin applied.
- `remote_prop: slot N GRAB-IN ... eid=N` then `drive #1 ... [proxy]`, `drive #60 [proxy]`, ... — the carry
  drive established + ADVANCING. This is the proof it follows. (If you see `CLIENT HOLD carry pose ...
  known=0` repeating and NO `GRAB-IN`, the ToClump convert was never adopted — capture it.)
- `[PILE] CLIENT recv convert LAND(clump->pile) eid=N ctx=3 -> re-skinned IN PLACE to PILE` — the landing.

RED flags (report): `mirror NOT-FOUND`, any `trash_proxy: ... FAILED`, a doubled/INVISIBLE pile, RSS
climbing without bound.

## Honest status
- **DUP FIX + VISIBILITY — hands-on VERIFIED** (`69405445`, carried into `70f1f04b`).
- **CARRY — MECHANISM SMOKE-PROVEN on a settled join** (`70f1f04b`, runs `b97z33gyh` + `b7oxr23uy`): the
  convert/mesh/drive/land all fire and the drive follows the host's path. **NOT yet a render confirmation**
  — this hands-on supplies that. Mark the carry VERIFIED only after you SEE the clump follow smoothly on the
  client on a fully settled join.
- **Known phase-1 edge (not the common case):** grabbing DURING a client's active join is not guaranteed to
  mirror (the convert may beat that pile's proxy). Phase-1 expectation: let the join settle, then interact.
