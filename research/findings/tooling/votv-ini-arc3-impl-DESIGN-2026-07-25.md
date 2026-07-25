# ini rework ARC 3 — impl design: T2-migrate + T2b + the const Row& ratchet (DESIGN, /qf-converged)

**Date:** 2026-07-25 · **Status:** DESIGN converged — /qf 16 rounds, genuine "that holds" at R16
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
