# The mod source

One binary, `main.dll`, shipped as the UE4SS mod folder `Mods\Multivoid\dlls\main.dll` plus
`enabled.txt` and started through UE4SS's C-ABI `start_mod()` contract
(`src/loader/cppmod_entry.cpp`). Reflection, hooking, transport and replication are the mod's own
substrate; the DLL imports nothing from UE4SS, which is the loader and the development tool. The
identity, game target plus build, is in the boot banner and the DLL's generated VERSIONINFO
(`version.rc.in`), not in the filename.

## Subtrees (principle 7, `docs/ARCHITECTURE.md`)

```
include/
  coop/      gameplay and network headers
  harness/   the test harness: scenarios, config, screenshot
  ue_wrap/   the engine wrapper: one module per engine or game surface; reflection access,
             struct offsets, function thunks; no network or gameplay logic
  ui/        the menus, the server browser, the HUD, the overlay

src/
  bootstrap/   dllmain.cpp (the detach backstop) and boot.cpp (the boot thread)
  loader/      cppmod_entry.cpp, the UE4SS start_mod() contract, and cppmod_stubs.asm
  ue_wrap/     the engine wrapper and the MinHook ProcessEvent detour
  coop/        gameplay and network: RemotePlayer, sessions, transport, the sync lanes
  ui/          the native screens and the ImGui overlay
  harness/     the scenario runner, the scenarios, sdk_check, screenshot, config

third_party/
  minhook/  GameNetworkingSockets/  imgui/  opus/   submodules, built into the DLL
  freetype/  miniaudio/                             vendored in-tree
```

A file that both dereferences engine memory and owns network state is split: the engine-touching
half goes to `ue_wrap/`, the network state to `coop/`, joined by a header API.

## Build and deploy

`BUILDING.md` at the repository root is the whole story: the toolchain, the configure, the build,
and `tools/deploy-all.ps1`, which deploys `main.dll` to the four local game copies and refuses a
DLL whose VERSIONINFO disagrees with the tree. The UE4SS substrate is installed once per copy by
`tools/install-ue4ss.ps1`.

## What is not in this tree

- UE4SS's code: the mod imports nothing from it. UE4SS itself is the loader every game copy
  runs (`tools/install-ue4ss.ps1` pins one build; the mod manager delivers the same one), and
  the development copy also uses it for Live View, Lua probes and header dumps
  (`docs/RE_WORKFLOW.md`).
- Game assets: never touched. Every sync rides reflected function calls and reads through cached
  offsets.
