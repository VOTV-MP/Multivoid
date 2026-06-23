# Hands-on runbook -- Path 1c: join-window pile DUP fix (in-window drop repro)

**Deployed:** `votv-coop.dll` MD5 `FD9D2DC67496CF96B7953F72282067EE` (host + client + copy2 + dev), proto **v86**.
> NOTE (2026-06-23, L5 work after take-4): the deployed DLL is no longer the take-4 pile build (`D3C5BEA`) --
> the L5 FPS-hitch work rebuilt on top. **The take-4 PILE self-seed fix is intact in this build** (committed
> `5b01bc0e`); the L5 changes are an UNVERIFIED interactable migration (door detection regresses to ~50/57 --
> irrelevant to the pile test) + diagnostic instrumentation + `perf_probe=1`. For the PILE hands-on below,
> the door regression does not matter; ignore any door-sync oddity. The save-time pile capture is unchanged.
**HEAD:** take-4 fix `5b01bc0e` (1c `4c286cae` + load-tail `124fbc9d` + **self-seed the capture, this commit**; the take-3 OnRequest gate `57ad49e5` was REMOVED -- it never fired). Push HELD.
**Test flag:** `pile_delta_probe=1` is set in both deployed inis -> the in-game JOIN-WINDOW cues + the
`[PILE-DELTA]`/`[PILE-CENSUS]` probe logs are ON.

