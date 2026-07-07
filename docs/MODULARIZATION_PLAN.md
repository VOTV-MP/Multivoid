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
> | C engine_world/engine_spawn | DEFERRED — shared world-context, not a clean move; under cap |
> | **Tier B smears (B1/B2/B3)** | **NOT STARTED — need a hands-on smoke window (touch live gameplay)** |
> | reflection.cpp extraction | NOT DONE — off-limits without explicit user direction (§11 substrate guard) |
>
> **Safe autonomous lane (behavior-preserving, no smoke) is now EXHAUSTED.** What remains is
> the high-value Tier B (one-owner-per-smear) work, which is smoke-gated, plus the substrate
> items the scope guard reserves for explicit direction.

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
> **B1b (remaining, NOT started):** the actual smear consolidation — fold `prop_lifecycle`'s
> take-obj observers + the held-item broadcast into ONE grab owner. Bigger/riskier; assess
> whether it's worth it now that the interceptor is cleanly separated.

- `prop_lifecycle.cpp:379-510` — take-obj grab observers (`TakeObj_PRE/_POST`) + `InstallInventory`
- `trash_collect_sync.cpp:94-437` — BeginDeferredSpawn observer + `EnsureHeldItemBroadcast`
- `remote_prop.cpp:814-1071` — `OnConvert` (grab-driven form change)
The includes already point at an existing intended home: **`coop/props/grab_observer`**.
**Plan:** consolidate the grab/held-item observers + broadcast into `grab_observer.{h,cpp}`
as the single owner (CAPTURE the grab edge in one place; other modules subscribe). B1a (above)
already carved out the interceptor half.

### B2. "Prop-position reconcile" (pile/kerfur divergence) squats in two files
- `save_transfer.cpp:541-730` (~190 LOC) — pile/kerfur divergence flush riding save-transfer timing
- `net_pump.cpp:402-738` (~330 LOC) — prop-reaper + world-change re-seed episode machine
Both DELEGATE into `prop_element_tracker` (the real owner of eid<->actor transforms).
**Plan:** move the divergence-flush + reaper/re-seed episode logic OUT of save_transfer and
net_pump INTO `prop_element_tracker` (or a sibling `prop_reconcile.{h,cpp}` under `coop/props/`
if that keeps the tracker under the soft cap). save_transfer/net_pump keep only the trigger
call. This is also the single biggest LOC relief for net_pump (1183->~850) and save_transfer.

### B3. "Conversion / adoption" squats across three files
- `remote_prop.cpp:814-1071` — `OnConvert` apply
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
| `props/remote_prop.cpp` | 1174 | `OnConvert` L814-1071 (~258) | `remote_prop_convert.cpp` | or fold into B3 |
| `props/remote_prop_spawn.cpp` | 972 | `OnSpawn` is a 740-line monolith — split by PHASE | (in-file or siblings) | phases: unkeyed-drop / trash-proxy / exact-key converge / kerfur fuzzy-rekey / deferred finish. Kerfur rekey -> B3 |
| `creatures/npc_sync.cpp` | 964 | `NpcSuppress_Interceptor` L330-524 (~195) + `Install` L595-809 | `npc_sync_install.cpp` | cohesive file otherwise; interceptor is the fat block |
| `props/prop_element_tracker.cpp` | 1030 | `DebugCheckPropElementReap` self-test L926+ (~104) | a `_test`/dev TU | plus B2 moves reconcile IN — net LOC may rise; watch the cap |
| `session/save_transfer.cpp` | 838 | pile/kerfur divergence L541-730 | -> B2 target | same move as B2 |
| `session/player_handshake.cpp` | 828 | UTF-8 codec L132-181 + nick-color/skin wire pack-parse L90-131 | shared string util + `nick_color`/`nameplate` | codec is a generic dup (see D) |
| `props/trash_collect_sync.cpp` | 810→498 | use-interceptor family (~258) | `trash_use_intercept.{h,cpp}` (~370) | **DONE (B1a, smoke-verified `03d38d2b`)** — see the B1 section |
| `world/weather_sync.cpp` | 1141 | cohesive — LOW priority | (sky/lightning are internal sub-concerns) | only split if it grows; no misplaced code |
| `net/session.cpp` | 885 | `HandleMessage` switch L324-606 | per-message handlers | conceptually core routing — split only if it keeps growing |
| `harness/harness.cpp` | 1204 | -> B5 (autotest move) + nick-color parse L759-780 -> `nick_color` | | most relief comes from B5 |

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
  (engine.cpp:379,508; autotest.cpp:97,620; autotest_saveui.cpp:143; join_membership_sweep.cpp:624;
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
