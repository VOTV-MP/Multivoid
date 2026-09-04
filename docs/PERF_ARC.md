# PERF_ARC — the living arc for Multivoid's performance: survey, measurement, redesign

**Status: OPEN — census running. Started 2026-09-02 on the user's mandate (verbatim): *"What if us
having zero UE4SS imports is bad for performance? Like people report random fps drops. I don't want
any of that, I wan't the best performance and not hurt the mod. Can we maybe research this? ...
Survey, research, re-design everything that eats up performance from us. Per rule 1, throughly."***

This is the canonical home for every performance question, measurement, and fix in the mod. The rule
of the arc is the project's standing one: **measure, don't infer** — every cost claim here carries a
`[V]`/`[RD]`/`[?]` tag and its instrument. A "random fps drop" report is a symptom; this doc exists
so each symptom gets an attributed mechanism, and each mechanism a root-cause fix (RULE 1 — no
"good enough", no suppressive band-aids like "just lower settings").

Related arcs: `docs/UE4SS_ARC.md` §9 (the loader-build fps study — CLOSED, pin moved),
[withdrawn at the author's request]
root-cause fix for the polling-scan cost class named below; if the census confirms that class is
hot, WP-2 re-enters through this arc on its own merit). The born-rule for post-ship perf audits and
the audit vocabulary: `reference/agency-agents/audit-prompt-perf-template.md`.

---

## 0. The question under test, framed honestly

**H-IMPORTS: "zero UE4SS imports is bad for performance."**

Mechanism analysis (what imports could and could not change):

- An import is load-time plumbing. Calling UE4SS's C++ API through its import thunk vs calling our
  own code is the same cost class (one indirect jump). **Zero imports per se costs zero frames.**
  What zero imports DOES mean is that we DUPLICATE machinery instead of sharing UE4SS's — and the
  duplicated machinery is where any real cost would live:
  1. **Two ProcessEvent detours in the chain** (UE4SS's PolyHook detour + our MinHook relay,
     `docs/UE4SS_ARC.md` §4). Every PE call in the game pays both prologues. Per-call this is tens
     of ns; at the measured ~170k dispatches/s (see §2) the chain overhead is sub-ms/s territory —
     but our DETOUR BODY is not free (see §2: ~1.0 ms/frame total dispatch cost) and that body is
     ours to optimize regardless of imports.
  2. **Reflection lookups.** UE4SS keeps an object cache fed by FUObjectArray listeners
     (`bUseUObjectArrayCache`); we walk `GUObjectArray` (measured 1.1–1.6 ms per full walk, §2).
     This is the one real performance GAP between the substrates — and it is closable WITHOUT
     imports, by registering on the engine's own listener seam (+0x68/+0x78) exactly as UE4SS does
