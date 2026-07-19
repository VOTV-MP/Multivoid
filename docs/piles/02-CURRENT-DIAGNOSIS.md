# Pile sync — current diagnosis (the two live bugs, root-caused)

> Root causes for the two dupes the user hits, from the cooked-pak bytecode RE + the user's
> REAL hands-on logs (the s31 doom run = `*.failrun-s31.log`; the s32 strip run = the current
> `multivoid.log`). Both diagnosed 2026-06-20. Citations are file:line / log markers.

## Bug 1 — JOIN DUPE (the client ends with 2 of each pile)

### Mechanism
The joiner loads the host save → it materializes its OWN ~870 pile actors at the host's
positions. The host then streams a keyless host-range eid per pile. Two outcomes:
- **ADOPT (correct):** the mirror eid binds onto the client's existing pile → 1 actor.
- **FRESH-SPAWN (dupe):** the mirror is a NEW actor spawned beside the client's save pile, and
  the mechanism meant to delete the save original never runs → 2 actors.

The uncommitted s30-31 "thin-client doom" does the fresh-spawn variant and relies on a doom to
delete the originals. **The doom wedges and never fires.**

### Evidence (s31 doom run logs)
- Client connect: `RegisterPropMirror ... actorChipPile_C` x870 (fresh mirrors), `PILE DOOM
  armed for the CONNECT bracket -- live catalog=870`.
- Then forever: `pile-doom gate DEFERRED -- liveMirrors=758 < expected=868 ... retry next tick`.
  **ZERO `PILE DOOM FIRING` lines** anywhere. The gate (`prop_adoption.cpp:762`) requires
  `liveMirrors >= expected`; the numerator (live registered mirrors) and denominator (the live
  `g_hostPileCatalog`, never decremented for engine-silent frees) never reconcile after the
  shadow-drain → permanent defer → the save originals are never destroyed → DUPE.
- `pile-dupe-rootcause-RE-2026-06-20.md` §1: connect burst = "870 spawned NEW + 513 already
  aligned" both sides = the structural double-source.

### Root cause
The recent stack abandoned **adopt-by-position** (which re-uses the client's pile, so there is
never a second actor) for **fresh-spawn + doom-the-original**, and the doom's cross-population
count gate cannot converge after the join shadow-drain. The adopt mechanism that prevents the
dupe by construction is intact at committed HEAD `1272b0a3` (`EnsurePileBindIndex`,
`remote_prop_spawn.cpp:498-563`) and was deleted in the uncommitted churn (working tree
`remote_prop_spawn.cpp:452-454`: "ALWAYS falls through to the fresh-spawn path").

### Fix
Restore adopt-by-position (the client's save pile BECOMES the mirror). See
[03-RESTORATION-PLAN.md](03-RESTORATION-PLAN.md). The doom/catalog/resync apparatus is then
DELETED (RULE 2) — there is nothing to reconcile when the original IS the mirror.

## Bug 2 — INTERACTION DUPE (client spamming E accumulates local-only clumps)

This bug is INDEPENDENT of the save piles — it reproduced even in the save-strip build (where
the client loaded zero save piles). The client is **authoring new local pile/clump actors on
its own E-presses**.

### Evidence (the strip run logs, client = `Game_0.9.0n_copy`)
1. Connect 16:50:49-56: `RegisterPropMirror ... actorChipPile_C` x870 (every visible pile is a
   host mirror, host-range eid, ownerSlot=0).
2. **16:51:01 — the killer:** `net_pump: mass-purge detected (reaped 256 >= 64, keyed 256 > 0
   ... world-change re-seed deferred to drain-complete)` — the join's same-world **shadow-drain**.
   The engine frees the 870 mirror chipPile actors. The reaper then runs at its 256/scan cap
   CONTINUOUSLY to session end (never drains below threshold) → `InPurgeEpisode` stays `true`
   for the whole session.
3. On a **bound** pile E-press (works): `grab CANDIDATE cached eid=5261` → `RelayClientGrab` →
   host `OnPileGrabRequest -> clump eid=5263` → client `adopted local clump onto host
   clumpEid=5263`. eids 5261-5282 cycle cleanly.
4. On an **unbound** pile E-press (the dupe): `grab CANDIDATE cached eid=4294967295`
   (`0xFFFFFFFF` = kInvalidId) → `PileGrabRequest relay=no-op (unbound / peer-range pile)`. The
   BP still morphs the pile locally: `net: NEW held actor ... cls='prop_garbageClump_C'
   key='None' eid=0 -> prop-mirror=no`. PART 2 correctly suppresses wire-authoring (`CLIENT
   holds shared trash ... NOT authoring it`) so the **host does not dupe**, but the client is
   left holding a **local-only clump (eid=0) that nothing ever destroys.** ~17 accumulated.
5. The cleanup (the doom) is wedged: `pile-doom gate DEFERRED -- liveMirrors=1 < expected=869`
   forever. Counts: `mass-purge detected` = 1, `world-change re-seed added` (the exit branch
   that sends `PileResyncRequest`) = **0** → the client NEVER sent the resync → the host never
   re-streamed → the freed mirrors never came back.

### Root cause (the actual fixable defect)
The host pile-mirror actors freed by the join shadow-drain are **never re-streamed**, because the
`PileResyncRequest` trigger (`net_pump.cpp:524-527`) is gated on the purge-episode **EXIT** edge
(`InPurgeEpisode` false→true→**false**) — and under sustained world churn + E-spam the reaper
stays saturated, so the episode is entered once and **never exits**. With the mirrors gone and
never restored, the piles the player grabs are mirror-UNBOUND (`ResolveMirrorEidByActor` →
`kInvalidId`, `pile_handle.cpp:237`) → the relay no-ops (`pile_handle.cpp:456`) → the BP's local
`toClump()` morph runs unsuppressed → a permanent local-only clump per E-press.

### Fix
Decouple the pile re-stream trigger from the purge-episode-EXIT edge: send `PileResyncRequest` on
the **entry** edge (the first scan where the host mirror set collapses while the catalog is
non-empty), not on episode-complete. The host re-stream machinery already exists, works, and is
debounced (`prop_snapshot.cpp:566-663`, coalesces duplicate triggers). NOTE: under the
**restored adopt-by-position** scheme this bug largely dissolves — the piles are the client's own
adopted actors, and a post-drain re-run of the adopt re-binds them rather than needing a host
re-stream of fresh mirrors. The trigger fix is the belt-and-suspenders for the re-stream path.

## The unifying insight

Both bugs trace to the same root: **the recent stack treats the client's pile as disposable
(fresh-spawn a mirror, destroy the original) instead of as the thing to ADOPT.** Adopt-by-position
makes Bug 1 impossible (no second actor) and makes Bug 2 self-healing (the unbound pile is the
client's own actor; re-running the adopt re-binds it). The keyless-identity problem that drove
the destroy+fresh-spawn detour is real, but the answer is "re-run the position adopt after the
drain," not "stop adopting."
