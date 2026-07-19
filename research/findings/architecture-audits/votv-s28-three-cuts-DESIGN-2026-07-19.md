# s28 three cuts — remote_prop / npc_sync / puppet (AS-BUILT, 2026-07-19)

> Point-in-time record of the s28 modularization session ("go next: remote prop, npc sync,
> puppet; БЕЗ smoke тестов"). Companion to `votv-s27-three-cuts-DESIGN-2026-07-19.md`; the
> recipe is [[lesson-refactor-equivalence-frozen-digest-instrument]] (9th/10th/11th
> applications). **NO runtime smoke was run — user directive**; the equivalence claim rests on
> the literal instruments + per-commit Release builds + two clean audits, and each commit
> message records that waiver. Status: AS-BUILT (not hands-on; rides take 4).

## Commits

| Commit | Cut | Result |
|---|---|---|
| `6c910046` | remote_prop cut1a | NEW `remote_prop_convert.cpp` 291 (OnConvert verbatim; zero new seams; 4 span-only includes pruned) |
| `d0c7879e` | remote_prop cut1b | NEW `remote_prop_physics.cpp` 196 (reflected UPrimitiveComponent thunks + Aprop_C.thrown; DrivePropThrown anon->named via `remote_prop_internal.h`); residual **758** |
| `fd7c7409` | npc_sync cut2 | NEW `npc_sync_install.cpp` 327 + NEW `npc_sync_internal.h` 45; residual **709** |
| `ca12e11d` | puppet cut3 | NEW `puppet_spawn.cpp` 512 + NEW `puppet_internal.h` 40; residual **491** |
| `c36ba368` | docs closeout | ledger-4 rows, runbook DLL flip, lesson row, audit cosmetic |

Deployed `votv-coop.dll 1626b6e093cd07c7` x4 (HOST/CLIENT_1/2/3), hash-verified; proto 121
(no wire change).

## Design decisions (per /qf pass)

1. **remote_prop (5-round /qf):** the lane-split family continued (spawn/destroy precedent).
   OnConvert's dependency census (enumerated 31-symbol anon-ns grep with a known-positive
   control) showed exactly ONE hit — a string literal in the GT assert — so the convert TU
   needed zero new declarations. The physics group is self-contained (reverse sweep: zero refs
   to residual privates or active_drive); its one cross-TU symbol (DrivePropThrown, single
   caller OnRelease) rides the existing internal.h seam.
2. **npc_sync (3-round /qf):** the install/bootstrap axis, accessor-with-its-state (the s27
   session_runtime shape) — Install + IsInstalled + GetDevSpawnRefs + IsHostNpcSyncDisabled
   move together so g_installed + both resolve caches + both observer latches + g_npcSpawnFn
   stay PRIVATE (the /qf R1 critic caught g_npcSpawnFn mislabeled "shared": a re-census proved
   all its readers move — the extern set narrowed to FIVE: 3 param offsets + g_npcAllowlist +
   g_npcSyncDisabledThisProcess, defined install-side, extern in `npc_sync_internal.h`). The
   three ProcessEvent callbacks flip anon->named for cross-TU registration (bodies
   byte-identical; tree-grep clean). ONE enumerated non-verbatim edit: Install's first line
   `g_session_ptr.store(...)` -> `SetSession(session)` — the public setter's body verified
   byte-exact equal to the store; the instrument guards BOTH sides + a dedicated mutate.
3. **puppet (2-round /qf):** the SPAWN-path axis. The R1 critic's "move the head-gate hook
   with its sole caller" was adopted — the whole block (HeadGateBUAPost + InstallHeadGateHook)
   moved into puppet_spawn.cpp's anon namespace, which dissolved BOTH the planned cross-TU
   decl AND the anon-close relocation. `puppet_internal.h` = the ReadPtr/ReadAt/WriteAt offset
   templates (verbatim) + extern g_meshComp + LiveAnimInstance decl (two named flips, defined
   in puppet.cpp). ufunction_hook.h moved with the hook; call.h pruned (its only users — bare
   `Call(` sites — lived in the spawn span; the qualified-only census pattern missed them and
   the BUILD caught it, see the lesson). Pre-existing dead GT alias + game_thread.h include
   flagged, NOT touched (not this cut's scope). Also pre-existing dead in remote_prop.cpp
   (correctness-audit note, predates s28): kerfur_entity.h / prop_echo_suppress.h /
   fname_utils.h includes.

## Verification

- Instruments (scratchpad s28, session 67b608d1): `diff_c1.py` / `diff_c2.py` / `diff_npc.py`
  / `diff_puppet.py` + surgery scripts + BASELINE copies of all three files. All PASS on the
  final committed bytes.
- Must-FAIL mutates: 4 + 5 + 7 + 7 (per-region: span bodies, scaffold adds, residual edits,
  residual deletions, edited-line, internal.h decl drops). **The m6 puppet mutate (deleting an
  internal.h decl) initially PASSED** — a scaffold WHITELIST is structurally blind to DROPPED
  lines; the instrument was fixed to an exact-content sequence compare and all mutates then
  failed as required. Lesson recorded (the mutate battery tests the INSTRUMENT):
  [[lesson-refactor-equivalence-frozen-digest-instrument]] s28 extension + docs/LESSONS.md.
- Release build at EVERY commit boundary (independent revertability).
- Audits on the full diff (post-commit, pre-deploy): correctness (feature-dev:code-reviewer) —
  CLEAN on behavior preservation / seam types / includes / sizes, 0 findings >=80, the
  SetSession swap verified byte-equivalent; perf — PASS x5 (hot-path: DrivePropThrown is a
  COLD event edge; the extern seam reads compile to the same direct loads; the header
  templates stay inlined; zero new dynamic initializers; latch/thread discipline unmoved).
  One cosmetic (a stale include comment) fixed in `c36ba368`.

## Queue after s28 (live scan)

save_transfer.cpp 925, meadow_db_sync.cpp 884, autotest_chippile.cpp 877 (single-family,
watch), player_handshake.cpp 828; near-cap: ue_wrap/actors/prop.cpp 799, item_activate.cpp
792, save_identity_bind.cpp 786, weather_sync.cpp 784, remote_player.cpp 781. Headers:
engine.h 1074 (watch), sdk_profile_names.h 860, protocol.h (constants, exempt).
