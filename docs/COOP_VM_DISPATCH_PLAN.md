# COOP VM DISPATCH PLAN — the EX_Local* interception substrate + kerfur form-flip assembler

> STATUS: **DESIGN, /qf-CONVERGED** (2026-07-13, 13-round Question-Form design pass, critic "that
> holds"; thread transcript in the session scratchpad `qf_thread.md`, summarized here). Nothing
> below is built yet. The implementation pass is HALT-GATED: no consumer code before the spike +
> counter verdicts. This is the living plan doc — keep it current as phases land.
>
> AMENDED by the **comparative pass 07-13** (5 rounds, converged, same thread file): user asked to
> re-weigh the pak family (granular patch + auto-repatch automation) against this plan. Verdict:
> **A stays primary — re-derived from structure, not incumbency.** New option **E** (§1a) displaces
> C as the PENULTIMATE fail-ladder rung; §2 ladder + §6 entry costs updated accordingly.

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

Wrapper mechanics (all /qf-hardened):
- **Non-destructive peek** of the callee identity from `Stack.Code` (LocalVirtual = FScriptName;
  LocalFinal = serialized `UFunction*`). No stream advance — a wrong decode mis-FILTERS, never
  corrupts (the original handler re-reads its own operands).
- **Two-stage filter**: identity compare (FName 8-byte / pointer) → `ClassOf(Stack.Object)` +
  `IsDescendantOfAny` vs the registered class family. LocalVirtual name-key = correct
  virtual-call semantics (subclass overrides intentionally included; callback sees the actual
  subclass). LocalFinal pointer-key registration must collect DECLARER + override pointers
  (`FindFunction` exact-owner lesson; `PickDropPropFn` precedent).
- **Empty/disabled fast path** = 1 atomic load + branch + tail-call. Active = peek + ≤16-slot
  linear scan. Fixed-capacity table, slots never freed, `atomic<bool>` enabled flags
  (OnDisconnect disables via the full teardown fanout). Process-lifetime swap, no unpatch.
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

**E is evaluated near-free INSIDE the §2.1 IDA spike** (same functions decompiled). Written
POSITIVELY-falsifiable pass criteria — E PASSES iff ALL of: (i) PLSF directly callable (not fully
inlined); (ii) one FFrame field provably distinguishes PE-shape from caller-shape across ALL
construction sites (candidates: PreviousFrame linkage, NewStack Code init, CurrentNativeFunction)
— zero probabilistic elements; (iii) FUNC_Native side-effect audit clean (netcode flag checks
inert in SP shipping, no Bind() re-run path); (iv) install = existing Func-patch discipline
verbatim; (v) same ENTRY/EXIT + context contract. Residual static ambiguity in (ii) resolves by
PIGGYBACK on the §2.2 counter experiment (dump frame fields of both shapes on the kerfur verbs) —
no new gate steps. **Pre-written tie-breaker: if the spike passes BOTH A and E → A wins**
(invariant-not-a-site-list: the substrate must hold the WHOLE dispatch class; A's decode-validation
and telemetry structurally require seeing every dispatch; E has no discovery mode). If the ladder
falls to E, E inherits **ALL of A's gates verbatim**: the ≤0.1 ms/frame numeric gate + A/B ini
toggle, the 5-window counter re-run in E shape (measured, not assumed), a validation-mode analog
(first-N per-shape routing cross-check), loud latches, 1/s `Func==thunk` integrity check, atomic
disable, process-lifetime no-unpatch. No gate-less rung exists.

## 2. Measurement gates (HALT-gated ladder — no consumer code before verdicts)

1. **IDA spike** (read-only): locate GNatives + fill timing (GRegisterNative statics); decompile
   both handlers; pin operand layouts FROM THIS BINARY; **xref-classify every handler reference**
   `{table-indirect | hookable-direct → A' per-copy MinHook | inlined-copy → option C primary
   for affected verbs + coverage claim narrowed}`. No ship with an uncovered firing route.
   **Scope absorbs the §1a option-E checks** (near-free, same functions): PLSF callability,
   ProcessLocalFunction native-branch semantics, FFrame construction-site enumeration for the
   shape discriminator, FUNC_Native side-effect audit (incl. Bind() re-run paths), BP
   class-object lifetime across save-reload.
2. **Frequency counter experiment** (throwaway, ini-gated, same wrapper shape = cost upper
   bound), per-thread, **FIVE windows**: boot / join-load spike / steady / pile-burst /
   **solo-SP** (the process pays the swap forever, session or not).
3. **Numeric gate (written, pre-committed): added cost ≤ 0.1 ms/frame** on the worst-case scene
   (6 live kerfurs + NPC AnimBP load + pile burst) + A/B ini toggle with no measurable fps drop.
   **Fail-ladder (amended, comparative pass 07-13): A → ONE asm-thunk iteration → option E
   (§1a, under the same gates verbatim) → option-C feasibility spike** (C is UNPROVEN e2e
   here — never sold as a solid rung until its own spike passes).
4. **Concession threshold (written):** substrate+assembler > 600 LOC, or a
   discriminator-to-fix-a-discriminator appears ⇒ the trade is lost, concede and stop.

Game-update story: VOTV updates = content on the same UE4.27; VM layout changes only on an
engine upgrade = the existing whole-mod AOB-rebase class. Spike re-run = one line in the
existing rebase checklist. Content updates changing the verbs are caught loudly by the
FunctionParams guard + per-consumer cross-check.

## 3. The first consumer: kerfur FORM-FLIP ASSEMBLER (the ONE conversion author)

- Brackets on the two verbs (param-less — kismet RE + FunctionParams install guard; no nested
  watched calls — bytecode-measured; `Stack.Object` = the converting actor).
- **Successor resolution = ZERO probabilistic matchers**: the successor is the kerfur-class
  `FinishSpawningActor` (existing Func seam) caught INSIDE the bracket window — deterministic,
  same-callstack. No scans, no proximity, no stamps, no watch-generation guards.
- **Authority ROUTING in the one consumer** (substrate stays coop-ignorant, principle 7):
  authoritative actor → silent register + BindFormActor → ONE `KerfurConvert` at verb exit
  (covers host-own toggles, request-path — which dispatches via PE and is self-bracketed — and
  solo-host); mirror actor → request AT the bracket + prediction (local execution proceeds,
  ghost parked; parked-ghost teardown measured: 4 s orphan deadline + OnDisconnect fanout).
- Inherits the **load-tail quiescence gate** (SYMPTOM-1 carried over).
- **Every request answered**: OnConvertRequest drop branches (stale eid / no live prop) extended
  to loud `BroadcastConvertRejected` → the existing client restore runs (state-shape verified;
  live-fire forced in the take, see §5).
- The death-watch poll **loses ALL authoring** → permanent **alarm-only tripwire** (element
  present + actor dead + unclaimed ⇒ ERROR + overlay). Written response protocol: session =
  log-and-continue; after = pull logs → census the missed entry → EXTEND THE WATCH TABLE (never
  re-enable poll authoring); revert = escalation only.

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

## 8. Sequence

IDA spike → counter (5 windows) → gate verdict → wrapper + validation + kerfur assembler →
static census → verifying take (legacy off) → retirement commit (same session) → melee RE (its
own /qf) → melee onboarding or Func-choke resolution → smart-items per-item.

Parked user decision: the **1h bridge fix** (temporal-pairing v2 of the broken take-10 capture,
own commit, dies in retirement) so kerfur coop works during the substrate build — recommended.
