# VOTV VM dispatch substrate — IDA spike (RE, 2026-07-13)

> DURABLE RE. Binary: `VotV-Win64-Shipping.exe` (Alpha 0.9.0-n, HOST install), imagebase
> `0x140000000`. All addresses below are MEASURED from this binary via IDA (idb saved,
> functions + GNatives renamed in the IDB). This is the HALT-gate §2.1 spike for
> `docs/COOP_VM_DISPATCH_PLAN.md`. Outcome: **option A viable + chosen; option E eliminated
> by measurement; option C untouched.**

## Addresses (measured, renamed in IDB)

| Symbol | Addr | Notes |
|---|---|---|
| `GNatives_table` | **`0x144D8ECD0`** | The 256-entry exec-handler table. Filled at static init (registrars), so the static IDB shows the default handler until run. |
| `UObject::ProcessEvent` | `0x141465930` | AOB-confirmed (`kSigProcessEvent`). Builds NewStack, calls Invoke. |
| `UFunction::Invoke` | `0x141302DC0` | Calls `(*(Function+0xD8))(Context, Stack, Result)` = **Func**. 0xD8 is exactly our existing UFunction::Func-patch offset — cross-confirmed. |
| `execLocalVirtualFunction` (op **0x45**/69) | `0x1414751A0` | GNatives[0x45]. Registrar `0x140695D00` sets `v2=69`. |
| `execLocalFinalFunction` (op **0x46**/70) | `0x141474FB0` | GNatives[0x46]. Registrar `0x140695AD0` sets `v2=70`. Name-registry string "execLocalFinalFunction" at `0x140695B60` confirms identity. |
| `ProcessScriptFunction` | `0x141453550` | Real callable shared helper (BOTH handlers tail-call it on the script branch). Builds callee frame, marshals params from caller stream, runs ProcessInternal. |
| `ProcessInternal` (bytecode loop) | `0x141465DF0` | Passed as the executor (a5) to ProcessScriptFunction. |
| `FindFunctionChecked` (by name) | `0x14145DE50` | LocalVirtual resolves the UFunction* from the FScriptName on Context's class (subclass override included). |
| `execUndefined` (bad opcode) | `0x141477C70` | The default fill for every unregistered slot. |

## FFrame layout (measured from the dispatch at `0x1414720fc`)

```
lea  r9, GNatives_table         ; table base
movzx ecx, byte ptr [rax]       ; opcode = *Stack->Code
inc  rax
mov  [r15+20h], rax             ; Stack->Code++   => Code at FFrame+0x20
mov  eax, ecx
mov  rcx, [r15+18h]             ; Context = Stack->Object => Object at FFrame+0x18
call qword ptr [r9+rax*8]       ; GNatives[opcode](rcx=Context, rdx=&Stack, r8=Result)
```
- **FFrame+0x18 = Object (Context)**, **FFrame+0x20 = Code (bytecode ptr)**.
- Handler ABI: `void exec(UObject* Context /*rcx*/, FFrame& Stack /*rdx*/, void* Result /*r8*/)`.

## The two handlers (measured decompile)

**execLocalFinalFunction (0x46):**
```c
UFunction* fn = *(UFunction**)(Stack->Code);   // 8-byte serialized pointer operand
Stack->Code += 8;                               // advance one pointer
if (fn->FunctionFlags /*@+0xB0*/ & 0x400 /*FUNC_Native*/)
    return UFunction_Invoke(fn, Context, Stack, Result);          // NATIVE: Func@+0xD8 (our patch)
else
    return ProcessScriptFunction(Context, fn, Stack, Result, ProcessInternal);  // SCRIPT
```

