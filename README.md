# VOTV_MP — Cooperative multiplayer for Voices of the Void

A standalone mod that adds functional coop multiplayer to **Voices of
the Void** (VOTV), a single-player **Unreal Engine 4.27** game. **No
original game files are modified.** The mod ships as two DLLs
(`xinput1_3.dll` proxy loader + `votv-coop.dll` payload) that drive
VOTV's own `APawn` / `APlayerController` systems for a second player.

## Status

**Target game version: Alpha 0.9.0-n** (the mod version mirrors the
VOTV release it targets — versioning rule in `CLAUDE.md`).

**Phases 0-3 + 5 done.** The transport, the puppet, the LAN test
framework, the reliable channel, the chat + event feed, the physics-
prop pickup, the snapshot-on-connect, and dev convenience features (F2
HUD / F3 vitals refill / F4 teleport / HOME freecam) all ship. Phase 4
replication is the live workstream — see `docs/ROADMAP.md` "Live
workstreams" for the active 5N (NPCs), 5S (save), 5T (terminals), and
5D (doors+lights) increment series.

## Architecture in one paragraph

VOTV runs on UE4.27. The mod uses **reflection** (`GUObjectArray` /
`GNames` / `ProcessEvent` resolved standalone via AOB signatures) to
discover and call the game's classes, properties, and `UFunction`s — no
binary RE for the common path. A custom UDP transport
(host-authoritative, LAN-first) carries unreliable pose snapshots and a
parallel reliable channel for state events. The local UE engine
re-derives animation, physics, and rendering on each machine. The mod
is split into an engine-wrapper layer (`ue_wrap/`) and a gameplay/
network layer (`coop/`) per principle 7 of `docs/COOP_METHODOLOGY.md`.
UE4SS is a **development tool only** (Live View, Lua probes, header
dumps) — it is not loaded at runtime by the shipping mod (RULE №3).

## Layout

| Path | What |
|---|---|
| `docs/` | Living design docs (architecture, roadmap, scope, feasibility, methodology). |
| `research/findings/` | Append-only dated RE / reflection findings. |
| `reference/` | Vendored read-only references (UE4SS, MTA:SA, VoidTogether for cross-pollination). |
| `src/votv-coop/` | The mod source (`ue_wrap/` + `coop/` + `dev/` + `harness/` + `loader/`). |
| `tools/` | PowerShell helpers: build / deploy / launch / autonomous test. |
| `Game_0.9.0n*/` | Local game install(s). **Gitignored** — never committed. |

## Building / running

Requirements: Visual Studio 2019 (or 2022) Build Tools with the C++
workload, CMake 3.20+, a legitimate Voices of the Void install at
`Game_0.9.0n/`.

```powershell
# One-time CMake configure:
cmake -B build/votv-coop -S src/votv-coop -G "Visual Studio 16 2019" -A x64

# Build:
cmake --build build/votv-coop --config Release

# Launch as HOST (deploys the DLLs, sets env vars, launches the game):
./mp_host_game.bat              # port 47621, nick "Host"
./mp_host_game.bat 47700 MyNick # custom port + nick

# On another machine (or in a sibling Game_0.9.0n_copy/ folder for
# same-box testing), launch the CLIENT:
./mp_client_connect.bat <host-LAN-IP>
```

The `mp_*.bat` scripts call `tools/deploy-loader.ps1` (idempotent,
skip-if-identical) to copy `xinput1_3.dll` + `votv-coop.dll` into the
game install, then launch the shipping exe with the coop config in env
vars. No injection; no UE4SS. See `docs/RE_WORKFLOW.md` for the "three
copies" convention (`Game_0.9.0n/` host, `Game_0.9.0n_copy/` client,
`Game_0.9.0n_dev/` dev with UE4SS for diagnosis).

For autonomous (Claude-driven) testing: `tools/lan-test.ps1` runs both
host + client in the dev copy with per-PID log capture. Scenarios live
in `harness::autotest` (driven by `scenario.txt` next to the DLL).

## Legal

This is a standalone hook-only mod. It contains **no** Voices of the
Void code or assets. You must own a legitimate copy of the game to use
it. Distributed under the same terms as the upstream references it
borrows from (MIT for MinHook and UE4SS-derived reflection patterns;
unaffiliated with the VOTV authors).
