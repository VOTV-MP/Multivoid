# comp_pane extraction + the g_fields table retirement (2026-07-19, AS-BUILT)

The console_desk 822>800 residual cut (the s24 audit's PROPOSED row). 6-round /qf
("that holds" round 6); the pass REDESIGNED the plan mid-flight: round 1 proved the
literal-diff instrument is structurally BLIND to a missed index renumber in the
staying half (an unshifted line is an exact HEAD match AND compiles), so the fix
became TWO commits -- kill the fragility class first, then move.

## Commit 1 -- `f74d05dc` [refactor] named offsets, self-binding resolve rows

The positional `g_fields[i]` table + magic indices (`FieldPtr<T>(d, 7)`) retired:
each resolve row is now `{L"wire_name", &g_offVar}` -- the row self-binds
name<->variable, a removed row cannot shift any other, and every deref is a
named read (`*OffPtr<float>(d, g_offCoordCooldown)`), lexically checkable.
One-shot resolved log now dumps all 23 name:offset pairs.

Verification (all PASS):
- `offs_correspondence.py` (scratchpad): asserts (A) every table row's string
  matches the MECHANICAL identifier derivation (split '_' + capitalize tokens,
  uppercase token -> Capitalized: DL->Dl; zero hand exceptions), (B) both
  marshals pair member<->g_off<Member>, (C) enumerated expected pairs for the
  remaining consumers. **--mutate control: injected table-row swap AND consumer
  -line swap BOTH flagged** (the detector is not a dead guard).
- Swapped-binding honesty: NO dump can see a swap (C++ can't reflect variable
  identity) -- the lexical script is the swap detector; the dump's role is the
  absolute offset check.
- Build Release; deploy x4 `63C84B32DCF8230B`; 120s 2p smoke PASS: the 23-pair
  dump IDENTICAL on both peers and equal to the documented cook values
  (console_desk.h @-comments, verified 2026-06-11) and to the offsets the old
  build itself logged (catch 0xA38/0x900/0x978); one-shot fired once; desk_diag
  scalars sane (vol=10 int, jitter floats drifting); zero new Warn/Error.

## Commit 2 -- comp_pane one-surface-per-file split

NEW `ue_wrap/desk/comp_pane.{h,cpp}` (58+212), namespace `ue_wrap::comp_pane`:
CompScalars + ReadCompScalars/WriteCompScalars/CompDataPtr/UnlatchDecode/
UpdComp/PaintCompProgress/PaintCompProcess/CompCueStart/CompCueStop/CompBeepDone
moved VERBATIM (11 regions incl. AtlasTextBlock helper). console_desk.cpp
822 -> **740** (UNDER the 800 soft cap); header comp section -> pointer comment.

Design points (the /qf ledger):
- Candidate choice MEASURED: the alternative v70 catch surface's DL_* offsets
  straddle the cut (shared with the staying ReadSimOutputs/WriteSimOutputs);
  the comp surface is consumed ONLY by coop/comp_sync + one desk_diag read.
- comp_pane owns its resolution: throttled 2s ResolvePass + `g_required` fast
  latch (coords_panel shape). REQUIRED set = the 4 comp field offsets ONLY;
  verbs/texts/cues/sounds stay OPPORTUNISTIC with per-function null guards --
  the pre-split semantics exactly (console_desk's core latch never covered
  them either). One-shot log prints yes/NO per opportunistic member.
- ONE desk actor cache in the tree: comp_pane uses public console_desk::
  Instance(); NEW public `console_desk::AtlasWidget()` is the desk-half seam
  for the text-block chain (the AtlasUiCoordsSlot precedent).
- Split-latch coherence MEASURED: every comp_sync entry gates on
  CD::EnsureResolved() BEFORE any comp call (Tick/OnState/QueueConnect/
  OnDisconnect); ComputeFinalLevel checks ReadScalars' return. Enumerated
  body deltas: `Instance()`->`Desk()`, `g_coreResolved`->`g_required`, the
  AtlasWidget seam, added resolve nudges.
- comp_sync's MID-JOIN answer (world-up UnlatchDecode + QueueConnectBroadcast
  seed) moves verbatim -- principle 8 (mid-activity join) untouched.

Verification (all PASS):
- `comp_body_diff.py`: 11 moved regions literal-equal modulo the enumerated
  deltas; **--mutate control (type change in ReadCompScalars) flagged**.
- Negative-grep of the 10 moved names + comp statics in console_desk: comments
  only, 0 code hits (known-positive: the same grep fires in comp_pane).
- Build Release; deploy x4 `7C811EA5ED37A912`; 120s 2p smoke PASS with
  scripted unfiltered greps over BOTH raw logs: `comp_pane: resolved` with
  ZERO "NO" tokens on both peers (offsets == the commit-1 dump values);
  `console_desk: resolved -- 19/19` both peers; the OLD comp log segment = 0
  (stale-DLL negative); desk_diag line-2 `comp(prog=0.0000` != the -1.0
  read-fail sentinel on both peers (the read-success axis discriminated).
- Audit: see the verdict table in the session record (0 CRIT gate for commit).

## Honest status

Behavior preservation is script+smoke+audit proven; NOT hands-on (comp decode
interaction rides the standing take-4 runbook -- no new steps: the surface is
gameplay-identical).
