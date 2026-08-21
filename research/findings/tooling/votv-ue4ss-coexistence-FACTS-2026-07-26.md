# UE4SS coexistence + port-overlap — the measured fact base (2026-07-26)

**Status: FACT BASE, not a design.** Produced by a 3-round qf-research workflow
(3 ingest + 3 critic + 16 measure agents; stopped by the user before convergence —
the residuals are enumerated in §6). Everything below is tagged; [M] = measured on
code/bytes with the citation the agents returned, [C] = circumstantial, [U] = needs
the named probe. The user's two questions: (1) can an installed Multivoid coexist
with UE4SS + UE4SS mods on one game copy; (2) what would "porting onto UE4SS"
overlap with (inventory only).

## 1. File level — NO collision for any real install channel today [M]

- UE4SS >= 3.0.0 proxy = `dwmapi.dll` (proxy_generator CMakeLists DEFAULT_PROXY_NAME;
  release packager copies exactly dwmapi.dll). UE4SS <= 2.5.2 WAS `xinput1_3.dll`
  (v3.0.0 changelog orders its deletion). We are `xinput1_3.dll`.
- Both `XINPUT1_3.dll` and `dwmapi.dll` are in VotV-Win64-Shipping.exe's STATIC
  import table (PE parse) -> both proxies load at process init.
- Channel survey (2026-07-26): v3.0.1 stable is still "Latest" (1.99M downloads,
  flat layout); experimental-latest rebuilt daily (dwmapi + `ue4ss/` subfolder);
  Thunderstore VOTV = 175 packages, 199 zip indices scanned via Range requests with
  a VALIDATED positive control (the real 2.5.2 Xinput zip flags) -> **zero
  xinput1_3.dll anywhere**; shimloader + every bundler ships dwmapi.
- The only physical cannot-both-load scenario: a user hand-installing a 2023-era
  <=2.5.2 Xinput build. Nothing in circulation ships it.

## 2. Hook level — ONE hard shared surface: ProcessEvent

- **UE4SS 3.0.1 (dominant cohort): the PE double-detour is REAL.** 3.0.1 has NO PE
  AOB — it resolves PE via Default__Object's vtable slot 68 (offset 0x220) and
  detours it EAGERLY and UNCONDITIONALLY at init (PLH x64Detour; call site
  RVA 0x5400d2 has no settings gate; the HookUObjectProcessEvent ini key does not
  exist in 3.0.1). Slot 68 resolves to the SAME address our AOB hits
  (exe+0x1465930, IDB-cross-checked n=1). It demonstrably initializes on this exe
  (the on-disk ObjectDumps are post-init artifacts). [M]
- **Today's experimental build: NO double-detour on a bare install.** Measured on
  the shipped 2026-07-26 bytes + exact-commit source (g6c26f038): Initialize no
  longer calls HookProcessEvent; the detour is lazy (DetourInstance on first PE
  callback registration), and the default mod set registers zero PE callbacks
  (ExecuteInGameThread defaults to EngineTick; native RegisterHook rides the Func
  path; script RegisterHook rides ProcessLocalScriptFunction). [M]
- **Stacking**: two inline detours chain in install order; both sides are
  chain-tolerant on install. **Teardown**: MinHook AND PLH restore their saved
  prologue bytes BLINDLY (both pinned sources read). Whichever restores first can
  silently unhook the other — but we never unhook PE mid-run: the sole
  `game_thread::Uninstall` caller is the process-close path (shutdown.cpp:210), so
  the exposure window is process teardown only, not a routine mid-run crash. [M]
  (Residual [U]: PLH's follow-jmp behavior at the pinned commit — whether its
  detour lands on the prologue or inside OUR detour body — read-only readable,
  not yet read.)

## 3. Everything else — benign or narrowly opt-in

- **DXGI/overlay: zero conflict** [M]. UE4SS's GUI is its OWN window + own
  D3D11/OpenGL swapchain; it never hooks the game's Present/ResizeBuffers/ECL. No
  DX12 backend at all (ours has one).
- **GNatives: UE4SS core never writes entries** (both eras) [M]. Only
  KismetDebuggerMod swaps them — and it is NOT in release zips
  (include_in_release=False), has no mods.txt line, and swaps only when a human
  clicks "enable" in the GUI. Two opt-in risk shapes: (a) it swaps BEFORE our
  vm_dispatch install -> our >=200/256-in-exe validation FAILS -> **permanent
  process-lifetime latch, all 0x45 verb lanes dead with ONE log line**
  (vm_dispatch.cpp:210-215; note: the latch premise "can never succeed later" is
  false for THIS scenario — the table heals when the debugger disables, we never
  re-try); (b) it enables after us and its wholesale restore stomps our wrapper
  out. Both require deliberate user action. [M]
