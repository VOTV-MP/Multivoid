# ini rework ARC 3 — impl design: T2-migrate + T2b + the const Row& ratchet (AS-BUILT)

**Date:** 2026-07-25 · **Status:** **ARC 3 BUILT WHOLE + verify package GREEN 2026-07-25 night.**
Commits: C1 `f88a78cf` (registry .inc + typed handles + ValidateRows) · C2 `faa0289d` (typed
Resolve/ResolveString + all string/Resolve sites + fonts ENUM rows) · C3a `3b9aba38` (62-site flag
sweep; IsIniKeyTrue DELETED) · C3b `1a7c70fb` (WRITE door re-keyed onto typed handles, 15 sites;
IdentityRow write-only handle; RoleIniKey retired) · C4 `beb73208` (T2b: session_manager
g_configured/env-fallback + ReadEnvA deleted; 24 autotest role-env reads → ONE latched
harness::autotest::IsClientRole(); ReadMasterUrl/FillP2PFields env literals ride row->envVar) ·
C5 `ad15ae7c` (registry_gate.ps1 + build-core.yml wiring; **enum_check.ps1 RETIRED** behind the
verified inheritor map) + `ce035619` (typed-resolver selftest twins + drills) · soft-cap cut
`fed851cb` (config_selftest.cpp; config.cpp 822→770).
**Verify (all green, evidence in §Verify below):** compile-proof C2664/C2665 recorded ·
registry_gate drills RED×5 on real identifiers + green control · AFTER-compare 100/100 defaults
equal + fonts column [3,3,1,1,3] migrated + must-FAIL control · deletion asserts must-ZERO with
positive controls on `db4d4790` · corpus gate **AFTER==NEWSIM on all 248** · arc-3 selftest
drills 15/15 ok, DONE fail=0 · smoke ×2 PASS (DLL `2ba9014653033cd5` ×4, proto 127 in tree) ·
perf audit PASS 0 CRIT/0 WARN · LOC caps clean. NOT hands-on (rides the next take).
DESIGN: /qf 16 rounds, genuine "that holds" at R16
("the plan's remaining risk is execution fidelity, not design") · **Parent:** the certified
`votv-ini-config-registry-DESIGN-2026-07-24.md` (arc-3 row: T2-migrate ~106 literal sites + T2b +
ratchet; enum_check.ps1 retires) · **Fact base:** measured THIS session on post-arc-2 HEAD
(`db4d4790` lineage): 108 literal-key sites (62 `IsIniKeyTrue` / 24 `ReadIniValue` / 22 `Resolve*`),
102 registry rows (68 F + 5 I + 5 Fl + 4 E + 18 S + 2 Identity).

## The commits

**C1 — registry: typed defaults + named typed handles.**
- `Row` gains a TYPED def (members `defB/defI/defF/defS`) — NOT a canonical string (a string would
  mint a second copy of defaults whose canonical owner is a compiled constant; the endpoint-move
  lesson). Where an owning constant exists, the row def ALIASES it: `net.port` →
  `coop::net::kDefaultPort` (measured the ONE owner, session.h:73); `net.master`/`net.signaling` →
  `coop::net::kOfficialMasterUrl`/`kOfficialSignalingUrl` (config.cpp's file-static `kBuiltin*`
  copies DELETE, RULE 2); `net.nick` → `kMyNameDefault`. Where today's owner IS the site literal
  (ui.scale 1.25f, voice numerics, `save`="s_may2026", dev flags false), the REGISTRY becomes the
  owner and the site literal deletes.
- ONE X-macro list generates BOTH the row table AND named TYPED handles
  `config_registry::rows::<key_with_underscores>` (types FlagRow/IntRow/FloatRow/EnumRow/StringRow;
  wrong-kind Resolve = compile error). Handle ctors take a PRIVATE registry tag — even
  `FindRow(...)`'s `const Row*` cannot be wrapped outside the registry TU (closes the R10 loophole).