**execLocalVirtualFunction (0x45):**
```c
FScriptName nameKey = *(12 bytes @ Stack->Code);  // {ComparisonIndex@0, DisplayIndex@4, Number@8}
Stack->Code += 0xC;                                // advance 12 bytes
UFunction* fn = FindFunctionChecked(Context, nameKey);  // resolves subclass override
if (fn->FunctionFlags & 0x400) return UFunction_Invoke(...);    // same branch
else return ProcessScriptFunction(Context, fn, Stack, Result, ProcessInternal);
```
**OPERAND LAYOUT — CORRECTED BY STEP 1.0 LIVE MEASUREMENT (2026-07-13, hands-on `gnatives_probe` v2/v3):**
the middle int32 (the spike's `?`) is **DisplayIndex**, and **Number is the THIRD int32 (@byte 8)** — the
layout is `{ComparisonIndex@0, DisplayIndex@4, Number@8}`. In the shipping non-case-preserving build
`ComparisonIndex == DisplayIndex`, so bytes 0-7 read as the DUPLICATED index (every live catch showed
`low32 == high32`, e.g. `Init_904` = `0x0000038900000389`). **To match a verb by FName, compare
`op[0]==StringToFName(verb).ComparisonIndex` AND `op[8]==StringToFName(verb).Number` (=0 for the clean
verb names, measured).** Comparing raw bytes 0-7 against an 8-byte `{ComparisonIndex, Number}` FName is
WRONG (it compares `{CmpIdx, DispIdx}` vs `{CmpIdx, 0}` → never matches) — that was the v1 probe's
silent-miss bug, caught before any permanent swap. Live evidence: scratchpad `step1v3_{host,client}.log`
(`*** VERB-HIT ... op[0]=CmpIdx op[1]=DispIdx op[2]=Number=0x0 (dropKerfurProp/spawnKerfuro) ***`).

Both share: the operand peek, the **identical `FUNC_Native (0x400)` branch**, and the two
downstream targets (Invoke/native vs ProcessScriptFunction/script).

## ProcessScriptFunction (measured) — why it matters for E

`ProcessScriptFunction(Context, Function, CALLER_Stack, Result, Executor)`:
1. Builds a NEW callee FFrame (locals `v37..`).
2. Marshals params by walking Function's param chain and, for each, `GNatives[*CallerCode++](Context, CallerStack, dest)` — i.e. it PULLS args from the **caller's** bytecode stream into the new frame's locals.
3. Calls `Executor(Context, &newFrame, Result)` = ProcessInternal to run the callee bytecode.

So a script-function call via EX_Local* has NO pre-built frame — the args live in the
caller stream and ProcessScriptFunction is the thing that reads them.

## Xref classification (the §2.1 coverage gate) — CLEAN

Both handlers' xrefs:
- **Dispatch reach: ONLY via `GNatives_table[opcode]`** (the `FF 14 C1` = `call [r9+rax*8]`
  indirect). There are **zero direct code calls** to either handler and **zero inlined
  copies** at the dispatch layer.
- Other data-xrefs are benign: (a) the GNatives registrar (writes the slot); (b) the
  name-registry `RegisterFunction(cls, "execLocalFinalFunction", handler)` at `0x141306370`
  — the **UClass::Bind lookup table**, not a dispatch path; (c) a `.pdata` RUNTIME_FUNCTION
  unwind entry (`0x14501cf5c` region), not a call.

=> **Classification = table-indirect.** Option A (swap `GNatives[0x45]`/`[0x46]`) has FULL
coverage. No A′ per-copy MinHook, no inlined-copy → C fallback. The "no ship with an
uncovered firing route" gate PASSES for A.

## Verdict

**Option A (GNatives swap) — VIABLE + CHOSEN.**
- Non-destructive peek layout confirmed (LocalFinal 8-byte ptr; LocalVirtual 12-byte
  FScriptName) — read without advancing; the real handler re-reads its own operands.
- Successor path (kerfur FinishSpawn caught inside the bracket) is unaffected — the wrapper
  tail-calls the untouched handler, which routes on the REAL flag.
- Full dispatch coverage (table-indirect). No shape ambiguity — the wrapper sits at the
  opcode handler where the frame shape is known by construction.
- Still owed before building the consumer: the §2.2 frequency counter to validate the
  ≤0.1 ms/frame perf gate (A pays on every EX_Local* dispatch; empty path = peek + tiny
  watch-set compare + tail-call).

**Option E (runtime FUNC_Native flip) — ELIMINATED BY MEASUREMENT (not taste).**
- Mechanically real: flipping `FUNC_Native (0x400)` on a watched UFunction makes BOTH
  handlers take the Invoke/native branch → our thunk. PLSF is a real callable function
  (E-check (i) PASS).
- BUT: a native function called via EX_Local* is handed the **caller's** frame with args
  still in the caller stream — so the thunk must **reimplement ProcessScriptFunction's
  caller-stream param marshaling** for that shape (the engine's own ProcessScriptFunction is
  bypassed the moment FUNC_Native is set). That is reimplementing an engine internal inside
  our thunk.
- AND it needs a frame-shape discriminator (`Stack.Node == Function`: PE-shape TRUE vs
  caller-frame FALSE) that **recursion structurally breaks** (a recursive EX_Local* self-call
  carries caller Node == Function == target → misroute). Confirmed structural, not
  hypothetical.
- AND flipping FUNC_Native perturbs the Bind name-registry lookup (`0x141306370`).
- E loses the pre-written tie-breaker AND independently trips the §2.4 concession smell
  (discriminator-to-fix-a-discriminator + engine-internal reimplementation). **Not built.**

**Option C (kismet+_P pak) — UNCHANGED.** No reason to enter: A passed the coverage gate.

## Gate 2.2 — frequency counter (MEASURED 2026-07-13, autonomous run)

Probe `coop::dev::gnatives_probe` (ini `gnatives_probe=1`, DLL `6b6dfe6f13a9df16aa5d487560f631eb`)
swapped `GNatives[0x45]`/`[0x46]` with the substrate's wrapper shape (non-destructive operand
peek + a full 16-slot watch-table MISS = cost upper bound + per-thread-class counter + 1/1024
RDTSC sample) and tail-called the untouched handlers. Runtime confirmed the swap
(`GNatives@00007FF6D60AECD0`, orig handlers = our static addrs modulo ASLR; tsc 4.69 GHz). One
autonomous host run (story save `s_asdasd` → in-world `mainPlayer` gameplay), 105 one-second
samples; probe log preserved in the session scratchpad (`gnatives_probe_run1_2026-07-13.log`).