[withdrawn at the author's request]
  3. **What imports would COST:** UE4SS's PE callback cannot cancel a call (void return, no skip) —
     we cancel ~20 native calls by design, so option C is architecturally dead
     (`docs/UE4SS_ARC.md` §4). The imports question is therefore not a fork we could take even if
     it measured faster.

**Prior evidence already against H-IMPORTS** (§2): the 48-fps field-scale swing was the UE4SS
*build version* — a variable that exists only BECAUSE UE4SS runs as its own layer, and that our
zero-import DLL survived unchanged (`abi_gate` PASS on both builds). An import-linked mod (DebugMod
shape, 32–130 symbols) breaks or loads at the loader's mercy; ours ran at 118 fps on the new build
the same day. Verdict `[?]` until agent census + the differential arms below close it, but the
working hypothesis is: **the real costs are (a) our own hot-path code and (b) UE4SS's runtime
services — neither is bought back by importing.**

## 0a. The UE4SS question, formally OPENED (2026-09-02, user)

The user re-opened the substrate relationship with UE4SS through the performance lens. The
standing decision is F2/D-3 (`docs/VERSION_MIGRATION.md` §11, TRIPWIRE-DECISION ledger,
machine-checked per release) — UE4SS is the loader, zero imports, whole substrate owned. This arc
does not re-litigate it by argument; it feeds the ledger EVIDENCE. Three sub-questions, each with
its closing criterion:

1. **Does the zero-import stance itself cost frames?** Mechanism analysis says no (§0); closes
   with the §6 arms (dispatch bypass A/B on the new loader + per-PE cycle sampling). If some
   UE4SS-internal machinery were ever measured meaningfully faster than our equivalent, the
   answer per RULE 3 is still not "import it" — it is "port the algorithm" (attributed, as
   always) or "use the same engine seam it uses" (the WP-2 listener shape).
2. **Does UE4SS itself cost the player frames we never use?** Its PE detour chain, Lua VM + 5
   started mods, object-array cache upkeep, console/GUI — census + knob arms (§4-B, §6 arm 3).
   If yes, the lever is CONFIGURATION we ship/document (settings ini, mods.txt guidance), not
   imports — and upstream evidence goes to the §11 ledger.
3. **Is the loader-build spread in the FIELD a live cause of reports?** (H-LOADER-SKEW) — closes
   with reporter intake (`UE4SS.log` banner).

What would actually re-open D-3 itself: a measured, structural cost of running UNDER UE4SS at all
(not fixable by config), or an upstream change breaking the C-ABI start contract. Neither is in
evidence; both belong to `docs/UE4SS_ARC.md` if they ever appear.

## 1. Hypothesis register

Each row gets a verdict with evidence before this arc closes. "Fix" column filled from §5 queue.

| id | hypothesis | prior | status |
|---|---|---|---|
| H-IMPORTS | zero UE4SS imports itself costs fps | mechanism analysis §0 + census B (§4-B): UE4SS's PE detour usually isn't even armed (0/15 boots), the import world would run BOTH detours (their callback can't cancel), the lookup gap is our unbuilt cache (fixable import-free) | **CLOSED at mechanism level: NO** — §6 arm 2 re-measures our own detour share for the record |
| H-LOADER-SKEW | field "random drops" = players on the SLOW UE4SS build | `[V]` build worth ~48 fps (§2). Timeline bounds it: the pin moved 2026-08-31, the FIRST public release is b150-dev 2026-09-01 — so every install that followed OUR instructions got the fast loader. The exposed population is players with a PRE-EXISTING UE4SS (installed for another mod, any vintage) who SKIP INSTALL.md step 1 because "UE4SS is already there" — the instructions do not force-replace it (`docs/INSTALL.md:88-106`) | `[?]` intake: ask reporters for their `UE4SS.log` banner (prints the Git SHA) |
| H-DISPATCH | our PE detour body costs real frame time | census A (§4-A): fast path verified clean (~8-10 loads, no lock/alloc); the ~1.0 ms bypass figure covered mechanism+observers+pump together; detour self ~0.5 ms/frame = memory latency (F1) | NARROWED — detour NOT a suspect; F1 packing demoted to micro (Q-M1) |
| H-WALKS | uncached GUObjectArray walks fire at steady state | census A+C: in a HEALTHY build no per-frame full walks exist (steady load = the budgeted scan hub + reseed, ~2 ms floor). The class survives as (a) MISS-path per-frame walkers (firefly/event_cue/weather/garbage/hand_item/death_revive) and (b) per-PACKET walks (`GetWorldContext` uncached — 47 walks/frame on a coin sale) | CONFIRMED-CONDITIONAL — fix = Q5 family |
| H-CLIENT-ASYM | the CLIENT does disproportionate work — field: client 20 fps vs host 60 on a WEAKER machine | census A (§4-A headline): CONFIRMED, and the mechanism is ENGINE-side actor-population asymmetry — 871 GC-rooted Movable C2 trash proxies DUPLICATING natively-owned piles + ~1,061 extra keyed props ≈ 1,930 extra actors; client hitches are mostly `[HITCH]` without `[HITCH-SRC]` = engine time. Our own client-only CPU excess (mirror-drive PE set, ParkWalk) is secondary. The C2 aim-cone is per-E-press (~1,742 PE), not per-frame | **CONFIRMED — fix = Q1 (C2 retirement per RULE 1)**; §6 arm 1 re-prices at HEAD |
| **H-REJOIN** | **the 20-fps client is a REJOINED client: the previous session's world (or a large residue) survives the rejoin — held by one of our GC pins/roots/caches, or double-registered in our element/mirror registries — doubling GUObjectArray (every walk pays 2x), keeping stale mirrors under upkeep, and inflating memory** | USER 2026-09-02: *"20 фпс я ловил один раз случайно в тестах у клиента, это было с перезаходом клиента связано"* — the rig reproduced it once, tied to a client rejoin. Corroboration: issue #13's 20-fps player is the one who REJOINED (its bugs 1 and 6 are rejoin defects). Prior art, same class: the 2026-09-01 rejoin CRASH root was our rooted proxy holding a DEAD WORLD (fixed for the crash path); the dying-world purge can also lag legitimately (44+ s measured on a pawn, `world_identity.h`); v122 killed ~2,200 zombie double-rows per join — the pattern has form | RESOLVED by census D (§4-D): the PERSISTENT pin mechanism is CLOSED by `bb881bab` and `[V]` b150-dev SHIPS it — a field rejoin costs the ~5-s transient purge window + **C2 re-materialized from scratch** (the real persistent term, → Q1). The user's own pre-09-01 catch = the then-live pinned world, fixed. Residue that DOES survive = §4-D gaps 1-4 (memory/correctness, not per-frame fps) → Q3a |
| H-UE4SS-SERVICES | UE4SS's own steady-state services cost frames we never use | census B (§4-B): GUI/console/PE-detour all off-or-unarmed on our installs; the TWO live costs are the script_hook post-callback at BP-dispatch volume (armed by ConsoleEnabler+CheatManager) and `bUseUObjectArrayCache=true` upkeep — which the FIELD runs as false and nothing of ours uses (B-FIND-1) | NARROWED — §6 arms 3a (cache false A/B) + 3b (script_hook mods A/B) price the two |
| H-LOG-IO | steady-state logging (format + synchronous flush) costs frames or hitches | CONFIRMED as a CONDITIONAL collapse lane (§4-C C-CRIT-2): WARN/ERROR = sync fflush on the logging thread, no level threshold exists, and un-latched per-packet WARN sites can fire at 60/s — quiet when nothing warns, catastrophic when a join gap arms one | fix = Q2 |
| H-ALLOC | heap/wstring churn on hot paths | history: the 19-GB Install incident (fixed); pattern class uncensused since | OPEN |
| H-SEH | high-rate SEH absorption (exception dispatch ~µs each) | history: ~2,508 AV/s absorbed (fixed by world stamps); coverage of other absorb sites uncensused | OPEN |
| H-OVERLAY | overlay per-frame work with menu CLOSED (ImGui full frame? DX12 UpdateTexture INFINITE wait — noted "already runs today, unmeasured") | CONFIRMED half: the early-out is pinned open by voice (§4-C C-CRIT-1) → full empty ImGui+DX11 pass every frame for every player. DEFUSED half: the DX12 INFINITE wait is event-rate only, not steady state | fix = Q1 |
| H-GAMEWORLD | the drop is the GAME's own cost (prop-dense base, host's big save loaded by client) and not the mod at all | `[V]` menu = 120 with mod; the 08-29 hunt measured Multivoid ≈ 0 on that rig/scene | OPEN — needs with/without-mod arm on a HEAVY save |

## 2. Already-measured ledger (do not re-derive; instrument named per row)

| fact | value | instrument / source | status |
|---|---|---|---|
| UE4SS BUILD delta | v3.0.1 = 70 fps vs e31aaaa6 = 118 fps, same save/build/install; de-confounded 3-arm, PID-asserted | `docs/UE4SS_ARC.md` §9.1 | CLOSED — pin moved to shimloader-1.1.7 (2026-08-31); both install lanes same bytes SINCE then |
| Multivoid static footprint | ≈ 0 fps (menu 120 with mod; loaded+hosting+paks = 119 vs 119) | fps hunt 2026-08-29, bisect on one save | CLOSED for the STATIC half |
| Our whole PE dispatch path | **~1.0 ms/frame** (clean bypass A/B, 79→86 fps) | fps hunt 2026-08-29 | STANDS — this is our largest KNOWN own cost; fast-path census owed |
| PE dispatch rate | ~170k/s cross-thread atomics (perf_probe_dispatch; probe now opt-in OFF) | 2026-08-29 | STANDS (scene-dependent) |
| Full GUObjectArray walk | ~1.1–1.6 ms per walk | `[WALK-TIME] sync:event_cue` line | STANDS |
| `R::FindFunction` | NO result cache; 476 call sites (filed, unfixed); `FindClass` got its cache `ca1cd5e4` | browser perf design, `docs/MULTIPLAYER_UI.md` §8c.-1 | OPEN — the filed item lands in THIS arc |
| `FindObjectByClass` census | 136 sites outside reflection.cpp, ~52 world-scoped | `ue_wrap/engine/world_identity.h:94` | STANDS — per-site rate census owed |
| Lua-mods cost on OLD loader | ~45 fps (6 mods, dev rig); ~5 fps attributable to the un-started one on new loader | 08-29 bisect + §9 arm C | outranked in scale by the build delta (~5 of ~48 fps); residual on NEW build `[?]` |
| `stat unit` instrument cost | ~0 (78.0 vs 78.7) | 08-29 | CLOSED |
| The 120→60 born-rule incident | a per-PE observer + per-frame FindObjectByClass halved fps | CLAUDE.md post-ship-audit rule provenance | fixed then; the CLASS is what §4 censuses |
| Input focus scan | ~9,300 → ~6,500 dispatches/s after fix; never moved steady fps | 08-29 | CLOSED |
| SEH absorb storm | ~2,508 AV/s absorbed pre-world-stamp | world_identity arc 2026-08-23 | fixed; absorb-site coverage census owed |
| 19-GB alloc storm | ~1.1B wstrings/s in an Install loop | 2026-05-27 incident (born the pre-deploy checklist) | fixed; alloc-class census owed |
| `fps=0` ≠ slow frame | a dead render path with a live game thread reads fps=0; `overlayPresent`/s separates them | §9 arm D VOID run | instrument discipline — keep |

## 3. Field intake (the actual reports)

- **Issue #13 (Endertrey, 2026-09-02), comment, verbatim:** *"I should also mention i was host and
  have a much better comptuer than him / when he's at the base he gets 20fps while i get 60 fps"* —
  CLIENT at 20 fps at the base, HOST at 60 on stronger hardware. The 9-item bug list in the body is
  sync defects (separate lanes); the perf datum is this comment.
  Unknowns to collect from the reporter: install lane (r2modman vs manual — decides which UE4SS
  build, H-LOADER-SKEW), client fps in the SAME spot single-player (H-GAMEWORLD control), client
  hardware, save age/size.
- **User (2026-09-02): "people report random fps drops"** — source channels beyond #13 not yet in
  the tree; intake wanted (Discord/Thunderstore comments — ask the user for links or screenshots).

What would make field reports diagnosable instead of anecdotal: a lightweight always-on hitch
attribution line in `multivoid.log` (frame time + what the mod did that frame: walks, syncs, atlas
ops, absorb counts) — cheap enough to ship ON, so a report arrives with data. Design owed in §5;
the existing `perf_probe`/`[HITCH]` instruments are dev-gated and dead at the menu
(`session_runtime.cpp` gate) — a field report cannot benefit from them today.

## 4. Census (2026-09-02, four read-only audit agents over HEAD `504b5c90`)

- **A — DLL hot-path census:** DONE — §4-A below.
- **B — UE4SS runtime footprint:** DONE — §4-B below.
- **C — alloc/IO/lock/SEH + overlay:** DONE — §4-C below.
- **D — rejoin residue census:** DONE — §4-D below.

(Censused HEADs: A/C at `504b5c90`, D at `78b956bb` — the delta between them is the parallel
session's docs-only QF_ARC commit; same code.)

### 4-A. Census A results (DLL hot paths, host vs client) — landed 2026-09-02

All `[A]` with file:line (spot-verified where marked). The one that changes the arc:

**A-HEADLINE — the field 20-fps client is dominated by CRUTCH C2's true price: actor-population
asymmetry, ENGINE-side.** A same-save host/client field-log pair
(`ignore_folder/arigalit_atv_and_props_report/`, 2026-08-26) shows the client carrying **~1,930
more actors than the host**: **871 mod-spawned `AStaticMeshActor` trash proxies** — Movable +
GC-rooted (`trash_proxy.cpp:184-218`; mobility `:197`, root pin `:207`) — DUPLICATING native piles
the client already owns, plus **~1,061 extra keyed props** (client 4,293 vs host 3,232).
`native_pile_mirror: MATERIALIZED = 0` in that log: the bind-the-native-instead-of-spawning guard
(`remote_prop_spawn.cpp:169-208`) **never fired once in the field** — a live instance of the
dead-capability class (`docs/DEAD_CAPABILITY_REGISTER.md` candidate). Client `[HITCH]`≥40 ms = 253
vs host 107, most WITHOUT a `[HITCH-SRC]` — by net_pump's own discriminator
(`net_pump.cpp:417-448`) that is ENGINE time (render/physics/GC of the extra Movable rooted
actors), not our code. Caveat, stated: the pair predates the R-2 scan hub — it proves the
MECHANISM, not today's exact numbers; re-run at HEAD is §6 arm 1's first order of business. The
rejoin tie-in: every join re-creates the 871-proxy burden, and census D owes the answer on whether
a rejoin tears the previous set down or stacks it.

**A-FASTPATH — the PE detour is NOT a suspect.** For an uninteresting call (>99%): ~8-10
relaxed/acquire loads + branches + one int inc/dec; NO lock, NO allocation, NO reflection; pump
empty-check short-circuits before the thread-id read. Bloom + count-bounded walk verified. The
detour's measured ~0.5 ms/frame self-cost stands as the memory-latency price of that bookkeeping
(F1's cache-line packing remains valid but is a MICRO item, demoted in the queue).

**A-FLOOR — our symmetric per-frame floor is ~2 ms:** `scan_hub::Tick` (sliced GUObjectArray pass,
~1 ms GT budget/frame while any consumer is unsettled — grime churn makes unsettled NORMAL in live
play, `object_scan_hub.cpp:263,186`) + `DrainReseedQueue` (self-budgeted 1 ms/frame, per-item
`ToString(NameOf)` render — scales with prop count, `prop_census.cpp:422,504-507`). 12% of a
60-fps budget, and a much larger slice of a weak client's frame.

**A-CADENCE CORRECTION (load-bearing for every rate in this doc):** the pump composite runs at
**~60 Hz** (`session_runtime.cpp:849` `Sleep(16)`, coalesced; measured ~57/s). Code comments
saying "125 Hz" are STALE.

**A-TOP offenders beyond the headline (condensed; full ranked table in the census):**
- **`[WA-TRACE]` shipped diagnostic** (`world_actor.cpp:141,157,162`): 2 `GetActorLocation` PE + a
  log line PER MIRROR PER SECOND, client-only — and coin mirrors NEVER despawn
  (`world_actor_sync.cpp:491`), so one 47-coin sale = +47 log lines/s and +94 diag PE/s, growing
  monotonically for the session. Delete/ini-gate.
- **`engine::GetWorldContext` fully uncached** (`engine.cpp:678-681`): a full
  `FindObjectByClass(GameInstance)` walk PER SPAWN PACKET — a coin-gun sale minting ~47 coins = ~47
  full walks in ONE frame. Its sibling `EnsureWorldContext:61` IS cached; give it the same.
- **Miss-path per-frame walk family** — in a healthy build NO per-frame full walks exist, but each
  of these walks EVERY FRAME while its target is absent: `firefly_sync.cpp:131` +
  `event_cue_sync.cpp:91` (`FindObject` — the ~237k-render primitive — hoisted ABOVE their own
  latch+throttle), `weather_sync.cpp:149` (no negative TTL), `garbage_sync.cpp:245` (no throttle at
  all), `hand_item.cpp:289`. And `death_revive::ResolveVerbs` (`death_revive.cpp:127-161`) is an
  ALL-OR-NOTHING latch over ~13 Find* calls running per frame forever if ANY one name drifts on a
  game update — a latent update-day bomb.
- **Client-only mirror-drive PE set** (scales with population): `npc_mirror::TickClientNpcs` 3-6 PE
  × N mirrors un-throttled (`npc_pose_drive.cpp:139`), `world_actor_mirror` 2 PE/mirror/frame,
  `dish` 24-slot scan + ≤48 PE while slewing, `spawn_authority::ParkWalk` 1 Hz FULL walk until the
  spawner classes park (`spawn_authority.cpp:189,340`).
- **`interactable_channel::PollAndBroadcast` un-throttled per frame** (host 7 / client 5 channels):
  per-actor wstring alloc+free + mutex+hash per indexed interactable per frame
  (`interactable_channel.h:469,632`) — O(all indexed interactables) at a base; grime already
  throttles to 50 ms, the rest should follow.
- **`Registry::SnapshotActorsByType`**: fixed 65,536-slot scan under the registry mutex, 1-4 Hz
  from 6 lanes (`registry.cpp:287-298`) — aggregate ~0.4-1.6M iterations/s; wants a per-type index.
- **C2 aim-cone cost is per-E-PRESS, not per-frame — but heavy:** the cone calls a
  `GetActorLocation` PE + malloc per proxy BEFORE the distance filter (`trash_proxy.cpp:329-332`) ≈
  ~1,742 dispatches per E-press; `pile_spawn_bind.cpp:267` runs O(natives × proxies) ≈ **~740k PE
  dispatches at join**. Cheap fix independent of the crutch's fate: use the cached `ProxyEntry`
  positions the module already maintains.
- **`pause_guard::Tick`**: 1 PE + heap alloc per frame both roles to read `IsGamePaused`
  (`pause_guard.cpp:18`) — cheapest win in the census.

**A-MODULARITY (soft-cap flags from the census):** `net_pump.cpp` 882, `session_runtime.cpp` 857,
`reflection.cpp` 801 (read in full); past-cap not on hot paths: `host_session_settings` 1199,
`autotest_death` 1201, `session_manager` 1077, `save_transfer` 1066, `session.cpp` 1028,
`native_ui_probe` 1000, `imgui_overlay` 996, `session_status` 961, `container_contents_sync` 940,
`server_browser_rows` 939, `meadow_db_sync` 902 — extraction proposals owed when touched.

### 4-B. Census B results (UE4SS runtime footprint + the imports verdict) — landed 2026-09-02

All `[A]` with file:line into `reference/RE-UE4SS` (vendored HEAD `7f7cc36f` = pinned `e31aaaa6`
+ 19 commits, so the pinned build's code is fully contained) and the four real installs' inis
(byte-identical, diff-verified). The UEPseudo + patternsleuth submodules are EMPTY in the checkout,
so hook-install bodies and the object-cache implementation stay `[?]`.

**B-VERDICT on H-IMPORTS: NO — there is no mechanism by which zero imports costs frame rate.**
Three pillars, each measured or code-cited:
1. **UE4SS's PE detour is usually NOT EVEN ARMED.** `HookUObjectProcessEvent=1` is a capability,
   not an install: the PolyHook detour arms lazily on the first Lua
   `RegisterProcessEventPreCallback` (`LuaMod.cpp:3843-3850`), and `docs/UE4SS_ARC.md:107-110`
   measured 0/15 solo boots arming it. So in practice the chain is OUR SINGLE detour; the
   double-detour composition, when present, is a few extra jumps ≈ <20 µs/frame.
2. **The import world is strictly WORSE:** UE4SS's PE callback is void-return, no cancel — our ~20
   interceptors need the hook OWNED, so under imports BOTH detours would always run.
3. **The lookup gap is an unbuilt cache on OUR side** — UE4SS's object cache is fed by the
   engine's own FUObjectArray listener seam, reachable to us import-free (WP-2 / Q5).
   The measured 48 fps was a UE4SS BUILD (DLL code) difference with our imports at zero in every
   arm — orthogonal to the question.

**B-FIND-1 — our four dev installs run `bUseUObjectArrayCache=true`; the FIELD default
(shimloader-1.1.7 package) is FALSE.** The cache is per-object-construct/destruct listener upkeep
inside UE4SS that NOTHING uses on our installs (we import nothing) — pure cost, and preserved-state
inheritance from the old zDEV ini (the installer never overwrites an existing ini). Same for
`EnableHotReloadSystem=1` vs field 0 (cheap — a key poll). Consequence both ways: our rig slightly
OVERSTATES UE4SS's cost vs the field, and flipping to `false` is a plausible free win + measurement
hygiene. **A/B arm owed before flipping the checked-in template.**

**B-FIND-2 — the ONE UE4SS path at BP-dispatch volume on our installs is the Lua `script_hook`
post-callback** (ProcessLocalScriptFunction/ProcessInternal), armed because `ConsoleEnablerMod` +
`CheatManagerEnablerMod` each `RegisterHook` a script function: per BP dispatch it takes a
`recursive_mutex` + linear find over the (2-entry) hook vector (`LuaMod.cpp:5592-5602`). The
2026-08-29 "~5 fps for a mods.txt row" measurement is most plausibly THIS mechanism on the old
loader; changelog **#801 "Improved performance of script hooks created with RegisterHook" is the
strongest candidate for the 48-fps root** (it scales with ~2,200 dispatches/frame; Live-View
candidates are ruled out — GUI off; ini-default candidates ruled out — the arms held the ini
constant). Root stays `[?]` — the bodies live in the unpopulated UEPseudo submodule.

**B — everything else is off or free on our installs:** GUI/Live View/its listeners/D3D-Present
path never created (`GuiConsoleEnabled=0` gates the whole block, and UE4SS's GUI is a separate
GLFW/OpenGL window, not a game-swapchain hook); native console off; two ~200 Hz loops (main event
loop + async Lua) do near-zero steady work, and our `fire_update()` stub is a no-op; the engine-tick
hook carries an empty callback list; the Lua global UObject DELETE listener always runs (mutex +
set-erase per destroyed object — teardown-churn-scaled, not per-frame).

### 4-C. Census C results (alloc / IO / locks / SEH / overlay) — landed 2026-09-02

All `[A]` (agent-verified with file:line) except the two CRITs, which are `[V]` — re-read in the
main session the same hour. Full detail in the agent report; the load-bearing rows:

**C-CRIT-1 — the overlay early-out is PINNED OPEN by voice chat.** The ImGui pass IS gated
(`imgui_overlay.cpp:688-689`: `AnyOpen() || hud::IsActive() || join_curtain::IsActive()`), but
`hud::IsActive()` includes `voice_chat::Enabled()` (`hud.cpp:328`) — voice is ON by default, so the
designed skip is dead code for essentially every player, and EVERY presented frame runs the full
`NewFrame → atlas_watch::OnFrame → Render → RenderDrawData` chain. The vendored DX11 backend has NO
zero-vertex early-out (`imgui_impl_dx11.cpp:170-174`): 2× `Map(WRITE_DISCARD)`/`Unmap`, ~7 KB state
backup, ~50 immediate-context calls per frame — for an EMPTY draw list. This is a per-frame tax on
every player, menu closed. `[V]` main-session re-read: the pinning term's own comment says its
intent was the v66 mic indicator, but `DrawLocalVoiceIcon` early-outs on `!enabled || !started` and
PTT-idle draws NOTHING — the gate holds open for frames with ZERO vertices. **Fix (root): a draw
predicate must ask "is there anything to DRAW", not "is a subsystem ENABLED" — replace the
`voice_chat::Enabled()` term with voice's has-something-to-show, plus `TotalVtxCount==0 → skip
RenderDrawData` as the backend's own guard.**

**C-CRIT-2 — un-throttled WARN per packet + WARN==synchronous fflush = a log-storm fps collapse
lane, and its trigger is a JOIN GAP.** Logger policy today (`log.cpp:265-267`): WARN/ERROR flush
synchronously per line ON THE LOGGING THREAD; there is NO log-level threshold anywhere — every
executed UE_LOG* formats + takes 3 locks. `remote_prop.cpp:186/200/221` WARN per PropPose packet
with no latch: a keyed prop streamed at 60 Hz whose mirror is ABSENT on the receiver (join gap /
destroyed mirror / rejoin residue) = **60 WARN/s = 60 synchronous disk flushes/s on the game
thread, self-sustaining for the session**. This is a concrete H-REJOIN mechanism candidate: a
rejoin that leaves any streamed key unmatched buys a permanent flush storm. Siblings: epoch-0 WARN
per packet on the NET thread (`session.cpp:493`, attacker-rate); pump task-fault ERROR un-latched
(`game_thread.cpp:300,304`, 60/s if a task faults deterministically); `[REL-EDGE]` 60 Hz during
throw-flights (`local_streams.cpp:499`).

**C-HIGH — steady allocations:** reliable inbox pays 1 malloc + 1 cross-thread free per message
(240-B element > MSVC deque block, `session.h:1032` — the comment claiming otherwise is FALSE);
the three batch-pose lanes alloc a fresh `std::vector` per datagram = **180 cross-thread
malloc/free pairs/s on every client** (`session_npc.cpp:70`, `session_worldactor.cpp:90`,
`session_trashcarry.cpp:90` — fixed arrays + count is the fix, caps already in protocol.h).
Join-window cluster: reassembly geometric realloc (last = 32 MB malloc + 16 MB memcpy) on the net
thread INSIDE the mutex the join loop polls at 60 Hz (`save_transfer.cpp:408`); connect-replay
drains ~2,300 reliable messages unbounded in ONE game-thread frame (`event_feed.cpp:200`); host
late-arm flush does ~2,000 reflected GetActorLocation per 5th tick for ~25 s/join
(`save_transfer.cpp:877`).

**C-MED — locks:** PE fast path has ZERO locks (confirmed); the one to watch is `remoteMutex_` —
one global mutex for 12 stream lanes, 2×/packet net-side + ~20×/frame game-side. `g_classCacheMu`
is NOT per-dispatch (2 acquisitions per Find* call, low rate).

**C — SEH coverage:** absorb sites are latch-covered at the detour/observer/IsLive tier;
UNCOVERED at rate: the pump task path + Func-thunk cb path (un-latched ERROR per fault) and
`RawViewportSwapChain` (silent absorb, per-present-capable). **No absorb-RATE line exists — the
next storm is invisible until felt**; a 1 Hz "absorbs since last sample" diag is owed.

**C — defused:** DX12 `UpdateTexture` INFINITE wait is UNREACHABLE at steady state (gated on
`WantCreate||WantUpdates`; reached only on atlas build/grow/first-seen codepoint/UI-scale change —
event-rate hitch, bounded); scalar pose path is allocation-free both directions; voice ring
alloc-free; no base64/hex on steady paths; per-tick `FindObjectByClass` consumers in `coop/` are
all cached/throttled (32 non-dev sites verified) — the steady walk load lives in the budgeted
scan hub (~1 ms slices at 2 s cadence, `object_scan_hub.cpp:28-33`), so H-WALKS at steady state is
CONTAINED; the cold-path owner remains `FindFunction` (F2).

## 4a. The substrate deep-read (main session, 2026-09-02 — code read in full, not delegated)

The machinery itself, walked file by file. Findings `[V]` = read from the code this day.

**F1 — the interposition mechanism is memory-bound, not instruction-bound, and its state is
scattered.** `[V]` The shipping fast path per PE dispatch (probes off, function unregistered — the
>99% case) is ~12–16 atomic/TLS loads + 3 call frames + 2 TLS RMW (`pe_detour.cpp:328-434` +
`game_thread_detail.h:80-102`; every registry check is count-load + Bloom-word load, table-based SEH
is free at runtime). Instruction count is trivial; the loads touch ~8–10 DIFFERENT cache lines
(bypass atomic, depth TLS, gameThreadId, countOn, queueDepth, 3 Bloom arrays of 512 B each, 4
active counters) interleaved with engine work that evicts them. The code's own measured figure:
~0.5 ms/frame at ~2,200 dispatches/frame (`pe_detour.cpp:81`) ≈ 230 ns/dispatch — exactly the
shape of per-dispatch cache misses. **Fix shape: pack the whole fast-reject state into ONE
cache-line struct (bypass | queueDepth | 4 active counts) + ONE combined any-registrant Bloom
(intc|pre|post|diag OR-ed at register time) so the common case touches 1–2 lines; per-table blooms
consulted only on a combined hit.** Expected ~0.5 → ~0.1–0.2 ms/frame. This is detour-BODY work;
the double-detour CHAIN with UE4SS adds two jumps (~ns) and is not the cost.

**F2 — `FindFunction` is architecturally wrong, not merely uncached.** `[V]`
`reflection.cpp:561-574`: every call walks ALL ~237k GUObjectArray slots (ObjectAt chunk math +
OuterOf deref per slot) to find a function that structurally lives in the CLASS's own
`UStruct::Children` linked list (~tens of nodes incl. the super chain). No result cache; 476 call
sites. **Fix shape (root): walk `owningClass->Children` via `UField::Next`, following
`SuperStruct` — O(hierarchy functions) ≈ 2,000–5,000x less work cold — PLUS a
(class, name)→UFunction* result cache (CachedObjRef-validated) for O(1) warm.** Profile carries
`UStruct_SuperStruct=0x40` and `UStruct_ChildProperties=0x50` already; `Children` sits at +0x48
between them (4.27 layout) — one verification probe owed, same health-check pattern as the rest.

**F3 — by-NAME lookup renders every object's FName through an engine-heap alloc+free pair.** `[V]`
`FindObject` (`reflection.cpp:506-520`) calls `NameEquals` per object; `NameEquals` →
`RenderNameToScratch` (`:334-360`) frees the previous engine buffer and lets `FName::ToString`
allocate a fresh one EVERY call (IDA-verified: ToString never reuses; the 2026-06-13 RAM-balloon
fix made this an alloc+free per compare instead of a leak). A single by-name lookup ≈ ~237k
engine-heap alloc/free pairs + renders. `FindClassDefaultObject` rides it. **Fix shapes: (a)
compare FNames by ComparisonIndex (int ==, the engine's own semantics — case-insensitivity for
free), learning the literal's index at first textual match, same priming pattern BeginClassWalk
already uses; (b) `FindClassDefaultObject` should read `UClass::ClassDefaultObject` (+0x118 on
4.27, not yet in the profile — self-verifying probe: the pointee's name starts `Default__`) and
never walk at all.**

**F4 — the by-CLASS walkers stay O(all objects) even cache-warm.** `[V]` `FindObjectByClass` /
`FindObjectsByClass` / `CountObjectsByClass` (`reflection.cpp:576-677`) walk all ~237k slots with
pointer compares once `BeginClassWalk` primed the class — ~1M scattered reads ≈ the measured
1.1–1.6 ms per call. This is the class the `[WALK-TIME]` instrument exists for. **Fix shape (root):
the event-driven per-class instance index fed by the engine's OWN FUObjectArray create/delete
[withdrawn at the author's request]
mechanism, now entering through the perf door on its own merit. Zero-import, zero steady-state
walks; the world stamp + kill-flag semantics stay (the index is a candidate set, liveness still
checked per read).** Until it lands, the interim rule stands: no per-frame call sites (census in
§4-A decides how many exist today).

**F5 — the pump architecture is sound.** `[V]` Queue-empty probe is lock-free (TLS + one acquire
load, `game_thread.cpp:41-51`); the mutex is taken only per posted task (~60–125/s) and per drain
pop. Not a cost center. But note what the 08-29 "~1.0 ms/frame bypass A/B" number actually
covered: the transparent bypass skips interceptors + observers + THE PUMP — so that figure is the
mechanism AND the drained coop work (net_pump::Tick fan-out) together, consistent with the
instrumented-bucket sum of 0.86 ms. The mechanism's own share is the ~0.5 ms of F1.

**F6 — perspective for the field report.** Our measured total (~1 ms/frame at the 08-29 scene) is
12% of a 120-fps frame budget but only 2% of a 20-fps frame (50 ms). A client at 20 vs host at 60
therefore CANNOT be explained by the substrate constants above — it needs a mechanism that SCALES
(per-prop/per-frame gameplay-layer work, walk sites firing at rate, or the game's own cost on that
save). That is what the §4-A client-asymmetry census and the §6 arms discriminate.

**F7 — the OUTBOUND call marshalling is the substrate's second real per-rate cost (after the
walks).** `[V]` `call.cpp:85-103`: every `ParamFrame` construction takes `g_metaMutex` + a hash
find (metadata IS cached — the D4-1 fix), then `buf_.assign(frameSize)` — **one heap alloc + a
memset PER CALL**; every `SetRaw`/`GetRaw` does a LINEAR scan of the param-name vector with
`_wcsicmp` per entry (`:105-117`). Our authored dispatch volume is ~9k/s ≈ 135/frame — measured at
14-16% of the game thread's whole script load (`reflection.cpp:88-98`) — so the marshalling tax is
~9k allocs/s + tens of thousands of wide-string compares/s, paid mostly by the per-frame pose
readers and mirror drives. **Fix shapes: (a) small-buffer optimization or a thread-local arena for
the frame buffer (frames are ≤ a few KB, most ≤ 256 B); (b) BOUND frames — the hot callers use
fixed param sets, so resolve offsets once at the call site and reuse (an API that returns a
prebound frame kills the per-call name scans whole); (c) the biggest lever is VOLUME and already
queued: interp-gating the mirror-drive writes (Q5) removes dispatches, not just their overhead.**

**F8 — the remaining seams are verified CHEAP (read in full).** `[V]` `vm_dispatch.cpp:151-156`:
the eternal 0x45 tax outside a session is ONE relaxed load + a predicted branch + tail-call;
enabled, a dispatch pays a TEB read + 1-2 counter XADDs + a linear verb match (~10 entries × 3
atomic loads) at a measured ~238 0x45-dispatches/s ambient — µs/s territory. `[V]`
`ufunction_hook.cpp:78-100`: Func-patches are per-UFunction pointer swaps with STAMPED thunks —
an unhooked native pays ZERO, a hooked call pays 2 derefs + the cb body under table-based SEH.
`[V]` `engine_heap.cpp` is two vtable calls, trivial. `fname_utils::StringToFName` dispatches PE
(GT-only) but only at verb-resolve time — cold. **What remains `[?]` in the substrate: the GNS
internals** (crypto per packet, its own timers inside the 200 Hz `Poll()` loop) — vendored, never
measured by us; census C covered our locks around it. If an arm ever shows the net thread starving
the game thread, that is where to look; nothing points there today.

### 4-D. Census D results (rejoin residue) — landed 2026-09-02

All `[A]` with file:line; the headline ancestry check re-verified in the main session.

**D-VERDICT on the PERSISTENT rejoin mechanism: CLOSED.** After `bb881bab` (2026-09-01, "the
rejoin crash was 871 of our own rooted proxies holding a dead world") the static census finds **no
`GcPin`/`AddToRoot` site that can hold a dead world across a rejoin**: `GcPin::Release()` is
unconditional, raw Add/RemoveFromRoot have zero callers outside gc_pin.cpp (CI-policed), all three
teardown drivers funnel through `subsystems::DisconnectAll()`, which ends by ASSERTING zero
world-scoped pins (`subsystems.cpp:493`). **`[V]` main-session check: the b150-dev release tag
CONTAINS `bb881bab`** (tagged 22:58 vs fix 12:16, 2026-09-01) — so field players have the fix, and
the field 20-fps-on-rejoin is NOT the pinned-world mechanism; what a rejoin costs them is (a) a
TRANSIENT window — ~5 s of 2x `NumObjects` while the old world purges (scan_hub cost scales with
it) + the reaper's throttle-cancelled every-frame drain (~0.15 s) — and (b) **C2 rebuilt from
scratch: every join/rejoin re-materializes the 871 proxies + the keyed-prop excess (§4-A)**, the
persistent term. The USER's own one-time 20-fps catch on a client rejoin, if it predates 2026-09-01
midday, is fully explained by the then-live pinned dead world (2x everything) and is already fixed.

**D-GAP-1 (the big one) — `save_transfer::OnDisconnect()` is DEAD CODE, zero callers**
(`save_transfer.cpp:1030`) — the THIRD dead-capability instance on the join path in a week
(`docs/DEAD_CAPABILITY_REGISTER.md` candidate). It is the only path that frees the ~17 MB client
download buffer (`ClientArm` only `clear()`s, capacity stays for the process lifetime), zeroes the
stale `g_cliTotal` its own 2026-09-02 comment calls "a wrong number waiting for the first path
that reads it", deletes the `zcoop_<pid>.sav` temp (the header's no-steal window never closes),
and is the sole caller of `save_identity_bind::OnDisconnect()`. **Fix: wire it into
`DisconnectAll` (client-side clear).**

**D-GAP-2 — wrong-world RAW caches during the 44-s purge-lag window** (guarded by `IsLiveByIndex`,
blind to a dying-unflagged world): worst is `daynightcycle.cpp:149 g_gm` — `LatchDailyDelivery`
WRITES through it into the OLD world's saveSlot; same class: `save_block.cpp:54`,
`event_fire_sync.cpp:43`, `sleep.cpp:20`, `email.cpp:33`, `dish.cpp:20`, `meadow_store.cpp:32`,
`saved_signals.cpp:22`, `drone.cpp:33`, `device_screen.cpp:212`, `prop.cpp:453`. **Fix: type-swap
to `CachedObjRef` (world-stamped; the sibling `g_cycleCache:42` is already done right).**

**D-GAP-3 — per-slot recycle residue:** the desk-cursor stream QUINTET is missing from
`Session::ResetPeerRemoteState` (`session_status.cpp:324-351` resets pose/prop/ragdoll/hand,
omits `remoteDeskCursors_`+4 siblings, `session.h:970-974`) — a recycled slot's new occupant has
their desk-cursor stream **silently dropped for the rest of the session** (monotonic-seq gate reads
the departed peer's high-water mark). Similar: `desk_cursor_sync` receiver latches have no
per-slot clear; `remote_prop.cpp:68 g_pendingUnstick[4]` cleared nowhere;
`flashlight_click_sound.cpp:34` per-peer latch cleared nowhere; `RemotePlayer::Destroy` leaves
vitals/nick for the nameplate's spawn window. **Fix: add the quintet to ResetPeerRemoteState;
sweep the per-slot latches into `PerSlotState<T>` or the slot fan-out.**

**D-GAP-4 — bounded-but-notable:** host inbound `player_inventory_sync g_assembler` never
`ClearSlot()`s on the slot fan-out (half-assembly can merge with the recycled slot's next occupant
inside the 10-20 s TTL); and `container_contents_sync g_parked` has a host-side retention
gap with a SECURITY dimension, so its mechanism and its fix are tracked locally rather than here
(register **A66**; `docs/DOCS_ARC.md` WP-2 — an unfixed weakness with its conditions written
out is an exploit recipe, and this doc is public). Counted here so the gap is not lost; not
described here.

**D — verified NOT gaps:** all `PerSlotState<T>` users, the puppet/drive/hand/mirror per-slot fan-out,
every `CachedObjRef` holder (~30), the retired weather/item retry queues (the `subsystems.cpp:499-503`
comment about them is STALE — nothing queued in session N can fire into N+1).

## 5. Fix queue (per RULE 1 — root causes, ranked by measured ms; filled from §4)

Ranked by measured field impact (censuses A/B/C folded; D pending). **Q1 is USER-CONFIRMED
appetite (2026-09-02: "Я давно хотел с мусором pile типа разобраться и передизайнить").**

| # | fix | class | expected |
|---|---|---|---|
| **Q1** | **C2 RETIREMENT — the trash-pile/clump redesign per RULE 1** (A-HEADLINE): stop spawning GC-rooted Movable proxy actors for piles the client already owns natively; extend the rooted-native bind recipe to the clump form; find WHY `native_pile_mirror MATERIALIZED=0` in the field and why the client carries ~1,061 extra keyed props; retire the parallel aim-cone with the proxies (its per-E-press ~1,742 PE + join-time ~740k PE go with it). Design pass = /qf to convergence BEFORE build | client-asymmetric, engine-side — THE field 20-fps root | ~1,930 extra actors → ~0; the single biggest field win |
| Q2 | overlay: draw-predicate fix — voice term asks has-something-to-SHOW, not Enabled() + zero-vertex `RenderDrawData` skip (C-CRIT-1) | per-frame, EVERY player | full empty-overlay ImGui+DX11 pass → ~0 |
| Q3 | log discipline: latch `remote_prop` no-match WARNs (+ epoch-0, pump-fault, `[REL-EDGE]`), take WARN's sync fflush off the game thread; DELETE `[WA-TRACE]` (2 PE + 1 line per mirror per second, monotonic coin growth) and give coin mirrors a despawn answer (C-CRIT-2, A) | hitch/storm class | kills the 60-flush/s collapse lane + the growing trace tax |
| Q3a | rejoin-residue family (D): wire the DEAD `save_transfer::OnDisconnect` into `DisconnectAll` (~17 MB leak + stale progress + temp file + identity-bind clear); type-swap the §4-D raw `g_gm` caches to `CachedObjRef` (wrong-world WRITES in the purge window); desk-cursor quintet into `ResetPeerRemoteState`; per-slot latch sweep; assembler `ClearSlot` on the fan-out; `g_parked` host cap (+ TRACKER row) | memory + correctness on every rejoin | the leak class and the recycled-slot sync defects go; third join-path dead capability lands in the register |
| Q4 | miss-path walk family: `GetWorldContext` cache (per spawn packet!); firefly/event_cue resolve BELOW latch+throttle; weather negative-TTL; garbage_sync throttle; death_revive per-field latches (update-day bomb) (A) | conditional per-frame full walks | absent-asset windows stop costing ~1.3 ms×N/frame; coin sale stops costing 47 walks/frame |
| Q5 | per-frame fan-out hygiene: interactable channels get the grime 50 ms throttle + pointer scratch; npc/world-actor mirror drives interp-gate their PE writes when rested; dish 24-slot scan gates on window-open; ParkWalk attempt cap; pause_guard → event-driven (A) | client-heavy per-frame CPU | trims the client's structural CPU excess |
| Q6 | batch-pose lanes → fixed arrays; reliable inbox → SPSC ring + bounded per-tick drain; reassembly reserve-with-cap (C-HIGH) | steady + join allocs | 180 cross-thread mallocs/s → 0; the ~2,300-msg one-frame join spike → bounded |
| Q7 | `FindFunction` → Children-chain walk + result cache (F2) — also de-fangs every positive-only latch drift | substrate primitive | ~1.3 ms → µs cold, O(1) warm; 522 sites |
[withdrawn at the author's request]
| Q9 | FName index-compare + `ClassDefaultObject` direct read (F3); reseed drain caches the `Default__` verdict per class (A#4); ParamFrame small-buffer/arena + prebound frames (F7) | substrate primitive | retires the alloc+free-per-compare class; halves the reseed floor; ~9k marshalling allocs/s → ~0 |
| Q10 | SEH absorb-rate 1 Hz diag + latch pump/Func-thunk fault logs (C); UE4SS ini: `bUseUObjectArrayCache=false` pending arm 3a (B-FIND-1) | observability + config | storms name themselves; rig matches field config |
| Q-M1 | detour fast-path cache-line packing + combined Bloom (F1) | micro (~0.3 ms/frame) | after the above |

## 6. Method for the differential arms (when measurement starts)

The §9 discipline is the template, plus its two instrument lessons baked in:
- ONE variable per arm; PID-asserted logs (`boot: entry=cppmod ... pid=`); in-world witness
  independent of fps (atv_probe heartbeat class, never an fps threshold — §9.4 lesson 1).
- `overlayPresent`/s guards against grading a dead render path as slow frames (§9 arm D lesson).
- The rig is shared: `tools/game_lock.py` via `mp.py`'s dispatch, `docs/CROSS_SESSION.md` rules —
  a parallel session replaced the game four times during the §9 runs.
- Arms needed (draft): (1) heavy save, host+client, mod vs no-mod on the CLIENT — the H-GAMEWORLD /
  H-CLIENT-ASYM discriminator; (2) our dispatch bypass A/B re-run on the NEW loader (the 1.0
  ms/frame number predates the pin); (3a) `bUseUObjectArrayCache=false` A/B (B-FIND-1 — match the
  field default; if free, flip our installs + the shipped template); (3b) script_hook A/B
  (ConsoleEnabler+CheatManager rows off — prices B-FIND-2 on the NEW loader; capability loss noted,
  measurement only); (4) micro: per-PE fast-path cycle counter before/after any dispatch fix;
  (5) **the REJOIN arm (H-REJOIN)**: join → play → disconnect → rejoin on the same client process;
  sample fps + `NumObjects()` + `liveWorlds` + WARN-line rate + RSS before/after each phase — the
  discriminator between "residue held" (NumObjects stays doubled after purge window) and "purge
  lag" (recovers in ~min), with the WARN-rate column tying C-CRIT-2 in.

## 7. Log

- **2026-09-02:** arc opened. Prior art consolidated (§2), field intake seeded (§3), four census
[withdrawn at the author's request]
  this arc. Substrate deep-read done in the main session (§4a F1-F8: every ue_wrap/core hot TU
  read line-by-line). H-IMPORTS closed NO at mechanism level; H-CLIENT-ASYM confirmed = C2's true
  price; H-REJOIN resolved (persistent pin closed + shipped; residue = §4-D gaps); H-OVERLAY
  confirmed (voice-pinned gate); H-LOG-IO confirmed conditional. Fix queue ranked Q1-Q10 + micro.
  USER same day: C2/piles redesign appetite confirmed → Q1. NOTHING BUILT YET — this arc's first
  build lands after the Q1 design pass (/qf) or as the small Q2/Q3 items, user's pick.
