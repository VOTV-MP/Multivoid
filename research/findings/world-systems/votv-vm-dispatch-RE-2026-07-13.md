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

## Next

Frequency counter experiment (throwaway, ini-gated, same wrapper shape) → validate ≤0.1
ms/frame across the 5 windows → then build the wrapper + kerfur form-flip assembler per
`docs/COOP_VM_DISPATCH_PLAN.md`. The counter needs a game run (perf-gate step).
