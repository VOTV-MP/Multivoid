# Multivoid

> **Multiplayer for Voices of the Void.**
> A mod that adds drop-in co-op to a single-player UE4.27 game —
> **no original game files are modified**.

[![Support on Boosty](https://img.shields.io/badge/Boosty-support%20the%20project-FF7C00?style=for-the-badge)](https://boosty.to/pelmentor)

| | |
|--|--|
| **Install** | [**Multivoid on Thunderstore**](https://thunderstore.io/c/voices-of-the-void/p/Pelmentor/Multivoid/) — through r2modman or the Thunderstore Mod Manager, which set the loader up for you ([other routes](docs/INSTALL.md)) |
| **Current build** | the newest `.zip` on the [Releases page](https://github.com/VOTV-MP/Multivoid/releases) (dev prereleases; the zip name and the in-game banner carry the identity) |
| **Game target** | Voices of the Void Alpha **0.9.0n** |
| **Status** | Alpha — dev prereleases published for testing; no stable release yet |
| **Players** | up to **4** (host + 3) |
| **Platform** | Windows · UE4.27 · LAN + Internet |
| **Website** | [multivoid.dev](https://multivoid.dev) |
| **Community** | [Discord](https://discord.gg/bA6tGBvGMN) — chat about the project, ask questions, report bugs |
| **Support** | [Boosty](https://boosty.to/pelmentor) — **the mod is free and always will be**; this covers the master-server VPS and the tooling bill |

---

## What works today

- **LAN and Internet sessions** — one host, up to three clients; direct IP or the built-in
  **server browser** backed by the official master server (NAT traversal via signaling + TURN).
  Lobbies advertise `game + build`; a mismatched peer is refused before joining, and old cohorts
  keep playing together — updates are never forced.
- **Visible remote players** — full body, IK feet, per-player skins, animated locomotion, ragdolls,
  floating nameplates with nickname and ping, text chat, 3D positional **voice chat**.
- **Join at any time** — a connecting client receives the host's full world state, and joining
  mid-event, mid-download or mid-drive is handled per system, never "don't join during X".
- **Synced world** — physics props (pickup, carry, throw, per-grab authority, identity that survives
  saves and rejoins), piles and trash collection, NPCs and creatures (host-simulated, including the
  kerfur prop-to-NPC cycle), world events, weather, doors, lights, switches, keypads, terminals,
  sleep, damage, the ATV, and world props that change over time (the host owns the clock).
- **The signal-processing pipeline** — dish control, ping, signal catch, downloads, decoding, the
  playback deck, drives and racks, the laptop, and the meadow signal database: one authority per
  axis, with the desk's audio feedback mirrored to observers.
- **Infrastructure** — ships as a standard UE4SS mod folder with **zero imports from UE4SS** (the
  reflection, hooks, transport and UI are the mod's own); an official master server for the lobby
  list, signaling and an informational update check.

The per-system answer to "how far along is it", with the evidence for each claim, is
[docs/COOP_SYNC_PROFILES.md](docs/COOP_SYNC_PROFILES.md). There is deliberately no single "N% done"
figure: a co-op mod is finished system by system.

## How it works

VOTV runs on Unreal Engine 4.27. The mod is one DLL in a standard UE4SS mod folder
(`Mods\Multivoid\dlls\main.dll` + `enabled.txt`). UE4SS, or r2modman's unreal-shimloader, only
*loads* it: the DLL imports nothing from it. The mod resolves the engine's reflection primitives
with its own signatures, drives the game's own classes and functions through reflection, and hooks
where reflection cannot see. No asset edits, no repacked paks.

Transport is GameNetworkingSockets carrying an unreliable pose stream and a reliable ordered
channel for events and state. Each machine's engine re-derives animation, physics and rendering
from the streamed state. The host is authoritative for world state, randomness and NPC simulation;
a client acts by naming an intent that the host performs.

The code splits along one principle: `src/votv-coop/src/ue_wrap/` wraps the engine and holds no
gameplay; `src/votv-coop/src/coop/` holds gameplay and network and reaches the engine only through
the wrapper. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) is the one-read overview and
[docs/CODE_MAP.md](docs/CODE_MAP.md) says where every concept lives.

## Versioning

The version identity is the pair **(game version, build number)**: `Multivoid 0.9.0n b<N>` in the
in-game banner, in the DLL's own version resource, and in the release tag. The game target bumps
when the mod adapts to a new VOTV build; the build number is the wire-protocol revision and bumps
with every release and every wire change. The zip is named `Pelmentor-Multivoid-0.9.<N>.zip` because
mod managers require a numeric `major.minor.patch`; it is the same pair, not a third axis. Join
compatibility is byte-equality on the pair, per lobby. Source of truth:
[`src/votv-coop/CMakeLists.txt`](src/votv-coop/CMakeLists.txt).

## Quick start

### For players

> **There is no stable release yet.** Every build on the [Releases page](https://github.com/VOTV-MP/Multivoid/releases)
> is a dev build, and everyone playing one is a tester. Expect bugs, and please report them on
> [Discord](https://discord.gg/bA6tGBvGMN); good reports get credited. What to attach:
> [docs/INSTALL.md](docs/INSTALL.md).

One zip, two ways to install it: through **r2modman** (recommended; it sets up the loader itself)
or manually into the game's UE4SS `Mods\` folder. Launch, and a **Multiplayer** button appears in
the main menu. No port forwarding needed. Full steps, updating, troubleshooting and uninstalling:
[docs/INSTALL.md](docs/INSTALL.md).

### For developers

[BUILDING.md](BUILDING.md) covers the local toolchain and building via GitHub Actions from a fork
(no local toolchain needed). [CONTRIBUTING.md](CONTRIBUTING.md) is the rulebook: the rules a change
must respect, how it is tested, and the shape a commit and a doc must have.

```powershell
cmake -B build/votv-coop -S src/votv-coop -G "Visual Studio 16 2019" -A x64
cmake --build build/votv-coop --config Release
```

## Repository layout

| Path | What |
|--|--|
| [`docs/`](docs/README.md) | **Start here** — the documentation index |
| [`src/votv-coop/`](src/votv-coop/) | The mod: `ue_wrap` / `coop` / `ui` / `harness` / `loader` |
| [`tools/`](tools/) | Build, deploy, the autonomous test rig, release, and the master server (`coop-server-rs/`) |
| [`reference/`](reference/) | Vendored read-only references (UE4SS, MTA:SA) |
| `Game_0.9.0n_HOST*/` | Local game installs. **Gitignored** — never committed |

## Roadmap

The long-term arc, in order; each phase gates the next ([docs/ROADMAP.md](docs/ROADMAP.md)):

| # | Phase | Status |
|--|--|--|
| 1 | **Functional co-op** — deep sync of VOTV's systems on the mod's own substrate | **in progress (current)** |
| 2 | **The arbiter** — per-element authority moves into a separate, engine-free server process; the host's game becomes an ordinary client of it | planned |
| 3 | **Sandbox mode** — VOTV's sandbox rules as an explicit, portable "mode" layer | planned |
| 4 | **LuaJIT embedding** — the scripting substrate over the engine and co-op APIs | planned |
| 5 | **Lua API** — mode rules move to Lua; the C++ core stays native | planned |
| 6 | **Resource system** — custom modes and plugins as one mechanism | planned |
| 7 | **Dedicated server** — 24/7 hosting with no live player required | planned |
| 8 | **Resource infrastructure** — client-side resource download, sandboxing, public server browser | planned |

## Credits

Multivoid is written and directed by one person (Pelmentor) with heavy use of AI coding tools:
direction, architecture, testing and every release decision are mine; much of the code was written
with Claude. The full commit history is public, so you can judge the process as well as the result.

Everything else in it came from outside, as code, reports or review. If it changed the mod, it gets
a row; the full ledger is [docs/CREDITS.md](docs/CREDITS.md).

| Who | Kind | Contribution | Landed |
|--|--|--|--|
| **Pelmentor** | code | Architecture, direction, releases — the whole mod | 474 commits |
| **Claude** (Anthropic) | code | Implementation, across the whole mod | 1,368 commits |
| **Tarangok** | code | KO respawn, live skin preview, held-prop visibility, container extraction | 10 commits |
| **hediiiqq** | code | Dish mirror interpolation | 4 commits |
| [**arigalit**](https://github.com/arigalit) | code · report | ATV seat contention ([#9](https://github.com/VOTV-MP/Multivoid/pull/9)); join-time prop-count divergence | 2 commits |
| [**huoyan1231**](https://github.com/huoyan1231) | code · report | CI and automated builds; the b125 host-log pack | 2 commits · b134 |
| [**archhn0madd**](https://github.com/archhn0madd) | code | Rejoin without a relaunch — the boot poll answered from the dying world | 1 commit |
| **Moddy** (Discord) | review | The architecture and documentation reviews that became the UE4SS move and the repository cleanup | b122 · b143 |
| **SentientYeet** | review | The substrate critique that re-opened the loader decision | b143 |
| **Violet** (Discord) | report | ~9 FPS for a friend joining on Linux — five separate defects behind it | b134 |
| **decodinatorX** ([#5](https://github.com/VOTV-MP/Multivoid/issues/5)) | report | Couldn't type `sv.request` at the SAT console — `T` kept opening chat | b133 |
| **gediao** (Discord) | report | The b125 host-log pack, with huoyan1231 | b134 |
| **SirWilliam** (Discord) | report | Rejoining a session requires fully relaunching the game | fixed, unreleased |

Prior art this project learned from, with thanks:

| Project | What it gave Multivoid |
|--|--|
| [MTA:SA](https://github.com/multitheftauto/mtasa-blue) (GPLv3, vendored read-only) | The architectural precedent: the parallel class hierarchy, per-element syncers, the keysync shape, host-authoritative AI. Multivoid follows MTA's shapes deliberately; no MTA code is copied. |
| [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) (MIT) | The UE4 modding substrate this project stands on: its reflection algorithms are ported with attribution in the source, it is the everyday development tool, and it is the loader the shipping mod runs under. The mod imports nothing from it. |
| [MinHook](https://github.com/TsudaKageyu/minhook) (MIT) | The x64 trampoline hooking engine (vendored). |
| **VoidTogether** | The first multiplayer attempt for VOTV, useful to read while designing this one. No VoidTogether code is in Multivoid; two ideas came from studying it and are cited in the source where they are used (the nickname sanitizer, the nameplate styling comparison). |
| [Dear ImGui](https://github.com/ocornut/imgui) (MIT), [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets) (BSD), [Opus](https://opus-codec.org/) (BSD), [FreeType](https://freetype.org/) (FTL), [miniaudio](https://miniaud.io/) (MIT/public domain) | Vendored libraries: UI, transport, voice, text rendering, audio. |

## Legal

This is a **hook-only mod**: its code contains **no Voices of the Void code or assets**. You must
own a legitimate copy of the game to use it. The repository is licensed under the
**MIT License** (see `LICENSE`); the full license texts of every statically linked or embedded
component ship in [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md), inside the release zip as
well. The optional starter-skin pak bundled with releases (`scientists.pak`) is a community
conversion of third-party game assets and is **not** covered by the MIT license. Unaffiliated with
the VOTV authors.

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
