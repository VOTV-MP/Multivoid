# VOTV_MP — Cooperative multiplayer for Voices of the Void

A hook-only mod that adds functional coop multiplayer to **Voices of the
Void** (VOTV), a single-player **Unreal Engine 4.27** game. No original
game files are modified — the mod loads at runtime via
[UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) and drives the engine's own
`APawn` / `APlayerController` systems for a second player.

## Status

**Target game version: Alpha 0.9.0-n** (the mod version tag mirrors the
VOTV release it targets). Phase 0 done (feasibility + bootstrap +
substrate); Phase 1 (engine archaeology) underway. See `docs/FEASIBILITY.md`
for the viability assessment and `docs/ROADMAP.md` for the phase plan.

## Architecture in one paragraph

VOTV runs on UE4.27. Rather than blind binary RE, the mod uses UE4SS's
reflection access (`GUObjectArray` / `GNames`) to discover and hook the
game's classes, properties, and `UFunction`s. A custom UDP transport
(host-authoritative, LAN-first) replicates **input** and **authoritative
state**; the local UE engine re-derives animation, physics, and rendering
on each machine. The mod is split into an engine-wrapper layer
(`ue_wrap/`) and a gameplay/network layer (`coop/`) per principle 7 of
`docs/COOP_METHODOLOGY.md`.

## Layout

| Path | What |
|---|---|
| `docs/` | Living design docs (architecture, roadmap, scope, feasibility, methodology). |
| `research/findings/` | Append-only dated RE / reflection findings. |
| `reference/` | Vendored read-only references (UE4SS, MTA:SA). |
| `src/votv-coop/` | The mod source (`ue_wrap/` + `coop/` subtrees). |
| `tools/` | PowerShell / Python test + launch helpers. |
| `Game_*/` | Local game install. **Gitignored** — never committed. |

## Building / running

TBD (Phase 2+). Will document UE4SS install + mod load + LAN launch
scripts here once the foundation exists.

## Legal

This is a hook-only mod. It contains **no** Voices of the Void code or
assets. You must own a legitimate copy of the game to use it.
