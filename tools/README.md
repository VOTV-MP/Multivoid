# tools — PowerShell + Python helpers

Build, launch, deploy, and test helpers for the VOTV coop mod. All
regenerable; nothing here is load-bearing game state.

## User-facing launchers (project-root .bat scripts)

These live at the **project root**, not in `tools/`, because the user
runs them frequently:

- **`mp_host_game.bat [port] [nick]`** — deploy + launch as coop HOST
  (port default 47621, nick "Host"). Thin shim over `tools/mp.py host`
  (which deploys via `deploy-all.ps1`, sets the per-launch env signals,
  launches the shipping exe windowed from `Game_0.9.0n_HOST/`).
- **`mp_client_connect.bat [peer-ip] [port] [nick]`** — same shape for
  CLIENT, launching out of `Game_0.9.0n_CLIENT_1/` (the sibling game folder
  for same-box testing — see `docs/RE_WORKFLOW.md`).
- **`stop-coop.bat`** — removes the Multivoid mod folder from the host
  copy (`deploy-mod.ps1 -Remove`); the UE4SS substrate stays.
- **`play-coop.bat`** — legacy single-process play launcher (kept for
  backward compatibility; `mp_host_game.bat` is the canonical entry).
- **`shot.bat`** — quick wrapper around `tools/capture-window.ps1`
  (external screenshot of the VOTV window; in-process F12 captures
  black on the 3D swapchain).

## Deploy scripts

- **`deploy-mod.ps1 -GameWin64 <path> [-Remove]`** — idempotent deploy
  of the built DLL into the UE4SS mod folder
  (`Mods\Multivoid\dlls\main.dll` + `enabled.txt`), SHA-skip when
  byte-identical (a re-run while VOTV is loaded doesn't fail on the
  locked DLL), plus one-time removal of the retired xinput-proxy files
  beside the exe. `-Remove` deletes the mod folder; the UE4SS substrate
  stays (`install-ue4ss.ps1` owns it).
- **`deploy-all.ps1`** — multi-target deploy (HOST + CLIENT + CLIENT2 +
  DEV in one). Run after `cmake --build`.
- **`deploy-probe.ps1 -Name <ProbeName>`** — copy a UE4SS Lua probe
  into the dev copy's `Mods/` dir + enable it in `mods.txt`. Source of
  truth lives under `tools/probes/`. Only meaningful for the dev copy.

## Test runners

- **`run-test.ps1 -Scenario <name>`** — single-process autonomous
  scenario runner. Writes `scenario.txt`, launches the shipping exe.
  Scenarios: `play`, `load:<slot>`, `none`,
  `probe_terminals:<slot>`. See `docs/AUTONOMOUS_TESTING.md`.
- **`lan-test.ps1`** — TWO-process LAN test (host + client in the dev
  copy), per-PID log capture in `tools/test-runs/`. Found multiple
  real handshake bugs single-process loopback hid. Flags: `-GrabTest`,
  `-NameplateTest`, etc.
- **`probe-terminals.ps1`** — one-shot Phase 5T terminal probe
  launcher. Parks the Multivoid mod (renames `enabled.txt` →
  `.probe-disabled`), deploys the UE4SS probe, sets the scenario,
  launches the dev copy. `-Restore` unparks the mod after.

## Probes + RE helpers

- **`probes/`** — UE4SS Lua experiments (dev copy only). See
  `tools/probes/README.md`.
- **`install-ue4ss.ps1 [-Win64Dir <path>] [-Quiet] [-Force]`** — one-time
  install of the pinned UE4SS release (v3.0.1) into a game copy — since
  WP-2 UE4SS is the LOADER on every copy. Never overwrites existing
  `Mods/mods.txt` / `UE4SS-settings.ini` state; `-Quiet` = play profile
  (GUI console off). The committed source of truth for substrate setup.
- **`sdk_diff.py <old.txt> <new.txt>`** — compare two
  `multivoid-compat-report.txt` outputs (the boot health-check writes
  one per launch); flags offset drift across recooks.

## Other

- **`brightness.ps1`** — quick OS-side brightness toggle while
  iterating on the in-game post-process pipeline.
- **`capture-window.ps1`** — external Win32 PrintWindow grab of the
  VOTV window (in-process F12 / GDI captures black from the 3D
  swapchain).
## Retired / removed

- **`inject.ps1`** — legacy DLL-injection script from the pre-
  standalone-proxy era, and the standalone `xinput1_3.dll` proxy it
  yielded to — both deleted at UE4SS_ARC WP-2 commit 3. UE4SS loads
  the mod as `Mods\Multivoid\dlls\main.dll` (deploy: `deploy-mod.ps1`).
- **`Game/coop-host.bat` / `Game/coop-client.bat`** — early prototype
  launchers that lived inside the game folder. Superseded by
  `mp_host_game.bat` / `mp_client_connect.bat` at the project root
  (RULE 2 — no parallel launchers).
