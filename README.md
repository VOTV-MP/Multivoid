# VOTV_MP

> **Cooperative multiplayer for Voices of the Void.**
> A standalone mod that adds drop-in coop to a single-player UE4.27 game —
> **no original game files are modified**.

| | |
|--|--|
| **Mod version** | `0.0.1+votv-0.9.0-n` |
| **Game target** | VOTV Alpha **0.9.0-n** |
| **Status** | Alpha — playable on LAN, features landing weekly |
| **Players** | up to **4** (1v1 hands-on tested) |
| **Platform** | Windows · UE4.27 · LAN-first |

---

## What works today

### Multiplayer foundation
- **LAN sessions** — one host, up to three clients connect by IP
- **Visible remote players** — full body, legs, IK feet, your skin, animated locomotion
- **Floating nameplates** with nickname + live ping (`Alex (42 ms)`)
- **Chat + system event feed** — joins, leaves, reliable-channel events
- **Connect-time world snapshot** — joining clients receive the current world state from the host

### Synced gameplay
- **Physics-prop pickup, drag, drop, throw** — sync the ~540 `Aprop_C` classes including throw impulse
- **World prop spawn + destroy** — picking, harvesting, breaking all replicate
- **Weather** — rain, snow, fog enables, wind, lightning strikes (host-authoritative, client never rolls)
- **Flashlight on/off + intensity + cone shape** on the remote player
- **3D positional click sound** on the puppet when the remote toggles their light
- **Mushroom growth stages** suppressed on client (host-authoritative)

### Quality of life
- **Polished nameplate text** — outline + drop shadow, sanitized nicknames
- **Master kill switch** in `votv-coop.ini` for ship lockdown
- **Per-process window titles** (`VotV (Host)`, `VotV (Client)`) for OBS capture

---

## How it works

VOTV runs on Unreal Engine 4.27. The mod is a single DLL pair:

```
xinput1_3.dll   -- thin proxy loader (Windows auto-loads it next to the .exe)
votv-coop.dll   -- the mod payload
```

The payload resolves engine primitives (`GUObjectArray` / `GNames` /
`ProcessEvent`) via AOB signatures, then drives VOTV's own
`UClass` / `UFunction` machinery through reflection — no asset edits,
no `.pak` repacks, no UE4SS at runtime.

A custom UDP transport carries two channels over one socket:

- **Unreliable pose stream** — 30 Hz player + held-prop snapshots, freely dropped
- **Reliable ARQ channel** — chat, weather state, prop spawn/destroy, lightning, system events

Each machine's local UE engine re-derives animation, physics, and rendering
from the streamed state. Host is authoritative for world state; per-grab
authority transfers for held props.

The codebase splits cleanly along Principle 7:
- [`src/votv-coop/src/ue_wrap/`](src/votv-coop/src/ue_wrap/) — engine wrapper (reflection, offsets, hooks)
- [`src/votv-coop/src/coop/`](src/votv-coop/src/coop/) — gameplay/network layer (RemotePlayer, sessions, UDP)
- [`src/votv-coop/src/harness/`](src/votv-coop/src/harness/) — boot glue + autonomous test scenarios

---

## Quick start

### Requirements
- Windows 10+
- Visual Studio 2019 (or 2022) **Build Tools** with the C++ workload
- CMake 3.20+
- A legitimate copy of Voices of the Void at `Game_0.9.0n/` next to the repo

### Build

```powershell
# Configure once:
cmake -B build/votv-coop -S src/votv-coop -G "Visual Studio 16 2019" -A x64

# Build:
cmake --build build/votv-coop --config Release
```

### Play coop on LAN

```powershell
# On the host's PC (deploys the DLLs, launches the game):
./mp_host_game.bat                 # default port 47621, nick "Host"
./mp_host_game.bat 47700 MyNick    # custom port + nick

# On the client's PC:
./mp_client_connect.bat <host-LAN-IP>
```

Same-PC testing? Use the sibling `Game_0.9.0n_copy/` install — the launchers
detect it automatically.

---

## Versioning

