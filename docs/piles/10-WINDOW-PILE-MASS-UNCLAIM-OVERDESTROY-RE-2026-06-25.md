# 10 — in-window pile MASS-UNCLAIM over-destroy (ALL piles vanish) — RE 2026-06-25

**Status: ROOT MECHANISM RE'd from a REAL hands-on log (2026-06-25 11:16, build `b70f9aec`).
NOT a regression (the docs/piles/09 fix was NOT deployed). FIX NOT built — one open root (why the host
failed to express the piles this join). This is WORSE than the dup: a full-class WIPE, not a duplicate.**

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
So "host expresses 0 piles" does NOT by itself create the 870-unclaimed-in-universe state. **NEW PRIMARY
OPEN QUESTION: why do ~871 SEEDED native chipPiles collapse to only 88 in the sweep's in-universe set
(non-mirror local Prop Elements) by sweep time, even with zero host expression?** The 11:16 953-unclaimed
was an ANOMALOUS state (the natives stayed in-universe AND unclaimed); the steady state is 88. Pinning
this collapse (where do the other ~783 go -- claimed-as-mirror? doomed earlier? a re-bracket reset?) is
the real root of both (a) reproducing the wipe to prove the Phase 0 floor and (b) the 11:16 bug itself.

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
