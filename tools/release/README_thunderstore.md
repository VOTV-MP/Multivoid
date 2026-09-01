# Multivoid

> **Multiplayer for Voices of the Void.**
> A mod that adds drop-in co-op to a single-player game —
> **no original game files are modified**.

## Play together

1. **Install with the mod manager.** It sets up the loader for you; there is nothing
   else to configure.
2. **Launch the game.** A **Multiplayer** button appears in the main menu.
3. **One of you hosts** — pick a save, choose who may join, press Host.
4. **The others join** — pick the lobby in the browser, or type the host's address
   into Direct Connect. If the host set a password, type it in the box beside it.

That is the whole setup. Everyone needs their own copy of Voices of the Void on the
version this build targets (**0.9.0n**), and the same Multivoid build — the server
browser marks mismatched lobbies before you click.

| | |
|--|--|
| **Game target** | Voices of the Void Alpha **0.9.0n** |
| **Players** | up to **4** (host + 3) — LAN + Internet, no port forwarding needed |
| **Status** | Alpha — dev builds; everyone playing one is a tester |
| **Website** | [multivoid.dev](https://multivoid.dev) |
| **Community** | [Discord](https://discord.gg/bA6tGBvGMN) — questions, bug reports, people to play with |
| **Source** | [github.com/VOTV-MP/Multivoid](https://github.com/VOTV-MP/Multivoid) (MIT) |

## What you get

- **Co-op over LAN or Internet** — one host, up to three friends; join by direct IP or
  through the built-in server browser backed by the official master server.
- **Real remote players** — full body and animations, per-player skins, nameplates with
  live ping, chat, and 3D positional **voice chat**.
- **A shared world** — physics props, trash collection, NPCs and kerfurs, world events,
  weather, doors, lights, terminals, sleep, damage. The host's world is the world.
- **The signal pipeline, end to end** — dishes, pings, downloads, decoding, playback,
  drives, the laptop, the signal database.
- **Join at any time** — a connecting client receives the host's full world state;
  joining mid-event, mid-download, mid-anything is a supported case by design.
- **Starter skins** — four bundled scientist player models to pick from.

## Installing without the mod manager

The steps above are the managed lane. Manual install, updating and troubleshooting:
[install guide](https://github.com/VOTV-MP/Multivoid/blob/main/docs/INSTALL.md).

## Versions and joining

A build's identity is the pair **game version + build number** — shown in the main
menu and on every lobby row. **Joining needs the exact same pair on both sides**;
the server browser marks mismatched lobbies before you click. Updates are never
forced: old versions keep playing together on their own builds.

## Bugs

There is no stable release yet — expect rough edges. Report what you hit on
[Discord](https://discord.gg/bA6tGBvGMN) or in
[GitHub issues](https://github.com/VOTV-MP/Multivoid/issues); reports that change
the mod get credited in the project's public credit ledger.

## Legal

Hook-only mod: it contains **no VOTV code or assets** and never modifies the game's
own files. The source is MIT-licensed; the optional starter-skin pak
(`scientists.pak`) is a community conversion of third-party assets and is **not**
covered by the MIT license. Unaffiliated with the VOTV authors.

<sub>Multivoid is alpha software. Back up your saves before testing.</sub>
