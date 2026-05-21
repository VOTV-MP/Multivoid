# probes — throwaway UE4SS experiments

Short-lived UE4SS Lua mods used to derisk a question before committing to a
C++ implementation. Each probe is an **experiment**, not production: once it
has taught us what we need, it is superseded by code in `src/votv-coop/`
and deleted (RULE No.2 — no migration baggage / no parallel old+new paths).

Lua (not C++) on purpose: probes need fast iteration with no build step.
The shipping mod is C++ (`ue_wrap/` + `coop/`).

## Deploy

```powershell
./tools/deploy-probe.ps1 -Name <ProbeName>
```

Copies the probe into the game's UE4SS `Mods/` and enables it in
`mods.txt`. The game dir is gitignored, so the copy here under
`tools/probes/` is the source of truth.

## Probes

- **coopTestHarness** — the autonomous test driver (launch + skip-to-
  gameplay + screenshot + report), and the Phase 2.1 orphan spawn.
  Scenarios (via `run-test.ps1`): `newgame`, `load:<slot>`, `orphan`
  (enter gameplay then spawn a 2nd `mainPlayer_C` and soak it), `inspect`,
  `none`. Keybinds: `CTRL+8` new game, `CTRL+P` spawn orphan, `CTRL+9`
  screenshot, `CTRL+7` report.
  (The orphan spawn was consolidated here from the former standalone
  coopSpawnProbe — RULE No.2, one implementation.)
