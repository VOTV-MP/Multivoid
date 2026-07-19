# Modularization / boundary-cleanup plan (per RULE 1 + folder-per-domain-concept)

> ## Execution ledger (2026-07-07, Opus 4.8 window) — all pushed, all behavior-preserving
> | Item | State | Commit |
> |---|---|---|
> | A1 spawn_menu DI (real layer breach) | DONE, audited | `96c50dfd` |
> | A2 delete dead `include/votv-coop/` | DONE | `91f31c6e` |
> | A4 `g_shuttingDown` file-private | DONE, audited | `2b0f9849` |
> | A3 `identity.h` reclassified (living doc, kept) | RESOLVED (no code) | `b961c6d9` |
> | D string-dedup MEASURED divergent → rescoped | corrected (no bad merge) | `96ec2068` |
> | C `engine_save.cpp` extracted (engine.cpp 1112→657, under cap) | DONE, byte-faithful | `245ae2a5` |
> | B5 autotest island → `src/harness/autotest/` (17 files) | DONE, renames | `9c93b0f3` |
> | B1a trash_use_intercept extraction | DONE, smoke-verified | `03d38d2b` |
> | B4 blob_chunks → `coop/net/` | DONE, behavior-identical | `7ba3d9e3` |
> | C engine_world/engine_spawn | DEFERRED — shared world-context, not a clean move; under cap |
> | **B1b grab-owner consolidation** | **CANCELLED — MEASURED mis-scoped: would CREATE smears (see B1 §)** |
> | **B2 prop-reconcile / B3 conversion-adoption** | **DEFERRED — measured REDESIGNs (not moves); modularity-only, need a dedicated hands-on window** |
> | reflection.cpp extraction | ~~NOT DONE — off-limits (§11 Opus substrate guard)~~ **DONE 2026-07-10** `6b9d9309` (Fable session, user blanket green light + the standing >800-queue proposal): FProperty walkers -> `reflection_props.cpp` (877 -> 651), byte-faithful, self-contained family |
>
> ## Execution ledger 2 (2026-07-10, Fable window, user-green-lit) — all pushed
> | Item | State | Commit |
> |---|---|---|
> | Placement series #5-#13 (see votv-folder-placement-audit-2026-07-10.md) | DONE — coop/{moderation,save,dispatch,config,world}/, ui/{multiplayer_menu,input_focus}, element/mirror_defer, items/inventory_pickup_sync; ini_config RETIRED | `c7c29894`..`4541e37f` |
> | prop_element_tracker census family -> `prop_census.cpp` (1066 -> 848) | DONE, detail header `prop_element_tracker_detail.h` | `b0b8040f` |
> | reflection FProperty walkers -> `reflection_props.cpp` (877 -> 651) | DONE, self-contained | `6b9d9309` |
> | prop_lifecycle destroy seam -> `prop_destroy_seam.cpp` (966 -> 804) | DONE, detail header `prop_lifecycle_detail.h` | `2a802322` |
> | event_dispatch_state INTENT family -> `event_dispatch_intent.cpp` (816 -> 636; 4th dispatch family) | DONE | `5b95c602` |
> | Series audit be98beb6..606fda3b | 0 CRITICAL / 0 HIGH; residue: tracker 848 / trash_channel 808 / lifecycle 804 marginally over cap (proposals queued) | — |
> | tracker key->live-actor index family -> `prop_key_index.cpp` (848 -> 662; maps private to one TU, tracker uses IndexKeyForActor_/EraseKeyIndexForActor_ via detail hdr) | DONE (eve) | `dab12a2e` |
> | trash_channel client grab/throw INTENT LANE -> `trash_grab_intent.cpp` (808 -> 486; owns HELD_BY + client toggles; core uses HeldByAny/ClearHeldBy/ResetIntentState via NEW `trash_channel_detail.h`). NOTE: the queued "clump-birth family" proposal was set aside on read — births are inseparable from TickCarry's prune/express; the intent lane is the self-contained concept | DONE (eve) | `1aa93f5e` |
> | prop_lifecycle takeObj container-extract seam -> `prop_container_extract.cpp` (804 -> 646; PRE/POST pair + `g_takeObjInFlight` defined there, shared via detail hdr; InstallInventory moved) | DONE (eve) | `a7f02f22` |
> | Dead residue: hand-item empty-streak debounce shell + dropGrabObject diag thunk + shutdown.h `<atomic>` | DONE (eve), filtered-staged around the held [ROCK-DROP] WIP | `fb490e36` |
>
> ## Execution ledger 3 (2026-07-18/19..07-19, s21b..s26 — the >800 queue, frozen-instrument recipe) — ALL PUSHED 2026-07-19 (d0c7b9e0..43426e82)
> | Item | State | Commit |
> |---|---|---|
> | drive_sync rack family -> `drive_rack_sync.cpp` (1007->606) | DONE, digest-equivalence proven | `73dc9ba1` |
> | session.cpp 9 scalar stream channels -> `net/session_streams.cpp` (1208->679+620) | DONE, mutant-proven 4p matrix + literal diff | `06921557` |
> | net_pump 915-line Tick -> `props/registry_reaper.cpp` (401) + `player/puppet_drive.cpp` (218); net_pump 1237->744 | DONE, same apparatus + menu-guard audit trace | `de249463` |
> | console_desk generic component calls -> `ue_wrap/core/component_calls.cpp` (1021->928) | DONE, smoke | `b5c1b911` |
> | console_desk ui_coordinates_C one-class-per-file split (928 -> 822 + coords_panel 173) | **DONE** (2026-07-19, 2-round /qf; seams = console_desk::AtlasUiCoordsSlot + CallUpdateCoordCoords publics, PlayScanEffects gates on coords_panel::Instance; literal body-diff PASS w/ mutate control, smoke PASS w/ both coords_panel log lines on both peers) | s24 |
> | console_desk residual 822>800 — comp-pane cut chosen over the v70 catch surface (MEASURED: the DL_* offsets straddle that cut, shared with the staying sim vector; comp is single-consumer). TWO commits: `f74d05dc` retires the positional g_fields table (named offsets, self-binding {name,&var} rows — /qf R1 find: literal-diff AND the compiler are both BLIND to a missed index renumber; correspondence script w/ mutate control) then `f9dfb5d5` moves the surface to NEW ue_wrap/desk/comp_pane (58+212; own g_required latch = 4 field offsets required, rest opportunistic; seams = NEW public console_desk::AtlasWidget() + Instance()). console_desk 822 -> **740, UNDER the cap**. Body-diff 11 regions + mutate PASS; smoke PASS both peers; audit 6/6 PASS 0 findings | **DONE** (2026-07-19 s24b, 6-round /qf) | `f74d05dc`+`f9dfb5d5` |
> | prop_identity (prop.cpp) / laptop lid axis | **MOOT** — re-measured 799 / 691, both under the soft cap | — |
> | weather_sync 1154>800 — the FAMILY axis (fog/lightning/redsky precedent; the ue_wrap daynight_cycle axis REJECTED — would fragment the cycle substrate). TWO commits: `828844b2` NEW coop/world/weather_rain (432+82: the rain+snow cycle-side sub-lane — own 5 mutator resolves + latch, causeRain echo interceptor in-module, ReadState/ApplyFromHost(cycle,payload,cur,&outcome), DebugForceRain/Snow, own IsLiveByIndex cache; orchestrator resolves its 3 observer targets INDEPENDENTLY — no fn-ptr handoff; fused "applied" line byte-identical via the outcome struct; RULE-2: 3 thin forwards RETIRED, 16-row caller census migrated) then commit 2 NEW coop/dev/weather_probe (110+23: the ini-gated [probe weather]/[probe wind] block + ReadComponentIsActive; counters via accessors, cycle via weather_rain::Cycle()). weather_sync 1154 -> 850 -> **784, UNDER the cap**. Evidence: 315+75 moved lines diff w/ mutate controls; baseline-first gated smokes (imported lan-test literals 4/6/75, probes >=88, 4/4 settles, final cross-peer parity, injection-proven WARN gate) | **DONE** (2026-07-19 s25, 7-round /qf "that holds") | `828844b2`+`cd59ad13` |
>
> | autotest.cpp 1002>800 -- the island's own one-feature-per-file convention applied (audit flag resolved). THREE commits: `89ce6602` ReadEnv retirement in-place (8 sites -> coop::config::ReadEnv, byte-identical helper; isolated so the move diff stays verbatim) then `f299107c` the 9-routine dissolve into 6 new TUs (clump 200 / weather 198 / flashlight 154 / worldrules 69 / worldctx 49 / tracker_selftest 77; residual grab-only; 8 unused includes pruned; autotest.h re-anchored) then `cc4c93c3` pure git mv -> autotest_grab.cpp (99% rename, --follow survives; fused mv+strip would have been ~41% < the 50% threshold). autotest.cpp 1002 -> **393, UNDER the cap**. Evidence: body-diff 556 nb-lines verbatim + residual sequence-equality w/ 7 must-FAIL mutate controls; TWO differential smoke pairs exercising ALL TEN routines (pair A 8 scenarios / pair B grab+clumpvis, one-writer-per-axis census; 36 verdict keys identical baseline vs post; baselines on post-commit-0 bytes) | **DONE** (2026-07-19 s26, 4-round /qf "that holds") | `89ce6602`+`f299107c`+`cc4c93c3` |
>
> | kerfur_convert 1259>800 -- the three-lane axis (detection / client half / host executor), 5-round /qf "that holds". TWO commits: `bcd7b44b` NEW coop/creatures/kerfur_convert_client (395+76: ghost custody Claim/Cleanup/Take + CollectMirrorActors + the KerfurConvert wire apply; custody data+ops whole in one TU) then `bd82c596` NEW kerfur_convert_host (390+83: converge family + PickDropPropFn + OnConvertRequest + the request-verb bracket incl. ActiveRequestVerbEid + the NEW one-way RecordSeamConvergedInBracket for the residual destroy seam). Two-layer Install handoff (SetSession per-call; SetClasses every attempt; SetVerbs+latch at the success site only). ONE documented fail-closed deviation: DISABLED-state requests now DROP instead of CallFunction-ing signature-changed verbs over a zeroed frame (a latent pre-cut over-read; unreachable on the current build). kerfur_convert 1259 -> **633 UNDER the cap**. Evidence: kerfur_body_diff_c2.py 18 spans verbatim + residual sequence-equality + 5 must-FAIL mutates (incl. the gate line); kerfurtoggle differential batched | **DONE** (2026-07-19 s27, 5-round /qf) | `bcd7b44b`+`bd82c596` |
>
> | autotest_vitals 1013>800 -- the s26 island recipe verbatim (the PuppetFrame flag resolved). FOUR commits: `ba317803` ReadEnv retire (6 sites -> cfg::ReadEnv) then `247a7037` FOUR one-feature TUs (damage 238 flash-e2e / dmghazard 252 / playerdmg 158 / puppetframe 175; THREE damage TUs -- grep proved zero shared code; per-TU WaitDone copies = the island's measured convention) then `c43d8878` pure git mv -> autotest_ragdoll.cpp (100% rename, --follow intact) + `d7899730` doc-header fix. autotest_vitals -> **ragdoll 373 UNDER the cap**. Evidence: vitals_body_diff.py 8 spans + residual sequence-equality + 5 must-FAIL mutates; V1-V4 differential pairs batched | **DONE** (2026-07-19 s27, 4-round /qf "that holds") | `ba317803`+`247a7037`+`c43d8878`+`d7899730` |
>
> | harness 1223>800 -- the B5-residual axis: session lifecycle driver vs process boot, 7-round /qf. THREE commits: `8a9c509c` the Tier-C nick-color parse -> coop::nick_color::SetInitialLocalFromIniHex then `e6f8576e` RULE-2 retire of the DEAD netloopback scenario (a stub since PR-2 2026-05-28; nothing launches it) + the displayOffsetX loopback knob it alone fed (net_pump::Tick param -> DriveTick -> the one shift line; the R6 alias-vocabulary census caught what the name-grep could not) + all doc rows same-commit then `a48b21d8` NEW harness/session_runtime (709+57: g_session moves to its lifecycle OWNER + Session() accessor x3 in Start; boot/bringup/RunPlayLoop; the pump template verbatim-internal -- possible only because B killed its last external consumer; 26 dead residual includes pruned by census). harness.cpp 1223 -> **526 UNDER the cap**. Evidence: harness_body_diff.py 12 spans + residual sequence-equality + 4 must-FAIL mutates; every batched smoke pair rides the boot path | **DONE** (2026-07-19 s27) | `8a9c509c`+`e6f8576e`+`a48b21d8` |
>
> | remote_prop 1180>800 -- the lane-split family continued (spawn/destroy precedent). TWO commits: `6c910046` NEW remote_prop_convert.cpp (291: OnConvert verbatim, the pile<->clump re-skin receiver; ZERO new seams -- every dep was already public or on remote_prop_internal.h; 4 span-only includes pruned) then `d0c7879e` NEW remote_prop_physics.cpp (196: the reflected UPrimitiveComponent thunk group + Aprop_C.thrown; ONE linkage flip DrivePropThrown anon->named via internal.h, single caller OnRelease). remote_prop 1180 -> **758 UNDER the cap**. Evidence: diff_c1/diff_c2 instruments (spans verbatim-contiguous; scaffolds + internal.h enumerated; residual sequence-equal) + 4+5 must-FAIL mutates; NO smoke (user directive s28) | **DONE** (2026-07-19 s28, 5-round /qf) | `6c910046`+`d0c7879e` |
>
> | npc_sync 989>800 -- the install/bootstrap axis (the s27 session_runtime accessor-with-its-state shape). ONE commit: `fd7c7409` NEW npc_sync_install.cpp (327: Install + IsInstalled + GetDevSpawnRefs + IsHostNpcSyncDisabled + the install-owned latches/caches) + NEW npc_sync_internal.h (45: FIVE shared globals defined install-side, extern for the seam TU's hot paths -- the /qf R2 census killed a 6th, g_npcSpawnFn, all-mover private; THREE callbacks flip anon->named for cross-TU registration). ONE enumerated edit: Install's store -> SetSession(session) (body verified byte-exact). npc_sync 989 -> **709 UNDER the cap**. Evidence: diff_npc.py (6 spans; the edited line guarded BOTH sides; internal.h exact-content) + 7 must-FAIL mutates; NO smoke (user directive) | **DONE** (2026-07-19 s28, 3-round /qf) | `fd7c7409` |
>
> | ue_wrap/actors/puppet 972>800 -- the SPAWN-path axis. ONE commit: `ca12e11d` NEW puppet_spawn.cpp (512: GetSpawnMeshOffsetZ + the static 363-line SpawnPuppetMainPlayer orphan rig + SpawnPuppet + the head-gate hook block moved WHOLE with its sole install site -- the /qf R1 find that dissolved both the planned InstallHeadGateHook seam AND the anon-close relocation) + NEW puppet_internal.h (40: ReadPtr/ReadAt/WriteAt templates verbatim + extern g_meshComp + LiveAnimInstance decl; two named flips). call.h + ufunction_hook.h includes moved/pruned (cut-created dead); pre-existing dead GT alias flagged NOT touched. puppet 972 -> **491 UNDER the cap**. Evidence: diff_puppet.py (4 spawn spans + 2 relocated residual blocks at NEW positions; internal.h exact-content -- the m6 mutate EXPOSED a whitelist-misses-dropped-lines instrument blind spot, fixed to exact-compare before it could hide anything) + 7 must-FAIL mutates; NO smoke (user directive) | **DONE** (2026-07-19 s28, 2-round /qf) | `ca12e11d` |
>
> Residue >800 after ledger 4 (upd. 07-19 s28, LIVE `wc -l` scan): the s27 three-item head of the queue is CLOSED (remote_prop 758; npc_sync 709; puppet 491). Remaining: save_transfer.cpp 925, meadow_db_sync.cpp 884, autotest_chippile.cpp 877 (single-feature family, borderline-exempt, watch), player_handshake.cpp 828 (Tier-C row exists). Near-cap watch (s28 perf audit): ue_wrap/actors/prop.cpp 799, item_activate.cpp 792, save_identity_bind.cpp 786, weather_sync.cpp 784, remote_player.cpp 781. Headers: engine.h 1074 (wrapper decl umbrella, watch), sdk_profile_names.h 860 + protocol.h (constants, exempt).
> console_desk CLOSED at 740; weather_sync CLOSED at 784; autotest.cpp CLOSED (dissolved, grab residual 393). protocol.h exempt (constants header).
>
> **(2026-07-07 claim, superseded by ledger 3 above for the >800 queue):** the A-D modularization was COMPLETE at the RULE-1-correct boundary. Every safe/valid extraction
> shipped (A/D/C-engine_save/B5/B1a/B4). B1b was measured to be mis-scoped (executing it would
> ADD smears — the current homes are already correct). B2/B3 are genuine multi-subsystem redesigns
> for modularity-ONLY gain (no bug), reserved for an explicit per-rule-1 window with hands-on.
> reflection.cpp is substrate (scope guard). Nothing safe/valuable remains to extract.

Written 2026-07-07 by the Fable-5 session at the user's request, right before the switch
to Opus 4.8, from a three-agent read-only survey of `src/votv-coop` (LOC census + internal
concern maps + boundary audit). This is a PLAN of PROPOSALS — every claim below was spot-
checked in code the day it was written, but you (Opus) MUST re-verify each item against the
current tree before executing it (labels are claims, code is truth — the documentize rule).

**How to execute (OPUS_48_DISCIPLINE applies):** one concept per commit, build (Release) +
audit each, no batching. Do the tiers in order — Tier A is correctness of the layering
(cheap, high value), Tier B is the real per-RULE-1 architectural win (one owner per concept),
Tier C is mechanical LOC relief. Do NOT start Tier B/C mid-arc if a gameplay task is open;
this is refactor debt, safe to interleave between features. Nothing here changes wire format
or behavior — if a step would, you mis-scoped it.

RULE 2 reminder: when you extract, the old location's code GOES — no parallel copy, no
back-compat alias, no "kept for now". A move is a move.

---

## Tier A — true boundary violations (fix first; small, decisive)

> **Live status** (updated as executed by the Opus 4.8 window):
> - **A1 — DONE** (commit pending, built Release clean). DI applied: `spawn_menu::Open/Close/Toggle`
>   now take `void* localPlayer`; the coop-side caller (`spawn_menu_unlock.cpp`) resolves
>   `Registry::Get().Local()` on the game thread and injects it. `ue_wrap/spawn_menu.cpp` no
>   longer includes any `coop/` header — verified coop-free. Behavior-preserving.
> - **A2 — DONE** (commit pending). `git rm`'d the two empty `.gitkeep` placeholders; the dead
>   `include/votv-coop/` tree is gone. Zero includers, not a CMake include root — no compile impact.
> - **A3 — RESOLVED, no code change.** Reclassified: `identity.h` is a living architecture-doc +
>   umbrella-include header (content is current, not stale) — the audit mislabeled it as "dead".
>   Kept as-is (see below).
> - **A4 — DONE** (commit pending, built Release clean). `g_shuttingDown` is now file-static
>   (internal linkage) in `shutdown.cpp`; `IsShuttingDown()` moved out-of-line into the .cpp;
>   `shutdown.h` keeps only the `bool IsShuttingDown();` declaration. NO public setter added —
>   the survey/plan assumed an external writer, but grep proved the sole writer is
>   `DoShutdown()`'s own compare_exchange (same TU), so a setter would be speculative (YAGNI).

### A1. `ue_wrap/spawn_menu.cpp` reaches UP into `coop` [the one real layer breach] — DONE
`src/votv-coop/src/ue_wrap/spawn_menu.cpp:5` includes `coop/player/players_registry.h`;
lines 55 + 155 call `coop::players::Registry::Get().Local()`. This inverts principle 7
(ue_wrap must never know coop). Verified 2026-07-07.
- **Root fix:** ue_wrap must not resolve the local player. Two clean options, pick per
  what the two call sites actually need:
  (a) **Dependency injection** — the two ue_wrap entry points take the local-player pawn
      `void*` as a parameter; the coop-side caller (who already has the Registry) passes it.
  (b) **Relocate** — a "spawn menu" (the Q-menu) is gameplay/UI, not a thin engine-class
      wrapper. If the file is mostly coop policy wearing a ue_wrap coat, move it to
      `coop/player/` (or `coop/interactables/`) and keep only the genuinely engine-primitive
      parts (UMG widget calls) behind an `ue_wrap` API it calls DOWN into.
  Prefer (a) if the file is truly engine-mechanism; prefer (b) if it's policy. Read the 227
  lines first and decide from the ratio.

### A2. Delete the dead duplicate include tree `include/votv-coop/`
`include/votv-coop/coop/.gitkeep` + `include/votv-coop/ue_wrap/.gitkeep` — two empty
placeholder dirs shadowing the real `include/coop` + `include/ue_wrap`. Nothing includes
from `votv-coop/...` (grep = 0 hits). RULE 2 baggage. `git rm` both.

### A3. `include/coop/element/identity.h` — RECLASSIFIED: NOT dead code, KEEP (no action)
The boundary audit flagged this as a "dead API header." Read 2026-07-07: it is NOT dead and
NOT a violation. Lines 1-97 are a living **architecture-doc** for the whole `coop::element`
identity layer — and the content is CURRENT, not stale: it accurately describes the built
state (`element::Registry` as the unified eid<->actor owner, `MirrorManager<T>`,
`ElementDeleter`, `quiescence_drain`, the sealed-`Install` compile wall, and the
HOST-AUTHORITATIVE / CLIENT-RELAY-INTENT / PEER-SYMMETRIC authority contract). Lines 99-104
are an **umbrella include** (`quiescence_drain.h` + `identity_create.h` + `identity_destroy.h`).
It simply has no `#include` sites yet — that makes it unused, not stale.
- **Decision: KEEP as-is.** Deleting it would lose current, accurate architecture documentation
  (RULE 2 is about retired CODE, not a living design note). Do NOT delete.
- Optional, LOW value: realize its stated intent by having the `coop::element` consumers include
  the umbrella `identity.h` instead of the three headers individually — but that is include-churn
  across many TUs for near-zero gain and risks each TU pulling more than it needs. Not worth it;
  left as a documentation/umbrella header. This item needs no code change.

### A4. Tighten `g_shuttingDown` global — DONE
`include/coop/session/shutdown.h:48` exposed `extern std::atomic<bool> g_shuttingDown;` as a
mutable global across the module boundary. **As built (2026-07-07):** the atomic is now
file-static (internal linkage) in `shutdown.cpp`; `IsShuttingDown()` is a real out-of-line
function there; the header exposes only `bool IsShuttingDown();`. NO setter was added — the
original plan text assumed an external writer, but a grep proved the ONLY writer is
`DoShutdown()`'s compare_exchange (same TU), so a public `SetShuttingDown` would be
speculative (fix-then-generalize / YAGNI). All 36 `IsShuttingDown()` callers rebuilt clean.

---

## Tier B — the real RULE-1 win: dissolve three cross-cutting SMEARS into one owner each

These are the ANTI-SMEAR rule applied at folder scale: a concern with ONE clear owner is
currently spread across 3+ files. This is the work that actually makes the project "properly
modular", not the LOC counting. Each is a multi-file arc — treat like a feature: design from
the existing homes, MTA-check the shape, audit after.

### B1. The "grab / held-item" concern is smeared across 3 files
> **B1a DONE + SMOKE-VERIFIED (2026-07-07, commit pending).** Extracted the InpActEvt_use
> client-grab interceptor family (`OnPileUseIntercept`/`OnPileUseDenySuppress`/
> `OnPileUseReleaseSuppress`/`OnFirePre` + the `_41`/`_38`/`_42`/`_58`/`_59` registration +
> `g_cancelPairedUseRelease`) out of trash_collect_sync into `coop/props/trash_use_intercept.
> {h,cpp}`. The 4 functions moved byte-identically; Install/OnDisconnect delegate (own session
> cache — standard per-module pattern, NOT a shared global). **trash_collect_sync 810→498,
> trash_use_intercept ~370** (both under cap). Audit: 5/5 wiring points faithful. **Autonomous
> LAN smoke PASS**: both peers connect, all interceptor seams install (2/2, 2/2) on host+client,
> retained observers arm, grab test drives held-state pipeline, 0 err/warn from the area, clean
> exit. Full client-grab BEHAVIOR still wants a user hands-on E-press (autonomous smoke can't
> press E), but registration + pipeline + no-regression are proven.
> **B1b — CANCELLED as mis-scoped. MEASURED 2026-07-07 (Opus): executing it would CREATE
> smears, not remove them.** The survey saw the `GrabObserver_*` names in prop_lifecycle and
> assumed they belonged with `coop/props/grab_observer`. They do NOT. Three code facts refute
> the plan:
> 1. `grab_observer.cpp` is a self-contained DIAGNOSTIC physics-pickup LOGGER (Stage-1 RE, all
>    observers are UE_LOGI-only; the file even labels AddImpulse "diagnostic, not shipped"). It
>    does ZERO wire-broadcast/identity work. Folding the wire-broadcasting takeObj observers into
>    it would MIX two concepts (diagnostic logging vs wire identity) — a NEW folder-rule violation.
> 2. `g_takeObjInFlight` is read/written ONLY inside prop_lifecycle.cpp (82/171/383/390/467/917):
>    the takeObj PRE sets it so the CORE `GrabObserver_Aprop_Init_POST` spawn-catch (line 171)
>    skips MarkPropElement for a container-extract. takeObj + Init-POST are ONE coupled spawn-catch
>    machine. Moving takeObj out would SPLIT a mutable flag across two files — the exact cross-file-
>    global anti-smear the folder rule forbids. takeObj BELONGS in prop_lifecycle.
>    *(2026-07-10 eve `a7f02f22` nuance: the soft-cap extraction moved the takeObj pair to
>    `prop_container_extract.cpp` — a SIBLING TU of the SAME module (namespace coop::prop_lifecycle),
>    flag shared via the module's own `prop_lifecycle_detail.h` (the session_lanes.h precedent, same
>    as `g_session_ptr` with prop_destroy_seam). That is a within-module file split, NOT the
>    cross-CONCEPT move into grab_observer this point forbids — the B1b cancellation stands.)*
> 3. `EnsureHeldItemBroadcast` (trash_collect_sync.cpp:229) is a shared held-item broadcast SERVICE
>    called from 3 sites (hand_item, local_streams, net_pump) — not a diagnostic observer; it is
>    correctly a residual trash/prop service, not grab_observer material.
> **The `GrabObserver_*` prefix in prop_lifecycle is a MISNOMER** (these are prop SPAWN-CATCH
> observers, not physics-grab observers) — that name is what mis-led the survey. The only
> defensible follow-up is an optional pure-rename of those prop_lifecycle symbols to accurate
> names (`PropSpawnCatch_*`) to prevent a future survey repeating this error; low value, internal-
> only, deferred unless requested. NET: the current homes are already RULE-1 correct — do NOT
> execute B1b.

- ~~`prop_lifecycle.cpp:379-510`~~ take-obj observers + `InstallInventory` (now `prop_container_extract.cpp`, `a7f02f22`)
- `trash_collect_sync.cpp` — BeginDeferredSpawn observer + `EnsureHeldItemBroadcast` (line refs shifted by the `fb490e36` thunk retirement)
- `remote_prop_convert.cpp` — `OnConvert` (grab-driven form change; extracted from remote_prop.cpp s28 `6c910046`)
The includes already point at an existing intended home: **`coop/props/grab_observer`**.
**Plan:** consolidate the grab/held-item observers + broadcast into `grab_observer.{h,cpp}`
as the single owner (CAPTURE the grab edge in one place; other modules subscribe). B1a (above)
already carved out the interceptor half.

### B2. "Prop-position reconcile" (pile/kerfur divergence) squats in two files
> **MEASURED 2026-07-07 (Opus): NOT a byte-faithful move — this is a REDESIGN, not an extraction.**
> The two blocks delegate to prop_element_tracker at the LEAF (`Registry::Get().Get(eid)`), but
> their ORCHESTRATION is inseparable from their host files' file-local state + lifecycle timing:
> - net_pump reaper (402-738) reads/writes net_pump statics `g_everInGameplayThisSession`,
>   `g_fleeing`, `sNextReap`, `g_reapWorld/Idx`, and CALLS the file-local `TearDownCoopStateForSessionEnd(session)`
>   + `session.running()` — the RAM-balloon/flee guard is woven THROUGH the reap scan (piggybacks its 4s world walk).
> - save_transfer flush (541-730) reads save_transfer's OWN join-window maps `g_blobPileXforms`,
>   `g_lastFlushedPilePos`, `g_pileFlushArmUntil` (populated BY the join blob transfer — this is
>   save_transfer's data by construction) + the per-slot arm window.
> Moving either "into prop_element_tracker" requires inventing a NEW API to thread session +
> flee/teardown + join-slot state across the boundary — a multi-subsystem redesign (>3 subsystems,
> OPUS §1 STOP) for MODULARITY-ONLY gain (the smear is not a bug). DEFER pending explicit
> per-rule-1 green light that accepts a gameplay-lifecycle redesign + a real hands-on (join +
> pile-move) to verify — the autonomous smoke cannot exercise the join-window flush or the
> quit-to-menu balloon guard.

- `save_transfer.cpp:541-730` (~190 LOC) — pile/kerfur divergence flush riding save-transfer timing
- `net_pump.cpp:402-738` (~330 LOC) — prop-reaper + world-change re-seed episode machine
Both DELEGATE into `prop_element_tracker` (the real owner of eid<->actor transforms).
**Plan:** move the divergence-flush + reaper/re-seed episode logic OUT of save_transfer and
net_pump INTO `prop_element_tracker` (or a sibling `prop_reconcile.{h,cpp}` under `coop/props/`
if that keeps the tracker under the soft cap). save_transfer/net_pump keep only the trigger
call. This is also the single biggest LOC relief for net_pump (1183->~850) and save_transfer.

### B3. "Conversion / adoption" squats across three files
- `remote_prop_convert.cpp` — `OnConvert` apply (extracted s28 `6c910046`)
- `kerfur_convert.cpp:398-737` (~340 LOC) — parked-ghost claim/adopt machinery
- `remote_prop_spawn.cpp:~600-700` — kerfur fuzzy-match + rekey
Existing homes referenced from net_pump: **`coop/creatures/npc_adoption`** +
**`kerfur_prop_adoption`**.
**Plan:** the deferred-adoption sub-system (parked-ghost claim, fuzzy-match rekey) consolidates
into the adoption files; `OnConvert` apply either joins `kerfur_convert` (owns the protocol) or
becomes `remote_prop_convert.cpp`. Design this one carefully — it's the most entangled; MTA has
no direct analog, so cite the reasoning inline.

### B4. Folder-per-concept: two "session" concepts named identically across two folders
- `coop/net/` = the TRANSPORT/wire layer (`coop::net::Session` — GNS connections, net thread,
  lanes, lobby, ICE, protocol.h). Concept: **wire transport**.
- `coop/session/` = session LIFECYCLE (manager, handshake, join curtain, save guards, pause,
  moderation, event dispatch). Concept: **session lifecycle**.
Two files sit in the wrong folder by concept:
  - `coop/session/blob_chunks.cpp` — self-described "chunked blob TRANSPORT over the reliable
    lane" -> belongs in `coop/net/`. **DONE 2026-07-07** (`git mv` .cpp+.h into `coop/net/`;
    6 include paths + 1 CMake path updated; namespace was already `coop::blob_chunks`, no rename;
    diff is ONLY the path relocation; built Release clean, links). Behavior-identical.
  - `coop/session/net_pump.cpp` — the per-tick net ORCHESTRATOR; it's the seam between transport
    and gameplay. Judgment call: it drives puppets (gameplay) from net state, so it may
    legitimately live in session/ as the lifecycle-driven pump — but its NAME says "net". Decide
    the concept crisply (is it "the per-tick coop orchestrator"? then maybe rename, don't move).
Do NOT rename the folders themselves (`net` = transport and `session` = lifecycle are both
crisp concepts). Just relocate `blob_chunks` and settle net_pump's identity. Low risk.

### B5. Harness mixes production boot-glue with ~2800 LOC of dev autotests — DONE (.cpp island)
**As built (2026-07-07):** all 17 `autotest_*.cpp` (~4900 LOC total incl. autotest.cpp 953,
autotest_vitals.cpp 1013, autotest_chippile.cpp 877, ...) moved via `git mv` into
`src/harness/autotest/`; the 17 CMake source paths updated. Recorded as pure renames (zero
content change); no cross-`.cpp` includes existed and all headers resolve via the `include/`
root, so nothing else changed. Built Release clean, dll links. `src/harness/*.cpp` now holds
only boot-glue + support (harness.cpp, harness_diag.cpp, config.cpp, screenshot.cpp,
sdk_check.cpp) — the "trivially separable dev tooling" claim in harness.h:9 is now structurally
true for the compilation units. NOT moved (deliberate, low-value churn): the `include/harness/
autotest*.h` headers stay flat under `include/harness/` (already clearly named; moving them
would touch every includer for no real gain).

### B5-ORIG. Harness mixes production boot-glue with ~2800 LOC of dev autotests
`include/harness/harness.h:9` claims the subtree is "dev tooling, trivially separable" — but
`harness.cpp` owns `g_session` (the production session) + wires every sync module
(`StartCoopSession`, lines 392-459). Meanwhile `src/harness/autotest_*.cpp` (~16 files,
autotest.cpp 953 + autotest_vitals.cpp 1013 + autotest_chippile.cpp 877 + ...) are pure dev
test routines in the same folder.
**Plan:** split the concept. Production boot/scenario glue (`Start`, `TimelineThread`,
`RunPlayLoop`, the `Boot*`/`Drive*` orchestrators, `StartCoopSession`) is the real "harness".
The `autotest_*` family is DEV TOOLING — move it to its own subtree (`src/harness/autotest/` or
`src/dev/autotest/`) so the "trivially separable" claim becomes true. RULE 3 flavor: none of
autotest ships, so making it a clean island is aligned. Big LOC move, zero behavior change —
do it as a pure file-move commit (git mv + include-path fix), audit that the build still links.

---

## Tier C — mechanical >800-LOC extractions (soft-cap relief, low risk)

Each is a single-file extraction: pull a cohesive block into a new `.{h,cpp}` pair under the
same folder, leave a call. Ordered by value (biggest catch-all first). Re-measure LOC before
each (`wc -l`) — some may already be handled by Tier B.

> **Live status:** **`engine_save.cpp` DONE** (commit pending). Extracted the save-load/GameMode/
> campaign-cache/fresh-game/return-to-menu block (engine.cpp L168-621, 454 lines) into
> `ue_wrap/engine_save.cpp` (484 LOC). Measured self-contained first (its file-private statics
> don't leak out; it calls none of engine.cpp's other file-private helpers; its public fns are
> declared in engine.h so callers are unaffected). **engine.cpp 1112 -> 657 LOC — now UNDER the
> 800 soft cap**, so the further engine_world/engine_spawn splits are optional (cohesion, not
> cap-driven). Byte-faithful move verified (deleted lines == moved lines, diff empty); built
> Release clean; both TUs compile, dll links.

| File | LOC | Extract | Into | Notes |
|---|---|---|---|---|
| `ue_wrap/engine.cpp` | 1112->657 | save/GameMode/menu block L168-621 (454) | `engine_save.cpp` (484) | **DONE** — engine.cpp now under the 800 cap |
| `ue_wrap/engine.cpp` | 657 | world-context + spawn | ~~engine_world/spawn~~ | **DEFERRED — measured NOT a clean move.** `EnsureWorldContext`/`g_worldContext` are cross-cutting file-privates shared by console + pause + spawn + world-recovery; splitting spawn out needs a NEW shared internal world-context API + edits to console/pause (a real refactor, not a byte-faithful move). engine.cpp is already under the 800 cap, so this is unjustified now (§11 bias-smaller). Revisit only if engine.cpp grows back over cap. |
| `ue_wrap/engine.h` | 1032 | split into per-domain headers mirroring the .cpp set | (several) | every consumer currently drags the whole 1032-line header; do LAST, after the .cpp splits settle |
| `props/remote_prop.cpp` | 1174 | `OnConvert` L814-1071 (~258) | `remote_prop_convert.cpp` | **DONE** (s28 `6c910046` + physics cut `d0c7879e`; residual 758) |
| `props/remote_prop_spawn.cpp` | 972 | `OnSpawn` is a 740-line monolith — split by PHASE | (in-file or siblings) | phases: unkeyed-drop / trash-proxy / exact-key converge / kerfur fuzzy-rekey / deferred finish. Kerfur rekey -> B3 |
| `creatures/npc_sync.cpp` | 964 | `NpcSuppress_Interceptor` L330-524 (~195) + `Install` L595-809 | `npc_sync_install.cpp` | **DONE** (s28 `fd7c7409`: Install axis chosen, interceptor stays with its seam state; residual 709) |
| `props/prop_element_tracker.cpp` | 1030 | `DebugCheckPropElementReap` self-test L926+ (~104) | a `_test`/dev TU | plus B2 moves reconcile IN — net LOC may rise; watch the cap |
| `coop/save/save_transfer.cpp` (moved 2026-07-10) | 838 | pile/kerfur divergence L541-730 | -> B2 target | same move as B2 |
| `session/player_handshake.cpp` | 828 | UTF-8 codec L132-181 + nick-color/skin wire pack-parse L90-131 | shared string util + `nick_color`/`nameplate` | codec is a generic dup (see D) |
| `props/trash_collect_sync.cpp` | 810→498 | use-interceptor family (~258) | `trash_use_intercept.{h,cpp}` (~370) | **DONE (B1a, smoke-verified `03d38d2b`)** — see the B1 section |
| `world/weather_sync.cpp` | ~~1141~~ 784 | **DONE 07-19 s25** (ledger 3): weather_rain + weather_probe extracted | family axis | closed under cap |
| `net/session.cpp` | 885 | `HandleMessage` switch L324-606 | per-message handlers | conceptually core routing — split only if it keeps growing |
| `harness/harness.cpp` | ~~1204~~ **526, CLOSED 07-19 s27** | nick-color parse -> `nick_color::SetInitialLocalFromIniHex` (`8a9c509c`); netloopback+displayOffsetX RULE-2 retire (`e6f8576e`); session_runtime extraction (`a48b21d8`) | | see ledger 3 |

---

## Tier D — duplicated helpers (collapse to one owner)

The survey found genuine copy-paste with no shared home. Each is a small, safe dedup —
**EXCEPT the string one, which a 2026-07-07 measurement (below) proved is NOT mechanical.**

- **String narrow/widen — MEASURED 2026-07-07: NOT a mechanical dedup. The copies DIVERGE by
  design; do NOT naive-merge (it would change non-ASCII behavior — RULE 1 trap).** The actual
  implementations fall into three semantic classes:
  1. **ASCII class-name narrowing** — `NarrowAscii` in identity_create.cpp:50 (`c & 0xFF`),
     kerfur_entity.cpp:71 + trash_channel.cpp:113 (raw `(char)c`), order_sync.cpp:84 (ASCII-guard
     with `'?'` fallback). All inputs are ASCII BP class/order names, so they AGREE in practice but
     differ on non-ASCII. These four CAN share one canonical (pick the defensive `'?'`-guard —
     strictly safer, identical for ASCII). Safe subset.
  2. **Full UTF-8 Win32 codecs** — `Widen` in http_client.cpp:29 (`MultiByteToWideChar(CP_UTF8)`),
     `ToUtf8` in player_handshake.cpp:132 (`WideCharToMultiByte(CP_UTF8)`, returns `vector<uint8_t>`).
     Real converters; could share a Win32-based canonical, but note the DIFFERENT return types.
  3. **Specialized — do NOT merge.** chat_feed.cpp:57 `ToUtf8` is a hand-rolled UTF-8 encoder that
     DELIBERATELY strips control chars (`cp < 0x20`); client_model.cpp:31 `Widen` is a naive ASCII
     byte-widen (`wstring(s.begin(),s.end())`, "names are validated ASCII"). These are intentional
     specializations; merging them loses behavior.
  **Home constraint:** the folder rule forbids `coop/util/` (catch-all name). The two existing
  string headers (`ue_wrap/fstring_utils.h`, `ue_wrap/ftext_utils.h`) are FString/FText engine
  MINTERS, not generic `wstring<->string` codecs — so there is no ready home. A crisp home must be
  chosen (candidate: fold class-1 into `ue_wrap/reflection` since every call site narrows a
  `ClassNameOf()` output; or a new crisply-named header). **Scope: do class-1 only (safe), leave
  class-3 alone, class-2 optional. This is NOT the "highest-count easy win" the first draft claimed.**
- **Local-player-pawn lookup: ~9 inline sites** of `R::FindObjectByClass(P::name::MainPlayerClass)`
  (engine.cpp:379,508; autotest_grab.cpp:86 + autotest_flashlight.cpp:76 [ex-autotest.cpp, s26 dissolve]; autotest_saveui.cpp:143; join_membership_sweep.cpp:624;
  remote_player.cpp:67) plus two differently-named local wrappers (`LocalPlayerPawn`,`LocalPawn`).
  **Fix:** one `ue_wrap::engine::GetLocalPlayerPawn()`; replace all sites. (Also unblocks A1's
  injection option — the coop caller can use the Registry, ue_wrap uses this.)
- **Resolve-once-then-latch + throttled-scan idiom:** hand-rolled in ~a dozen ue_wrap TUs
  (daynightcycle, drone, wisp, save_browser, trace, prop, console_desk, engine...), with comments
  cross-referencing each other as a copied "shape". **Fix (optional, higher effort):** a shared
  `ue_wrap::ResolveLatch` / `ThrottledScan` helper. Lower priority — it's an idiom, not a bug;
  only worth it if you're already touching several of these.

---

## Sequencing recommendation for Opus

1. **A2, A3, A4** first — trivial deletions/tightening, one commit, proves the build loop.
2. **A1** — the one real breach; small, decisive; pairs naturally with the D "GetLocalPlayerPawn" dedup.
3. **D string-conv dedup** — safe, high-count, mechanical; good confidence-builder.
4. **B5 (autotest island move)** — pure git-mv, big LOC win, zero behavior risk.
5. **C engine.cpp splits** — mechanical, well-bounded, the catch-all most needs it.
6. **B1 / B2 / B3** — the real architectural arcs; do ONE at a time, design + audit each, treat
   as features (they touch gameplay paths — smoke after each per the pre-deploy checklist).
7. **B4** (blob_chunks move) + remaining C rows as cap-relief between the above.

Everything in Tiers A/C/D is behavior-preserving and can ship without a hands-on (build +
link + the existing autotest smoke suffice). Tier B touches live gameplay seams — full
pre-deploy checklist + a real smoke before handoff.

Related: `[[feedback-folder-per-domain-concept-rule]]` · `[[feedback-modular-file-size-rule]]`
· `[[feedback-one-owner-order-axis]]` · `docs/OPUS_48_DISCIPLINE.md`.
