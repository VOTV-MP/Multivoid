# F2 migration — Multivoid as a UE4SS mod via the SLIM C-ABI CONTRACT (design of record)

**Status: DESIGN, converged through 3 critic rounds 2026-08-21; the WP-1 spike is HALT
GATE 1 before any build.** Decision trail: `docs/VERSION_MIGRATION.md` §11 ledger
(2026-08-21 entries). Round transcripts: session `qf_thread.md`.
**This file was REWRITTEN WHOLE at design-round-3** — the round-0 full-API plan
(re-plumb reflection onto UEPseudo, ride their PE callbacks, Epic linkage, CI token)
is SUPERSEDED and appears only in §5 as the rejected fork. Do not implement anything
from older copies of this file.

## 0. What was decided, and by whom

1. **USER 2026-08-21: switch to the UE4SS ecosystem** (after the VOTV devs' critique
   and the 5-round re-audit that broke the old F1 record twice).
2. **USER, same day, the delivery constraint that picked the shape:** *"Probably it
   would be better if we only delivered only our sole mod and our mod just works
   with almost any loader, any build"* + *"I'm not sure what's best decision here"*
   (mandate: recommendation + WHY, not another fork).
3. **THE RECOMMENDATION: D-3 "SLIM CONTRACT"** — Multivoid becomes a real UE4SS mod
   folder speaking ONLY the C-ABI loading contract, keeping our entire proven
   substrate underneath. One binary, every C++-mod-capable UE4SS ever shipped.

## 1. The measured physics (why D-3 and nothing else)

All measured 2026-08-21 on the real artifacts (v3.0.0/v3.0.1 tags fetched into the
vendored submodule; main = 2026-05 checkout; shimloader zip downloaded and parsed):

- **The C loading contract is stable and old.** `Mods/<name>/dlls/main.dll`,
  `GetProcAddress("start_mod")` + `"uninstall_mod"` — byte-identical semantics
  v3.0.1 ↔ main-2026-05, present since **v3.0.0**. `enabled.txt` in the mod folder
  starts it without a `mods.txt` row (both eras). shimloader's `--mod-dir` carries
  "ue4ss lua and **dll mods**" (README verbatim) → the r2modman/Thunderstore
  pipeline delivers C++ mods.
- **The C++ class ABI is provably unstable.** Between v3.0.1 and main,
  `CppUserModBase` got `on_ui_init` INSERTED mid-vtable, five virtuals appended,
  and `on_dll_load`'s signature changed. A full-API binary mis-dispatches across
  eras BY CONSTRUCTION; "experimental" is a rolling tag (users hold arbitrary
  dates) — it is not one ABI. Full-API + one-binary is impossible. Per-channel
  builds fragment (3.0.1 / shimloader's frozen 2026-02-03 snapshot / floating
  manual-experimental); bundling our own UE4SS collides with shimloader-managed
  installs and was steered away by the user.
- **The object coupling is empty.** Every `fire_*` into the mod object returns
  void (censused, both eras); UE4SS NEVER deletes the object (`CppMod` dtor only
  `FreeLibrary`); no UE4SS-side code reads the object's data fields (`GUITabs`
  is touched only inside their own base-class member functions, which a
  non-deriving object never invokes); `dynamic_cast` runs only on THEIR `CppMod`
  wrapper, never on our object; `ModIntendedSDKVersion` is never checked anywhere.
