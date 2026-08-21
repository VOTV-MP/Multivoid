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

## 3b. Dev tooling adoption (user question 2026-08-21: DebugMod)

`acitulen/DebugMod` (github.com/Acitulen/DebugMod — README-only repo; the mod ships
via Thunderstore as a LogicMods BP pak + a `Mods/Acitulen-DebugMod/` folder; targets
0.9.0n) — **USE it as a dev instrument, do not rebuild it** (WP13 don't-reinvent;
dev tooling is RULE-2-exempt).
The ownership boundary: **single-world inspection/manipulation** (object locator,
inspector, spawn menu, teleports, time control) = DebugMod's — install it beside
Multivoid on a dev peer; **wire-aware debug** (anything that must act on BOTH peers
or exercise the sync) = ours, as today (F8 scenarios, sv. console, autotest).
Second use, and the sharper one: DebugMod is a ready-made **divergence-injection
instrument** — mutate ONE peer's world through it (spawn/destroy/teleport/time) and
watch our census/sweep/authority lanes react; that is the coexistence doc's
adopt/amplify/fight/drift taxonomy made drivable on demand. Not a player-facing
recommendation (a one-peer world mutator mid-session IS the semantic-conflict
class); a dev-bench one.
**Attribution rule (USER 2026-08-21): "keep for sure, also credit if taken
something there"** — if any DebugMod function/technique is ever ported into our
code, it gets source-comment attribution + a README credit row, exactly the
RE-UE4SS discipline.

## 3c. WP-7 — the NATIVE DEBUG SUBSYSTEM (USER 2026-08-21: "We need the whole debug
mod in our mod, but the debug things needs to be stopped from getting published on
our github public repo and all debug features stripped from getting into the
official mod releases")

**The ask, decomposed:** (1) DebugMod's whole feature surface, implemented natively
in Multivoid (multiplayer-correct, credited per the attribution rule); (2) the debug
CODE never appears in the public repo; (3) official releases contain none of it —
STRIPPED, not disabled.