| Window | GT dispatch | worker (AnimBP) | GT added @120fps | vs 0.1 ms gate |
|---|---|---|---|---|
| boot / menu | 0/s | 0/s | 0.0000 ms | trivial |
| in-world STEADY (solo-SP) | ~44,000/s | ~156,000/s | **0.013 ms/frame** | **7.5× under** |
| PEAK second (world-load spike) | 177,946/s | ~157,000/s | **0.038 ms/frame** | **2.6× under** |

- avg sampled cost ~150–168 cyc/call (~33 ns) — a GENEROUS upper bound (the RDTSC bracket wraps
  the atomic + `IsGameThread()` too; the real steady wrapper, counter-free, is cheaper).
- The GT number is the gate metric (only the game thread costs the main render frame). Worker
  threads (the parallel AnimBP eval the /qf flagged, ~113–156 k/s) run on the task graph in
  parallel — even the absurd all-serialized GT+worker worst case at peak stays <0.06 ms/frame@120.

**VERDICT: the ≤0.1 ms/frame gate PASSES with margin.** The worst second observed (world-load,
the highest-VM-churn moment of the game lifecycle, already including the AnimBP worker load) added
only 0.038 ms/frame@120. Option A (GNatives swap) is cleared to build.

Windows measured directly: boot, world-load, in-world steady (= solo-SP, single instance in-world
not connected). Not yet captured: coop join-load + pile-burst — both are transient events on top
of a baseline 7.5× under the gate, and both are bounded by the measured world-load peak (a pile
drag is a few props ≪ the 44 k/s BP baseline; a client join-load ≈ the host world-load already
measured; the host during a join does save-transfer = network, not VM). A LAN-pair confirmation
is optional belt-and-suspenders, not a blocker.

## §4 conversion-entry census (measured call-sites 2026-07-13; classification = DESIGN, verify at build)

Tool: `research/bp_reflection/_find_spawn.py` (resolves import call-names per expr — no
grep false-negative, per `lesson_bp_json_grep_resolve_imports`). Scanned the two conversion
assets. MEASURED call-sites (the opcode + callee is tool-resolved fact); the CONVERSION-vs-noise
classification below is my reading and must be re-verified when the assembler is built.

