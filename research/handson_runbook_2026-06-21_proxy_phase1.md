# Hands-on runbook — trash-proxy PHASE 1 — 2026-06-21, take-23 (DUP-FIX + VISIBILITY VERIFIED; LIVE CARRY OPEN)

**Deployed:** `votv-coop.dll` SHA `69405445` to all 4 copies (host / copy / copy2 / dev). Proto **v82**
(UNCHANGED — none of this touches the wire). Code HEAD `245148c6`. Build CLEAN (Release).

**STATUS — read this before testing:**
- **DUP FIX — hands-on CONFIRMED.** No doubled piles; resting + landed piles mirror correctly and VISIBLY.
  (The earlier-build `f2344bab` proxies were INVISIBLE — a runtime `AStaticMeshActor` defaults to STATIC
  mobility, on which `SetStaticMesh`/`SetActorLocation` silently no-op; the render-blind smoke passed anyway.
  FIXED with `SetComponentMobility(Movable)` in `245148c6`; the user then confirmed it works visually.)
- **THE LIVE CLUMP CARRY — does NOT mirror (OPEN).** When the host grabs + carries a pile, the client's proxy
  STAYS A PILE at its rest spot — it does NOT re-skin to a clump in the host's hand and does NOT follow. The
  user saw "old pile not removed when host grabbed it" + "the clump/pile animation is ~2 frames a second"
  (the carried clump only jumps on an occasional convert; it is NOT a smooth carry, and global FPS is fine).
  This is the OPEN phase-1 north star — root-caused in the CARRY-MIRROR OPEN section of
  `research/findings/votv-pile-mirror-staleness-robustness-DESIGN-2026-06-21.md`.
- **The earlier "autonomous smoke FUNCTIONALLY GREEN / grab→carry→throw→re-pile end-to-end" is WITHDRAWN** —
  that smoke is render-blind AND the autotest grabbed DURING the client join (a race), so it never cleanly
  tested a post-join carry. Only the dup-gone + the final landing were ever evidenced by it.

> This SUPERSEDES take-22 (the s38 re-pile thunk on `BA79E705`). The thunk + triple-cue fix (s38, A+B)
> are FOLDED IN and were log-GREEN on `BA79E705`. Phase 1 adds the **host-authoritative `AStaticMeshActor`
> proxy** (the dup fix) on top — its DUP-FIX + VISIBILITY are now hands-on confirmed; the live carry is the
> remaining OPEN item.

## What phase 1 changes (vs the `BA79E705` you last tested)

The client no longer mirrors a host pile/clump with a **real `actorChipPile_C` / `prop_garbageClump_C`
blueprint**. It now spawns an **`AStaticMeshActor` WE own** (no BP, `AddToRoot`, our eid->actor registry)
and **re-skins it in place** on every convert. Because that actor never self-morphs, never GCs, never goes
stale-index, the "mirror NOT-FOUND -> spawn fresh -> orphan the original = DUP" path is **structurally
unreachable**. Plus three robustness fixes for the long carry:

- **VISIBILITY (`245148c6`)** — the proxy's `UStaticMeshComponent` is set to **Movable** mobility so
  `SetStaticMesh` + `SetActorLocation` actually take effect (a runtime `AStaticMeshActor` is STATIC by
  default → those calls no-op silently). This is what makes the mirrored piles VISIBLE at all.
- **HIGH-1** — a convert that beats its spawn now renders the correct form (clump vs pile); a stale trailing
  spawn can no longer flip a carried clump back to a pile.
- **HIGH-2** — the clump now wears the correct **per-chipType material** (verified `setTex` bytecode:
  `SetMaterial(0, getChipPileType(chipType).GetMaterial(0))` on the fixed dirtball mesh).
- **km-walk lerp + freeze (BUILT, but NOT yet exercised)** — the proxy is meant to be **interpolated**
  between host poses and to **FREEZE** at the last pose on a network hitch (releasing only on an explicit
  reliable edge: throw / re-pile / disconnect). **This code does NOT run yet** — it is downstream of the
  carry pose-drive establishing, and the carry drive currently never starts (the live carry is OPEN). It
  cannot be tested until the carry is re-fixed.

