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
  - The folder path and the "delete the old multivoid-*.dll" rule are anchor
    phrases shared with the release-body template (tools/release/ledger_lib.ps1);
    the lint asserts they appear here verbatim. Reword them only together.
-->

# Installing Multivoid

Multivoid is a co-op mod for **Voices of the Void**. It does not modify any game
files — it is two DLLs you drop next to the game's executable and delete to
uninstall.

## What you need

- **Voices of the Void 0.9.0n** (the game version a mod build targets is part of
  its filename: `multivoid-<game>-<build>.dll`).
- The two files from the newest entry on the
  [Releases page](https://github.com/VOTV-MP/Multivoid/releases):
  `multivoid-<game>-<build>.dll` (the mod) and `xinput1_3.dll` (the loader).

## Fresh install

1. Download **both** files from the release page.
2. Put them into your game install at
   `WindowsNoEditor\VotV\Binaries\Win64`
   (the folder that contains `VotV-Win64-Shipping.exe`).
3. Start the game. The main menu now has a **Multiplayer** entry showing the
   Multivoid version — that is how you know the mod loaded.

## Updating

1. Download the new `multivoid-<game>-<build>.dll` from the release page.
2. Put it in the same folder and **delete the old `multivoid-*.dll`** — if
   several are present, the game shows a "MOD INSTALL PROBLEM" notice until only
   one is left.
3. `xinput1_3.dll` almost never changes; overwriting it with the release's copy
   is always safe. If a release requires a new loader, its notes say so.

## Playing

- **Host**: Multiplayer menu → host. Your world and save are the session's
  single source of truth.
- **Join**: pick a lobby in the server browser, or connect directly by IP.
- Host and joiners must run the **same build** — the game refuses a mismatched
  join with a popup telling you which side is older.

## Uninstall

Delete the two DLLs from `WindowsNoEditor\VotV\Binaries\Win64`. The game boots
completely stock without them.

For a full clean sweep, also delete the mod's runtime files next to the
executable (all optional — they only store mod settings and logs):
`multivoid.ini`, `multivoid.ini.example`, `multivoid.log`, `multivoid.prev.log`,
`multivoid-loaded.txt`, `multivoid-compat-report.txt`, `multivoid-players.txt`,
`multivoid-banlist.txt`, and the skin-pak folder
`VotV\Content\Paks\LogicMods\multivoid` if you created one.

## Troubleshooting

- **"MOD INSTALL PROBLEM" notice in game** — more than one `multivoid-*.dll` is
  in the folder. Keep the newest, delete the rest.
- **Version-mismatch popup when joining** — host and client run different
  builds. Whoever is older updates (see [Updating](#updating)).
- **No Multiplayer entry in the main menu** — the files are in the wrong
  folder. Re-check step 2 of the fresh install: the DLLs must sit next to
  `VotV-Win64-Shipping.exe`, not in the game's root folder.
- Still stuck? Ask in the [Discord](https://discord.gg/bA6tGBvGMN).