- `constexpr ValidateRows()` + `static_assert` in the registry TU = the PERMANENT compile gate:
  numeric defs within [lo,hi]; enum defs ∈ tokens OR on the explicit empty-sentinel allowlist
  (`net.role`, `net.ice` — def "" = unset); `kFontRoleDefaultFamily[]` count == kFontRoleCount and
  every index < kFontFamilyCount.

**C2 — the API flip.**
- Public read API becomes `ResolveFlag/Int/Float/Enum(<typed handle>)` + NEW
  `ResolveString(StringRow&)` (env → ini → row def). Key-string overloads DELETED; `ReadIniValue` +
  `IsIniKeyTrue` LEAVE the public header (internal twins in config-internal headers only).
- The PUBLIC WRITE door becomes handle-keyed too (`WriteIniValue(<handle>, value)`; value stays a
  string behind the existing ValueValidForKey refusal) — symmetric ratchet, ~24 write sites convert.
  The string-keyed machinery (`WriteIniValueAt`, RemoveDuplicateKeyLines, reformat, skeleton) stays
  INTERNAL: it operates on keys discovered IN the file.
- Bespoke wrappers STAY (`ReadMasterUrl` custom gate + "DEFAULT" sentinel; `ReadNickname` 255 cap;
  FillP2PFields chains) — their conversion retires exactly the default copies + raw
  `ReadEnv("VOTVCOOP_NET_*")` literals (env rides the row's envVar via the shared PickRawLayered).
- Fonts: registry gains `kFontRoleDefaultFamily[]` (index into kFontFamilyTokens); the composed
  per-role rows are born ENUM-kind (tokens = the one shared family list; def = the role's token);
  fonts.cpp `RoleDesc.defaultFam` DELETES SAME COMMIT (derived from the registry index; the per-role
  assignment — user 2026-07-09 — is proven at migration by the BEFORE/AFTER instrument, then
  single-ownership). Garbage font values become panel-reported (INTENDED delta toward the ask: the
  report is SWEEP-owned — harness.cpp:137 RunBootSweep → ValueValidForKey — measured decoupled from
  fonts' resolve timing at overlay init).
- FindRow/IsKnownKey remain available only to the schema's own machinery (sweep/writer/panel —
  inherently string-keyed; their output feeds no read API).

**C3 — the flag sweep.** 62 `IsIniKeyTrue` sites → `ResolveFlag(rows::<flag>)` (def=false in-row);
`MasterEnabled()` stays public = `ResolveFlag(rows::enabled)` def=true. Vocabulary identity MEASURED
(both paths share FlagVerdictFromValue, config.cpp:632/:682; outcome tables equal for def=false and
def=true). Env axis is a no-op: the ONLY envVar'd flag rows are voice.enabled/test_tone/loopback/
fresh_boot — all already-Resolve sites, ZERO overlap with the 58 IsIniKeyTrue keys (grep-asserted in
the instrument, not prose).

**C4 — T2b, both F21 duplicates retire.**
- (a) session_manager's `!g_configured` env-fallback branch DELETES WHOLE + `g_configured` itself
  (write-only after) — tree-wide census: exactly 6 MasterUrl() callers, all internal post-boot
  actions; `g_masterUrl`'s static init is already `kDefaultMaster` → `kOfficialMasterUrl`.
- (b) the 19 autotest `ReadEnv("VOTVCOOP_NET_ROLE")` reads → ONE LATCHED helper riding
  `ResolveEnum(rows::net_role) == "client"` — THROUGH the registry (no bypass; the raw-env read was
  a SECOND resolver that could diverge from the product's own role resolve, config.cpp:444; the ini
  already authors product role, rig installs are four separate folders). Enumerated deltas: env
  `CLIENT`/`Client` now canonicalizes (mp.py writes exact lowercase — unreachable); env-unset + ini
  net.role present would now be honored (consistent with product; autotest routines are
  RUN_*-env-gated so unreachable in practice; drill-pinned). Latch = one file scan per process
  (T11 idiom; ReadNetConfig itself has ONE boot caller, harness.cpp:241 — no divergence window).

**C5 — instruments, drills, the standing gate, retirement.**
Proof classes (each with per-pattern POSITIVE CONTROLS per the NEG-grep lesson):
1. **Default-equality**: BEFORE = MULTIMAP {key → set(site, literal-default)} harvested from HEAD
   typed call sites, LOUD FAIL on same-key-different-default (known multi-site keys measured equal:
   voice.ptt_key ×2 "G", mic/output_device ×2 "", save ×2 "s_may2026"); AFTER = registry dump;
   per-key compare. Fonts' 5 per-role defaults harvested from RoleDesc vs the registry array.
2. **Alias class**: compile-enforced (row def IS the owning symbol — no runtime compare possible or
   needed).
3. **Deletion asserts**: tree-wide must-ZERO with positive controls on pre-arc-3 HEAD:
   `kBuiltinMasterUrl`/`kBuiltinSignalingUrl` statics; `ReadEnvA("VOTVCOOP`; raw
   `ReadEnv("VOTVCOOP_NET_` anywhere; `RoleDesc.defaultFam`.
4. **Corpus gate re-run**: arc-2's independent PS oracle (arc2_verdicts.ps1) unchanged — all 248
   (file,key) verdicts must hold (read semantics unchanged in arc 3).
5. **Absent-path drills** (fresh temp files via PATH-PARAMETERIZED Selftest twins of the typed
   resolvers — the live Resolve is untestable for absence: module ini + mp.py sets NET_ROLE in the
   smoke): per-kind representative absent-key read returns row def; MasterEnabled absent→true; the
   net.role sentinel TRIPLE (absent silent / present-empty loud / garbage loud → all def "" in
   memory). ENV layer gets its own control: `SetEnvironmentVariableA` on a dedicated dev row
   (set → assert-wins → clear). Layer completeness measured: PickRawLayered = env + the ONE module
   ini (config.cpp:664-675), nothing else.
6. **Grep-asserts**: SanitizeNickname's "Player" THEIRS-fallback INTACT (net.nick is read ONLY at
   config.cpp:506, the MINE axis); envVar'd-flag-rows ∩ IsIniKeyTrue-keys == ∅.
7. **WRITE∩LATCH census table** (as-built): per T3b write-target key — write surface / read sites /
   latch moment / live authority; proves every written key has one boot read + an in-memory live
   authority (fonts + ui.scale measured: picker/slider update memory, ini is persistence-only).

**The standing CI gate** — `tools/config/registry_gate.ps1` (replaces the retired enum_check's
non-inherited jobs), wired as a FAIL step into the GHA build lane (windows job, pwsh, EXPLICIT exit
per the last-child-code lesson):
- **Dead rows**: token = the QUALIFIED TAIL `rows::<name>` (-cmatch/Ordinal; bare names collide with
  locals like `save`/`enabled`); all alias vocabulary FORBIDDEN + asserted (using-directive,
  using-declaration, namespace-alias — three patterns, three injected must-FIRE controls);
  handle-pass positive control (a site naming the handle counts; a `const Row&`-taking helper needn't).
- **Write-only rows**: fail-unless-enumerated — FAIL on any write-only row NOT in the committed
  in-script allowlist (each entry carries a review comment); SYMMETRIC REAPING: FAIL on an allowlist
  entry matching no row AND on one whose row gained a reader. The table is forced to shrink.
- **Ratchet surface**: public config.h must not declare the enumerated string-keyed read/write API
  patterns (the standing robot twin of the one-shot compile-proof).
- Standing controls are FIXTURE-INJECTED in the hot lane (every push); the pre-arc-3-HEAD run is the
  one-shot COMMISSIONING drill, verdict-logged.

**enum_check.ps1 retirement** — in the SAME ratchet commit, behind a BLOCKING gate: the
assertion-by-assertion inheritor map is verified against the script's ACTUAL TEXT before deletion
(any assertion with no inheritor blocks the retire). Map: literal-keys⊆rows → compiler;
rows⊆readers → registry_gate; producer-whitelist → dissolves (handles); must-FAIL controls →
compile-proof (a temp `ResolveFlag("bogus")` edit must NOT build — no string overload; recorded) +
registry_gate controls. HONESTY NOTE: enum_check was HAND-RUN in arc 2, never CI-wired — the new
gate is a strict upgrade; same-commit swap leaves no coverage window.

**Real-lane drill** (b125-matrix style): one BRANCH push with an injected violation → the actual GHA
lane must go RED (failure + exit-code paths proven in the real workflow), then removed → green.

**Panel wording**: rejected rows say "using <def>" (from the row); empty-sentinel enum rows keep the
arc-2 "built-in default is used" wording — never "using ''".

**Then**: audits (perf + correctness agents) + smoke ×2 + LOC caps (config.cpp 762, watched;
config_internal.h seam exists) + the pre-handoff checklist. Ships as the next dev release (b127)
via the RELEASE.md ritual; b126's frozen assets = the arc-2 baseline for bisecting any future panel
finding (attribution stays structural).

## Verify (as-run, 2026-07-25 night)

- **Compile-proof (must-FAIL, recorded then reverted):** `ResolveFlag("bogus")` → C2664;
  `WriteIniValue("bogus","1")` → C2665 (config.cpp temp edit; scratchpad arc3/compile_proof.md).
  This is the enum_check must-FAIL inheritor.
- **registry_gate drills (real gate, real identifiers):** drill A = injected .inc dead row +
  using-directive in peer_action_feed.cpp + string decl in config.h → 3 violations, exit 1;
  drill B = player_guid dropped from allowlist + ghost entry → 2 violations, exit 1; reverted,
  green control run PASS (103+5 rows, allowlist=2). Fixture must-fire controls run EVERY
  invocation (dead/write-only/alias×3/ratchet).
- **AFTER-compare (proof class 1):** scratchpad arc3/after_compare.ps1 — BEFORE multimap
  (100 keys/108 sites/0 conflicts) vs the .inc dump: 100/100 default-equal (aliases matched by
  canonical symbol); expected-no-BEFORE = enabled (semantics-measured R2) + 2 identity rows;
  fonts pre-arc-3 RoleDesc column [3,3,1,1,3] == kFontRoleDefaultFamily. Must-FAIL control
  (999 perturbation) fired.
- **Deletion asserts (class 3):** ReadEnvA("VOTVCOOP / raw ReadEnv("VOTVCOOP_NET_ /
  ReadEnv("VOTVCOOP_MASTER / kBuiltin* / fonts defaultFam == 0 now; positive controls >0 on
  pre-arc-3 `db4d4790`.
- **Corpus gate (class 4):** smoke-1 host log → arc2_verdicts.ps1: **GATE PASS: AFTER == NEWSIM
  on all 248 verdicts** (read semantics unchanged through arc 3).
- **Absent-path/sentinel/env drills (class 5):** 15/15 `config-selftest: arc3 ... ok`,
  `DONE fail=0` (smoke-1 host log 22:39:22).
- **Grep-asserts (class 6):** net.nick = ONE read (config.cpp:505 ReadNickname, MINE axis) +
  ONE write (server_browser edit-edge); SanitizeNickname "Player" THEIRS-fallback intact
  (player_handshake.cpp:39). envVar-flag ∩ IsIniKeyTrue = moot (API deleted).
- **Smoke ×2 PASS** on final bytes (DLL `2ba9014653033cd5` ×4): run 1 with selftest+corpus,
  run 2 plain; both "both peers stable, client connected, host puppet spawned, no RAM breach";
  log diff clean (all WARNs pre-existing known classes, none config-domain).
- **Perf audit:** PASS, 0 CRITICAL / 0 WARN — all 20 write conversions edge-gated,
  IsClientRole one-shot magic static, C5 instrument zero product cost, MasterUrl strictly
  cheaper. Standing note: PickRawLayered = one file scan per Resolve* call — safe ONLY while
  every caller is latched/event-driven; keep in the audit prompt.

## WRITE∩LATCH census (class 7, as-built)

Every written key: write surface → the ONE boot/latch read → the live in-memory authority
(ini = persistence only; writes are edge-gated user actions — perf table confirms).

| key | write surface | boot/latch read | live authority |
|---|---|---|---|
| voice.mode | panel radio ×2 | voice_chat.cpp:99 init | VC activation state (devices restart re-reads) |
| voice.threshold_db | slider edit-edge | voice_chat.cpp:104 | VC::SetThresholdDb |
| voice.mic_gain_db | slider edit-edge | voice_chat.cpp:105 | VC::SetGainDb |
| voice.volume | slider edit-edge | voice_chat.cpp:113 | VC::SetMasterVolume |
| voice.mic_device | DeviceCombo click | voice_chat.cpp:106 (+ panel cache :44) | VC device (restart) |
| voice.output_device | DeviceCombo click | voice_chat.cpp:112 (+ :46) | VC device (restart) |
| net.nick | browser edit-edge | config.cpp:505 ReadNickname | g_nick buffer / session nick |
| browser.lastdirect | browser edit-edge | server_browser.cpp:72 latch | g_directIp buffer |
| ui.netstats | checkbox | net_stats_panel.cpp:29 latch | g_enabled atomic |
| ui.scale | slider edit-edge | scale.cpp:89 boot | SetUserScale in-memory |
| ui.font.<role> | family combo (SetRoleFamily) | ReadRoleFamiliesOnce | g_roleFamily + g_rolesRead |
| nick_color | picker ×2 | harness.cpp:132 | nick_color live state |
| nameplate | F1 checkbox | harness.cpp:128 | nameplate visibility |
| player_skin | mint + skin picker | config.cpp mint (internal read) | applied skin (local_body) |
| player_guid | mint once | config.cpp mint (internal read) | per-launch guid |
| ui.chat.peer_actions | checkbox | peer_action_feed.cpp:24 latch | g_enabled atomic |

## /qf rounds ledger (16 rounds, R16 = "that holds")

R1 typed-def aliasing (not string copies) / multimap harvest / fonts same-commit / deviation
surfacing. R2 nick MINE/THEIRS measured split; bespoke wrappers stay; string-env census (no site
gains env); vocabulary identity. R3 MasterUrl census → branch deletes; three-class instruments;
autotest-through-registry ADOPTED (the flagged deviation dissolved). R4 tree-wide census;
g_configured deletes; must-ZERO tree-wide; constexpr static_assert gate; unset-path measured.
R5 ci-flip enumerated; second-resolver dissolution; latched helper. R6 one-ReadNetConfig-caller (no
divergence window); analytic→drilled absent path; sentinel triple drilled. R7 sweep-owned reporting
measured; enum_check inheritor census; fonts assignment migration-proven; drill layer isolation.
R8 no init-ordering hazard; counts re-measured on HEAD; reverse census wired into CI. R9
retire-in-commit + compile-proof; case controls; T11 no-snapshot reconciled; no write-back path.
R10 private-tag handles; WRITE∩LATCH census; drills on shipped bytes. R11 coverage map; symmetric
write ratchet; constexpr stability; release-frozen attribution. R12 dead/write-only verdict classes;
blocking retire gate. R13 WARN eliminated (fail-unless-enumerated); per-spelling-class controls; CI
wiring + explicit exit. R14 never-robot-run RETRACTION; qualified-tail token + forbidden using
forms; symmetric allowlist reaping; standing ratchet-surface assert. R15 namespace-alias covered;
fixture-form standing control; branch-push real-lane drill. R16 "that holds".
