# F2 migration — Multivoid onto the UE4SS C++ API (design of record)

**Status: DESIGN IN FLIGHT (qf design pass running 2026-08-21). Nothing here is built.**
Decision: `docs/VERSION_MIGRATION.md` §11 ledger, 2026-08-21 entry (USER: F2 taken).
Re-audit fact base: the 5-round /qf recorded in the session thread (scratchpad
`qf_thread.md`); headline numbers parts-sum-checked.

## 0. The decision + the pin

- **F2**: Multivoid becomes a UE4SS C++ mod. RULE 3 inverts. Standalone loader / AOB
  reflection / own PE detour retire whole (RULE 2) when this ships.
- **PIN: UE4SS stable v3.0.1** (2024-02-14; the "Latest" 1.99M-download channel; the
  EAGER-PE-detour channel). The spike proves against the **official shipped 3.0.1
  binary**, not only our from-source build — ABI compat with what users actually
  install is the point.
- Identity: `ModIntendedSDKVersion` carries the UE4SS axis at INSTALL time; the
  Paper-pair (game target × build) wire gate is substrate-independent and unchanged.
- `[U]` which exact UE4SS version VOTV's Thunderstore bundles carry (unzip one, read).

## 1. What stays vs what moves

**STAYS (ours under any substrate):** all of `coop/`, GNS transport, voice/opus, the
DX11+DX12 in-frame overlay + WndProc chain, `vm_dispatch` (GNatives — UE4SS core never
writes entries, measured 2026-07-26), `sdk_profile*` + the 29 BP offsets + 235 content
names (the recook-fragile half), config/ini registry, fonts/freetype.

**MOVES (public `ue_wrap/core` headers FROZEN; internals re-plumbed):**

| ours | LOC | fate under F2 |
|---|---|---|
| `reflection.cpp/.h` + `reflection_props` | ~1,242 | reimplement on `UObjectGlobals::StaticFindObject/FindFirstOf/ForEachUObject`, `UStruct::GetPropertyByNameInChain/GetFunctionByNameInChain`, `FProperty::GetOffset_Internal` — the chain-walk class (3 instances, 1 still open) dies here |
| `sig_scan` + AOB resolve | ~110 | DELETED — their init resolves the engine |
| `pe_detour.cpp` | 557 | DELETED — observers/interceptors/pump re-mount on their PE pre/post callbacks |
| `xinput_proxy.cpp` | 147 | DELETED — loader = `Mods/Multivoid/dlls/main.dll` |
| `ufunction_hook` (Func patches) | ~204 | FORK WP-3: keep our Func-patch mechanism (measured compatible: their Lua `UnregisterHook` never restores Func) vs adopt their `RegisterHook` — decide by spike |
| `types.h` | 87 | ZERO re-plumb (layout PODs) — verified |
| `game_thread` pump | ~900 | KEEP semantics; re-mount drain on their callbacks (THE behavioral seam — WP-1) |

API mapping census: our ~45 `R::` functions each get a UEPseudo-equivalent row during
WP-2; census of our surface done 2026-08-21 (see session thread).

## 2. Work packages + HALT gates

- **WP-0 prereqs**: USER links GitHub↔Epic (self-service; gates everything buildable).
  Clone UE4SS **v3.0.1 tag** + UEPseudo; build from source with the TAG's own toolchain
  docs (`[U]` — the 2024-era build system may be xmake, not today's CMake+Rust README);
  hello-world C++ mod loads in VOTV against the OFFICIAL 3.0.1 zip.
- **WP-1 SPIKE = HALT GATE 1** (under official 3.0.1, eager PE detour): (a) pump
  drain-at-top-level via own thread_local depth in their pre/post callbacks — incl.
  whether post-callbacks fire on ALL return paths; (b) MinHook DXGI Present/Resize
  hooks coexist; (c) Func-patch coexistence; (d) GNatives swap + our 200/256
  validation; (e) boot health-check timing vs `on_unreal_init` (their init replaces
  our settled-scan trigger); (f) perf: their always-on callback dispatch vs our
  short-circuit detour (baseline: 0.015 ms/fr our side).
- **WP-2 the swap**: freeze `ue_wrap/core` public headers; reimplement internals;
  delete AOB/proxy/detour (RULE 2). Equivalence = frozen-instrument discipline
  (body-diff where code moves verbatim; smoke + selftests on both peers).
- **WP-3 hooks fork**: our Func patches vs their `RegisterHook` — by spike measurement.
- **WP-4 distribution/release re-home** (40-file `multivoid-*` lane census, 2026-08-21):
  Mods-folder layout; dup/stale-install detection re-shape (`ModVersion` + boot banner
  replace the filename scan + "MOD INSTALL PROBLEM" dialog); `deploy-all`/`mp.py`/
  `lan-test` rewrite; CI builds UE4SS from source with an Epic-linked token (+ build
  cache; fork-PR story honestly limited); release lane + INSTALL.md (r2modman path +
  manual path); version identity gains the INSTALL-time UE4SS axis.
- **WP-5 Lua unification ("the double lua thing")**: our C++ mod injects `multivoid.*`
  into UE4SS's own Lua states (`on_lua_start` hands us every mod's
  `LuaMadeSimple::Lua`; `register_function` measured present, LuaMadeSimple.hpp:513).
  ROADMAP phases 4-6 REWRITTEN: the vendored-LuaJIT plan DIES; resources become
  UE4SS-Lua-hosted with our net/sync/authority API injected; the dedicated server
  (ghost-host) runs the same UE4SS+mod so ONE Lua API serves both sides. Sandboxing/
  client-trust remains the phase-7 question, shape unchanged. UE4SS Lua surface: 53
  documented global functions + 37 classes.
- **WP-6 docs/repo**: RULE 3 rewrite; README/INSTALL/VERSION_MIGRATION sweeps;
  tripwires retire; **ACT-1** freetype/miniaudio → gitlinks (515k → ~174k tracked);
  **ACT-2** publish-split of `research/` (user decides details).

## 3. Open [U] / risks (design-pass fodder)

1. v3.0.1 tag's exact build toolchain + whether official-binary ABI matches a
   current-MSVC Release mod build (spike-proven, not assumed).
2. Their C++ API docs describe "4.0.0, incomplete" — code against the TAG's headers.
3. C++ mod enablement mechanics: `mods.txt` row vs dll-presence; load ORDER among mods.
4. Their `CrashDumper`/exception handling vs our SEH wraps.
5. Users with EXISTING experimental installs — support matrix + polite version dialog.
6. Their PE callback dispatch fires for every registered callback on every PE call —
   perf delta measured at spike; also their detour is always-on even with zero mods.
7. UE4SS GUI console keybinds vs our overlay hotkeys (minor).
8. Boot ordering: our `GUObjectArray`-settled scans re-anchor on `on_unreal_init` /
   `on_program_start` — verify equivalents for "world ready" timing.
9. What the 2 real networked-C++-mod precedents did (pseudoregalia-archipelago) —
   optional prior-art read.

## 4. The pump seam (WP-1a), stated precisely

Today: MinHook detour on PE; `thread_local` depth; on depth-0 exit,
`DrainPostedTasksAtTopLevel()` runs posted game-thread tasks (game_thread.cpp).
Under F2: their PLH detour owns PE; we ride `RegisterProcessEventPreCallback` /
`...PostCallback`, keep our own depth counter, drain at depth-0 in the post callback.
MUST verify in their source: post-callback fires on every return path (exceptions?),
callback registration is process-wide, re-entrancy safe. If post-callbacks can be
skipped, the drain anchor moves (their `on_update` tick is the fallback anchor —
different latency profile, measure).
