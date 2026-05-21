# Finding: UE4SS substrate bring-up + Phase 1 dump procedure

**Date**: 2026-05-21
**Phase**: 0 → 1 enabler
**Status**: substrate installed; awaiting first game launch to capture
reflection dumps (requires the user — interactive GUI launch).

## Summary

Stood up the reflection/hooking substrate for VOTV coop. Confirmed the
target is a stripped UE4.27 shipping binary, so blind static RE is the
wrong primary path (it would reinvent what UE reflection gives for free —
WP13). UE4SS v3.0.1 is installed into the game and will provide the
authoritative class/property/function data for Phase 1 archaeology.

## Binary facts (from IDA, `VotV-Win64-Shipping.exe`)

- MD5 `28139c7c314659f8d3122155a6f33a69`,
  SHA256 `ad478218ec5513cc4c1682937db3214cbf2694d1dcd0583eb423447c049dd3ae`.
- Image base `0x140000000`, image size `0x538f000` (~87 MB).
- **204,544 functions; only 755 named** (all third-party: libpng, AMD AGS,
  libsamplerate). No UE symbols — standard stripped UE4 shipping build.
- Engine version string: `++UE4+Release-4.27` (matches FEASIBILITY).
- IDB is loaded and analyzed in the IDA Pro MCP (`hexrays_ready`), kept as
  the **fallback** for things reflection can't see (raw layout, native
  non-reflected code, Phase 2 crash-site root-causing).

## Substrate installed

- **UE4SS v3.0.1** (pinned; latest stable, 2024-02-14) extracted into
  `Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/` next to the shipping
  exe. Additive only — `dwmapi.dll` (proxy) + `UE4SS.dll` +
  `UE4SS-settings.ini` + `Mods/`. No original game file modified
  (principle 1 holds). Reproducible via `tools/install-ue4ss.ps1`
  (game dir is gitignored, so the script is the committed source of truth).
- UE4SS auto-detects the engine version via AOB scan; no
  `EngineVersionOverride` needed for 4.27.
- Settings: `GuiConsoleEnabled=1`, `GuiConsoleVisible=1` so the debug GUI
  (log + **Live View** object browser) opens on launch and the first run
  self-verifies. `LoadAllAssetsBefore*` kept `0` (the =1 path is unstable /
  multi-GB and crashes if you load past the menu after dumping).

## Bundled mods of immediate interest

- **SplitScreenMod** (`Mods/SplitScreenMod/Scripts/main.lua`) — a working
  reference for creating a 2nd local player (`APlayerController` + pawn)
  via UE's native split-screen path. Direct precedent for the Phase 2.1
  spawn-orphan derisk. Read before designing `RemotePlayer`.
- ConsoleEnablerMod / ConsoleCommandsMod — enable the UE console + `dumpobject` etc.
- CheatManagerEnablerMod, LineTraceMod, Keybinds, ActorDumperMod.

## Phase 1 dump procedure (authoritative — from UE4SS docs, not memory)

Built-in keybinds (customizable in `Mods/Keybinds/Scripts/main.lua`):

| Keybind | Output | Use |
|---|---|---|
| `CTRL+H` | `CXXHeaderDump/` (.hpp per class/BP, with offsets+sizes) | Player/pawn/controller class layouts → Phase 1.2 |
| `CTRL+J` | `UE4SS_ObjectDump.txt` (all loaded objects + offsets) | Object/property offsets, ownership graph |
| `CTRL+Numpad9` | UHT-compatible headers | Mirror `.uproject` if needed |
| `CTRL+Numpad6` | `.usmap` | Unversioned property mapping |

**Capture strategy**: dump once at the **main menu** (engine + core BP
classes), then **load a save and dump again in-world** to capture VOTV's
actually-instantiated player pawn, controller, and gameplay actors.
`DumpOffsetsAndSizes=1` is on, so the `.hpp` files carry property offsets —
exactly what Phase 1.2 (player class layout) needs.

## Next (needs the user — one interactive launch)

1. Launch VOTV (YeetPatch or the shipping exe). Confirm the UE4SS debug GUI
   appears and reports detected engine version 4.27 + no AOB scan failures.
2. At main menu: `CTRL+H`, `CTRL+J`.
3. Load a save; in-world: `CTRL+H`, `CTRL+J` again.
4. Hand the generated `CXXHeaderDump/` + `UE4SS_ObjectDump.txt` back (or
   leave in place — they're under the gitignored game dir; we can read
   them directly from there).

Then Phase 1.2 (player class layout) and the Phase 2.1 spawn-orphan
experiment proceed against real reflection data.

## Open verification (carry forward)

- Confirm UE4SS injects cleanly on this build + render API (note the
  YeetPatch DX11/DX12/Vulkan switch; UE4SS GUI uses opengl by default and
  is a separate window, so it should be API-agnostic). → FEASIBILITY 0.3
- VOTV player pawn + controller class names. → 1.2
- Single-instance enforcement for same-machine dual-launch. → 0.8
