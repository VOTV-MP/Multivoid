# FEASIBILITY — VOTV coop (Phase 0 deliverable)

**Game**: Voices of the Void (VOTV), version 0.9.0n.
**Engine**: Unreal Engine **4.27** (confirmed: `++UE4+Release-4.27` string
in `VotV-Win64-Shipping.exe`; PhysX3 + `WindowsNoEditor` packaging
corroborate UE4, not UE5).
**Assessed**: 2026-05-21. **Status**: VIABLE — proceed to Phase 1.

This file answers Phase 0.2–0.8 of `docs/COOP_METHODOLOGY.md`, adapted for
a UE4 game. Findings marked **(verify)** are inferred and need a concrete
check during early Phase 1.

---

## Decisive context: this is Unreal Engine, not a native engine

The methodology was written around GTA:SA / MTA — a closed native engine
requiring blind binary RE. VOTV is UE4.27, a fully documented engine with:

- **Reflection**: every `UClass`, `UProperty`, and `UFunction` is
  enumerable at runtime via `GUObjectArray` / `GNames`. Most of Phase 1
  "engine archaeology" becomes reflection introspection, not IDA work.
- **A mature modding stack**: [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)
  provides injection, a `UFunction` hook engine, Blueprint mod loading,
  Lua + C++ mod APIs, a live object browser, and an ImGui overlay.
- **Native multi-pawn support**: the engine already runs multiple
  `APawn` / `APlayerController` instances (split-screen, listen servers).
  "Spawn the orphan" = `UWorld::SpawnActor` of the player pawn class.

**Chosen approach** (user decision, 2026-05-21): **UE4SS + reflection.**
Custom UDP transport for game state. NOT the engine's built-in
replication (VOTV blueprints are SP-authored — no replicated props/RPCs —
and adding them would mean editing assets, anti-pattern A6).

---

## 0.2 Is the binary unpacked? — YES, trivially

Shipping build, no DRM/anti-tamper observed (`VotV-Win64-Shipping.exe`,
~85 MB, standard UE4 shipping layout). UE4 shipping binaries are not
encrypted at runtime. **Not a blocker.** UE4SS injects into shipping UE4
games as a matter of course.

## 0.3 Rendering API — D3D11 / D3D12 / Vulkan (user-selectable)

YeetPatch (the official launcher) exposes a render-API switcher
(Vulkan / DX11 / DX12). The overlay strategy is **UE4SS's built-in ImGui
integration**, which hooks the active swapchain regardless of API — we do
not hand-roll a present hook. **Not a blocker.** Pin a known render API
(DX11) for development to keep the overlay path stable. **(verify)** which
API UE4SS's ImGui hook is most stable on for this build.

## 0.4 Input API — UE4 enhanced/legacy input + WndProc

UE4 reads input through its own `UPlayerInput` / `UInputComponent`
pipeline, ultimately fed by the OS message pump. For mod-menu hotkeys and
synthetic test input we use UE4SS's input handlers / `GetAsyncKeyState`
(WP15/WP16 — do not gate mod input on engine focus). For replicating P2,
we inject into UE's input/movement at the `APlayerController` /
`UCharacterMovementComponent` level, not at the OS layer. **Not a
blocker.**

## 0.5 Entity model — UE Actor/Pawn/Controller (the easy case)

- **Clear factory?** YES — `UWorld::SpawnActor<T>` (+ `SpawnActorDeferred`).
  Universal UE entity factory, parameterized by `UClass*`.
- **Player a distinct class?** YES — VOTV's player is a `ACharacter` /
  `APawn` subclass driven by an `APlayerController`. **(verify)** the exact
  VOTV pawn/controller class names via reflection in Phase 1.2.
- **Can we spawn a second player-class entity?** Architecturally yes —
  this is what listen servers / split-screen do. The cheapest derisk
  (methodology 0.5) is: via UE4SS, `SpawnActor` the VOTV pawn class +
  create a 2nd `APlayerController`, confirm it ticks. **This is the first
  concrete Phase 2 experiment.** **(verify)**

## 0.6 Script VM — Blueprint VM (UE's `UFunction` bytecode)

VOTV is heavily Blueprint-driven (typical for this game). The "script VM"
is UE's Blueprint VM; "script commands" are `UFunction`s. We hook
Blueprint events / functions via UE4SS rather than parsing a custom script
format. Host runs gameplay logic; client receives state. NPC/AI logic
lives in blueprints + engine — host-authoritative replication applies.
**RESOLVED:** VOTV gameplay is heavily Blueprint-VM-driven; the full
dispatch-path split (BP-VM `EX_*` INVISIBLE vs native ProcessEvent VISIBLE) is
mapped + code-verified in `docs/COOP_DISPATCH_VISIBILITY.md`.

## 0.7 Save format — UE `SaveGame` (`.sav`)

