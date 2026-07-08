# probes — UE4SS Lua development experiments

Short-lived UE4SS Lua mods used to derisk a question before committing
to a C++ implementation. Each probe is an **experiment**, not
production: once it has taught us what we need, the discovered offsets
/ UFunction names / behaviour are baked into `src/votv-coop/` and the
probe stays here only as historical reference.

Lua (not C++) on purpose: probes need fast iteration with no build
step. The shipping mod is C++ (`ue_wrap/` + `coop/` etc.) and never
loads UE4SS at runtime (RULE №3).

**Probes run in the DEV copy only** (`Game_0.9.0n_CLIENT_3/`). The HOST
and CLIENT copies have UE4SS disabled — see `docs/RE_WORKFLOW.md` for
the three-copies convention.

## Deploy

```powershell
./tools/deploy-probe.ps1 -Name <ProbeName>
```

Copies the probe into the dev copy's UE4SS `Mods/` and enables it in
`mods.txt`. The dev copy is gitignored, so the canonical source under
`tools/probes/` is the source of truth.

## Probes

- **coopTestHarness** — multi-purpose UE4SS Lua harness, kept around
  as a richer-than-C++ Live View / scenario sandbox for the dev copy.
  Two scripts:
  - **`Scripts/main.lua`** — the original autonomous scenario driver
    (skip-to-gameplay, screenshot, orphan spawn / soak, report).
    Mostly superseded by the C++ `harness::autotest` path in
    `src/votv-coop/src/harness/`. Kept for keybind-driven manual
    inspection: `CTRL+8` skip-to-gameplay, `CTRL+P` spawn orphan,
    `CTRL+9` screenshot, `CTRL+7` report.
  - **`Scripts/probe_terminals.lua`** — Phase 5T terminal probe
    (2026-05-25). Hookless v3: polls state at 250 ms; auto-fires
    a safe test suite when `mainPlayer.activeInterface` is populated.
    Tests: ProbeWidgetScoping, ProbeRootConsole, ProbeModuleEnum,
    ProbeBndEvtSurface, ProbeDelegateSubscribers, ProbeDriveData,
    ProbeDriveSlots, ProbePanelRadarWidget, ProbeMouseWheel,
    ProbeScreenButtonInvoke, ProbeGatherSignal, ProbeSpaceRendererIntegrator,
    ProbeSetDataAudio, FinalSummary. Driven by
    `tools/probe-terminals.ps1`. Findings folded into
    `research/findings/votv-interactable-terminals-RE-2026-05-25.md`.

## Why we don't ship probes

- Probes depend on UE4SS at runtime.
- Probes write to UE4SS's `UE4SS.log` (separate from our
  `votv-coop.log`).
- Probes are debug-grade — no robustness against missing classes /
  half-loaded state.

When a probe-discovered behaviour is needed in production, it gets
re-implemented in `src/votv-coop/` (C++, standalone) and the probe
either retires (RULE 2 — no parallel old + new paths if the C++ path
fully replaces it) or stays as dev-copy-only inspection scaffolding.
