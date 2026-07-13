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
FScriptName nameKey = *(12 bytes @ Stack->Code);  // {NameIndex:int32, ?:int32, Number:int32}
Stack->Code += 0xC;                                // advance 12 bytes
UFunction* fn = FindFunctionChecked(Context, nameKey);  // resolves subclass override
if (fn->FunctionFlags & 0x400) return UFunction_Invoke(...);    // same branch
else return ProcessScriptFunction(Context, fn, Stack, Result, ProcessInternal);
```

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

## Next

Build the real substrate: the GNatives wrapper (per §1 of the plan — non-destructive peek,
two-stage filter, coverage-gated validation, loud latches, GT/worker thread policy) + the kerfur
form-flip assembler (§3), then the static conversion-entry census (§4), the verifying take (§5),
and the one-commit crutch retirement. Retire the throwaway `gnatives_probe` with that work.
