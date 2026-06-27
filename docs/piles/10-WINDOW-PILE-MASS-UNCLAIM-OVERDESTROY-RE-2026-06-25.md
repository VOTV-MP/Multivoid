# 10 — in-window pile MASS-UNCLAIM over-destroy (ALL piles vanish) — RE 2026-06-25

> **CONTINUATION (2026-06-27): the join-window PURGE-TIMING race.** A related but DISTINCT join-window pile bug
> was RE'd + fixed: UE's incremental GC sporadically destroys + re-instantiates a SPARSE handful (~2 of 870) of
> save-placed natives mid-join; they re-create UNBOUND = ghost piles (the bulk SURVIVE — the "purge re-creates
> all 870" model is FALSE). Lever (a) reaper-escalation (`bfe9182a`, drain 37s->1s, VERIFIED 11:32) + VARIANT 1
> host-wire position re-bind (`54ee4b06`, sidecar v2, audit SHIP, HANDS-ON PENDING). A `cursor-reset` attempt was
> built, REFUTED (mis-bind), and REVERTED (`86bca8cb`). Full:
> `research/findings/coop-purge-timing-reconcile-race-DESIGN-2026-06-27.md` + `[[project-grab-throw-joinwindow-2026-06-26]]`.

**Status: ROOT PINNED (a RACE — sweep fires mid-purge) + BOTH-LAYER FIX BUILT + PROVEN by a deterministic
controlled A/B (2026-06-25, binary `BCDD46DA`). Root fix = the purge-aware quiescence gate (commit
`5e91519a`); the per-class floor (Phase 0) is the complementary NET. Proof below (§FIX PROVEN). Remaining:
a hands-on VISUAL confirm (autonomous smoke is rendering-blind, but the floor's KEEP is engine-truth from the
re-seed pile count).**

## FIX PROVEN — deterministic A/B over the timing fix (2026-06-25, binary `BCDD46DA`, autonomous)

One binary, one variable (the floor toggle), the host forcing-flag `force_chippile_unclaim=1` injecting the
under-express on BOTH runs. The timing fix makes the sweep fire against the FULLY reloaded world (3082/3106
in-universe, NOT the old 88-trough) in BOTH runs -- the race is GONE -- so the only thing that differs is the
floor:

| | sweep FIRING | in-universe | doomed (pre-floor) | actorChipPile_C | destroyed | piles after |
|---|---|---|---|---|---|---|
| **A — floor OFF** (`disable_completeness_floor=1`) | `load tail quiesced; 2143ms after arm` | 3082 | 951 | 870 doomed | **951** | **0 (WIPED)** |
| **B — floor ON** | `load tail quiesced; 2176ms after arm` | 3106 | 975 | 870 **KEPT** | **105** | **870 (SURVIVE)** |

Run B log (client `Game_0.9.0n_copy`, 15:46:57):
`completeness FLOOR kept 870 unclaimed 'actorChipPile_C' -- host census 871, claimed only 0 this bracket` ->
`completeness floor KEPT 870 of 975 doomed actor(s) ... the unclaimed locals SURVIVE` -> `claim sweep -- 3106
in-universe, 2130 claimed, 105 unclaimed locals destroyed`. Post-sweep re-seed (15:46:59) still finds `870
keyless chipPile` = the actors SURVIVED (engine-truth, not rendering). Run A (floor off) wiped all 870 and
the post-sweep re-seed found `0 keyless chipPile`.

PROVES: (1) the timing fix made the catastrophe DETERMINISTIC (both runs fired at full-world via
`load tail quiesced`, never a deadline/ceiling, never the 88-trough); (2) the floor is the NET that flips
WIPE-870 -> KEEP-870 with everything else identical. The PATH B that was INCONCLUSIVE on 14:11 (host-skip
alone, sweep landed in the trough) is now CONCLUSIVE because the timing fix removed the race -- exactly as
designed (`research/findings/sweep-quiescence-purge-gate-DESIGN-2026-06-25.md` S6).

NOTE (separate, NOT a regression): Run B still destroyed 105 non-chipPile unclaimed locals (80 trashBitsPile
+ 1 mushroom7 + ~24 others) -- the floor's census covers chipPile (the mass catastrophe class) only. Whether
to extend the census to trashBitsPile is a SEPARATE scope question (smaller blast radius, under the >50%
valve); it is pre-existing sweep behavior, untouched by this fix.

---

## ORIGINAL RE (root pinning) -- retained below for the derivation

**Status: ROOT MECHANISM RE'd from a REAL hands-on log (2026-06-25 11:16, build `b70f9aec`).
NOT a regression (the docs/piles/09 fix was NOT deployed). This is WORSE than the dup: a full-class WIPE,
not a duplicate. (The "FIX NOT built" line is now SUPERSEDED -- see FIX PROVEN above.)**

> **PLAN (2026-06-25, DESIGN on-review): the catastrophe guard is specified in
> `docs/COOP_STABLE_ID_SIDECAR.md` §4 — a per-class COMPLETENESS FLOOR (a positive host->client
> per-class census manifest; the claim sweep may doom class C only when `claimedCount[C] >=
> manifest[C]`, else KEEP as incomplete). Ships as Phase 0, INDEPENDENT of the stable-ID work and FIRST.
> NOT a crude `>N%` threshold (that would block a legitimate mass-clear). A stable ID does NOT fix this
> (no expression -> no ID on the wire); the floor is the dedicated guard. Open root (WHY the host
> under-expressed) still needs the host expression-trace + the pure-pile-throw-no-kerfur isolation.**

## The symptom (user, 2 runs, hands-on)
- **11:14: clean** — all piles present on the client.
- **11:16: ALL piles VANISHED on the client** — the user threw piles NEAR kerfurs and toggled those
  kerfurs off/on in the SAME connect window. Pile×kerfur interaction in one join window.

## What the log proves (client `Game_0.9.0n_copy`, 11:16)
- **Keyless chipPiles 870 -> 0** between `11:16:59` (`re-seed found ... 870 keyless chipPile element(s)`)
  and `11:17:03` (`... 0 keyless chipPile element(s)`). The destroy happened at **quiescence**
  (~11:17:00), ~9 s after world-ready (11:16:51) — so the piles **LOADED (appeared) then were
  destroyed**, i.e. appeared-then-vanished, NOT never-delivered.
- The killer = the **claim sweep** (`remote_prop_spawn.cpp` ~1058-1140): `claim sweep -- 3083 in-universe
  actors live, 2129 claimed, 953 unclaimed locals destroyed (client adopts host world)`. A keyless
  chipPile that is UNCLAIMED is doomed UNCONDITIONALLY (`remote_prop_spawn.cpp:1071`
  `if (IsChipPile(a)) { doomed.push_back(a); }` — no key skip, no per-pile guard). All 870 piles were
  unclaimed -> all in the 953.
- **The pile save-time reconcile NEVER RAN** this join: zero `PILE-1C` / `sweep-reconcile` /
  `TryDestroyTwin` lines on the client. So nothing CLAIMED the piles. Contrast: kerfur scope-A DID run
  (`kerfur_reconcile: ARMED ... eid=3149` -> `sweep-retire 1 of 1`). The pile channel was the one that
  failed — the asymmetry is the heart of it.
- The bracket DID arm normally (`join_progress: BeginSnapshot -- receiving world (3087 objects)` 11:16:55;
  `claim tracking ARMED`; `mirror_defer: ARMED`). NOT a SnapshotBegin-lost flake. Instant-world deferred
  layer captured 0 mirrors (`lift-reveal 0 confirmed / 0 held`) — piles don't ride the W1 fresh-spawn
  choke, so the upper layer neither hid nor harmed them (it's a visibility layer; this is correctness).

## The mechanism (root, code-proven)
1. The host did **not claim/express** the 870 keyless piles this join (normally it does — L1 is
   verified). So the client's save-loaded native piles stayed OUT of `g_claimedActors`.
2. The claim sweep dooms **every** unclaimed keyless chipPile (`remote_prop_spawn.cpp:1071`,
   unconditional — chipPile is "expressible keyed OR keyless (eid lane)", so the host is EXPECTED to
   express each one; unexpressed == divergent == doomed).
3. **THE VALVE GAP (the over-destroy enabler):** the claim sweep's `>50%` abort valve
   (`remote_prop_spawn.cpp:1117`) is **GLOBAL** (`doomed*2 > inClass`, total in-universe props), NOT
   per-class. 953 doomed of 3083 = 31% < 50% -> NO abort — even though it destroyed **100% of the
   piles**. A whole-class wipe slips under a global valve whenever that class is a minority of the world.

## NOT the dup, NOT the docs/piles/09 fix
- `docs/piles/09` (the move-dup) = ONE pile not reconciled (eid-0-at-grab). THIS = ALL piles UNCLAIMED ->
  mass-destroyed. Different root, worse blast radius.
- The docs/piles/09 fix (self-seed eid + carry the key on the convert) is **orthogonal**: it helps a pile
  that IS expressed but mis-keyed; here the piles are never EXPRESSED at all. The fix neither causes nor
  cures 11:16 (and it was NOT deployed at 11:16 — build `b70f9aec` predates it).
- The user's first hypothesis (kerfur sweep position-collision DESTROYS the piles) is REFINED by the
  data: the sweep did NOT over-MATCH; it doomed UNCLAIMED piles. So it is an EXPRESSION failure, not a
  position collision. The kerfur toggle is the prime SUSPECT for disrupting the host's pile expression,
  but the log does not yet prove causation.

## OPEN ROOT (the one thing to pin next)
**WHY did the host fail to express the 870 keyless piles this join?** Normally (L1) it expresses them and
the client claims them. Candidates: (a) the in-window kerfur toggle + pile throws disrupted the host's
keyless-pile expression / re-seed; (b) a host connect-replay completeness flake independent of kerfur.
Decisive isolation (await user): does the over-destroy reproduce with a **pure pile-throw in-window, NO
kerfur toggle**? If yes -> kerfur is coincidental, the pile-expression is just flaky. If only-with-kerfur
-> the kerfur toggle is causal. Then grep the HOST log's pile-expression path for that run.

**UPDATE 2026-06-25 14:11 (PATH B forcing experiment -- the over-destroy is HARDER to trigger than
modeled).** A dev forcing flag made the host SKIP expressing ALL chipPiles (`force_overdestroy_test:
ARMED`), so the joiner should have been left with ~870 unclaimed-in-universe natives -> a deterministic
wipe. It did NOT happen: the client SEEDED 871 natives (`seeded ... 871 keyless chipPile element(s)`)
but its claim sweep still showed `88 in-universe, 88 claimed, 0 destroyed` -- IDENTICAL to a clean run.
So "host expresses 0 piles" does NOT by itself create the 870-unclaimed-in-universe state.

**UPDATE 2026-06-25 (read-only RE -- the 871->88 collapse + the TRUE 11:16 ROOT are now PINNED from the
14:11 client log).** The collapse is a **world-reload mass-PURGE during the join**, and the sweep firing
INSIDE the purge trough. Proven timeline (client `Game_0.9.0n_copy`, 14:11):
```
14:11:14  seed: 3316 live actors incl 871 chipPiles -> ALL MarkPropElement'd (Registry has them)
14:11:16  BeginSnapshot (3087 obj) + claim tracking ARMED
14:11:21  lift-reveal 412 mirrors; Complete (applied 2215/3087); census 871 received
14:11:26  prop_element_tracker: reaped 256 dead Prop Elements (scanned 5011)  <- MASS PURGE
14:11:26  net_pump: mass-purge detected (reaped 256>=64) -- world-change re-seed DEFERRED to drain-complete
14:11:27  claim sweep -- 88 in-universe, 88 claimed, 0 destroyed   <- FIRED MID-PURGE (InPurgeEpisode==true)
14:11:30..14:12:07  reaper grinds 5011 -> 2452 dead Prop Elements (256/call, ~10 ticks)
14:12:11  re-seed (drain-complete): 3249 live, added 3001 NEW (870 chipPile)  <- natives re-registered, 44s LATE
```
So the 871 native chipPile Prop Elements registered at the boot seed (14:11:14) are DESTROYED by the
save-load / world-reload purge that runs AFTER the seed. At sweep time (14:11:27) those 871 are DEAD Prop
Elements (`IsLiveByIndex` false) -> skipped at `remote_prop_spawn.cpp:1069` (`continue` before `++inClass`)
-> never enter `inClass` OR `doomed`. Only 88 cross-purge-surviving live props are in-universe. The reaper
then drains the 5011 dead elements over the next ~40s, and the re-seed re-registers the reloaded natives at
14:12:11 -- 44s AFTER the sweep already ran. **The ~783 "missing" went: DEAD (purged) -> reaped, then
re-created by the async loadObjects pass + re-registered too late to matter to the sweep.**

**TRUE 11:16 ROOT = the claim sweep's quiescence gate is BLIND to the purge episode, so the sweep fires at
a RANDOM point relative to the native pile reload. It is a RACE, not a deterministic under-express.** The
gate (`CountLoadTailUnsettled_`, `remote_prop_spawn.cpp:1308`) counts only (a) allowlisted NPCs and
(b) keyless NON-pile props (it EXPLICITLY excludes chipPiles at :1329) and does NOT consult
`InPurgeEpisode()` or the reaper backlog. So it declared "quiesced" at 14:11:27 while `InPurgeEpisode()`
was true (set `net_pump.cpp:530`, cleared only at drain-complete `net_pump.cpp:538`). Depending on where the sweep lands in the
purge->reload cycle:
- sweep fires AFTER the natives are re-created + re-registered LIVE but BEFORE the host claims them ->
  in-universe + unclaimed -> **mass-doomed = the 11:16 catastrophe (953 unclaimed, 870 piles 870->0).**
- sweep fires DURING the purge trough (natives dead/reaping, not yet re-registered) -> only ~88 survivors
  in-universe -> nothing doomed -> **14:11 / 13:21 "safe" -- but by luck, not by design.**

11:16's 953-unclaimed and 14:11's 88 are the two sides of that race. This is the SAME "snapshot before
state ready" recurring class ([[feedback-snapshot-before-state-ready]] / [[feedback-join-reconcile-sweep-safety]]):
the sweep adjudicates against a half-reloaded Registry.

**FLOOR RE-ASSESSMENT (the model revision):** the per-class floor is CORRECT and NECESSARY but it is a
NET, not the root. It can only engage when the natives reach the sweep LIVE + unclaimed (the danger-window
side of the race) -- there it keeps them (`claimed 0 < census 870`), directly preventing the wipe. In the
trough case there is nothing doomed, so it stays dormant (correctly). The floor does NOT remove the RACE.
The RULE-1 root fix is to make the sweep's quiescence gate wait for the purge to DRAIN + the current-world
re-seed to run (treat `InPurgeEpisode()` / an un-re-seeded current world as "unsettled", and include the
chipPile population in the load-tail count) -- the hard-deadline backstop stays. That removes the race:
the sweep always adjudicates the fully-reloaded world. BOTH layers are needed and complementary:
1. **Root/timing fix** -- the sweep never fires against a mid-purge Registry (kills the non-determinism).
2. **Floor** -- the catastrophe net for when, even at TRUE quiescence, the host genuinely under-expressed a
   whole class (the original kerfur-toggle-disrupts-pile-expression hypothesis). Irreplaceable for that.

**This also makes the floor DETERMINISTICALLY PROVABLE:** with the root timing fix in place the sweep
always fires with the natives live-registered, so `force_chippile_unclaim` (host-skip) then yields ~870
live-unclaimed at sweep -> the floor MUST keep them. PATH B failed precisely because, without the timing
fix, the sweep landed in the trough where host-skip is moot (no live natives to leave unclaimed).

## FIX DIRECTIONS (design only — NOT built; pick after the root is pinned)
- **Safety net (defends regardless of root): a PER-CLASS floor on the claim sweep.** Never destroy ~100%
  of a class (esp. chipPile) when the host expressed 0 of it — treat "0 expressed of N>threshold loaded"
  as an INCOMPLETE snapshot (the same logic the global valve uses), abort that class, keep the loaded
  piles. A full-class wipe is never a legitimate divergence. (`remote_prop_spawn.cpp:1071/1117`.)
- **Root fix: ensure the host always expresses/claims the keyless piles** (the L1 path), even under an
  in-window kerfur toggle + pile throw. Needs the host-side expression trace first.
- Order: pin the root (above) -> ship the per-class floor as the catastrophe guard regardless -> then the
  root fix.

## Source map (cited)
`remote_prop_spawn.cpp:1058-1140` (claim sweep: 1063 claimed-keep, 1071 chipPile unconditional doom, 1092
keyless-skip tripwire, 1117 GLOBAL >50% valve, 1132 destroy) · the 11:16 client log
`Game_0.9.0n_copy/.../votv-coop.log` (870->0 keyless, 953 destroyed, no PILE-1C, kerfur retire ran) ·
`docs/piles/09` (the move-dup, distinct) · `pile_reconcile.cpp` (SweepReconcileSaveTimeTwins + its own
>50% valve — did NOT run this join) · `docs/COOP_MIRROR_IDENTITY_WINDOW_RACE.md` (the class).