**The mechanism — one structure delivers both constraints:**
- The debug subsystem lives in a **private sibling git repo mounted at
  `src/votv-coop/dev-private/`** (the `site/` precedent: an internal git the public
  repo never contains). A `.gitignore` row enforces it can never be committed to
  the public repo (the project's ENFORCED never-commit pattern). Remote fork for
  the user: private GitHub repo under the org (backed up; lean) vs local-only
  (site/-pattern, max secrecy, bus-factor risk).
- **CMake optional hook (the only public trace):**
  `if(EXISTS .../dev-private/CMakeLists.txt)` → compile it in + define
  `VOTVCOOP_DEVBUILD`. A public clone / the CI release lane does not HAVE the
  directory, so **official releases are stripped BY CONSTRUCTION** — the release
  builder structurally cannot include code it does not possess. No flag to forget.
- Belt-and-suspenders (because b133 proved a local-build release exception can
  happen): dev builds embed a `MULTIVOID_DEVBUILD` marker string + a boot-banner
  "DEV BUILD" line + the F1 menu label; the **release judge gains a gate that
  scans the candidate DLL for the marker and REFUSES if present** (drill: build
  once with the dir present, assert the gate goes RED). RELEASE.md step 0 row.

**THE STRIP LINE (measured, the subtle half):** the release ritual MACHINE-ASSERTS
selftests on the SHIPPED bytes (`mp.py:848` requires `config-selftest: DONE fail=0`
in the host log; RELEASE.md:38; b133's release evidence was exactly this). So:
- **STRIPPED (private dir):** the interactive debug/cheat surface — the
  DebugMod-class menu (spawn menu, teleports, god/timestop/fullbright, ESP,
  object locator/inspector, function executor, event-runner UI, waypoint UI,
  base-repair cheats).
- **KEPT in official bytes (env-gated, inert, no UI):** the autonomous
  verification instrumentation — config/font/repertoire selftests, the autotest
  scenario machinery the smoke drives. It IS the release evidence chain; today's
  shipped reality already has this shape.

**Wire rule:** the private dir drives EXISTING lanes only (host-authoritative
where a lane exists; host-only action otherwise). It structurally CANNOT add wire
kinds — `protocol.h` lives in the public repo, so a dev build and an official
build always share the proto and interoperate. A debug action that would desync
without a lane is host-only until a public lane exists (per-feature review in the
private repo's own tracker).

**Scope — CENSUS-FIRST (reframed by the USER twice, 2026-08-21):** *"If our own
debug features are worst compared to debugmod, then it goes away"* AND *"DebugMod
can do a lot, and is probably not compatible with our own debug features, which
some of them must go probably."* Both directions of RULE 2 govern: no worse twin
survives on EITHER side. So WP-7 item 1 is not porting — it is the **three-way
census table**: every DebugMod feature (the full README list: timestop, time/speed
controls, reputation/points/drive-level cheats, immortality/satiety/stamina/
flashlight, fullbright, infinite inventory, spectator + swap, lights/clean/repair/
transformers/power, object locator/ESP/teleport/destroy/inspect, servers panel,
signal panel, event panel incl. meta-paranoia monitor, waypoints, portable console,
cross-level teleport, extended spawn with deferred spawn, property/function
inspector-executor) × our existing equivalent (harness/autotest/sv. console/dev
surfaces) × verdict: **OURS-DIES** (DebugMod better → ours deleted, feature served
by DebugMod on the dev bench) / **GAP-BUILD** (multiplayer-correctness demands a
wire-aware native version DebugMod structurally cannot be) / **KEEP-OURS**
(instrument-class, the smoke needs it — not a UI feature at all). Only GAP-BUILD
rows get built in the private dir; the smoke-instrument class stays public per the
STRIP LINE.

**COMPATIBILITY census (the user's "probably not compatible" — MEASURE, not
assume):** DebugMod beside Multivoid has named interaction risks with our sync
invariants, each a dev-bench measurement: its SPECTATOR toggles possession while
`GetController() != nullptr` is our codebase-wide local-vs-puppet discriminator;
its TIMESTOP/time-speed bends the clocks our interp/TTL lanes assume; its DESTROY
meets our census/sweep/orphan handling; its M/Alt+B/N/T/V/L binds meet our
input-owner key routing; its UMG menus meet our activeInterface term. Outcomes
feed the same table (an incompatible DebugMod feature on a NETWORKED session is
either the divergence-injection instrument working as intended, or a reason that
row goes GAP-BUILD). Note: its GitHub README targets 0.9.0k while Thunderstore
ships 0.9.0n-targeted — per-game-version drift is its normal too.

Credit: DebugMod (github.com/Acitulen/DebugMod) in the private repo's README +
source comments wherever a technique is ported; if any ported piece ever reaches
public code, the credit moves with it. Existing scattered dev-only surfaces
migrate into the private dir over time (RULE 2), EXCEPT anything the smoke needs —
that stays public + env-gated.

**WP-7 critic round (2026-08-21), the four sharpened edges:**
1. **The local-release trap:** on the ONE machine that has `dev-private/`, every
   in-tree build carries the marker → the judge refuses → a b133-class local
   exception could never mint official bytes here. Sanctioned path WITHOUT
   reintroducing a flag: **the local-exception lane builds from a CLEAN CLONE of
   the public repo** (scratch dir; structurally lacks the private dir) — the
   by-construction property is preserved because no override exists anywhere;
   RELEASE.md's exception recovery gains that step. Daily dev builds are never
   release candidates.
2. **The marker gate's standing positive control:** the gate SELF-TESTS every run —
   it embeds a control buffer carrying the marker in BOTH encodings (ASCII +
   UTF-16) and must find it there BEFORE scanning the candidate; plus it asserts
   the candidate path exists and parses as a PE (the b133 FINGERPRINT burn was
   path drift — this gate refuses to pass on a missing artifact). The marker
   itself is optimizer-proof by being LIVE CODE: it is the string the boot banner
   prints. Fingerprint coupling: the judge-gate workflow edit lands together with
   the already-owed fingerprint refresh (cacheless run 30610531855's artifact),
   one motion.
3. **DEVBUILD joins an official lobby — PRODUCT FORK, stated plainly:** the
   Paper-pair (target, build) admits a dev build compiled from the same source.
   Options: refuse / ANNOUNCE in the feed / silent. **Lean: ANNOUNCE** ("<nick>
   joined with a DEV build" — one flag bit riding the existing handshake, proto
   bump folds into the D-3 migration's own bump); refusing would break the daily
   dev workflow (dev+dev lobbies) and silent hides information peers deserve.
   HONEST FRAMING: this is dev HYGIENE, not anti-cheat — A3
   (docs/security/TRACKER.md) means any modified client can already do worse; the
   strip's purpose is that players are never SHIPPED a cheat menu and the code
   stays private.
4. **The wire rule's WRITTEN DEFAULT (not per-row discretion):** until the arbiter
   (COOP_SYNCER_MODEL) lands — **world-mutating debug actions are HOST-ONLY;
   self-state actions (own pawn: god/satiety/stamina/flashlight) are any-peer;
   read-only surfaces (inspector/locator/ESP) are any-peer.** Client dev peers do
   not get world writes even though A3 would technically relay them — dev tooling
   must not normalize the unvalidated-client-write path the security tracker
   flags. The per-feature review inherits this default; deviations need their own
   row and reason.

## 3d. WP-8 — THE HYGIENE SPLIT (USER 2026-08-21: "everything that is a
debug/dev/testing/tools goes in dev private, we need the best hygiene and that
requiers good qf sessions per rule 1" + "our builds with all that dev-private
stuff will be flagged as private or something")

**Widens WP-7 to the whole dev/test/tools surface. Round 1 run; the pass
CONTINUES next sessions per the user's commission. The census (measured):**
`harness/` = 34 files / 8,757 LOC and MIXED (session_runtime.cpp owns
`g_session` — shipping lifecycle in a dev-named folder; dllmain includes harness
headers); `coop/dev/` = 49 headers including an EXISTING half-DebugMod (freecam,
spawn_npc, spawn_menu_unlock, set_clock, force_weather, event_force/trigger,
add_points, restore_vitals, object_overlay/pos_hud ESP); shipping↔dev coupling
runs BOTH directions (remote_player, roster_ledger, grab_observer,
prop_lifecycle, engine_*, npc_* include dev/harness headers; `dev_gate.h`
exists); `tools/` is a THREE-way mix (public-CI gates that structurally cannot
move / ~30 probe+capture scripts / INFRA: the deployed Rust master's source,
vps scripts).

**The classification schema (by ROLE, never by folder name):**
CLASS-SHIP (public) · CLASS-CI-GATE (public, structurally — CI executes it) ·
CLASS-EVIDENCE (the tension class, fork below) · CLASS-DEVUI (private) ·
CLASS-PROBE (private) · CLASS-DEVOPS (fork below) · CLASS-INFRA (separate fork,
not dev/test).

**Round-1 answers (critic a2ef5140):**
1. **CLASS-EVIDENCE is a USER-SIGNED fork, conceded.** E2 (zero test code in
   shipped bytes; official smoke reduces to shipping-code evidence + a full
   smoke on a CI-built DEV TWIN) demotes a NAMED machine assertion
   (`config-selftest: DONE fail=0` asserted on the SHIPPED bytes, mp.py:848,
   RELEASE.md:35-43) to a structural argument — the exact demotion class the
   b133/fingerprint discipline exists to catch. **LEAN: E1** — the env-gated
   selftest/autotest instrumentation is NOT a debug feature (no UI, no cheat,
   inert without env vars; it is the release ritual's own instrument) and stays
   in official bytes; "everything" takes this one named carve-out. User signs
   E1 or E2.
2. **mp.py/lan-test/deploy: do NOT widen one sentence past its evidence,
   conceded.** A private mp.py re-creates the exact defect the user's own
   2026-07-29 decision fixed ("the release ritual's runtime gate is not
   runnable from a clean clone") and kills outside-contributor testability —
   the reviewability goal this whole workstream started from. **LEAN: the
   release-ritual runtime instruments (mp.py, lan-test, deploy-all) + all
   CI gates stay PUBLIC (role: release/contributor infrastructure); the ~30
   diagnosis probes + captures go private.** User confirms the "tools"
   boundary reading.
3. **The seam is ONE registration surface, not N site hooks:** a single public
   `dev_hooks` registry (named slots, null = no-op); dev-private registers
   everything through ONE entry point at boot. Machine enforcement: during the
   transition an include-boundary CI gate (the peerconn_gate/registry_gate
   pattern) FAILS any public TU including dev/harness headers; POST-split the
   headers do not exist in the public repo, so the compile error IS the gate;
   ongoing, the release marker gate covers the bytes.
4. **Sequencing: role promotions land FIRST** (session_runtime et al. out of
   dev-named folders — the public tree becomes honest before anything moves),
   then the D-3 spike, then the USER forks get signed, then the split migrates
   in slices (probes first: zero coupling; DEVUI second; evidence class per the
   signed fork), then D-3's WP-2/WP-4, then WP-7 GAP-BUILDs in dev-private.
   Per-commit invariant: build + smoke green at every intermediate state; no
   file is mid-flight in two workstreams at once.

**The DEV-BUILD FLAG (user-confirmed):** dev builds are flagged everywhere —
the MULTIVOID_DEVBUILD marker in the bytes (machine), the boot-banner DEV BUILD
line + version display "[DEV]" (human), the F1 label, and (pending the user's
join-policy call) the lobby feed announce.

**CLASS-CORPUS (USER 2026-08-21: "research folder also get fully offline from
public repo, what else?" — census run):** `research/` (345 md) goes private
IN-PLACE (its own git repo at the same disk path, public repo gitignores +
`rm --cached` — the site/ pattern; ZERO reading-order pointers break).
"What else", measured: **docs/security/ WHOLE** (13 files publishing the
threat model + a tracker of OPEN findings — the map of our own unfixed holes;
public repo keeps a one-paragraph SECURITY.md contact stub); the
internal-design half of docs/ (**92 of 129 files**: COOP_* maps, methodology,
events/items/signals/piles/upgrades/kerfur trees, VERSION_MIGRATION) — these
MOVE (into the private corpus) with a pointer sweep, a real migration slice;
the AI-process exhaust by name (docs/LESSONS.md, docs/OPUS_48_DISCIPLINE.md —
tracked today, the literal "improper ai usage" surface; .claude/ and
QUESTION_FORM_* and tools/workflows/ verified already untracked). STAYS
public: README, BUILDING, INSTALL, RELEASE, the CI gates, FEASIBILITY/ROADMAP
(fork-able). **HISTORY CAVEAT stated to the user:** tip-clean only — the
public git history keeps everything ever committed unless a rewrite/fresh-repo
cutover happens; lean NO rewrite; the D-3 migration is the one natural
fresh-repo moment if the user ever wants maximum removal (fork f).

**USER FORKS OPEN (the pass resumes on their answers):** (a) CLASS-EVIDENCE
E1-vs-E2; (b) the mp.py/"tools" boundary (lean: ritual instruments stay
public); (c) DEVBUILD-joins-official policy (lean: announce); (d) dev-private
remote: private GH repo (lean) vs local-only; (e) CLASS-INFRA (master-server
source + vps scripts: public transparency vs private attack surface);
(f) history: tip-clean only (lean) vs fresh-repo cutover at the D-3 release;
(g) ROADMAP/FEASIBILITY: public or corpus.

**EXECUTION PARKED (USER 2026-08-21, verbatim: "don't touch the repo for now,
we will apply those changes later, when we're done with UE4SS arc").** WP-7,
WP-8, and CLASS-CORPUS are DESIGN OF RECORD only until the UE4SS arc (D-3
spike -> swap -> lane re-home) ships. No repo mutations for the split before
then — no rm --cached, no moves, no private-repo creation, no role promotions.
Sequencing updates accordingly: the UE4SS arc runs to completion FIRST; the
split (role promotions included) executes after, with the forks signed by then.
Fork (f) note: if the user ever picks the fresh-repo cutover, the arc boundary
is exactly the moment — one more reason the parking order is right.

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
