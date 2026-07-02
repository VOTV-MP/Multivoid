# Hands-on runbook — EHH deny + client FPS (2026-07-01)

> **OUTCOME (2026-07-02, from the 19:41 run's log):** T1 EHH -- suppressors installed 2/2 (log line
> present); no EHH reported in 4+ subsequent hands-on runs => AS-BUILT + install-log-verified, not
> explicitly user-confirmed. T2 FPS -- **REFUTED-INSUFFICIENT**: `sync:npc_client` still 20-51ms
> (mean 23.5ms) with `70e0d899` deployed; real root = ~330k iteration x multiple walks/pass x 4Hz,
> pinned hot by never-retiring twins; walk-merge queued after the [DUP-PROBE] pins the dup residual
> (shared root). T3 dup -- class stays fixed; ONE residual, probe armed. Canonical: docs/piles/12.

**Deployed DLL** `votv-coop.dll` sha256 `3A8BB6AD6D2FC9A3` (all 4 folders hash-verified). Built Release.
Carries: the VERIFIED mass-move dup fix (owner + grabbed-clump gate) + the two fixes below. HEAD `70e0d899`.
Context: the mass-move DUP is already VERIFIED (19:06 "всё на своих местах"); these two surfaced in that test.

## Fix 1 — EHH (`use_deny`) on every client E-press (`d7620ed5`)
RE: the "use" action has THREE delegate bindings (`_41` IE_Pressed hooked, `_38` a 2nd IE_Pressed + `_42`
IE_Released UNHOOKED). `_38` fired on every E-press → native `useAction` → deny, in parallel with our cancelled
`_41`. Fix: side-effect-free deny-suppressor on `_38`+`_42` (client-only, cancel-if-pile, no intent/no throw).
Finding: `research/findings/votv-use-action-three-bindings-RE-2026-07-01.md`.

### T1 — EHH gone on client pile grab/release
1. Client: aim at a pile (hover GUI prompt showing), press E to grab, press E again (or LMB) to throw/drop.
2. **Expect:** NO "EHHH" deny cue on either the grab or the release. (Before: EHH on every E.)
3. Log (client, at install): `trash_collect: use_deny suppressor installed on 2/2 extra 'use' seam(s) (_38
   2nd-Pressed + _42 Released)`. If it says `0/2` or `1/2` → a BP-recook renumbered the ordinals (sdk_check
   would flag it); tell me and I'll re-resolve.
4. **Devices unaffected:** press E on a busy/invalid device → the vanilla deny SHOULD still play (the
   suppressor is client-only + pile-only; it returns false for non-pile presses).

## Fix 2 — client FPS hitch during the mass-move (`70e0d899`)
RE: `net_pump::Tick` hitched 48-57ms; `[HITCH-SRC]` = OUR code (not GC), `[WALK-TIME] sync:npc_client`. Root:
the reconcile's re-bind walk ran `NameOf` (a wstring ALLOC) on ALL ~330k GUObjectArray objects every pass
(4 Hz during the mass-move) before the cheap class filter. Fix: reorder — alloc-free class filter first, so
`NameOf` runs only for the ~870 real piles/kerfurs. Behavior-preserving.

### T2 — FPS stable during the mass-move
1. Repeat the mass-move (host mass-grabs+throws a cluster during the client's join, incl. late moves).
2. **Expect:** noticeably smoother on the client during the mass-move — the periodic `net_pump::Tick` hitch
   should be far smaller or gone.
3. Log (client): the `[WALK-TIME] sync:npc_client = <N> us` line should be MUCH smaller than the prior
   ~48000-57000 us (or absent). `[HITCH-SRC] net_pump::Tick` at 48-57ms should not recur from this source.
   (A residual engine-GC `[HITCH]` with NO `[HITCH-SRC]` is separate — pre-existing, both-peers.)

### T3 — dup NOT regressed (Fix 2 touched the reconcile file)
1. After the mass-move, confirm the corner still CLEARS (no dup, no mid-air clumps) — same as 19:06.
2. **Expect:** unchanged. The reorder is behavior-preserving (same filters, cheaper order).

## Honest status
Built + deployed + hash-verified 4/4; NEITHER hands-on yet (user present → user runs; Claude prepared ground).
If FPS still hitches: the follow-up is merging the sweep+re-bind into one shared walk (2 scans→1). 10 commits
held local (unpushed) pending these two verifications + the mass-move re-check; then RULE-2 collapse + push.
