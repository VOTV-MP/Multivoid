# multivoid (working name: votv-coop) — the mod source

Coop mod DLL for Voices of the Void (UE4.27). One binary:

- **`main.dll`** — the whole mod, shipped as the UE4SS mod folder
  `Mods\Multivoid\dlls\main.dll` + `enabled.txt` and started via the
  C-ABI `start_mod()` contract (`src/loader/cppmod_entry.cpp`). All
  reflection + hooking + transport + replication is our own substrate
  (D-3 slim contract: zero UE4SS imports); UE4SS is the *loader*.
  Identity (game target + build) is in the boot banner and the DLL's
  generated VERSIONINFO (`version.rc.in`), not the filename. (The
  standalone `xinput1_3.dll` proxy + versioned payload name retired
  whole at UE4SS_ARC WP-2 commit 3.)

## Subtrees (principle 7 — see `docs/COOP_METHODOLOGY.md` / `CLAUDE.md`)

```
include/
  coop/      Gameplay + network layer headers.
  harness/   Autonomous test harness scenarios + config + screenshot.
  ue_wrap/   Engine-wrapper layer headers. One module per UE/VOTV
             surface. Reflection access, struct offsets, UFunction
             thunks. NO network logic, NO gameplay logic.

src/
  bootstrap/   dllmain.cpp (DETACH backstop) + boot.cpp (the boot thread).
  loader/      cppmod_entry.cpp — the UE4SS C-ABI start_mod() contract
               (+ cppmod_stubs.asm, the era-safe vtable stub surface).
  ue_wrap/     Engine-wrapper implementations + MinHook-based
               ProcessEvent detour for game-thread context.
  coop/        Gameplay + network. RemotePlayer, sessions, transport,
               reliable_channel, prop_lifecycle, nameplate,
               event_feed, npc_sync, etc.
  harness/     Test harness, autotest, scenario timeline, sdk_check,
               screenshot, config.

third_party/
  minhook/     MinHook (MIT) — submodule. Used by ue_wrap/hook.cpp for
               the ProcessEvent detour. Built into our own static-CRT
               static lib so the runtime matches the mod.
```

**Principle 7 rule:** a file that BOTH dereferences engine memory /
reflection AND owns network state violates it — split (WP17): engine-
touching code to `ue_wrap/`, network state to `coop/`, communicating
via a clean header API.

## Build

Visual Studio 2019 Build Tools (or 2022) with C++ workload, CMake 3.20+.

```powershell
# From project root:
cmake -B build/votv-coop -S src/votv-coop -G "Visual Studio 16 2019" -A x64
cmake --build build/votv-coop --config Release
```

Output: `build/votv-coop/Release/main.dll` (VERSIONINFO carries the
`<game target> b<build>` pair).

Deploy: `tools/deploy-mod.ps1 -GameWin64 <path>` (or `deploy-all.ps1`
for all four game copies) is idempotent (skip-if-identical) and
fail-closed on a tree/DLL identity mismatch. The UE4SS substrate is a
one-time per-copy install: `tools/install-ue4ss.ps1`.
Both `mp_host_game.bat` and `mp_client_connect.bat` call it before
launching.

## What's NOT in this tree

- **UE4SS** — not a runtime dependency (RULE №3). Used only by the dev
  copy (`Game_0.9.0n_CLIENT_3/`) for Live View, Lua probe scripting, and
  header dumping. See `docs/RE_WORKFLOW.md`.
- **Game assets** — not touched (principle 1). All sync rides through
  reflected UFunction calls + direct memory writes via cached offsets.
