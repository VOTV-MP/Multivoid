# Multivoid

> **Multiplayer for Voices of the Void.**
> A standalone mod that adds drop-in co-op to a single-player UE4.27 game —
> **no original game files are modified**.

| | |
|--|--|
| **Current build** | `multivoid-0.9.0n-125.dll` — game target **0.9.0n**, build **b125** |
| **Game target** | Voices of the Void Alpha **0.9.0n** |
| **Status** | Alpha, pre-release — playable in development, no public builds yet |
| **Players** | up to **4** (host + 3) |
| **Platform** | Windows · UE4.27 · LAN + Internet |
| **Website** | [multivoid.dev](https://multivoid.dev) |

---

## Current phase

The project follows an 8-phase long-term arc (the MTA / gmod trajectory — see
[Roadmap](#roadmap) below). We are in **Phase 1: functional co-op** — the deep-sync
phase where VOTV's single-player systems are taken apart one by one and made
multiplayer-correct on a standalone engine-extension substrate. The multiplayer
foundation (transport, sessions, master server, join/save pipeline) is built and
live; the remaining Phase-1 work is hands-on verification breadth and the tail of
game systems still to sync.

### How far along is that

We deliberately don't publish a single "N% done" figure — a co-op mod is
finished system by system, not on one bar, and any summed percentage would hide
which systems are solid and which are untouched. The honest counts, generated
from the game's own class dump by [`tools/coverage.py`](tools/coverage.py): of
**2291** game classes, **1453** can actually diverge between peers, our code
names **175** of them, **12** carry a written status, and **2** are verified
hands-on by a player. Read those as a ladder, not a fraction — the last number
is the one that means "works for a player", and it is deliberately small. The
full derivation, the measurement error on each rung, and why a finer verb-level
percentage isn't yet honest live in the
[readiness-metric doc](research/findings/architecture-audits/votv-coop-readiness-metric-DESIGN-2026-07-22.md);
the facet-by-facet "what we sync and what we don't" per system lives in
[`docs/COOP_SYNC_PROFILES.md`](docs/COOP_SYNC_PROFILES.md).

---

## What works today

### Multiplayer foundation
- **LAN and Internet sessions** — one host, up to three clients; direct IP or the
  built-in **server browser** backed by the official master server
  (NAT traversal via signaling + TURN)
- **Version identity + join gate** — lobbies advertise `game + build`
  (e.g. `0.9.0n b125`); mismatched peers are refused pre-flight with a clear popup
  instead of desyncing mid-game. Old cohorts keep playing together forever —
  updates are never forced
- **Visible remote players** — full body, legs, IK feet, per-player skins,
  animated locomotion, ragdoll mirroring
- **Floating nameplates** with nickname + live ping, **chat**, and a system event
  feed (joins, leaves, activity lines)
- **Voice chat** — 3D positional, in-world
- **Join at any time** — connecting clients receive the host's full world state
  (a save-transfer snapshot), and **mid-activity join is a supported case by
  design**: joining mid-event, mid-download, mid-drive, mid-anything is handled
  per-system, never "don't join during X"

### Synced world
- **Physics props** — pickup, drag, drop, throw across the ~540 `Aprop_C` classes,
  including client-born props, per-grab authority transfer, and stable cross-peer
  identity that survives saves and rejoins
- **Piles and trash collection** — the full pickup/carry/deposit economy loop
- **NPCs and creatures** — host-simulated, pose-streamed to clients; kerfurs
  including the prop⇄NPC conversion cycle and per-kerfur skins
- **World events** — the scheduled/story event system replays host-observed events
  on clients with per-event dupe policies
- **Weather** — rain, snow, fog, wind, lightning; host-authoritative (clients
  never roll their own RNG — shared-world randomness is host-owned as a rule)
- **Doors, lights, switches, keypads, terminals**, sleep, damage/hazards,
  world-prop progression (drying, curing, growing — host owns the clock)

### The signal-processing pipeline (the heart of VOTV)
End-to-end sync of the workstation: dish control and calibration → ping →
signal catch → downloads → decoding → playback deck → drives and racks →
the in-game laptop (shared editable buffer, floppies, discs) → the meadow
signal database. Presser-authored state, one authority per axis, with the
desk's audio feedback mirrored to observers at the native audio seam.

### Infrastructure
- **Standalone loader** — `xinput1_3.dll` proxy + the versioned
  `multivoid-<game>-<build>.dll` payload
- **Official master server** — a static Rust binary on our VPS (lobby list,
  update check, signaling); the update check is informational only, never a gate
- **Kill switch** in the ini for emergency ship lockdown

---

## How it works

VOTV runs on Unreal Engine 4.27. The mod is a single DLL pair:

```
xinput1_3.dll                  -- thin proxy loader (Windows auto-loads it next to the .exe)
multivoid-0.9.0n-125.dll       -- the mod payload (versioned filename; highest build wins)
```

The payload resolves engine primitives (`GUObjectArray` / `GNames` /
`ProcessEvent`) via AOB signatures, then drives VOTV's own
`UClass` / `UFunction` machinery through reflection — no asset edits,
no `.pak` repacks, no UE4SS at runtime. Where ProcessEvent can't see
(Blueprint-internal dispatch), a bytecode-level VM interception substrate
catches the invisible verbs.

Transport is **GameNetworkingSockets** (Valve's UDP library) carrying an
unreliable pose stream plus a reliable ordered channel for events and state.
Each machine's local UE engine re-derives animation, physics, and rendering
from the streamed state. The host is authoritative for world state, RNG, and
NPC simulation; per-grab authority transfers for held props.

The codebase splits along a strict two-layer principle:
- [`src/votv-coop/src/ue_wrap/`](src/votv-coop/src/ue_wrap/) — engine wrapper (reflection, offsets, hooks; no gameplay)
- [`src/votv-coop/src/coop/`](src/votv-coop/src/coop/) — gameplay/network layer (element identity, sync lanes, sessions)
- [`src/votv-coop/src/harness/`](src/votv-coop/src/harness/) — boot glue + autonomous test scenarios

---

## Versioning

The version identity is the pair **(game version, build number)** — there is
no separate mod semver.

```
multivoid-0.9.0n-125.dll   ->   game target 0.9.0n, build 125
```

- **Game target** (`0.9.0n`) bumps when we adapt to a new VOTV cook
  (reflection offsets and BP layouts shift between game versions).
- **Build number** (`b125`) is the wire-protocol revision — it bumps with every
  release and every wire-format change.
- **Join compatibility is byte-equality on the pair, per lobby.** When VOTV
  0.10.0 ships we adapt immediately, but 0.9.0n cohorts keep playing among
  themselves on their old builds — updates are never forced. The server browser
  shows each lobby's pair and marks mismatches before you click.

Source of truth: [`src/votv-coop/CMakeLists.txt`](src/votv-coop/CMakeLists.txt)
(`VOTVCOOP_GAME_TARGET` + the build number parsed from `protocol.h`).

---

## Quick start

### For players

> No public builds are published yet — installation starts working with the first
> release on the [Releases page](https://github.com/VOTV-MP/Multivoid/releases).

1. Download the release pair: `xinput1_3.dll` + `multivoid-<game>-<build>.dll`.
2. Drop both files next to the game executable:
   `<your VOTV install>/WindowsNoEditor/VotV/Binaries/Win64/`.
3. Check the game version in the DLL name matches your VOTV version
   (e.g. `multivoid-0.9.0n-125.dll` targets VOTV `0.9.0n`).
4. Launch the game normally. A **Multiplayer** button appears in the main menu —
   host a lobby, or join one from the server browser (direct IP works too).
   No port forwarding needed.

To uninstall, delete the two DLLs. The mod never touches the game's own files.

### For developers

Requirements: Windows 10+, Visual Studio 2019/2022 **Build Tools** (C++ workload),
CMake 3.20+, and a legitimate copy of Voices of the Void at `Game_0.9.0n_HOST/`
next to the repo.

```powershell
# Configure once:
cmake -B build/votv-coop -S src/votv-coop -G "Visual Studio 16 2019" -A x64

# Build:
cmake --build build/votv-coop --config Release
```

Dev-only launchers (deploy the fresh build + start the game with a pinned
role/port — never how players run the mod):

```powershell
./mp_host_game.bat                 # host, default port 47621
./mp_client_connect.bat <host-ip>  # client
```

Same-PC testing? Use the sibling `Game_0.9.0n_CLIENT_1/` install — the launchers
detect it automatically. The autonomous two-peer test harness lives in `tools/`.

---

## Ecosystem

| Repo / place | What |
|--|--|
| [`VOTV-MP/Multivoid`](https://github.com/VOTV-MP/Multivoid) | **This repo** — the mod itself |
| [`VOTV-MP/Multivoid-server`](https://github.com/VOTV-MP/Multivoid-server) | The dedicated server (see roadmap phases 6 and 8) |
| [`VOTV-MP/Multivoid-wiki`](https://github.com/VOTV-MP/Multivoid-wiki) | User-facing documentation |
| [multivoid.dev](https://multivoid.dev) | Project website |

Repository layout:

| Path | What |
|--|--|
| [`docs/`](docs/) | Architecture, roadmap, scope, per-system sync docs, lessons ledger |
| [`research/findings/`](research/findings/) | Append-only dated RE / reflection / design findings |
| [`reference/`](reference/) | Vendored read-only references (UE4SS, MTA:SA, MinHook, GNS) |
| [`src/votv-coop/`](src/votv-coop/) | Mod source (`ue_wrap` / `coop` / `harness` / `loader` / `ui`) |
| [`tools/`](tools/) | Build / deploy / launch / autonomous-test helpers + the master server source |
| `Game_0.9.0n_HOST*/` | Local game install(s). **Gitignored** — never committed |

---

## Roadmap

The long-term arc, in order (each phase gates the next — detail in
[`docs/ROADMAP.md`](docs/ROADMAP.md)):

| # | Phase | Status |
|--|--|--|
| 1 | **Functional co-op** — deep sync of VOTV's systems on the standalone substrate | **in progress (current)** |
| 2 | **The arbiter** — per-element authority moves into a separate, engine-free server process; the host's game becomes an ordinary client of it | planned |
| 3 | **Sandbox mode** — support VOTV's sandbox rules as an explicit, portable "mode" layer | planned |
| 4 | **LuaJIT embedding** — the scripting substrate over the engine/coop APIs | planned |
| 5 | **Lua API** — mode rules move to Lua; the C++ core (transport, sync, identity) stays native | planned |
| 6 | **Resource system** — custom modes and plugins as one mechanism (the MTA shape) | planned |
| 7 | **Dedicated server** — 24/7 hosting with no live player required; the same server binary phase 2 already ships, launched by hand instead of spawned by the game | planned |
| 8 | **Resource infrastructure** — client-side resource download, sandboxing, public server browser | planned |

The old phase 8 ("native standalone server") was **retired as a phase** on 2026-07-20: the server is a
separate process from phase 2 onward, and its authority grows with every sync lane, so the MTA endgame
arrives by accumulation rather than as a milestone. See
[`docs/COOP_SERVER_MODEL.md`](docs/COOP_SERVER_MODEL.md).

---

## Architecture

Built on **eight architectural principles** documented in
[`docs/COOP_METHODOLOGY.md`](docs/COOP_METHODOLOGY.md):

1. **No modification of original game files**
2. **Engine-extension paradigm** — the mod is a new engine layer, not a patch
3. **Parallel class hierarchy** — our `RemotePlayer` owns network state; UE owns rendering
4. **Targeted crash fixes, not broad suppression**
5. **Minimum viable subset** — scope is a living document
6. **Augment SP, never replace it** — co-op is layered ON single-player
7. **Engine-wrapper layer vs gameplay/network layer** — strict subtree split
8. **Mid-activity join is always handled** — every sync lane defines its late-join answer

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

### A note from the author

This project is a free labour of love. I discovered VOTV in 2023 and played it
for weeks relentlessly, and I've been coming back every year since to explore
its new features. Every one of those runs was a great solo experience — and
eventually I wanted to share it with someone in multiplayer.

Let me be upfront: I'm not a programmer. Or rather, I am one — just with far
less baggage than a project of this magnitude demands. My roles here are
coordinator, director, tester, and architect.

I've always been into modding. My first mods were for GTA:SA when I was 10 or
11 — simple things like new objects on the map. Later I ran a SA-MP server with
my own gamemode, and a few Minecraft servers, and along the way I picked up how
it all actually works underneath. At some point I got into assembly-level mods
with Cheat Engine and learned a few things there — what opcodes are, how memory
scanning works, and so on — and made basic mods for some old games that way.

I never went especially deep, but that experience turned out to be useful
enough when I decided to build this project with Fable-5.

Going in, I already knew about projects like SA-MP and MTA, so I had somewhere
to pull principles and methodology from — and I did. Today's AI tools are
genuinely something, and combining them with IDA 9 over MCP, a proper
methodology, and agents analyzing Kismet bytecode gave me the dev environment
and the virtual team I needed.

To anyone hating on AI or AI-produced code: if you used AI for programming and
got garbage results, it means either your process is the problem or your tool
is a cheap one. Get a better tool, try a better methodology, and always
document your progress. Not just progress — document everything, every session.
And document it properly.

---

<sub>Multivoid is alpha software. Back up your saves before testing. Bug reports welcome.</sub>
