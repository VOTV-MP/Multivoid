# tools — PowerShell + Python helpers

Build, launch, deploy, and test helpers for the VOTV coop mod. All
regenerable; nothing here is load-bearing game state.

## User-facing launchers (project-root .bat scripts)

These live at the **project root**, not in `tools/`, because the user
runs them frequently:

- **`mp_host_game.bat [port] [nick]`** — deploy + launch as coop HOST
  (port default 47621, nick "Host"). Calls `tools/deploy-loader.ps1`,
  writes `scenario.txt = "play"`, sets `VOTVCOOP_NET_ROLE=host` env
  var, launches the shipping exe windowed (`Game_0.9.0n/`).
- **`mp_client_connect.bat [peer-ip] [port] [nick]`** — same shape for
  CLIENT, launching out of `Game_0.9.0n_copy/` (the sibling game folder
  for same-box testing — see `docs/RE_WORKFLOW.md`).
- **`stop-coop.bat`** — restores UE4SS in the host folder if it was
  disabled by a prior `deploy-loader.ps1 -Standalone` run.
- **`play-coop.bat`** — legacy single-process play launcher (kept for
  backward compatibility; `mp_host_game.bat` is the canonical entry).
- **`shot.bat`** — quick wrapper around `tools/capture-window.ps1`
  (external screenshot of the VOTV window; in-process F12 captures
  black on the 3D swapchain).

## Deploy scripts

- **`deploy-loader.ps1 -GameWin64 <path> [-Standalone]`** — idempotent
  copy of `xinput1_3.dll` + `votv-coop.dll` into the target Win64 dir.
  Skip-if-identical so a re-run while VOTV is loaded doesn't fail on
  the locked DLL. `-Standalone` renames `dwmapi.dll` → `dwmapi.dll.off`
  to disable UE4SS in that folder (the host + client copies are
  always run standalone; the dev copy keeps UE4SS active).
- **`deploy-all.ps1`** — multi-target deploy (HOST + CLIENT + DEV all
  in one). Run after `cmake --build`.
- **`deploy-probe.ps1 -Name <ProbeName>`** — copy a UE4SS Lua probe
  into the dev copy's `Mods/` dir + enable it in `mods.txt`. Source of
  truth lives under `tools/probes/`. Only meaningful for the dev copy.

## Test runners

- **`run-test.ps1 -Scenario <name>`** — single-process autonomous
  scenario runner. Writes `scenario.txt`, launches the shipping exe.
  Scenarios: `play`, `netloopback`, `load:<slot>`, `none`,
  `probe_terminals:<slot>`. See `docs/AUTONOMOUS_TESTING.md`.
- **`lan-test.ps1`** — TWO-process LAN test (host + client in the dev
  copy), per-PID log capture in `tools/test-runs/`. Found multiple
  real handshake bugs single-process loopback hid. Flags: `-GrabTest`,
  `-NameplateTest`, etc.
- **`probe-terminals.ps1`** — one-shot Phase 5T terminal probe
  launcher. Disables proxy, deploys UE4SS probe, sets scenario,
  launches dev copy. `-Restore` re-enables proxy after.

## Probes + RE helpers

- **`probes/`** — UE4SS Lua experiments (dev copy only). See
  `tools/probes/README.md`.
- **`install-ue4ss.ps1 [-Force]`** — install a pinned UE4SS release
  (v3.0.1) into the dev copy. The dev copy is the only one with UE4SS
  active. The script is the committed source of truth for the
  dev-substrate setup.
- **`sdk_diff.py <old.txt> <new.txt>`** — compare two
  `votv-coop-compat-report.txt` outputs (the boot health-check writes
  one per launch); flags offset drift across recooks.

## Other

- **`brightness.ps1`** — quick OS-side brightness toggle while
  iterating on the in-game post-process pipeline.
- **`capture-window.ps1`** — external Win32 PrintWindow grab of the
  VOTV window (in-process F12 / GDI captures black from the 3D
  swapchain).
- **`inject.ps1`** — legacy DLL-injection script from the pre-
  standalone-proxy era. Retained for the rare case of testing a debug
  build with manual injection. The standalone proxy
  (`xinput1_3.dll`) is the production load path.

## Retired / removed

- **`Game/coop-host.bat` / `Game/coop-client.bat`** — early prototype
  launchers that lived inside the game folder. Superseded by
  `mp_host_game.bat` / `mp_client_connect.bat` at the project root
  (RULE 2 — no parallel launchers).