VOTV uses UE's `USaveGame` serialization (per-slot save files, typically
under the game's `Saved/SaveGames/`). **RESOLVED:** root
`%LOCALAPPDATA%\VotV\Saved\SaveGames\`, flat GVAS/UE4.27 `<slot>.sav`
(unencrypted, ~19.6 MB, photos inline), non-atomic 4-in-place-write gap — see
`research/findings/saves/votv-save-path-RE-2026-05-30.md`. Coop is host-authoritative:
host loads their save; client's world is synced from host state. Loading a
save programmatically is done via the game's own load `UFunction` (callable
through reflection) — entry point RE'd + shipped (`ue_wrap/save_browser.cpp`
via VOTV's native `loadSlots`).

## 0.8 Per-process / per-machine state — single-instance? RESOLVED

UE4 games are not inherently single-instance. **RESOLVED 2026-05-23:** VOTV
enforces NO mutex / single-instance lock — two instances launch fine on one
machine, and same-box two-instance LAN testing is the standard harness
(`tools/lan-test.ps1`; commit 31726747). No bypass code is needed or present
in `src/`. Steam DRM (if present) does not block the working two-instance
harness in active use; cross-machine LAN is unaffected.

---

## Blockers

**None identified.** UE4.27 + no anti-tamper + UE4SS + native multi-pawn
support make VOTV a **good fit** per the methodology's fit criteria
(single-player 3D game, protagonist + AI NPCs, hookable engine, a story/
world to share). The main risk is not feasibility but **scope** — VOTV is
a large simulation-heavy game (signal processing, base management, day
cycle). `docs/COOP_SCOPE.md` must be disciplined.

## Open verification items (carry into Phase 1) — ALL RESOLVED

All six Phase-0 verification items were answered by the shipping mod across
later sessions. Recorded here as AS-BUILT (this list was frozen at the Phase 0
skeleton commit and never updated as Phase 1 closed each item).

1. [RESOLVED] VOTV pawn = `mainPlayer_C`; player-controller = `PlayerController`.
   Named as profile constants in `src/votv-coop/include/ue_wrap/sdk_profile.h:664`
   (MainPlayerClass) + `:920` (PlayerControllerClassName); reflection-resolved
   across the codebase. Done since Phase 2.1 (commit 27f5e9fd) / version-
   portability profile (22d83066).
2. [DONE] `SpawnActor` of a 2nd pawn (mainPlayer_C orphan) spawns + ticks:
   `engine.cpp:526` SpawnActor (commit 79236cb9), wired live via
   `RemotePlayer::Spawn` (remote_player.cpp:157) on join (net_pump.cpp:862);
   AnimBP forced-tick (puppet_spawn.cpp:365 SetAnimTickAlways, post-s28), driven per-frame by ApplyToEngine.
   NOTE: the evolved design deliberately spawns an UNPOSSESSED orphan — no 2nd
   PlayerController (engine.cpp:559-575); `GetController()==nullptr` is the
   puppet discriminator (see CLAUDE.md). Visible-2nd-player subsystem shipped
   commit bb16b208.
3. [RESOLVED] Save file location / layout / encryption. Root
   `%LOCALAPPDATA%\VotV\Saved\SaveGames\`; flat GVAS/UE4.27 `<slot>.sav`
   (unencrypted, ~19.6 MB, photos inline), `data.sav` meta, `b_*`/subsave
   files; non-atomic 4-in-place-write gap. See
   `research/findings/saves/votv-save-path-RE-2026-05-30.md` (commit 88a8a1f8);
   exploited in `coop/save_transfer.cpp`, `coop/save_block.cpp`,
   `ue_wrap/save_capture.cpp`, `ue_wrap/save_browser.cpp`.
4. [RESOLVED] Blueprint-vs-C++ split of gameplay logic. VOTV gameplay is
   heavily Blueprint-VM-driven; the dispatch-path split (BP-VM `EX_*` INVISIBLE
   vs native-engine ProcessEvent-dispatched VISIBLE) is fully mapped + code-
   verified in `docs/COOP_DISPATCH_VISIBILITY.md` (+ companion
   `COOP_ENTITY_EXPRESSION_MAP.md`) and relied on throughout `src/votv-coop`
   (e.g. trash_collect_sync.cpp:252-255).
5. [RESOLVED 2026-05-23] VOTV has NO single-instance lock; two instances run
   fine on one machine. Same-box two-instance LAN testing is the standard
   harness (`tools/lan-test.ps1`; commit 31726747; ROADMAP Phase 3 confirmed).
   No bypass code needed (none exists in src/).
6. [RESOLVED] Overlay render API = hook `IDXGISwapChain::Present` (DXGI), DX11
   backend (ImGui_ImplDX11 + ImGui_ImplWin32); DX12 detected + logged, not yet
   drawn. Implemented in `src/votv-coop/src/ui/imgui_overlay.cpp` (commit
   5dc7aa67), wired live at `harness.cpp:469` (Start; s27 renumber). NOTE: uses the mod's own
   vendored ImGui, NOT UE4SS (RULE 3) — the "UE4SS overlay" framing is retired.