- **UFunction Func patches: the Lua-wipe fear is FALSIFIED** [M]. Census: we patch
  10 UFunctions default (17 with the rng census dev flag), including the popular
  K2_DestroyActor / FinishSpawningActor / BeginDeferredActorSpawnFromClass /
  AudioComponent trio. UE4SS's Lua `UnregisterHook` NEVER restores Func (3.0.1
  bytes: its body is GetFunctionHookData+RemoveCallback; the dispatcher chains
  through whatever it saved — our forwarder included). The only Func-restore is
  `UnregisterAllHooks`, which has ZERO callers in the DLL and the tree — only a
  third-party C++ mod could invoke it. [M for 3.0.1 + d72d2f38-era source; [U] the
  same disasm on today's experimental DLL.]
- **GUObjectArray / GNames / GMalloc / LoadLibrary IAT: no conflict** [M]. UE4SS
  reads/listens (engine listener API, not code hooks); its LoadLibrary IatHooks
  land on the exe's IAT only (all four names present there; first-match walk) and
  are pass-through anyway; our proxy's own IAT is untouched.
- **LogicMods folder**: our skin paks live in `LogicMods/multivoid/` on purpose
  (native pak automount). 3.0.1's BPModLoader scans TOP-LEVEL paks only — our
  subfolder is INVISIBLE to the dominant cohort (boot-time directory-listing log
  lines only). v4/main's loader does walk one subdir level, but our pak indices
  (all 47 scanned: `VotV/Content/Mods/VOTVCoop/<stem>`, no ModActor anywhere)
  structurally cannot match its `/Game/Mods/<pakstem>/ModActor` lookup -> at most
  ~15 "ModClass not valid" log lines per map load, no spawns. [M]

## 4. The DOMINANT real risk is semantic, not mechanical [M receiver census]

Any world-mutating UE4SS mod runs on ONE peer with no wire lane. Receiver-code
census (not docs): exactly TWO receive-side validators exist in the whole tree
(container_contents CAS arbiter; DishAim holder check). Three behavior classes:

1. **ADOPT + AMPLIFY (~20 wire kinds)**: Symmetric keyed toggles (lights,
   containers, garage, appliances, lockers), DeskInput/DeskScanEvent/DeskSndFx,
   PlayDeck, ReelSlot empty-slot writes, SkySignalCatch, Drive/Meadow lanes — an
   unprimed change-edge from ANY peer is adopted and relayed session-wide. A
   one-peer Lua mod's writes become shared-world canon.
2. **FIGHT**: host-corrected lanes (dish/reel pose correctors, weather re-assert,
   device_occupancy reconciler, v122 host wall) rubber-band a client-side mod;
   a HOST-side mod becomes canon.
3. **SILENT DRIFT**: every no-lane facet (the COOP_WORLD_PROP_DIVERGENCE class,
   raw property writes outside watched verbs/polls).

This is architectural on both sides — not a bug to fix, a compatibility statement
to publish: "UE4SS mods that only read (UI, dumps, cosmetics on your own screen)
are fine by construction; mods that MUTATE world state will either be fought,
amplified to everyone, or silently desync, depending on the lane."

## 5. Port-overlap inventory (secondary question) [M]

What we hand-roll that UE4SS also provides: loader/proxy, reflection resolve,
PE detour, per-UFunction hooks, spawn catching — ue_wrap/core = 7,174 LOC of
146,347 (~5%). What UE4SS does NOT provide: our in-game-frame overlay (its GUI is
own-window, D3D11/OpenGL, no DX12), GNatives verb interception in core, and the
other ~95% (coop logic, per-class wrappers, UI, harness). The RULE 3 trade
(standalone; UE4SS = dev tool) stands as documented in VERSION_MIGRATION.md.

## 6. History correction + residuals

- **RETRACTED**: "UE4SS and Multivoid coexisted throughout development." Measured
  truth: dual-proxy was CONFIGURED in the dev copy for ~1 week (May 2026,
  pre-overlay pre-vm_dispatch era); the ONE recorded dual-boot observation is a
  CRASH (2026-05-30, 4-instance context); no UE4SS.log survives anywhere; today
  no game copy runs UE4SS (HOST's dwmapi is `.off`). No dual boot has ever been
  attempted with an overlay-era or vm_dispatch-era build.
- **Read-only residuals** (workflow stopped before measuring): PLH follow-jmp
  topology at pin fd2a88f0; today's-experimental UnregisterHook disasm; ~~the PE-era
  of the DLLs Thunderstore's shimloader (2026-02-03)~~ **MEASURED 2026-08-21: the
  shimloader v1.1.7 zip bundles an UE4SS.dll with PE timestamp 2026-02-03 and the
  `patternsleuth` marker = EXPERIMENTAL-era (lazy PE detour), NOT 3.0.1 — and even
  the community's MANUAL install guides (DebugMod's) install the shimloader bundle
  (fact base: votv-ue4ss-f2-migration-DESIGN-2026-08-21.md §1)**; VotV_RichPresence
  (2023-12-26, v3.0.0-era, likely eager) still unmeasured.
- **Runtime-gated probes (named)**: (a) dual-install smoke — rename HOST's
  `dwmapi.dll.off` -> `dwmapi.dll` (3.0.1) or `tools/install-ue4ss.ps1` on
  CLIENT_3 (experimental), one instance, read multivoid.log's pe_detour install
  line beside UE4SS.log, then a clean WM_CLOSE cycle for the teardown window;
  (b) a 5-line Lua mod flipping one watched Symmetric field on one peer in a live
  session — pins the amplification class end-to-end.

Workflow artifacts: journal at the session's workflow dir (wf_09d3e2d4-b08);
Thunderstore scanner + raw results in the session scratchpad (scan_ts.py,
scan_result.json).
