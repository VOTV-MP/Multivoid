# Finding: standalone reflection (GUObjectArray + FName::ToString) via AOB, validated

**Date**: 2026-05-22
**Game version**: Alpha 0.9.0-n
**Phase**: standalone foundation (RULE No.3) — `ue_wrap` reflection
**Status**: GUObjectArray + FName::ToString DONE and validated live against the
live object graph. ProcessEvent (call path) deferred to the spawn-port step.

## Result

The standalone mod now reads the engine's object graph itself — **no UE4SS**.
On load (via the `xinput1_3.dll` proxy), `ue_wrap::reflection` AOB-resolves the
two anchors, waits for the engine, and dumps a self-test. Live run output:

```
GUObjectArray   = 0x7FF7B17EF910 (rva 0x4d8f910)   <- matches UE4SS log exactly
FName::ToString = 0x7FF7ADCDD870 (rva 0x127d870)   <- matches UE4SS log exactly
ObjObjects @ +0x10: chunks=0x2DBDAF4B260 Max=2162688 Num=17770 NumChunks=1
NumObjects()=17770
[     0] obj=/Script/CoreUObject  class=Package
[     1] obj=Object               class=Class
[     2] obj=/Script/Engine        class=Package
[     3] obj=BlueprintFunctionLibrary class=Class
 ... (the canonical UE object graph)
instances: mainPlayer_C=0  mainGamemode_C=0  *PlayerController*=2
```

17770 live UObjects enumerated with correct names/classes. `mainPlayer_C=0` is
correct (run was at the OMEGA-WARNING splash / menu, no player pawn yet);
`*PlayerController*=2` are the menu controllers. Cross-check vs UE4SS ground
truth (its log): **both resolved addresses are identical**.

## Signatures (AOB), VOTV 0.9.0-n

- **FName::ToString** (unique function prologue; match == function address):
  `48 89 5C 24 18 55 56 57 48 8B EC 48 83 EC 30 8B 01 48 8B F1 44 8B 49 04 8B F8 C1 EF 10 48 8B DA 0F B7 C8`
  Verified UNIQUE in the image (1 match). Signature `void(const FName*, FString*)`
  (rcx=FName*, rdx=FString out) — confirmed from the decompile reading [rcx]/[rcx+4].
- **GUObjectArray** (static init-guard that does `lea rcx,[rip+&GUObjectArray]`):
  `8B 05 ?? ?? ?? ?? 3B 05 ?? ?? ?? ?? 75 13 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 45 33 C9 48 89 45 ?? 4C 8D 45`
  The `lea rcx` disp32 is at match+24; rip base (insn end) match+28; target =
  match+28 + disp32. Every match in this image decodes to GUObjectArray, so it
  is robust against picking the "wrong" match.

## Layout confirmed (standard UE4.27, validated by the runtime header dump)

- UObjectBase: ClassPrivate @ +0x10, NamePrivate (FName, 8 bytes) @ +0x18.
- FName: { int32 ComparisonIndex; int32 Number; }.
- FString: { wchar_t* Data; int32 Num; int32 Max; } (Num counts the null term).
- FUObjectArray.ObjObjects @ +0x10 (FChunkedFixedUObjectArray):
  Objects(chunks) @ +0x00, MaxElements @ +0x10, NumElements @ +0x14,
  NumChunks @ +0x1C. ElemsPerChunk = 64*1024. FUObjectItem stride 0x18
  (Object @ +0x00). All confirmed by the raw 0x40-byte header dump.

## Code

- `src/votv-coop/include/ue_wrap/sig_scan.h` + `src/ue_wrap/sig_scan.cpp` —
  AOB scanner over the main module image (PE SizeOfImage; `??` wildcards).
- `src/votv-coop/include/ue_wrap/reflection.h` + `src/ue_wrap/reflection.cpp` —
  Resolve(), NumObjects(), ObjectAt(), NameOf()/ClassOf(), ToString(),
  RunSelfTest(). Principle 7: engine layer only, no gameplay/net.
- `src/bootstrap/dllmain.cpp` — boot thread runs the self-test to
  `votv-coop-reflection.txt` next to the DLL.

Note: ToString reuses one scratch FString (reset Num=0 each call); one small
buffer is intentionally never freed (one-shot diagnostic; FMalloc::Free wiring
deferred). Not a shipping code path.

## Other addresses on hand (from the UE4SS log, RVAs computed) for next steps

- ProcessInternal rva 0x1465ce0; ProcessLocalScriptFunction rva 0x1465df0
  (bytecode stepper sub_1414573C0 dispatches GNatives at qword_144D8ECD0).
- StaticConstructObject_Internal rva 0x146d330; GMalloc rva 0x4d0b190;
  FName::FName(wchar_t*) rva 0x1270b20.

## Update: object/class/function lookup (DONE, validated)

Added `FindObject`/`FindClass`/`FindFunction`/`OuterOf`/`ClassNameOf` (linear
GUObjectArray walk; match on NamePrivate, meta-class for classes, Outer for
functions). Validated live (standalone, no UE4SS) at the menu:

```
FindClass(Actor)                         = found (class Class)
FindClass(World)                         = found (class Class)
FindFunction(Actor, K2_SetActorLocation) = found (class Function)
FindFunction(Actor, K2_DestroyActor)     = found (class Function)
first World instance                     = found (class World)
FindClass(mainPlayer_C)                  = NOT FOUND  <- expected
FindClass(mainGamemode_C)                = NOT FOUND  <- expected
```

`mainPlayer_C`/`mainGamemode_C` not found is CORRECT: those Blueprint gameplay
classes aren't loaded at the OMEGA-WARNING/menu; they load with the gameplay
map (`untitled_1`). The lookup path itself is proven by Actor/World/Function.
This means we already have the exact pieces the orphan-spawn port needs once in
gameplay: the `Actor` class, a live `World`, and `K2_SetActorLocation`.

## Next

1. Resolve **ProcessEvent** (UObject::ProcessEvent) by AOB — the universal
   UFunction call entry. Unblocks BOTH skip-to-gameplay (call
   `OpenLevel`/load UFunctions ourselves) AND driving the pawn
   (`K2_SetActorLocation`). Note: UFunction dispatches via `UFunction.Func`
   @ +0xD8 (UFunction::Bind = sub_1412FACF0 sets it to ProcessInternal
   sub_141465CE0 for script fns); ProcessEvent is a UObject virtual, not yet
   pinned statically -- find its vtable slot or a clean AOB next.
2. Then enter gameplay, find `mainPlayer_C`, and port the Phase 2.1 orphan
   spawn into C++ behind `coop::RemotePlayer`.
