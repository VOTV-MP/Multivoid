# ROADMAP — VOTV coop mod

**Living document.** Re-arrange as priorities shift. Phases follow
`docs/COOP_METHODOLOGY.md`, adapted for UE4.27 + UE4SS. Each phase has a
hard gate — don't proceed until met.

Legend: ☐ not started · ◐ in progress · ☑ done.

---

## Phase 0 — Feasibility + bootstrap
**Goal**: prove viability; lay down repo shape.
**Gate**: `docs/FEASIBILITY.md` answers 0.2–0.8; skeleton committed.

- ☑ Read methodology; confirm UE4.27 + approach (UE4SS + reflection).
- ☑ `docs/FEASIBILITY.md` written (no blockers found).
- ☑ Directory layout + skeleton docs + `.gitignore` + `CLAUDE.md`.
- ☑ Vendor references: `reference/RE-UE4SS/` (7f7cc36),
       `reference/mtasa-blue/` (c07ccb0).
- ☑ First commit (skeleton + submodule pins) — `43a6a11`.

**Phase 0 gate: MET.** Proceed to Phase 1 (engine archaeology via
reflection). First concrete derisk: Phase 2.1 spawn-orphan experiment can
be pulled forward once UE4SS is installed against the game.

- ☑ UE4SS v3.0.1 installed into the game (`tools/install-ue4ss.ps1`;
       additive, principle 1 intact). IDA MCP holds the shipping binary as
       the RE fallback. See `research/findings/coop-phase-0-ue4ss-bootstrap-2026-05-21.md`.
- ☐ **(needs user)** One interactive launch to verify UE4SS injects (4.27
       detected) and capture reflection dumps: `CTRL+H` (C++ headers) +
       `CTRL+J` (object dump) at main menu, then again in-world.

## Phase 1 — Engine archaeology (mostly reflection, not RE)
**Goal**: a research finding for each engine entry point we'll hook.
**Gate**: all of the below documented in `research/findings/`.

- ☑ 1.1 Entity factory — `UWorld::SpawnActor`; 2nd player via
       `UGameplayStatics::CreatePlayer` (engine-native). See finding
       `coop-phase-1-player-class-and-spawn-2026-05-21.md`.
- ☑ 1.2 Player class layout — pawn `AmainPlayer_C : ACharacter` (camera,
       grab/hold, stats, inventory mapped); controller = stock
       `APlayerController`; GameMode `AmainGamemode_C` (world/save hub).
- ☑ 1.3 Input dispatch path — stock `APlayerController`; VOTV input = pawn
       `InpActEvt_*` BP events. Full vocabulary mapped (movement, jump,
       crouch, run, fire, drop, inventory, hotbar 1-10, flashlight,
       equipment, ...) → 4.1 keysync. See finding
       `coop-phase-1-input-map-and-spawn-probe-2026-05-21.md`.
- ☐ 1.4 Sim tick — UE `UWorld::Tick` / actor tick groups VOTV relies on.
- ☐ 1.5 Render tick — confirm decoupling (UE renders on its own thread).
- ☐ 1.6 Level-load entry + completion — `UGameplayStatics::OpenLevel` /
       `LoadStreamLevel` + post-load callback used by VOTV.
- ◐ 1.7 Save / load entry — `UsaveSlot_C::save(...)` / `savePlayerOnly()`,
       `Usave_main_C::save()`; world-apply on GameMode (`Load Primitives`,
       `loadObjects`, `loadTriggers`). Load *entry* call-graph pending
       (Live View / IDA, Phase 4.5).
- ☐ 1.8 Screen / UI system — VOTV's UMG/HUD stack; how menus push/pop.
- ☐ 1.9 Script VM — Blueprint VM; map key gameplay `UFunction`s (AI,
       interaction, day cycle, signals). Blueprint-vs-C++ split.