> **TAKE 4/5 (after the self-seed fix).** Take 3's gate NEVER FIRED -- the host STILL captured
> **`0 pile save-time xforms`** (VOTV is one persistent UWorld, so `IsRegistrySeededForCurrentWorld()` stays
> TRUE across the prop-element mass-purge -> the gate predicate was wrong). The no-dup in take-3 was the
> non-deterministic loc-dedup/doom fallback (the same path that duped 3/4+4/4 in take-2), NOT the save-time
> path. **take-4 fix: SELF-SEED the capture** -- it now MINTS the eid inline for any unseeded live chipPile,
> so the map is never empty. **fix#1 is ALREADY AUTONOMOUS-VERIFIED** (joinchurn: host `self-seeded 870 ...
> -> 870 pile save-time xforms`, was 0; client census 0). **This hands-on is the END-TO-END test the smoke
> can't do: MOVE piles in the window and confirm the save-time key resolves the dup.** Same as before: the
> dup may FLASH at join then RESOLVE to 4 within ~10-15 s (the quiescence-gated sweep). Judge AFTER it settles.
**Status:** fix#1 (empty map) AUTONOMOUS-VERIFIED P:0->870. End-to-end moved-pile resolution = THIS hands-on.

You (host) click `mp_host_game.bat`; the client clicks `mp_client_connect.bat`. Claude does not launch.
Claude greps the logs after.

---

## What changed (the fix under test)

A chipPile the HOST MOVES during the join-load window dups on the client. Root: the client builds ONE
pile from TWO channels that share no identity -- the scratch-save NATIVE @OLD + the connect-replay PROXY
@NEW -- and the 1cm twin-destroy matched the proxy's CURRENT pose, which is >1cm off for a moved pile, so
both survived = DUP.

Path 1c: the host now stamps each pile's join-snapshot with its **SAVE-TIME position** (the frozen value
both peers loaded from the same transferred save). The client twin-destroy matches the save-loaded native
against THAT, so a moved pile reconciles (native @old destroyed, proxy @new is the sole mirror). With the
fix the in-window dup must be **0**.

---

## THE in-window-drop repro (the gate)

This reproduces exactly the controlled hands-on that found the bug (2026-06-23): 2 kerfur + 4 pile, host
moves them in the join window.

1. **Host:** `mp_host_game.bat`. Fresh New Game (or a recent fresh save). Get in-world.
2. **Host:** stage **6 items on the path** by the office: **2 kerfur + 4 chipPiles** (grab trash, throw to
   make piles; spread them a bit so they're individually visible). Note where they sit.
3. **Client:** `mp_client_connect.bat`. The client now enters the **join window** (requests the host save,
   downloads ~18 MB, loads ~30-60 s). **You (host) see the window edges IN GAME as chat lines** (because
   `pile_delta_probe=1`):
   - `[1c-test] JOIN-WINDOW OPEN -- ... move/drop test piles NOW` appears the instant the joiner requests
     the save = window **opens**.
   - `[1c-test] JOIN-WINDOW CLOSED -- ... post-load` appears when the joiner hits world-ready = window
     **closes**. (Do NOT use "<nick> joined the game" as the edge -- it fires +5 s LATER and is ambiguous.)
4. **Host -- BETWEEN the two on-screen chat lines (after OPEN, before CLOSED):** drag/throw all **4
   chipPiles onto the asphalt** (well away from the path, >1 m). Move the 2 kerfur too (control). Drop
   EARLY -- right after the OPEN line -- so you're comfortably inside the window, not racing the CLOSED line.
5. **Client:** once loaded, look at BOTH the path AND the asphalt.

### PASS / FAIL (what the client must see -- AFTER the ~10-15 s sweep settles)
- **PASS:** **4 chipPiles total** -- all on the **asphalt** (the host's new position). The path has **no
  leftover piles**. A brief flash of 8 right at join that RESOLVES to 4 within ~15 s is the PASS path (the
  sweep removed the late natives@old). 2 kerfur (wherever they ended). No doubled piles after it settles.
- **FAIL:** **8 chipPiles PERSIST** past ~20 s -- 4 on the path (old/save) AND 4 on the asphalt (new). The
  sweep didn't resolve it -> grep `[PILE-1C] sweep-reconcile` (did it run? how many removed? did the valve
  abort?).
- Kerfur is the control: it never duped before and must not now (single channel).

---

## Secondary check -- host-DELETED-in-window (separate case)

Path 1c fixes the MOVE case. A pile the host **collects/deletes** in the window is a different path (no
proxy at all -> the client native is an orphan with no twin to match). That case is meant to be cleaned by
the existing divergence sweep (doom-removes un-mirrored natives), NOT by this fix. So ALSO test it:

6. On a second join (host re-stage 4 piles, client reconnect), **DELETE 2 piles** in the window (walk up,
   grab + put in a bin / let them despawn) instead of moving them.
7. **Client:** the 2 deleted piles must NOT appear as orphans on the path. If they DO linger, that's a
   separate (sweep) gap -- report it; it is a fast-follow, not a 1c regression.

---

## Confirming probe (optional, settles the float-exactness residual)

To confirm the save-time position the host recorded == the position the client loaded (the Edge-1
bit-exactness assumption), set `pile_delta_probe=1` in `votv-coop.ini [dev]` on BOTH peers before the run.
With it on, the `[PILE-DELTA]` / `[PILE-CENSUS]` lines log nearest-native deltas. A clean in-window MOVE
run should show the matched twins at ~0.0 cm (save-time key vs loaded native) and **0 leftover orphans**
after the sweep. (Flag lives in the ini, not the bat -- the established probe pattern.)

---

## What Claude greps after (host log + client log)

Host log:
- **`prop_element_tracker: CollectTrackedPileTransforms self-seeded K unseeded live chipPile(s) ... -> P pile
  save-time xform(s)`** -- THE TAKE-4 FIX. Fires when the host connected mid-load (registry deferred); mints
  the eids the capture needs. (Already auto-verified at K=870.)
- **`save_transfer: slot N -- captured K keyed-prop keys + P pile save-time xforms at blob instant`** -- THE
  acceptance gate for the first break. **P must be NON-ZERO and ~the host's live chipPile count (hundreds, not
  4 -- the whole world's piles are mapped, not just the staged ones).** `P=0` here = the fix did NOT take ->
  STOP, the chain is still broken at capture; do not judge the visual. (Auto-smoke already showed P=870.)
- `[PILE-1C] slot N world-ready -- JOIN-WINDOW CLOSED` -- the window-close marker (the moves must precede it).

Client log:
- `pile_reconcile: pile-bind index built -- M local chipPile candidate(s)` -- the join twin index.
- `[PILE] DESTROY native level-pile twin eid=E at (X,Y,Z) ...` -- a twin destroyed AT WORLD-READY (the
  in-time piles). The MOVED piles will MISS here (their native@old hasn't loaded yet) -> that's expected.
- **`[PILE-1C] sweep-reconcile -- N of M pending save-time twin(s) removed`** -- THE NEW FIX. At the post-
  quiescence sweep (~10 s after join), the late-loaded native@old gets destroyed. **N should equal the
  moved-pile count (4).** If `M>0` but `N=0`, or `sweep-reconcile ABORTED ... >50%`, that's the failure to
  chase.
- `[PILE-CENSUS] ... live orphan native(s)` -- with the fix, **0** (or down from 4 to 0) -- it runs AFTER
  the sweep-reconcile, so it reflects the removals.

---

## Honest scope
- 1c fixes the host-MOVED-in-window dup (the proven repro). Unmoved piles: unchanged. New (post-save)
  piles in window: no save-native twin -> no dup, unchanged. Deleted-in-window: relies on the existing
  sweep (secondary check above), NOT this fix.
- An UNSEEDED-at-save pile (no host eid at the scratch-save instant) is skipped by the map -> falls back to
  the live-pose match; if such a pile is ALSO moved in-window it could still dup. **Take-3 CLOSES the broad
  case of this** (a client connecting mid-host-load left ALL piles unseeded -- the deferred re-seed -- which
  is exactly what take-2 hit and the OnRequest forced re-seed now prevents). A pile that genuinely spawned
  AFTER the seed walk but before the capture (a gameplay trash-spawn in the sub-second connect window) is
  still skip-to-live-pose -- vanishingly rare; flag if seen.