**prop_kerfurOmega (prop -> NPC, "turn ON"):**
- `ExecuteUbergraph_prop_kerfurOmega` -> `spawnKerfuro` [EX_LocalVirtualFunction] — the ONLY
  conversion dispatch on the prop side. ✓ (matches the measured verb.)
- `spawnKerfuro` -> `BeginDeferredActorSpawnFromClass(self, spawnKerfur, …)` [EX_CallMath] — the
  successor spawn INSIDE the verb (Func-visible; the seam the bracket catches).

**kerfurOmega (NPC -> prop, "turn OFF"):**
- `ExecuteUbergraph_kerfurOmega` -> `dropKerfurProp` [EX_LocalVirtualFunction] — the ONLY
  conversion dispatch on the NPC side. ✓
- `dropKerfurProp` -> `BeginDeferredActorSpawnFromClass(self, dropProp, …)` [EX_CallMath] — the
  conversion successor spawn (Func-visible).
- **NOISE (NOT conversions — the assembler must NOT treat these as identity flips):** the NPC
  ubergraph + helpers do MANY other `BeginDeferredActorSpawnFromClass` — `explosion_C` (death
  fx), `prop_food_C`/`prop_C`/floppy via `floppyFromType` (dropped LOOT), `kerfusFace_C`
  (`makeFace`), hold-item spawns (`loadHoldItem`, `holdObject_kerf`). These are the kerfur
  spawning effects/loot/held-objects, NOT the kerfur converting itself. The successor-catch
  logic (BeginDeferred inside the bracket window) MUST discriminate the conversion spawn
  (`dropProp`/`spawnKerfur`) from these co-located loot spawns — e.g. by spawned-class family +
  the bracket being open, not "any BeginDeferred while a verb runs."

Consequence for the build: the two verbs ARE the complete conversion-dispatch set (both
EX_LocalVirtualFunction — the wrapper covers them), but the in-bracket successor resolution is
NOT "the next BeginDeferred" — it must key on the spawned class, because the same ubergraph fires
loot BeginDeferreds. NOT yet censused: the ~28 kerfur VARIANT JSONs (assumed to inherit the base
verbs; verify a sample at build). This is a DESIGN-status census, not VERIFIED.

## Body walk 2026-07-13 — the two verb bytecodes RE-DERIVED (import-resolved) [V]