## Phase 2 — Foundation infrastructure
**Gate**: spawn a 2nd `APawn` + `APlayerController` (the "orphan") and
have it tick without crashing for ≥60 s (target several minutes).

- ☑ 2.1 Spawn the orphan — DONE. `SpawnActor<mainPlayer_C>` (unpossessed)
       confirmed live: orphan spawned, no crash, survived ~65s idle soak
       (gate met for idle case). Engine-native, autonomous. See finding
       `coop-phase-2-1-orphan-spawn-result-2026-05-22.md`.
- ☐ 2.2 Crash-by-crash root-cause fixes (per-site, no broad filters).
- ☐ 2.3 Registry / state mirror for any per-player subsystem VOTV/UE
       expects (subsystems registered per `APlayerController`).
- ◐ 2.4 Sustained-soak validation — 4-min idle soak PASSED (orphan alive,
       no crash, no singleton clobber: count 1->2 clean, local pawn
       unchanged). Remaining: driven/possessed orphan + movement under load.

### Standalone mod foundation (RULE №3) — the shipping vehicle
- ☑ C++ toolchain + build: `votv-coop.dll` (CMake + VS2019, x64, static CRT).
- ☑ Standalone load proven: DLL injected + ran in VOTV with UE4SS absent
      (marker written; `UE4SS.dll` not loaded). See finding
      `standalone-bootstrap-loads-2026-05-22.md`.
- ☑ Shipping loader = proxy DLL. **`xinput1_3.dll`** — VOTV imports only
      `XInputGetState`/`XInputSetState` (verified by name, IDA), forwarded to
      System32 `xinput1_4.dll` via `/export:` linker directives; the proxy's
      `DllMain` loads `votv-coop.dll` off the loader lock. Auto-loads on game
      start with NO injection and NO UE4SS — proven live (marker written,
      both DLLs in modules, `UE4SS.dll` absent, game renders, responding).
      `tools/deploy-loader.ps1` installs it; replaces `tools/inject.ps1`. See
      `research/findings/standalone-proxy-loader-2026-05-22.md`.
- ◐ `ue_wrap` reflection: resolve `GUObjectArray`/`GNames`/`ProcessEvent`
      via AOB sigs — our own SDK access, no UE4SS.
      - ☑ `GUObjectArray` + `FName::ToString` AOB-resolved and VALIDATED live:
            standalone walk of 17770 UObjects with correct names; resolved
            addresses match the UE4SS log exactly; FUObjectArray layout
            confirmed by the raw header dump. `ue_wrap/sig_scan` +
            `ue_wrap/reflection`. See
            `research/findings/ue-wrap-reflection-2026-05-22.md`.
      - ☑ Object/class/function lookup over GUObjectArray
            (`FindObject`/`FindClass`/`FindFunction`/`OuterOf`). Validated live:
            found `Actor`, `World`, a live `World` instance, and
            `K2_SetActorLocation`/`K2_DestroyActor` UFunctions. (`mainPlayer_C`
            not found at the menu is expected — BP gameplay classes load with
            the map.)
      - ☑ Version-portability infra: all version-specific knowledge (AOBs,
            offsets, content names) isolated in `ue_wrap/sdk_profile.h` (the
            porting surface); `ue_wrap/log` -> `votv-coop.log`; boot
            `RunHealthCheck` detects the game/engine version, resolves +
            FUNCTIONALLY validates every primitive, warns on exe-fingerprint
            mismatch, prints PASS/FAIL. Live: HEALTH PASS on 0.9.0-n. Strategy:
            `docs/VERSION_PORTABILITY.md`.
      - ☑ `ProcessEvent` (UFunction call path) AOB-resolved at rva 0x1465930
            (vtable index 68; found via runtime vtable dump + decompile
            confirm). Wired as `reflection::CallFunction(obj, func, params)`.
            Resolved live to the exact RVA. RULE No.3 "GUObjectArray/GNames/
            ProcessEvent via AOB" milestone COMPLETE: read the object graph +
            call any UFunction, no UE4SS. (Live call validated with the spawn
            port — needs a game-thread context.)
