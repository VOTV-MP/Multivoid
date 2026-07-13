# COOP VM DISPATCH PLAN — the EX_Local* interception substrate + kerfur form-flip assembler

> STATUS: **DESIGN, /qf-CONVERGED** (2026-07-13, 13-round Question-Form design pass, critic "that
> holds"; thread transcript in the session scratchpad `qf_thread.md`, summarized here). Nothing
> below is built yet. The implementation pass is HALT-GATED: no consumer code before the spike +
> counter verdicts. This is the living plan doc — keep it current as phases land.
>
> AMENDED by the **comparative pass 07-13** (5 rounds, converged, same thread file): user asked to
> re-weigh the pak family (granular patch + auto-repatch automation) against this plan. Verdict:
> **A stays primary — re-derived from structure, not incumbency.** New option **E** (§1a) was
> proposed as a candidate penultimate rung.
>
> **IDA SPIKE DONE 2026-07-13** (§2.1; RE in `research/findings/world-systems/votv-vm-dispatch-RE-2026-07-13.md`):
> GNatives + both handlers + operand layouts MEASURED on 0.9.0-n; xref classification CLEAN
> table-indirect ⇒ **A has full coverage (chosen)**; **option E ELIMINATED BY MEASUREMENT** (§1a
> verdict). Fail-ladder now A → A-asm → C-spike.
>
> **PERF GATE 2.2 — LOWER-BOUND PASS 2026-07-13** (autonomous probe run, `gnatives_probe`): in-world
> steady 0.013 ms/frame@120, worst-observed second 0.038. **CAVEAT (implementation /qf pass R9-R10):
> the probe used a SIMPLER filter (16-slot pointer scan), NOT the real class-first `IsDescendantOfAny`
> walk, and did NOT measure the ENABLED=false disabled path (the eternal solo-SP tax the process pays
> forever, since the swap is never un-swapped) NOR a worst-case kerfur-populated + join frame. So
> 0.013/0.038 is a LOWER BOUND, not the real-filter gate.** Option A is cleared to STEP 1.0 (below),
> NOT yet to the permanent-swap build.
>
> **IMPLEMENTATION /qf PASS 2026-07-13 — 15 grounded rounds** (design phase-3; transcript in the
> session scratchpad `qf_thread.md`; NO bare critic "that holds", but rounds 11-15 were uniformly
> implementation-detail HARDENING, not reframes — the residual is now purely EMPIRICAL, gated on STEP
> 1.0's game run, which is the correct convergence shape for an impl pass). It materially reshaped §3
> and §2.0. **8 changes rounds 11-15:** (1) filter INVERTED to name-first (8-byte name = correctness
> gate, class = confirm-only — kills the silent-miss on the unverified-descent variant family); (2) §9
> wording corrected (`BindFormActor` DEFERRED, `IsLiveByIndex`-B guard); (3) NEW pending-converge
> RESERVATION (mid-verb data store, keyed B index+serial, first-refusal, bounded TTL — take-8 intent
> done deterministically); (4) 5-seam inventory named, STEP 1.0 scopes only opener #1, per-seam census
> at 2a; (5) cross-peer authority owner NAMED (host author / client mirror by `IsMirror()`); (6) only
> 0x45 swapped (0x46 install gated on a measured customer); (7) un-swap downgraded to a simplicity
> choice (process-static wrapper → un-swap is safe); (8) STEP 1.0 fully specified (SHARED filter fn the
> probe + incr-1 both call; POSITIVE CONTROL to disambiguate zero-catches; HARD HALT). Design is
> build-ready behind STEP 1.0. **NEXT = STEP 1.0** (§2, item 0): write the shared filter fn + extend
> the throwaway `gnatives_probe` (real prologue + live-catch + positive control), re-measure
> enabled+disabled in a worst-case (kerfur-populated + join + multi-flip) scene, gate ≤0.1 ms/frame
> BEFORE the permanent swap lands (probe-first). Then incr 1 (permanent substrate, observe-only) →
> 1b/2a/2b/2c → verifying take → one-commit retirement.

## 0. The problem (why this exists)

Our standalone hook engine has TWO primitives: the ProcessEvent MinHook detour (external BP
dispatch) and the `UFunction::Func` patch (native thunks — catches EVERY dispatch route incl.
`EX_CallMath`, class-level [V] live catches, `docs/COOP_DISPATCH_VISIBILITY.md` line ~91).

**The one remaining invisible class: `EX_LocalVirtualFunction` / `EX_LocalFinalFunction` calls to
SCRIPT (BP) functions** — `ProcessLocalScriptFunction` interprets the callee bytecode directly;
neither PE nor Func ever fires. (`EX_CallMath` is NOT part of the wall — its targets are native,
already Func-patchable. The old blanket "EX_CallMath invisible" claim was true only vs the PE
detour.)

Cost of the wall so far: the kerfur conversion verbs (`dropKerfurProp` / `spawnKerfuro`, both
MEASURED `EX_LocalVirtualFunction` ubergraph self-calls, bytecode 2026-07-13) forced THREE
architectural reworks of fragment-stitching compensation (~400 LOC: death-watch poll, fresh-spawn
stamps, destroy-edge capture, express-side first-refusal — takes 8/9/10, take-10 regressed).
Per `feedback_recurring_bug_is_architectural`, the root is the dispatch layer.

**Justification: customer #1 (kerfur, measured) + structural closure of the invisible class.**
Melee and smart-items are candidate beneficiaries PENDING THEIR OWN RE — not justification
pillars (if melee turns out EX_CallMath→native, the existing Func patch serves it; good outcome).
The mirror-STATE-not-verb doctrine remains law for scalar lanes (devices L2, eventer, pause…) —
this substrate serves the identity-flip / intent-attribution class only.

## 1. The substrate: GNatives pointer-swap (option A)

Patch the two `GNatives[256]` exec-handler table entries (`EX_LocalVirtualFunction`,
`EX_LocalFinalFunction`) with wrappers. Data-pointer swap, no instruction rewriting;
`FFrame::Step` inlining does NOT bypass it (the table read is a data indirection).

**Wrapper contract:** ENTRY/EXIT bracket + context object (`Stack.Object`). Explicitly NO
argument values (they exist only inside ProcessLocalScriptFunction's execution window at every
candidate seam; a customer needing values gets a per-site solution).

Wrapper mechanics (all /qf-hardened; filter order INVERTED to name-first by impl /qf R12):
- **Non-destructive peek** of the callee identity from `Stack.Code` (LocalVirtual = FScriptName;
  LocalFinal = serialized `UFunction*`). No stream advance — a wrong decode mis-FILTERS, never
  corrupts (the original handler re-reads its own operands).
- **NAME-FIRST two-stage filter** (impl /qf R12 — inverted from class-first): `IsGameThread()` →
  **8-byte NAME compare** (peek operand bytes 0-7 = `ComparisonIndex`+`Number` as uint64, IGNORE
  `Display@8`; compare == `StringToFName(verb)` in the same global pool — this is the engine's OWN
  lookup mechanism, measured `sub_1412FDF90`) → **CLASS as a downstream CONFIRM only**
  (`IsDescendantOfAny(ClassOf(Stack.Object), family)`, runs only on the rare name-match). **Name is
  the CORRECTNESS GATE** — a real kerfur variant matches by name regardless of its class descent, so
  the class check CANNOT cause a silent miss (it only guards a theoretical foreign same-named fn).
  This is both safer (the variant family `prop_kerfurOmega_{bonerman,col,col_gamer,erie,erie1,…}` has
  UNVERIFIED descent from `prop_kerfurOmega_C` — a class-first gate would silently miss a
  non-descendant) AND cheaper on the busiest opcode (fixed 2-load+2-compare vs a variable class
  super-walk). LocalVirtual name-key = correct virtual-call semantics (subclass overrides included;
  callback sees the actual subclass). `Number==0` for a plain fn name is INFERRED (UFunction names
  aren't instance-suffixed) → STEP 1.0 LOGS the actual operand bytes and confirms live. LocalFinal
  pointer-key registration must collect DECLARER + override pointers (`FindFunction` exact-owner
  lesson; `PickDropPropFn` precedent).
- **Disabled fast path** = 1 relaxed atomic load + predicted-not-taken branch + tail-call (the eternal
  solo-SP tax — STEP 1.0 measures it on real 0x45 volume). Active = peek + name compare (O(1)).
  `atomic<bool>` enabled flags (OnDisconnect disables via the full teardown fanout). **ONLY 0x45 is
  swapped** (impl /qf R11 — 0x46 has ZERO measured customer; its wrapper is written but the slot stays
  un-swapped until a measured 0x46 customer registers; install is gated on ≥1 registered consumer PER
  OPCODE, so no un-measured process-lifetime tax on `EX_LocalFinalFunction` game-wide). **Process-
  lifetime swap by SIMPLICITY, not necessity** (impl /qf R13): the wrapper is process-static code (our
  DLL never unloads mid-session, RULE-3) so un-swapping the table pointer is SAFE — a late cross-thread
  call to the un-swapped-but-static wrapper just runs it inert once (disabled → tail-call), no crash.
  We keep it swapped because the disabled tax is EXPECTED negligible (STEP 1.0 confirms); if STEP 1.0
  shows the tax is material, un-swap-on-session-end is an available safe fallback.
- **Re-entrant** (stack locals only), depth passed to callbacks.
- **Thread policy per entry, default GT_ONLY**: off-GT watched match = skip callbacks + atomic
  tripwire counter (reported by the 1/s perf dump — no per-hit logging; AnimBP worker reality is
  measured, not assumed).
- **Coverage-gated validation**: per-opcode — consumers of an opcode enable only after ≥N
  successful structural decodes of THAT opcode (LocalFinal ptr must be a live UFunction;
  LocalVirtual FName must resolve via FindFunction on Stack.Object's class);
  enable-on-first-validated, not time-based. Per-consumer one-shot first-watched-decode
  cross-check. Permanent decode-fail counter.
- **1/s slot-integrity check** (`GNatives[op] == wrapper`): mismatch → ERROR + single re-swap +
  latch; second loss → latch-off + alarm.
- **ALL latches are LOUD + session-visible** (overlay/feed alarm) and release-blocking. No
  auto-limp, no silent degradation (precedent: "verb signature changed — module DISABLED").
- **TLS own-invocation scope** at the ONE ue_wrap dispatch chokepoint (`R::CallFunction`):
  brackets carry an `ownInvocation` flag — structural, so customer #2 cannot silently break the
  self-bracket convention.

## 1a. Option E — runtime per-function nativization (comparative pass 07-13; ladder rung 3)

Granular runtime substitution, NO pak, NO table swap: set `FUNC_Native` + `Func = thunk` on the
WATCHED script UFunctions at install time, so `ProcessLocalFunction`'s native-check branch routes
EX_Local* dispatch through `Invoke`/`Func` (the seam class our engine already owns). The thunk
brackets + discriminates frame shape (PE-shaped invoke → `ProcessInternal`; caller-frame →
tail-call `ProcessLocalScriptFunction`, resolved via IDA). Zero cost on unwatched dispatches BY
CONSTRUCTION. This is the honored form of the user's "granular substitution" instinct — in-memory,
no amendment, no redistribution.

Known weaknesses vs A (rounds C-1..C-3): (a) class-LIFECYCLE obligation A lacks by construction
(flip applied per class-load + every override in the name-keyed family; inherits the existing
Func-patch install-on-appearance discipline verbatim — a lifecycle debt, not a new seam);
(b) SHAPE ORACLE fragility: recursion breaks a naive `Stack.Node == fn` discriminator (recursive
caller-frame carries Node==fn → misroute → corruption). Kerfur verbs are measured non-recursive,
but a SUBSTRATE must hold the whole class — E needs a provably-safe discriminator or it loses by
the §2.4 concession threshold (a discriminator-to-fix-a-discriminator). A has no shape oracle at
all (the opcode handler seat knows the shape constructively); (c) `UFunction::Bind()` re-run after
the flip nulls Func — shipping re-Bind paths must be audited.

**E was evaluated near-free INSIDE the §2.1 IDA spike** (same functions decompiled). The written
POSITIVELY-falsifiable pass criteria were — E PASSES iff ALL of: (i) PLSF directly callable (not
fully inlined); (ii) one FFrame field provably distinguishes PE-shape from caller-shape across ALL
construction sites — zero probabilistic elements; (iii) FUNC_Native side-effect audit clean;
(iv) install = existing Func-patch discipline verbatim; (v) same ENTRY/EXIT + context contract.

**SPIKE VERDICT 2026-07-13 — E ELIMINATED BY MEASUREMENT** (the honest outcome the criteria were
written to allow): (i) PASSES — PLSF = `ProcessScriptFunction` `0x141453550`, a real shared
callable. But (ii) FAILS the "zero probabilistic elements" bar and (iii)/(iv) surface real costs:
a native called via EX_Local* is handed the **caller's** FFrame with args still in the caller
bytecode stream (measured: `ProcessScriptFunction` is the thing that marshals them; flipping
FUNC_Native BYPASSES it), so E's thunk must **reimplement ProcessScriptFunction's caller-stream
param marshaling** — an engine-internal reimplementation — AND still needs the `Stack.Node == fn`
discriminator that recursion structurally breaks, AND flipping FUNC_Native perturbs the Bind
name-registry (`0x141306370`). A, by contrast, sits at the opcode handler where the shape is known
by construction and tail-calls the UNTOUCHED handler (which routes on the real flag every time —
no discriminator, no reimplementation). **E loses the pre-written tie-breaker AND independently
trips the §2.4 concession smell. Not built.** The fail-ladder is now **A → A-asm → C-spike** (E rung
removed).

## 2. Measurement gates (HALT-gated ladder — no consumer code before verdicts)

1. **IDA spike** — ✅ **DONE 2026-07-13** (read-only; full RE in
   `research/findings/world-systems/votv-vm-dispatch-RE-2026-07-13.md`; IDB saved with all
   functions + `GNatives_table` renamed). Measured on `VotV-Win64-Shipping.exe` 0.9.0-n:
   - `GNatives_table` = **`0x144D8ECD0`**; dispatch = `call [GNatives + opcode*8]` (FFrame
     +0x18=Object, +0x20=Code; handler ABI rcx=Context/rdx=&Stack/r8=Result).
   - `execLocalVirtualFunction` (op 0x45) = `0x1414751A0`, operand = 12-byte FScriptName;
     `execLocalFinalFunction` (op 0x46) = `0x141474FB0`, operand = 8-byte `UFunction*`. Both
     peekable non-destructively.
   - **Xref classification = CLEAN table-indirect**: both handlers reached at dispatch ONLY
     via the table; zero direct calls, zero inlined copies (other data-xrefs = the Bind
     name-registry + `.pdata` unwind — benign). **A has full coverage; no A′/C fallback
     triggered.**
   - Both handlers branch identically on `FunctionFlags & 0x400 (FUNC_Native)` @UFunction+0xB0
     → `UFunction::Invoke` (Func@+0xD8 — our existing patch offset, cross-confirmed) vs
     `ProcessScriptFunction` (`0x141453550`, real callable helper) → `ProcessInternal`
     (`0x141465DF0`).
   - **Option E ELIMINATED BY MEASUREMENT** (see §1a): the FUNC_Native flip works
     mechanically, but a native called via EX_Local* gets the CALLER frame with args still in
     the caller stream ⇒ the thunk must reimplement ProcessScriptFunction's caller-stream
     marshaling AND carry the recursion-breakable shape discriminator AND perturbs the Bind
     name-registry. Loses the tie-breaker + trips the §2.4 concession smell. Not built.
2. **Frequency counter experiment** — ✅ **DONE 2026-07-13** (probe `coop::dev::gnatives_probe`,
   ini-gated, same wrapper shape = cost upper bound; full data in the RE findings doc + scratchpad
   `gnatives_probe_run1_2026-07-13.log`). Autonomous host run, 105 one-second samples. Measured:
   in-world STEADY (solo-SP) = ~44 k GT dispatch/s → **0.013 ms/frame@120**; PEAK second
   (world-load spike, incl. ~156 k/s AnimBP worker load) = 177,946 GT/s → **0.038 ms/frame@120**.
   Windows captured: boot, world-load, steady/solo-SP. Coop join-load + pile-burst not yet
   captured (transient, bounded by the measured world-load peak — optional LAN confirmation).
3. **Numeric gate (written, pre-committed): added cost ≤ 0.1 ms/frame** — ⚠️ **LOWER-BOUND PASS
   only** (steady 0.013, worst-observed 0.038). The probe used a 16-slot pointer scan, NOT the real
   `IsDescendantOfAny` class-first walk; did not measure the ENABLED=false disabled path nor a
   worst-case kerfur-populated+join frame (impl /qf R9-R10). The REAL-filter gate is **owed at STEP
   1.0 (§2.0)** before the permanent swap.

**STEP 1.0 (added by impl /qf R9-R10, fully specified R11-R15 — probe-first, do BEFORE any
permanent-swap code):** extend the throwaway `coop::dev::gnatives_probe` with the REAL production
prologue. **The filter is factored into ONE SHARED function that BOTH the probe AND the eventual
incr-1 wrapper call** (impl /qf R15 — NOT a probe reimplementation, or the gate would measure the
wrong code); writing the filter fn is not installing the swap; the probe installs the swap
TEMPORARILY (removable) and calls the shared filter. Filter shape (name-first, R12): `IsGameThread()`
→ 8-byte NAME compare (operand bytes 0-7 == `StringToFName(verb)`) → `IsDescendantOfAny` class confirm.
STEP 1.0 does TWO things in ONE game run (impl /qf R11 folded the live-catch in):
- **(i) LIVE-CATCH (the premise gate)** — trigger a REAL `kerfur_toggle` in the worst-case scene and
  assert `wrapper[0x45]` fires with `Context`=kerfur + name=`dropKerfurProp`; **LOG the actual peeked
  8-byte operand** (confirm `ComparisonIndex`+`Number` == `StringToFName`, i.e. `Number==0` is real,
  not inferred) + **LOG `Context`'s class per name-match** (turns the inferred variant family into a
  MEASURED set) + a **client-request-toggle-on-host** case to log whether the inner `[0x45]` fires on
  the PE-execution path (the two-openers double-fire question — R14). **POSITIVE CONTROL (R15,
  gated-probe-verify-the-gate lesson):** the run must INDEPENDENTLY confirm a flip occurred (the
  existing `KerfurConvert` broadcast path + a raw UN-filtered 0x45 hit counter), so zero catches is
  disambiguated: flip-confirmed + `[0x45]` fired = PASS; flip-confirmed + zero catches = FAIL (premise
  falsified → HALT); no flip confirmed = unexercised INVALID run (re-run with a forced flip).
- **(ii) PERF** — re-measure BOTH paths in the WORST-CASE loaded scene (several kerfurs ticking + a
  join + a multi-kerfur flip, not idle): (a) ENABLED=true = the in-session filter cost on the REAL
  prologue + REAL 0x45 volume; (b) ENABLED=false = the eternal solo-SP tax (1 relaxed atomic load +
  predicted-not-taken branch + tail-call). Gate ≤0.1 ms/frame on BOTH.
**HARD HALT GATE:** if the live 8-byte operand ≠ `StringToFName`, or `Number != 0` unexpectedly, or
`[0x45]` doesn't fire on a confirmed flip, or a col-variant slips the class confirm, or either perf
path exceeds 0.1 ms — HALT, install NOTHING permanent, and either fix the filter to the measured
reality or (premise falsified) drop to the ladder (A-asm → C) BEFORE the un-removable seam lands
(nothing to roll back — the probe is removable).
   **Fail-ladder (amended: spike 07-13 removed the E rung — E eliminated by measurement, §1a):
   A → ONE asm-thunk iteration → option-C feasibility spike** (C is UNPROVEN e2e here — never
   sold as a solid rung until its own spike passes).
4. **Concession threshold (written):** substrate+assembler > 600 LOC, or a
   discriminator-to-fix-a-discriminator appears ⇒ the trade is lost, concede and stop.

Game-update story: VOTV updates = content on the same UE4.27; VM layout changes only on an
engine upgrade = the existing whole-mod AOB-rebase class. Spike re-run = one line in the
existing rebase checklist. Content updates changing the verbs are caught loudly by the
FunctionParams guard + per-consumer cross-check.

## 3. The first consumer: kerfur FORM-FLIP ASSEMBLER (REWRITTEN by impl /qf pass 2026-07-13)

**The substrate is a deterministic CAPTURE + destroy-SUPPRESS mechanism that FEEDS THE EXISTING
deferred converge — NOT a converge rewrite** (impl /qf R4, the key reframe; grounded in measured
code). It replaces THREE probabilistic crutches (`TryAdoptFreshKerfurProp` / `TryCaptureKerfurPropDestroy`
/ death-watch poll) with ONE deterministic bracket signal, and REUSES the proven machinery
(`ConvergeAfterConversion`, `BindFormActor`, the two-phase-armed deferred queue, `OnKerfurConvert`,
park-by-eid). Reuse-the-proven-author, don't raw-reimplement.

**TWO bracket OPENERS, ONE capture** (impl /qf R6, measured):
- (a) the GNatives[0x45] wrapper opens the bracket for verb dispatches we DON'T initiate — the host's
  OWN menu toggle AND the client's OWN toggle (both are ubergraph `EX_LocalVirtualFunction` self-calls;
  covers col-variants — measured: `kerfurOmega_col`/`_col_gamer` have no own call site, they inherit
  the base ubergraph's by-name 0x45 dispatch, virtual-resolved to the override).
- (b) a self-bracket (TLS own-invocation at the ONE `R::CallFunction` chokepoint — ALREADY EXISTS as
  `g_requestVerbEid`) opens it for the host EXECUTING a client's request (measured kerfur_convert.cpp:
  82/350/437 — that path dispatches the verb via ProcessEvent/CallFunction, NOT EX_LocalVirtual, so
  the GNatives wrapper does NOT double-fire; capture is idempotent — no recursion, measured).
- Both set `TLS.converting = {A = Context actor, A-eid, verbId, depth}`. `Context = A` is measured
  (FFrame+0x18; the toggled kerfur is ALIVE at verb entry — the eid resolves on a LIVE actor, which is
  exactly why this AVOIDS take-10's post-destroy zero-read).

**FIVE SEAMS (impl /qf R13 — this substrate is NOT one seam; STEP 1.0 exercises only #1):**
(1) `GNatives[0x45]` wrapper = verb-entry opener-a; (2) `R::CallFunction` self-bracket = opener-b
(EXISTS as `g_requestVerbEid`, wired 1b); (3) `FinishSpawningActor` Func seam (existing
`host_spawn_watcher`) = B capture MID-verb; (4) `K2_DestroyActor` Func seam = A suppress-branch
MID-verb; (5) the deferred net-pump barrier (existing two-phase, `kerfur_convert.cpp:11-20`) =
`BindFormActor` + park + broadcast. A per-seam CENSUS (which native + call site fires for A-destroy in
turn-OFF[NPC] vs turn-ON[prop], B-FinishSpawning both directions) is owed at incr-2a's capture+LOG
(v106-enumerate-every-call-site discipline) — NOT deferred past it.

**CAPTURE (mid-verb = ZERO engine calls; reads + branches + DATA STORES only — the re-entrancy
discipline, impl /qf R4):**
- B's `FinishSpawningActor` (measured SYNCHRONOUS inside the verb, before A's destroy — bytecode
  `EX_CallMath→BeginDeferred→EX_CallMath→FinishSpawningActor`) is caught at the EXISTING
  `host_spawn_watcher` Func seam; if `TLS.converting` is set + the spawned actor is-a successor-form
  class (`prop_kerfurOmega_C` desc for turn-off, `kerfurOmega_C` for turn-on — the floppy
  `prop_floppyDisc_C` distinguished by class), record B into the bracket. Capture B deterministically
  HERE at the distinct mid-verb seam — NOT post-bracket (impl /qf R13: a post-bracket "find the
  successor that appeared" scan IS `TryAdoptFreshKerfurProp`, the probabilistic crutch being retired;
  the measured spawn-then-destroy order requires the mid-verb capture).
- **PENDING-CONVERGE RESERVATION (impl /qf R14 — NEW named element, a mid-verb DATA STORE):** the
  capture ALSO writes B into a pending-converge reservation **keyed on B's `(object-index, serial)`**
  (NOT the raw pointer — impl /qf R15, the recycled-slot trap: a raw ptr to a recycled slot would
  first-refuse an unrelated actor). Every prop enumeration in the ~1-frame window before the deferred
  barrier (the `MarkPropElement` enroll sweep, the `prop_snapshot` builder, the mirror pass, opener-b)
  CONSULTS the reservation and **refuses to author a fresh eid for a reserved prop** (first-refusal).
  This closes the eid-less window (B is live but eid-less from FinishSpawning until the deferred
  `BindFormActor`) — it is **take-8's first-refusal INTENT done DETERMINISTICALLY**, which is exactly
  why take-8's probabilistic `TryAdoptFreshKerfurProp` can retire. Bounded lifetime: the reservation
  is cleared on EVERY exit — converge (`BindFormActor` clears), RESTORE (no-B branch clears), teardown
  (the fanout drains the whole pending set), and a >N-tick staleness tripwire (loud alarm, impossible
  state). No live-while-pending-with-no-deadline.
- A's destroy seam fires mid-verb and (measured kerfur_convert.cpp:185 `UnmarkKnownKeyedProp`) would
  DRAIN A's eid + broadcast `PropDestroy`(A) → take-9 bug1 (client kills the mirror before
  KerfurConvert). It is **SUPPRESSED** — a branch (skip drain + skip PropDestroy on the host; suppress
  the keyed-destroy RELAY on the client — measured take-9 bug1) gated by the deterministic bracket
  signal (replaces `TryCaptureKerfurPropDestroy`'s proximity guess). No engine calls. Suppressing the
  drain is what keeps A's eid ALIVE to be migrated onto B at the deferred barrier.

**BRACKET-EXIT (deterministic decision) + DEFERRED action (impl /qf R7, R9):**
- At bracket EXIT the outcome is deterministically known (everything synchronous): record
  `{A-eid, B-or-null, was-suppressed}`. The ACTION runs DEFERRED via the EXISTING two-phase-armed
  net-pump barrier (MEASURED deliberate re-entrancy barrier, kerfur_convert.cpp:11-20 — a nested
  ProcessEvent pump mid-verb corrupts; converge stays deferred, only capture is mid-verb).
- B captured → converge: `BindFormActor` (DEFERRED — it can touch engine state, which is why the
  two-phase barrier exists; NOT at-birth, impl /qf R12 wording correction) migrates A's eid onto B,
  guarded by `IsLiveByIndex(capturedBIndex)` + serial match (impl /qf R15 — bare `IsLive(B)` passes a
  recycled slot; a serial mismatch means B's slot recycled → treat as no-B → RESTORE). take-10 is
  avoided NOT by "migrate before destroy" but by: (a) B's pointer captured at its BIRTH while LIVE (a
  read, never a post-destroy read); (b) A's destroy-drain SUPPRESSED so A's eid survives to migrate;
  (c) the `IsLiveByIndex`-guarded deferred migration. Every read is of a LIVE actor; the only dead
  actor (A) is never read (take-10 died reading `GetActorLocation` on an already-destroyed A). →
  A's destroy resolves no eid = husk by construction → ONE `KerfurConvert` broadcast.
- Suppressed-destroy + NO successor (e.g. spawn failed after destroy) → **RESTORE** the suppressed
  destroy by invoking the EXISTING destroy handler (NOT a new second emitter — suppression=loan paired
  restore). Turn-on failure (prop survives, no destroy) → clean no-op.
- Park (`NeutralizeAiTimers` = MEASURED ProcessEvent dispatch, kerfur.cpp:132 — CANNOT run mid-verb) is
  DEFERRED. The client's predicted B has a bounded brain-on window, no worse than today's 5 Hz poll.
- ALARM = pure tripwire for IMPOSSIBLE states only (wrong class, double-capture) — NOT a hedge for a
  non-synchronous case (there is none).

**Authority ROUTING inside the callback — the cross-peer owner is NAMED (impl /qf R14)** (substrate
stays coop-ignorant, principle 7). The discriminator is `IsMirror()` on the Context actor (the
"MirrorManager mixes local+wire rows — filter IsMirror()" lesson):
- **Authoritative actor** (`IsMirror()` false, i.e. the HOST's prop) → converge (`BindFormActor` +
  broadcast). **The HOST is the SOLE author.**
- **Mirror actor** (`IsMirror()` true, i.e. a client toggling its OWN kerfur) → the FORCED client
  prediction: park local B by eid + suppress the local destroy-RELAY + relay the request to the host;
  it does NOT author a fresh eid. B is adopted on the authoritative `KerfurConvert` (`TakeParkedGhostByEid`).
  (EX_LocalVirtual is un-cancellable + the menu interceptor was removed, so the client's local verb RUNS
  — the prediction is forced, not chosen.)
- **A-eid is the SHARED entity id, NOT per-peer** — a synced prop carries the same eid on both peers, so
  the client parks under the same eid the host converges under. Cross-peer reconciliation is by AUTHORITY
  ROUTING (`IsMirror()`) + the shared eid, NOT by A-eid dedup. **A-eid dedup is the SAME-peer
  double-opener guard only** (host running its own toggle hitting both openers → existing
  `ConvergeAfterConversion` dedups by eid).
Enable gates on **SESSION-ACTIVE (host OR client)**, re-armed at StartCoopSession/join (NOT hosting-only
— the client needs the bracket to capture its own conversion; impl /qf R9 correction to R6).
- **Every request answered**: `OnConvertRequest` drop branches → loud `BroadcastConvertRejected` →
  existing client restore (live-fire forced in the take, §5).
- The death-watch poll **loses ALL authoring** → permanent **alarm-only tripwire**. Response protocol:
  session = log-and-continue; after = census the missed entry → EXTEND the watch table (never re-enable
  poll authoring); revert = escalation only.

**INCREMENTS (each build+deploy+hash+smoke+code-reviewer-audit):** 1.0 = probe real-filter gate +
LIVE-CATCH + positive control (§2.0 — the premise gate, folded in by impl /qf R11; the live-catch is
NO LONGER in incr-1). Only 0x45 swapped. 1 = permanent substrate + observe-only logging consumer (first
PERMANENT-substrate step; opener-a only). 1b = harden (validation-mode first-N, 1/s slot-integrity, loud
latches) + the self-bracket TLS opener (b). **2a = capture+LOG both peers + the per-seam CENSUS live
(every FinishSpawning + every destroy, both directions, both peers) — and MEASURE one-capture-per-A-eid
with BOTH openers live (the two-openers dedup, impl /qf R14).** 2b = enable SUPPRESS + reconcile (smoke:
no premature kill). **2c = enable deferred park+converge — GATED ON 2a's dedup confirmation (impl /qf
R14: converge does not turn on until 2a proves one-capture-per-A-eid)** + gate the 3 crutches behind
`kerfur_legacy_converge=0` INERT (RULE-2 same commit). 3 = verifying take. 4 = delete inert legacy +
`gnatives_probe`.

## 4. Census (pre-take)

Static bytecode census of ALL conversion entry points across the pak_re JSONs (callers of
`dropKerfurProp` / `spawnKerfuro` + any BeginDeferred of kerfur classes outside the two verbs),
opcode-classified. Out-of-scope site → wired via the appropriate EXISTING primitive into the
same assembler (conversions must spawn/destroy actors ⇒ must pass natives ⇒ every entry is at
least Func-visible; a permanently tripwire-owned site is impossible by construction).

## 5. Verifying take + retirement (RULE 2)

- Take build ships `[dev] kerfur_legacy_converge=0` **by default** — legacy compiled but INERT
  (no live dual author ever; verify-before-retiring wins the compiled window, which has a
  written death date).
- Take script: both-direction toggles from BOTH peers + **JOIN with live kerfurs** + hand-place
  regression + **CONTESTED TOGGLE** (both peers toggle the same kerfur — forces the losing
  request into drop → loud reject → restore live-fire). 3 hand attempts, else the probe-class
  `[dev]` reject injector (deleted after use). Poll-tripwire active as the coverage alarm.
- **Retirement = ONE deletions-only commit, SAME SESSION as the green take** (clean single-hash
  revert): fresh-spawn stamps, destroy-edge capture (`TryCaptureKerfurPropDestroy`), take-8
  express-side first-refusal (`TryAdoptFreshKerfurProp` — the assembler registers the successor
  same-tick, the drain finds it tracked, generic lanes need no kerfur consult at all), poll
  authoring branches, the bridge fix (if taken), the ini flag itself. **K-6 adoption branches
  untouched** (take-7 LOAD-BEARING, CLOSED-keep — different axis: join-window save twins).
- Simplicity ledger: delete ~400 LOC + 5 probabilistic discriminators; add ~250-300 generic
  substrate + ~150-200 consumer + ~15 TLS. Net LOC ≈ 0; probabilistic matchers → zero.

## 6. Option C — game-file editing (kismet patch + `_P` pak) — considered per user request

Honest analysis: **zero runtime cost** (the FPS-ideal), but per-site whack-a-mole; **UNPROVEN
e2e in this project** (we ship an asset pak, but "edit a function's kismet → repak → 4.27 loads
it → behavior changes" has never been demonstrated — tools vendored: unrealpak,
kismet-analyzer); permanent per-game-update re-patch pipeline (ours forever, automatable);
**requires a written CLAUDE.md principle-1 / A6 amendment if ever selected** ("permitted to
consider" ≠ adopted — and per the comparative pass the amendment is named what it is: a
deliberate REPEAL of an architectural principle, quoted verbatim to the user for the decision).

**Comparative pass 07-13 findings (5 rounds, converged — same thread file):**
- The pak channel is HALF-proven [V]: NEW-asset paks ship + auto-mount on every peer since 07-02
  (client-model pipeline). The load-bearing half — `_P` override of an EXISTING cooked package +
  the engine accepting an edited kismet body — is UNPROVEN here.
- "Granular" at the pak layer is a tool-level illusion `[inferred-strong, NOT measured on VOTV]`:
  override shadowing is whole-package-file; a stale override SILENTLY REVERTS any game-update
  change to the same package. The C spike must MEASURE this (it may be worse: cook-version locks,
  partial/no shadowing — an outcome that kills the C branch entirely → renegotiate constraint).
- Peer pak-mismatch divergence is mitigable (pak-hash in the existing handshake = loud refuse,
  now a MANDATORY C component) — but the loud gate converts EVERY content update into a coop
  OUTAGE window until re-extract → re-transform → re-pak → redistribute. A/E have no such window
  (measured: VOTV alpha cadence is frequent CONTENT updates; those never touch engine code).
- MTA fidelity [V]: MTA ships ZERO modified game assets in 15+ years — all interception is
  runtime memory. C additionally means redistributing MODIFIED copyrighted game content to peers.
- C3-last in the ranking does NOT depend on the unmeasured shadowing claim — the measured pillars
  (MTA precedent, A6-as-written, unproven-e2e, site-list shape, outage window) hold it alone.

C's role in this plan: the FINAL escape hatch (rung 4, after option E — §2.3 ladder), entered
ONLY via (a) the numeric gate failing through A, A-asm AND E, (b) the spike finding inlined
handler copies, or (c) a future customer needing CANCEL semantics (structurally awkward at the
VM seam — requires param consumption). Entry goes through a **C feasibility spike first** (~1
day: patch one trivial BP function, verify in-game; scope: _P-override-accepted + kismet-edit-
accepted + whole-package silent-revert MEASURED) — C is never load-bearing on hope. Entry cost
EXPLICITLY includes: the auto-repatch pipeline (structural kismet signature match → re-apply
transform → re-pak; boot-time hash check in the DLL) as a MANDATORY component, the pak-hash
handshake gate, and the CLAUDE.md amendment. **Pipeline scope is priced from the C-spike's
measurements, never estimated ahead.**

## 7. Workaround retirement inventory (RULE 2, post-substrate; user mandate)

RETIRED by the kerfur assembler (one commit, §5): stamps · destroy-edge capture · take-8
first-refusal · poll authoring · bridge · ini flag.

RE-EVALUATED when their lane onboards (each needs its own RE first):
- melee input-side workarounds + LOCAL-ONLY client hits (pending MELEE RE — may resolve via
  Func choke alone).
- smart-items verb seams (pending per-item RE, docs/items/).
- `kerfur_menu_input` InpActEvt relay (client radial verbs — bracket may supersede).

NOT retired (correct shape, per doctrine): all state-mirror lanes (devices L2 interactable
channel, eventer v95 passEvents poll, pause-guard, updateHold poll, dead-retire pose-walks) —
scalar state, mirror-STATE-not-verb stays law.

## 8. Sequence (updated by impl /qf pass 2026-07-13)

✅ IDA spike → ✅ counter (lower-bound, simpler filter) → **NEXT: STEP 1.0 (probe REAL filter,
enabled+disabled, worst-case frame; §2.0)** → incr 1 substrate observe-only (LIVE-CATCH gate) →
1b harden + self-bracket → 2a capture+log → 2b suppress+reconcile → 2c park+converge + crutches
inert → verifying take (JOIN + contested toggle + re-host, legacy off) → retirement commit (same
session) → melee RE (its own /qf) → smart-items per-item.

Design detail lives in §3 (rewritten). The **1h bridge fix** is NO LONGER on the table — the user
decided (07-13) NO bridge, straight per plan; kerfur coop stays broken until the substrate lands.
