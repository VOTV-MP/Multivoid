# Finding: standalone mod DLL builds + loads into VOTV (no UE4SS)

**Date**: 2026-05-22
**Game version**: Alpha 0.9.0-n
**Phase**: standalone foundation (RULE No.3)
**Status**: build + standalone load proven; loader is still a manual
injector (proxy DLL is the next step).

## Result

The standalone mod (`votv-coop.dll`) builds with the VS2019 BuildTools
toolset (MSVC 14.29) via CMake (VS16 generator, x64, static CRT), and
**loads + runs inside `VotV-Win64-Shipping.exe` with UE4SS entirely
absent**:

```
UE4SS disabled (dwmapi.dll renamed)
launch VOTV (no UE4SS) -> inject votv-coop.dll (PID 30012)
marker: "votv-coop standalone bootstrap loaded into PID 30012 (no UE4SS)"
loaded modules: votv-coop.dll present; UE4SS.dll ABSENT
  (dwmapi.dll in the list is the SYSTEM dll VOTV imports, not UE4SS's proxy)
```

This validates the whole standalone pipeline: toolchain -> build -> a DLL
that maps into the UE4 process and runs its own code, with zero UE4SS
dependency.

## Build / run

- Configure: `cmake -S src/votv-coop -B build/votv-coop -G "Visual Studio 16 2019" -A x64`
- Build: `cmake --build build/votv-coop --config Release` -> `votv-coop.dll`
- Validate load (dev): `tools/inject.ps1 -Dll <abs path to votv-coop.dll>`
  (CreateRemoteThread+LoadLibraryW; a DEV tool, not the shipping loader).

## UE4SS coexistence

- Shipping: no UE4SS present -> no conflict.
- Dev (both loaded): coexist cleanly IF (a) our loader uses a DIFFERENT
  proxy DLL than UE4SS's `dwmapi.dll`, and (b) shared `UFunction`/
  `ProcessEvent` hooks chain trampolines rather than stomp. Reflection
  resolution is read-only -> no contention. For pure standalone checks,
  disable UE4SS (rename `dwmapi.dll`) as done here.

## Next

1. **Shipping loader = proxy DLL** (replace the manual injector). Pick a DLL
   VOTV imports with a small export surface, NOT `dwmapi` (UE4SS), e.g.
   `dxgi`/`version`/`winmm`; generate export forwarders; auto-loads from the
   exe dir. (Confirm the import via IDA `imports`.)
2. **ue_wrap reflection**: resolve `GUObjectArray` / `GNames` /
   `ProcessEvent` via AOB sigs (IDA to find them) -> our own SDK access,
   no UE4SS. The CXX dump is the type/offset reference.
3. Then port the validated orphan spawn (Phase 2.1) into C++ behind
   `coop::RemotePlayer`.
