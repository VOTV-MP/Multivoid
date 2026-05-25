# votv-coop — the mod source

Standalone coop mod DLL for Voices of the Void (UE4.27). Two binaries:

- **`xinput1_3.dll`** — proxy loader (`src/loader/xinput_proxy.cpp`).
  VOTV imports XInputGetState/SetState; we forward them to System32's
  xinput1_4.dll and side-load `votv-coop.dll` from `DllMain`. No
  injection. No UE4SS at runtime.
- **`votv-coop.dll`** — the payload. Self-contained reflection +
  hooking + transport + replication.

## Subtrees (principle 7 — see `docs/COOP_METHODOLOGY.md` / `CLAUDE.md`)

```
include/
  coop/      Gameplay + network layer headers.
  dev/       Developer-only convenience features (freecam, HUD,
             F3/F4 keybinds). Ini-gated; off by default.
  harness/   Autonomous test harness scenarios + config + screenshot.
  ue_wrap/   Engine-wrapper layer headers. One module per UE/VOTV
             surface. Reflection access, struct offsets, UFunction
             thunks. NO network logic, NO gameplay logic.

src/
  bootstrap/   dllmain.cpp — entry point, kicks off boot thread.
  loader/      xinput_proxy.cpp — the standalone xinput1_3.dll proxy.
  ue_wrap/     Engine-wrapper implementations + MinHook-based
               ProcessEvent detour for game-thread context.
  coop/        Gameplay + network. RemotePlayer, sessions, transport,
               reliable_channel, prop_lifecycle, nameplate,
               event_feed, npc_sync, etc.
  dev/         Freecam, pos_hud, restore_vitals, teleport_client.
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

Output: `build/votv-coop/Release/votv-coop.dll` and
`build/votv-coop/Release/xinput1_3.dll`.

Deploy: `tools/deploy-loader.ps1 -Standalone -GameWin64 <path>` is
idempotent (skip-if-identical) — safe to run while VOTV is loaded.
Both `mp_host_game.bat` and `mp_client_connect.bat` call it before
launching.

## What's NOT in this tree

- **UE4SS** — not a runtime dependency (RULE №3). Used only by the dev
  copy (`Game_0.9.0n_dev/`) for Live View, Lua probe scripting, and
  header dumping. See `docs/RE_WORKFLOW.md`.
- **Game assets** — not touched (principle 1). All sync rides through
  reflected UFunction calls + direct memory writes via cached offsets.