Phase 1 is **NoCollision**: the carried/mirrored trash is a kinematic host-driven follower; **your local
player passes THROUGH mirrored trash** (you cannot bump or grab the OTHER peer's mirrored pile yet). That is
the accepted phase-1 limitation — collision + client-grab is phase 2.

## The test (do this on a FRESH save with chipPiles, or one of your recent pile saves)

Two peers (host + client). Both load the same recent save that HAS chipPiles (the garbage litter).

1. **JOIN — the dup check + visibility (the part that WORKS, already confirmed).** Client joins. Look at a
   host pile cluster on BOTH screens.
   - PASS (confirmed): each host pile shows as exactly ONE pile on the client — VISIBLE, correct mesh, right
     place. NO doubled piles, NO piles that "don't disappear". Walk around the cluster on the client — still
     one-for-one. (Re-confirm this still holds; this is the verified part of phase 1.)
2. **GRAB + CARRY — the OPEN item; as of `69405445` this does NOT work.** HOST grabs a pile (it becomes a
   clump in the host's hand) and WALKS.
   - **CURRENT (BROKEN) behaviour to expect:** on the client the proxy **STAYS A PILE at its rest spot** — it
     does NOT re-skin to a clump in the host's hand and does NOT follow. The original pile looks "not
     removed," and any update is a ~2 fps convert-jump rather than a smooth carry. **This is the known OPEN
     bug** — do NOT report it as new; it is the phase-1 north star being worked. (Root cause: the ToClump
     convert is not adopted on the client → `0` `GRAB-IN`, the carry pose-drive never establishes →
     CARRY-MIRROR OPEN section of the staleness finding.)
   - **What a future PASS will look like (retest AFTER the carry is re-fixed):** the client proxy re-skins to
     a clump in the host's hand and tracks it SMOOTHLY (interpolated), stays the right per-chipType look, and
     never desyncs/drops however far the host walks; a brief network hitch FREEZES it (not a drop), then
     resumes.
3. **THROW / DROP (blocked on step 2).** HOST throws the clump.
   - Because the carry never re-skins on the client today, this can't be cleanly judged yet. (Future PASS:
     the clump freezes at release for the brief flight, then SNAPS to the host's authoritative landed pile —
     one pile, right place. Retest with step 2.)
4. **RE-PILE (no throw) — the s38 thunk path.** HOST grabs a pile and drops/re-piles it in place.
   - On the HOST this converts cleanly (no ~5 s vanish-return per the s38 thunk). On the CLIENT the in-place
     re-skin depends on the same convert-adoption that the carry needs, so judge it together with step 2.
     The dup must NOT appear either way.
5. **Reverse it.** CLIENT cannot grab a host's mirrored pile in phase 1 (NoCollision; the look-trace passes
   through). That's expected — do NOT treat "client can't grab the other peer's pile" as a bug. (Each peer
   can still grab ITS OWN local piles; those broadcast to the other as proxies.)
6. **Disconnect.** Drop the client (or vice-versa) while piles are mirrored.
   - PASS: no leaked floating ball, no crash; the proxy is cleanly retired on the surviving peer.

## What to read in the log (client log is where the proxy lives)

CONFIRMED-WORKING markers (client log):
- `trash_proxy: SPAWN eid=... pile/clump ... (AStaticMeshActor, rooted, NoCollision)` — proxies created at join.
- `CLIENT recv convert LAND(clump->pile) eid=... -> PROXY re-skinned IN PLACE ...` — a landed pile re-skin.
- `trash_proxy: RETIRE eid=...` on destroy/disconnect (no leak).

MARKERS THAT ARE CURRENTLY MISSING = the OPEN carry bug (their ABSENCE is the symptom, not a pass):
- `CLIENT recv convert GRAB(pile->clump) eid=... -> PROXY re-skinned IN PLACE` — the ToClump re-skin. NOT
  appearing today (the convert is not adopted).
- `remote_prop: slot N GRAB-IN` / `... drive #N -> target(...) [proxy]` — the carry pose-drive. NOT appearing
  today (`0` `GRAB-IN`), which is exactly why the carried clump never follows.
- When the carry is re-fixed these should appear for the carried eid; until then their absence confirms the
  open bug.

RED flags (report these):
- `mirror NOT-FOUND` / `no local mirror of E existed` — the staleness path that caused the dup (should be GONE).
- any `trash_proxy: ... FAILED`, `SetComponentMaterial unresolved`, `proxy spawn-on-convert FAILED`.
- a doubled pile visible on screen, or any pile INVISIBLE on the client (the Movable fix should make all
  mirrored piles visible).
- RSS climbing without bound (no leak).

## Honest status
- **DUP FIX + VISIBILITY — hands-on VERIFIED** (`69405445`): no doubled piles, resting + landed piles mirror
  correctly and VISIBLY (the user confirmed). This is the solid part of phase 1.
- **LIVE CLUMP CARRY — OPEN, does NOT work.** The host grabbing + carrying a pile does NOT re-skin/drive the
  client's mirror (it stays a pile at its rest spot). This is the remaining phase-1 north star.
- The earlier "autonomous smoke FUNCTIONALLY GREEN" framing is **WITHDRAWN** — that smoke is render-blind and
  the autotest grabbed during the join; it only ever evidenced the dup-gone + the final landing, never a
  clean post-join carry. Do NOT treat the smoke as proof the carry works.
- **NEXT:** the carry needs to be re-fixed (the ToClump convert adoption + the pose-drive establishment — see
  the CARRY-MIRROR OPEN "NEXT" steps in the staleness finding: instrument the convert reception + drive,
  then fix the autotest to grab only AFTER a settled join and HOLD-and-MOVE for a sustained carry). Mark the
  live carry VERIFIED only after a real hands-on shows it on your screen — NOT from a smoke.
