<!--
  MAINTENANCE (single-owner rule, 2026-07-26):
  This file is the ONE owner of install/update/uninstall instructions. Every other
  surface (README quickstart, release-page Install blocks, the website Download
  section) links here and must NOT restate steps beyond one sentence.
  - NO per-build data: no build numbers, no hashes, no release-specific facts.
    Those live on the release page. CI enforces this (ledger_lint INSTALL checks:
    a literal multivoid-<target>-<digits>.dll or a 40/64-hex string fails the lint).
  - The ONE living version literal is the game target below; it changes only on a
    game retarget (see docs/VERSION_MIGRATION.md, "version identity" step).
  - The manual-lane mod-folder path and the upgrade-from-standalone rule are
    anchor phrases shared with the release-body template
    (tools/release/ledger_lib.ps1); the lint asserts they appear here verbatim.
    Reword them only together.
  - The dev-build/tester DISCLAIMER below is owned HERE too (user-approved
    2026-07-27). README and the website Download section carry a one-line
    pointer, never a copy. It deliberately names no build number or hash, so it
    stays lint-clean if a release body ever quotes it.
-->

# Installing Multivoid

> **DISCLAIMER — there is no stable release yet.** Every build is a dev build,
> and everyone playing one is a tester.
>
> Expect bugs. The mod has cleared a lot of milestones and already does a great
> deal, but it is far from finished and some game systems are untouched
> entirely.
>
> If something breaks, a report genuinely helps. Say what you were doing, and
> attach `multivoid.log` from the folder that contains the game's executable.
> Testers who send good reports get credited, permanently.

Multivoid is a co-op mod for **Voices of the Void**. It does not modify any
game files. It ships as **one zip** that works for both install methods below;
the mod runs as a [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) mod folder and
is deleted to uninstall.

## What you need

- **Voices of the Void 0.9.0n** (each mod build targets one game version; the
  release page and the in-game Multiplayer entry both show the pair).
- The one `Multivoid-Multivoid-<version>.zip` from the newest entry on the
  [Releases page](https://github.com/VOTV-MP/Multivoid/releases) — or the
  Thunderstore listing, once published.

## Install with a mod manager (recommended)

1. Install [r2modman](https://thunderstore.io/c/voices-of-the-void/) (or the
   Thunderstore Mod Manager) and select Voices of the Void.
2. Install Multivoid from the Thunderstore listing — or, for a zip downloaded
   from the Releases page: Settings → Profile → **Import local mod** and pick
   the zip.
3. Launch the game **through the manager**. The manager installs the loader
   (unreal-shimloader + UE4SS) by itself; nothing else to set up.

## Install manually (no mod manager)

1. Install **UE4SS v3.0.1** into the game: drop the contents of its zDEV
   archive next to `VotV-Win64-Shipping.exe` (the game folder
   `WindowsNoEditor\VotV\Binaries\Win64`). UE4SS's own install guide covers
   this step.
2. Unzip the Multivoid zip. Copy the **contents** of its `mod\` folder into
   `WindowsNoEditor\VotV\Binaries\Win64\Mods\Multivoid`
   inside your game install — so that the file
   `...\Mods\Multivoid\dlls\main.dll` exists.
3. If the zip has a `pak\` folder (the player-model skins), copy its **contents**
   into `WindowsNoEditor\VotV\Content\Paks\LogicMods\multivoid`
   (create the folder if it does not exist). Without this step other players'
   scientist models fall back to the stock body on your screen.
4. Start the game. The main menu now has a **Multiplayer** entry showing the
   Multivoid version — that is how you know the mod loaded.

## Upgrading from an old (pre-mod-folder) install

Older Multivoid builds were two DLLs sitting next to the game's executable.
The new build **refuses to start** beside them and shows a removal dialog, so
finish the upgrade first: delete the old `multivoid-*.dll` and `xinput1_3.dll`
from the folder that contains `VotV-Win64-Shipping.exe`, then install normally.

## Updating

- Mod manager: update (or re-import) the new zip; the manager replaces the old
  version.
- Manual: overwrite the `Mods\Multivoid` folder with the new zip's `mod\`
  contents.
- Host and joiners must run the **same build** — the game refuses a mismatched
  join with a popup telling you which side is older.

## Playing

- **Host**: Multiplayer menu → host. Your world and save are the session's
  single source of truth.
- **Join**: pick a lobby in the server browser, or connect directly by IP.

## Uninstall

- Mod manager: uninstall Multivoid in the manager.
- Manual: delete the `Mods\Multivoid` folder. The game boots completely stock
  without it (UE4SS may remain; it does nothing on its own).

For a full clean sweep, also delete the mod's runtime files next to the
executable (all optional — they only store mod settings and logs):
`multivoid.ini`, `multivoid.ini.example`, `multivoid.log`, `multivoid.prev.log`,
`multivoid-loaded.txt`, `multivoid-compat-report.txt`, `multivoid-players.txt`,
`multivoid-banlist.txt`, and the skin-pak folder
`VotV\Content\Paks\LogicMods\multivoid` if you created one.

## Troubleshooting

- **"Multivoid — old install found" dialog** — leftovers of a pre-mod-folder
  install are still next to the game's executable. Follow
  [Upgrading](#upgrading-from-an-old-pre-mod-folder-install).
- **"Multivoid is installed twice" dialog** — two copies of the mod folder are
  installed (for example one via the manager and one by hand). Remove one,
  restart the game.
- **Version-mismatch popup when joining** — host and client run different
  builds. Whoever is older updates (see [Updating](#updating)).
- **No Multiplayer entry in the main menu** — the mod did not load. Manager
  lane: make sure you launched through the manager. Manual lane: check that
  `...\Mods\Multivoid\dlls\main.dll` exists and that UE4SS is installed next to
  `VotV-Win64-Shipping.exe`.
- **Modded game refuses to start under the manager** — a leftover
  `xinput1_3.dll` next to the game's executable makes the manager's loader
  abort. Delete it (see [Upgrading](#upgrading-from-an-old-pre-mod-folder-install)).
- Still stuck? Ask in the [Discord](https://discord.gg/bA6tGBvGMN).