- **The VOTV install base.** Thunderstore's VOTV community (185 packages) has
  ZERO UE4SS packages; the pipeline is `unreal_shimloader` v1.1.7 (251k dl)
  bundling an UE4SS.dll PE-dated **2026-02-03** (`patternsleuth` marker =
  experimental-era). **User-pointed cohort survey (2026-08-21): even the MANUAL
  install guides the community writes (DebugMod's) instruct installing
  shimloader's `dwmapi.dll` + its lowercase `ue4ss.dll` — the community standard
  is the shimloader bundle EVERYWHERE**; the vanilla-3.0.1 cohort ("Latest",
  1.99M dl globally) is whoever followed UE4SS's own docs instead. The live
  cohorts span both ABI eras — the user's "any build" is not a nice-to-have, it
  is the actual install base. Live ecosystem mods disclose per-GAME-version
  fragility as normal practice (VoidFax "Built for VotV a09k", DebugMod
  "designed to function with VotV 0.9.0n") — the game half stays the fragile
  half, exactly as our own recook analysis says.
- **Prior art (user-pointed).** `Diyagi/VotVSchema` (live VOTV C++ mod, pushed
  4 days ago; a PalSchema fork) took the full-API fork — UEPseudo, Epic linkage,
  a third-party UE4SS fork (`Okaetsu/RE-UE4SS`) — **and still hand-rolls 9+
  VOTV-specific AOB signatures**, its own comments admitting why: *"UE4SS has
  StaticFindObject, but this lets us use it earlier"*, *"issues using the IsA
  provided by UE4SS due to early init"*, *"probably going to break in the next
  update or so"*. Game-specific resolve is ecosystem-normal even in the full-API
  camp. (Palworld's C++ scene runs on UE4SS forks — the "successor fork becomes
  the live line" door already fired there.)

## 2. The D-3 mechanism

**The artifact:** `Mods/Multivoid/dlls/main.dll` + `enabled.txt`. The xinput proxy
DIES (RULE 2). Everything else we ship today survives byte-for-byte in role:
bootstrap, AOB/GUObjectArray reflection, PE MinHook detour + pump, Func patches,
vm_dispatch/GNatives, DX11+DX12 overlay, GNS, all of `coop/` — all of it is
game-version-coupled and **UE4SS-version-independent**, which is exactly what makes
"any build" possible.

**The shim (~60 LOC, `src/loader/cppmod_entry.cpp` replacing `xinput_proxy.cpp`):**
- `extern "C" __declspec(dllexport) CppUserModBase* start_mod()` and
  `uninstall_mod(CppUserModBase*)` — the names are GetProcAddress'd, C-ABI.
- Returns a hand-rolled NON-DERIVING object: vptr → a static array of **256**
  identical stubs (2 KB; upstream growing past our slot count must never read
  past the array). Stub = `xor eax,eax; xorps xmm0,xmm0; ret`.
  **Scoped honestly (round-3 correction):** this is safe-by-construction for the
  MEASURED dispatch universe (every `fire_*` today returns void, both eras) and
  deterministic-false/null/0.0 for future bool/int/ptr-returning virtuals; a
  future **sret** (aggregate-return) virtual has NO universal safe stub — that
  coupling is WATCHED (wire-e below), not solved. Zero data fields are readable
  by them (measured), so object layout beyond the vptr is free.
- **Self-PIN** via `GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_PIN)` — main-era
  UE4SS has GUI "Restart All Mods" → `uninstall_mods()` → `FreeLibrary`; v3.0.1
  has no restart. Pinned, the FreeLibrary is a refcount no-op.
- **Restart state machine (designed-permanent, not a crutch):** UNBOOTED →
  BOOTED (first `start_mod` = the real bootstrap) → `uninstall_mod` = log +
  ignore (a live multiplayer session does not quiesce because a debug button was
  clicked) → second+ `start_mod` = return a fresh dummy object, do NOT
  re-bootstrap (the project's standard Install() latch shape). Stale pointers
  held by UE4SS are harmless: void no-op virtuals, never deleted, never read.
- **Load timing:** UE4SS loads us at its mod-scan (later than the proxy's
  process-init). Our bootstrap already late-binds (waits for engine settle;
  overlay arms lazily on the live swapchain; WndProc subclasses whenever) —
  expected tolerant, MEASURED by the spike, not assumed.

**Duplicate/predecessor protection (round-3 Q4; `ModVersion` is unchecked by
UE4SS, so OUR code owns this whole):**
- **New-vs-new:** a named mutex (`Global\\MultivoidLoaded`) taken at `start_mod`;
  a second instance (manual `Mods/` copy + an r2modman-profile copy both merged
  in by shimloader, or two differently-named folders) sees
  `ERROR_ALREADY_EXISTS`, pops OUR dialog naming both paths, refuses to
  bootstrap.
- **New-vs-old (the upgrader):** boot scan for the standalone install beside the
  exe — `xinput1_3.dll` proxy + `multivoid-*.dll` on disk AND a
  `GetModuleHandle` live check — hard-refuse with a removal dialog. (Old builds
  predate the mutex; the disk+module scan covers them.)

**Coexistence becomes the baseline, not the edge case:** on 3.0.1 their eager PE
detour chain-stacks with our MinHook detour (measured 2026-07-26: install-order
tolerant, we never unhook mid-run); on experimental their detour is lazy and
default-absent. KismetDebugger (the one GNatives neighbor) is not in release zips
and is human-enabled — unchanged. **We require ZERO UE4SS settings edits** — a
shimloader profile is not ours to configure; the spike runs everything on
defaults.

## 3. Work packages + HALT gates

- **WP-1 SPIKE = HALT GATE 1 (buildable TODAY — no Epic linkage, no UEPseudo,
  no CI change; the round-0 user prerequisite is OFF the critical path):**
  build the shim wired to the existing bootstrap; load the ONE binary on the
  matrix, ordered by cohort size: **(a) a shimloader/r2modman profile (THE
  PRIMARY cohort — even manual community guides install the shimloader
  bundle)**, (b) official 3.0.1 manual install (the vanilla-docs cohort; the
  EAGER-detour era), (c) experimental-latest + the "Restart All Mods" drill
  against PIN+latch. Measure `start_mod` timing vs today's proxy-load markers;
  verify overlay/WndProc/GNS/pump under both PE-detour regimes; all on default
  settings; then the standard LAN smoke on the spike bytes.
- **WP-2 the swap:** delete `xinput_proxy.cpp` + the proxy deploy path (RULE 2);
  `cppmod_entry.cpp` in; predecessor detection + mutex; keep EVERYTHING else.
- **WP-4 distribution/release re-home** (40-file `multivoid-*` lane census,
  2026-08-21): artifact becomes a mod-folder zip (`Mods/Multivoid/...`);
  `deploy-all`/`mp.py`/`lan-test` re-point; release lane + `ledger_lint` +
  CI fingerprint adapt (toolchain UNCHANGED — same MSVC, no UE4SS build);
  INSTALL.md: r2modman path + manual path (user obtains official UE4SS
  "Latest" zip; link) + the upgrader path (delete the old two DLLs — the
  boot guard enforces); master `LATEST_URL` flow unchanged (points at our
  GitHub releases). Version identity: the Paper-pair wire gate is untouched;
  NO UE4SS pin axis exists under D-3 (that is the point); our log prints the
  loaded UE4SS.dll's version resource as support telemetry.
- **WP-5 modder API (the "double lua thing") — DEFERRED WHOLE, lean recorded:**
  the round-1 injection mechanism (implement `on_lua_start` + import
  `register_function`) died with D-3 — it is exactly the ABI coupling D-3
  removes. What survives: server-distributed resource code NEVER runs in their
  unrestricted Lua states (the subtraction fact; sandboxed runtime, phases 5-6).
  The modder-facing lean is **L-4, the ENGINE-LEVEL BRIDGE**: expose multivoid
  via the engine surface our substrate owns (console commands / a spawned API
  object with our-hooked callables) — era-independent, loader-independent, and
  it serves the ACTUAL ecosystem, which is BP/table-shaped (VoidMod 143k dl;
  VotVSchema JSON table mods; the wiki's main track is Blueprint modding),
  where Lua injection would have served the smallest cohort. Zero consumers
  exist today → fix-then-generalize: no build until a consumer does. ROADMAP
  phases 4-6 rewrite carries this. Future named lane (not designed):
  content-mod manifest exchange at join (VotVSchema-class table mods on one
  peer are the semantic-divergence class; the Paper-pair covers only OUR
  identity).
- **WP-6 docs/repo:** RULE 3 rewrite (the mod is a UE4SS-ecosystem mod with a
  standalone substrate; UE4SS is the loader, UEPseudo is nothing to us);
  README/INSTALL/VERSION_MIGRATION sweeps; tripwires repurposed (below);
  **ACT-1** freetype/miniaudio → gitlinks (515k → ~174k tracked); **ACT-2**
  publish-split of `research/` (user decides details); the chain-walk fix now
  OWED in our reflection (`FindFunction` superclass walk, reflection.cpp:427 —
  the "free under their API" carrot is forfeited, so we pay it ourselves).

## 4. The watched couplings (tripwires, round-3 Q3)

D-3 keeps exactly TWO couplings to upstream, both watched mechanically from
`tools/release/tripwires.ps1` (same host, RELEASE.md step 0):

- **wire-d (the contract):** upstream `CppMod.cpp` — the GetProcAddress names,
  the `dlls/main.dll` layout, the enablement semantics. Baseline = committed
  copies of the v3.0.1 + current-main readings; FIRED on drift in main or in a
  new stable.
- **wire-e (the safety premises):** upstream `CppUserModBase.hpp` + `CppMod.cpp`
  — `fire_*` return types staying void (the sret hole), virtual count vs our
  256 stubs, any new `delete` of `m_mod` or direct field read appearing in
  their src. FIRED = patch the stub table / re-derive the safety case BEFORE
  any release.
- wire-a/wire-b (UEPseudo access / new stable release) demote to informational
  under D-3 — no pin exists; a new stable is only news for wire-d/e baselines.

## 5. The rejected forks (recorded so they are not re-derived)

- **Full C++ API (round-0 plan):** re-plumb reflection onto UEPseudo, ride their
  PE callbacks, adopt their RegisterHook; Epic linkage per builder, CI builds
  UE4SS from source with an Epic token, fork PRs cannot build. KILLED by the
  vtable measurement (mis-dispatch across live eras) + the user's one-binary
  constraint. Its one carrot (the chain-walk class free) is small and now owed
  in our code.
- **Per-channel builds (D-1):** 3+ artifacts, rolling-tag roulette. KILLED.
- **Bundle our own UE4SS (D-2 / round-1 PIN-C):** we become UE4SS's manager on
  the user's machine; collides with shimloader profiles. User steered away.
- **Status quo (D-4, xinput proxy):** fails the ecosystem critique the user
  accepted ("custom injection", not r2modman-manageable, not a UE4SS mod).
- **Lua injection (round-1 WP-5a):** on_lua_start slot position shifts per era +
  C++-mangled imports — the coupling D-3 exists to remove. Superseded by L-4.

## 6. What this answers, stated for the handoff

- The user's steer: **one artifact, works on every C++-mod-capable UE4SS ever
  shipped (v3.0.0 → 3.0.1 → any experimental date → shimloader), under default
  settings, no version pin, no Epic linkage for anyone, public clone still
  builds.**
- The dev's critique: we become a normal UE4SS mod (their loader, their folder
  shape, r2modman-manageable, `enabled.txt`, coexistence by construction). The
  "reimplementing reflection" charge remains TRUE and is now DEFENDED by
  measurement instead of preference: the C++ API cannot give one-binary across
  the live install base (vtable instability, rolling tag), and the ecosystem's
  own full-API mod (VotVSchema) hand-rolls VOTV signatures anyway. The
  substrate we keep is the price of "any build" — and it is the half that was
  never UE4SS's to maintain (game offsets, BP seams, the recook-fragile half).
- **Distribution wins measured from the live ecosystem (user-pointed pages):**
  our Thunderstore package DEPENDS on `unreal_shimloader` → r2modman
  auto-installs the loader chain (proven live by VoidFax: "UE4SS/shimloader
  (installed automatically by your mod manager)"); **Linux/Steam Deck gets
  EASIER under D-3** — today our xinput proxy needs its own `WINEDLLOVERRIDES`,
  while as a UE4SS mod we ride the `dwmapi` override the ecosystem already
  documents and maintains (FusionFix's Linux section); Fusion's game-relaunch
  behavior is process-fresh and tolerated; we touch no prop datatables, so the
  Fusion conflict class does not apply to us.
- The morning's "switch to the C++ API" is AMENDED, not betrayed: the user's
  own delivery constraint ("sole mod, any loader, any build") is satisfiable
  ONLY by the C-contract shape — the API half was measured impossible to
  combine with it, and what the API would have bought (the chain-walk class)
  is small, known, and now owed in our own reflection.