Prompted by a consistency challenge (the design was leaning on the 2026-06-12 [RD] PROSE for the
class-successor filter while distrusting it for the latent-node question — incoherent). Re-derived
BOTH function bodies from the artifact (import indices resolved via `Imports[-idx-1]`, not text-grep),
sources `research/pak_re/kerfurOmega.json` (export #169) + `research/pak_re/prop_kerfurOmega.json`
(export #5):

- **`dropKerfurProp`** — standalone `FunctionExport`, ScriptBytecodeSize 906, 30 statements. Mainline:
  optional floppy `BeginDeferred`@STMT8→`FinishSpawning`@STMT12 (class from `floppyFromType` — DYNAMIC,
  data-driven, so the concrete floppy class is a variant e.g. `prop_floppyDisc_Y/R/G_C`, confirmed at
  runtime); `BeginDeferred`@STMT17→`FinishSpawning`@STMT21 (member `dropProp`, `EX_DynamicCast` to
  `prop_kerfurOmega_C`@STMT22); `sentient` copy@STMT25; `K2_DestroyActor(SELF)`@STMT26.
- **`spawnKerfuro`** — standalone, ScriptBytecodeSize 891, 23 statements. `BeginDeferred(spawnKerfur)`
  @STMT7→`FinishSpawning`@STMT15→`IsValid`→on-success `K2_DestroyActor(SELF)`@STMT18 / on-fail `addHint`
  + prop survives@STMT20.
- **Whole-body latent scan = NONE in either** — no Delay/RetriggerableDelay/LatentActionInfo/Timeline,
  and specifically NONE between any `BeginDeferred` and its paired `FinishSpawning`. The jumps present
  (`hasFloppy` skip-gate, `IsValid` gate) are synchronous bytecode jumps within the one
  `ProcessLocalScriptFunction` invocation, not tick deferrals.

**Verdict: the `BeginDeferred → [pure statements] → FinishSpawning → K2_DestroyActor(self)` synchronous
model is TRUE for both.** The child exists (post-FinishSpawning) before the parent self-destroy and
before return → CAPTURE-IN-WINDOW is sound. The SAME artifact confirms the two-spawn premise the class
filter rests on — both facts re-derived together, not cherry-picked.

## Runtime confirmation 2026-07-13 — increment 1 hands-on [V]

The observe-only substrate + containment counter (commit `722fbe18`) measured it live (host+client,
`vm_dispatch_log=1`): **18/18 own-toggles fully contained** — every `VERB` catch paired with a
`SPAWN FORM ... IN-WINDOW` + `DESTROY ... IN-WINDOW self` (`ctx==dying actor`), `depth=1`; HOST summary
`catch{off=5 on=5} spawn{formIn=10 formOut=10} destroy{selfIn=10 otherIn=0 kerfurOut=6}`, CLIENT 8/8.
`otherIn=0`, `offGtMatch=0`, RAII window leak-free. All out-of-window events were non-conversion
(world-load + host-authored mirror on the client). Static + runtime agree. See
`docs/COOP_VM_DISPATCH_PLAN.md` §3/§3a + `[[lesson-kerfur-verbs-synchronous-capture-in-window]]`.

## Variant-descent census (measured 2026-07-13 nite — the successor-filter completeness fact)

The 2a repoint design's within-bracket ROLE filter (is this `FinishSpawning` the kerfur successor B, vs
floppy / loot / `explosion_C`) keys on `IsDescendantOfAny(prop_kerfurOmega_C)` (turn-OFF) /
`IsDescendantOfAny(kerfurOmega_C)` (turn-ON). Census over ALL variant class declarations in the
CXXHeaderDump (the exact hierarchy `IsDescendantOfAny` walks via SuperStruct):

- **PROP family (turn-OFF successors, 24):** every `prop_kerfurOmega_*` → `Aprop_kerfurOmega_C` directly,
  EXCEPT `prop_kerfurOmega_col_gamer` → `Aprop_kerfurOmega_col_C` → `Aprop_kerfurOmega_C` (one level). Base
  `Aprop_kerfurOmega_C : Aprop_C`.
- **NPC family (turn-ON successors, 21):** every `kerfurOmega_*` → `AkerfurOmega_C` directly, EXCEPT
  `kerfurOmega_col_gamer` → `AkerfurOmega_col_C` → `AkerfurOmega_C` (one level). Base `AkerfurOmega_C : ACharacter`.

**Verdict: all 45 actor variants descend from the base** (directly or 1 level via the `col` intermediate),
so `IsDescendantOfAny` (which climbs SuperStruct) resolves every one → the descendant-specific successor
filter is CORRECT + COMPLETE. No orphan-from-a-missed-variant risk. (This retired the R5 "does col_gamer
descend?" open item as GREEN, statically, before any 2a code.)

## Next

**2a DESIGN /qf pass CONVERGED 2026-07-13 nite** (11 rounds + user injection, commit `5b58c86b`): the
"suppress OUR eid-drain" mechanism was caught as the §9 crutch + REPLACED by REPOINT ==
identity-migrate-at-birth (`SetActor` reverse-map re-key at B's birth → A husks eid-less via
`UnmarkKnownKeyedProp`'s `EidForActor==kInvalidId` early-return, ZERO suppression). U1 (user-found): a 2nd
host-only KerfurId identity map must migrate at birth too → split `BindFormActor`. Full design:
`docs/COOP_VM_DISPATCH_PLAN.md` §3.
**NEXT = 2a-observe** (4 gates on ONE hands-on run: two-openers dedup / floppy-loot separation /
seam-order-3b / B-index-3c + client local-verb-death husk trace + pending-eid pose-tick — logging only, no
capture code) THEN 2a-capture. Then the static conversion-entry census (§4), the verifying take (§5), the
one-commit crutch retirement (retires the 3 legacy crutches AND take-9-bug1 client suppression). Retire the
throwaway `gnatives_probe` with that work.