- ☑ Game-thread execution context (so `CallFunction`/ProcessEvent runs safely
      — it must NOT run on the boot thread). We hook `UObject::ProcessEvent`
      (already AOB-resolved) and use its detour as a guaranteed game-thread
      callback: a reentrancy-guarded task pump drains posted tasks, then
      forwards via the MinHook trampoline. MinHook vendored as a submodule
      (`third_party/minhook`, WP13) behind `ue_wrap/hook`. Proven live,
      standalone (no UE4SS): a posted self-test task ran on the game thread
      (tid != boot tid) and read engine state. See
      `research/findings/game-thread-context-2026-05-22.md`.
- ☐ Port the autonomous test framework into the standalone mod (RULE No.3;
      retires the UE4SS Lua `coopTestHarness`, RULE No.2). Driven from the
      game-thread context via `CallFunction`:
      - ☐ skip-to-gameplay (new game) — `ExecuteConsoleCommand("open untitled_1")`
            so `mainPlayer_C` loads (FString-only params; no FName ctor needed).
      - ☐ screenshot — `ExecuteConsoleCommand("HighResShot ...")`.
      - ☐ report — local pawn / mainPlayer_C count / gamemode.origPawn to the log.
      - ☐ scenario timeline (read scenario.txt or boot-delayed sequence).
- ☐ Port the validated orphan spawn into C++ behind `coop::RemotePlayer`.

## Phase 3 — Networking transport
**Gate**: both players see each other's pawn moving in real time on LAN
(position only; T-pose slide OK).

- ☐ 3.1 UDP, host-authoritative, LAN-first.
- ☐ 3.2 Sessions, not connections.
- ☐ 3.3 Pure I/O at bottom; 3-layer split.
- ☐ 3.4 Position-only pose sync (~13–30 Hz) + interpolation.
- ☐ 3.5 Auto-spawn the remote on first packet.

### Multiplayer menu (in VOTV's main menu) — in scope
- ☐ Native UMG menu built at runtime by our C++ mod: Host (choose save /
      New Game, load, listen), Connect (enter IP, join), server browser
      (future). No VOTV asset edited. Design: `docs/MULTIPLAYER_UI.md`.
      Build gated on the Phase 3 session API (the shell drives that API).

## Phase 4 — Replication layers (the bulk)
- ☐ 4.1 Input replication (MTA keysync pattern → engine drives the pawn).
- ☐ 4.2 Equipment / held-item / tool state.
- ☐ 4.3 Entity manifest + per-entity state (NPCs, interactables, signals).
- ☐ 4.4 Cutscenes / scripted events (Blueprint event RPC + skip semantics).
- ☐ 4.5 Save / world-state sync (host-authoritative).
- ☐ 4.6 Inventory / progression (shared, host-side; client UI overlays).

## Phase 5 — Validation
- ◐ 5.1 Autonomous test harness — launch + skip-menu-to-gameplay +
       screenshot + report built early for dev ease (`tools/run-test.ps1`
       + `coopTestHarness`). Synthetic-input scripting + JSON report still
       to add. See `docs/AUTONOMOUS_TESTING.md`. (Skip-chain validates on
       first run.)
- ☐ 5.2 LAN soak (dual-process).
- ☐ 5.3 Live testing (side-by-side windows; observation log).
- ☐ 5.4 Multi-agent audit (WP2; MTA-fidelity audit mandatory for network
       / replication / class-design work).

---

## Timeline note

Per the methodology's final note: coop is a 6-month-to-2-year effort.
UE4SS + reflection should compress Phase 1 (no blind RE) and de-risk
Phase 2 (engine natively supports multiple pawns) versus the GTA:SA
baseline — but Phase 4 (replication) remains the bulk regardless of engine.