The mod uses **SemVer 2.0 with build metadata** — the `+votv-X.Y.Z` suffix
identifies the game version this build targets.

- `kModVersion` — `0.0.1` — bumps when **mod code** changes (features, fixes)
- `kGameTarget` — `0.9.0-n` — bumps when we **adapt to a new VOTV cook**
- `kVersionFull` — `0.0.1+votv-0.9.0-n` — the canonical tag + log banner string

> **Why two parts?** Reflection offsets and BP field names shift between
> VOTV cooks, so the DLL is incompatible across game versions. The
> `+votv-X.Y.Z` suffix is the canonical "tested against" marker. Per the
> SemVer 2.0 spec, build metadata is **ignored** for version precedence —
> tools see `0.0.1` and `0.0.1` as equal regardless of game target.

Same scheme used by Minecraft Forge mods, SKSE, and Bannerlord BLSE.

Source of truth: [`src/votv-coop/CMakeLists.txt`](src/votv-coop/CMakeLists.txt)
(`project(... VERSION ...)` + `VOTVCOOP_GAME_TARGET`).

---

## Repository layout

| Path | What |
|--|--|
| [`docs/`](docs/) | Architecture, roadmap, scope, feasibility, methodology |
| [`research/findings/`](research/findings/) | Append-only dated RE / reflection findings |
| [`reference/`](reference/) | Vendored read-only references (UE4SS, MTA:SA, VoidTogether) |
| [`src/votv-coop/`](src/votv-coop/) | Mod source (`ue_wrap` / `coop` / `harness` / `loader`) |
| [`tools/`](tools/) | PowerShell helpers — build / deploy / launch / autonomous test |
| `Game_0.9.0n*/` | Local game install(s). **Gitignored** — never committed |

---

## Roadmap

- [x] Phase 0 — Feasibility audit
- [x] Phase 1 — Engine archaeology (reflection + UFunction substrate)
- [x] Phase 2 — Standalone bootstrap (proxy DLL + own loader)
- [x] Phase 3 — Pose sync + transport (UDP + reliable channel + RTT)
- [x] Phase 5F — Flashlight activation sync
- [x] Phase 5S0 — Snapshot-on-connect + continuous prop lifecycle
- [x] Phase 5W — Weather sync (rain / snow / fog / wind / lightning)
- [ ] Phase 5N — NPCs + interactable entities (RE done, implementation pending)
- [ ] Phase 5T — Terminals + signal-catching interactivity
- [ ] Phase 5D — Doors, lights, switches, keypads
- [ ] Phase 4.6 — Shared economy + save bootstrap
- [ ] Phase 7+ — Master server + public browser (WAN)

Detail: [`docs/ROADMAP.md`](docs/ROADMAP.md). Scope: [`docs/COOP_SCOPE.md`](docs/COOP_SCOPE.md).

---

## Architecture

The mod is built on **seven architectural principles** documented in
[`docs/COOP_METHODOLOGY.md`](docs/COOP_METHODOLOGY.md):

1. **No modification of original game files**
2. **Engine-extension paradigm** — the mod is a new engine layer, not a patch
3. **Parallel class hierarchy** — our `RemotePlayer` owns network state; UE owns rendering
4. **Targeted crash fixes, not broad suppression**
5. **Minimum viable subset** — scope is a living document
6. **Augment SP, never replace it** — coop is layered ON single-player
7. **Engine-wrapper layer vs gameplay/network layer** — strict subtree split

Three "no-compromise" rules govern day-to-day work:
- **RULE 1** — No crutches, no quick fixes. Root cause every time.
- **RULE 2** — No migration baggage. Old code goes when replaced.
- **RULE 3** — Standalone mod. UE4SS is a dev tool only; not loaded at runtime.

---

## Legal

This is a **hook-only standalone mod**. It contains **no Voices of the Void
code or assets**. You must own a legitimate copy of the game to use it.

Distributed under the same terms as the upstream references it borrows from:
**MIT** for MinHook and the UE4SS-derived reflection algorithms. Unaffiliated
with the VOTV authors.

---

<sub>VOTV_MP is alpha software. Backups before testing. Bug reports welcome.</sub>
