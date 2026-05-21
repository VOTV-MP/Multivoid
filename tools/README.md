# tools — PowerShell / Python helpers

Build, launch, and test helpers for the VOTV coop mod. All regenerable;
nothing here is load-bearing game state.

## Available

- `install-ue4ss.ps1` — download a pinned UE4SS release (v3.0.1) and
  install it next to `VotV-Win64-Shipping.exe` (additive; no game files
  modified). Re-run with `-Force` to reinstall. The game dir is gitignored,
  so this script is the committed source of truth for the substrate setup.

## Planned (filled in as phases land)

- `run-test.ps1` — single-process autonomous test (methodology 5.1).
- `run-coop-test.ps1` — dual-process LAN test (methodology 5.2).
- UE4SS install / mod-deploy helper — copy the built mod DLL into the
  game install's UE4SS `Mods/` directory.
- Launch scripts that live in the game install (`Game/coop-host.bat`,
  `Game/coop-client.bat`) are **force-added** past `.gitignore`
  (`git add -f`) because they `cd` to the install location to launch.

See `docs/AUTONOMOUS_TESTING.md` for harness design.
