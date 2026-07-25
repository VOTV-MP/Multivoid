# ini rework ARC 4 — impl design: the T8 `multivoid.ini.example` catalog (DESIGN, /qf-converged)

**Date:** 2026-07-25 · **Status:** DESIGN converged (/qf 12 rounds, genuine "that holds" at R12;
core stable since v5, R6-R12 = instrument hardening) · **Parent:** the certified
`votv-ini-config-registry-DESIGN-2026-07-24.md` (T8 spec :210-214, instrument row :264, §9d :339;
arc-4 row) · **Prereq:** arc 3 AS-BUILT (typed row defaults — the "no placeholder" DONE criterion
is reachable; `votv-ini-arc3-impl-DESIGN-2026-07-25.md`).

## What it is

A generated, never-read settings catalog `multivoid.ini.example` beside the DLL: every registry
key with its human description, its default as a copyable commented line, its allowed
values/range, and its env twin — regenerated every launch, canonical over any hand edit.

## The design (numbered; R-tags = the /qf round that decided)

1. **Descriptions live in the .inc** — every row macro gains a trailing `desc` string literal →
   `Row::desc`; ValidateRows: non-null, non-empty (compile gate). ~108 hand-written descriptions
   carry HUMAN SEMANTICS ONLY — allowed tokens, [lo,hi] ranges, and the env-twin name are
   GENERATOR-EMITTED from `row->tokens` / `row->lo,hi` / `row->envVar` — the SAME columns the
   runtime consumes (one owner; PickRawLayered reads row->envVar at config.cpp:663) [R2/R5].
2. **gatedBy structured column** (`const char* gatedBy`, nullptr for all but net.master /
   net.signaling = "net.master.custom"); ValidateRows constexpr-asserts the named key EXISTS in
   kRows and is Kind::Flag — a renamed/deleted gate key breaks the BUILD, not the prose (the
   false-comment lesson) [R1]. Catalog prints "read only when <gatedBy>=1".
3. **Generator TU** `src/coop/config/config_example.cpp` (one feature per file), path-parameterized
   core. Per row: wrapped `;; ` desc lines + `; key=<default>` copyable line + `;;   env twin:
   VOTVCOOP_X ...` where envVar. Identity rows: `; player_guid=` + "(minted automatically)".
   Formatting: flags `1`/`0`; ints `%ld`; floats `%.9g` **via `_snprintf_s_l` + a static
   `_create_locale(LC_NUMERIC, "C")`** — bytes deterministic under ANY process locale, and
   FLT_DECIMAL_DIG guarantees round-trip for any FUTURE float default (only `float defF` storage
   exists — measured config_registry.h:75-90) [R6/R8/R9].
4. **Line grammar (structural discriminator)** [R5]: prose lines `;; ` (double semicolon; env-twin
   prose contains NO '=' so even a doubly-uncommented prose line stays parse-invisible [R7]),
   copyable key lines `; ` (single), exactly 5 BARE section headers in kSectionOrder order, blanks.
   Bare headers decided by MEASUREMENT (the parser is section-blind, F3): parse-identical to every
   consumer, and a whole-file copy yields a headered ini (T3b placement works). Spec-literal "all
   lines commented" deviation surfaced: no KEY line is active, which is the spec's intent [R1].
   Banner is SELF-REFERENTIAL-BY-NAME ("multivoid.ini.example is generated every launch and never
   read by the mod; edits belong in multivoid.ini") — copied into the live ini it stays factually
   true; the sanctioned workflow in the header = per-line "strip the leading `; `" [R4].
5. **Writer**: the ONE atomic-swap primitive (config_ini_write's AtomicWriteLines, exposed via the
   internal seam); called at harness boot right after EnsureIniSkeleton; **fail-soft** (WARN, boot
   unaffected); **tri-state compare-first** (ABSENT→write / identical→skip / differs→swap /
   UNREADABLE→skip+WARN, no doomed swap); deterministic bytes (NO timestamp) [R1]. **Field
   observability**: unconditional log lines independent of the example's own fate — WARN on any
   fail/skip, one INFO per boot on success ("catalog regenerated/up-to-date, N keys") [R9].
   **Per-boot STATUS recorded in memory** (Regenerated / UpToDate / FailedWrite /
   SkippedUnreadable / NotRun + key count), exposed as `ExampleGenStatus()` [R11].
6. **Round-trip seam** `SelftestCatalogRoundTrip` in config_selftest.cpp — the drill cannot mint
   typed handles (RegistryCtorKey is registry-TU-only), but config_selftest reaches
   `internal::*FromRaw(Row*, ...)` — the SAME validate+default cores and the ONE lexer
   (ReadIniValueAtPath) [R3]. It exercises the REAL workflow bytes: strip exactly the leading
   `"; "` from each single-`;` key line → temp copy → parse THAT → assert **FOUND + typed-equal**
   per kind (bitwise float) — absence can never trivially pass as default-equal [R5/R6].
   Ownership split: this drill proves catalog↔registry AGREEMENT; parser correctness is owned by
   the arc-2 independent PS oracle (248/248) [R4].
7. **SIX detectors × SEVEN matched negative controls + the locale canary** (the drill, env-gated
   config selftest; **input = the LIVE boot-generated .example**, read-only copy — absence = FAIL,
   and the FIRST assert is `ExampleGenStatus() ∈ {Regenerated, UpToDate}` so a WARN boot fails the
   drill regardless of surviving old bytes — the stale-green channel [R9/R11]):
   1. round-trip comparator ← flag 1→0 + float nudge + enum OTHER-VALID-token flips in the
      stripped copy (parse succeeds, equality must fail) [R6];
   2. line-length (wrap ~100, tolerance ~110) ← wrap-join;
   3. TRI-directional exactly-once (every registry key exactly once + every single-`;` line is a
      registry key + no dup) ← dup-key AND deleted-key (the ZERO direction: absence must fire the
      presence scan AND the FOUND assert) [R4/R7];
   4. env-only pattern (single-`;` line containing `VOTVCOOP_` = violation) ← injected copyable
      `; VOTVCOOP_SCENARIO=x` [R3];
   5. orphan (single-`;` line whose key is not a registry key) ← doctored single-`;` prose line;
   6. SECTION-PLACEMENT (scan tracks the current bare header; assert row->section == current for
      every key line; headers appear exactly once, kSectionOrder order) ← moved key line [R6];
   - LOCALE CANARY: `ParseWholeDouble("1.25") == 1.25` through the PRODUCT parse path — a
     comma-locale flip would otherwise silently green the round-trip (parse-fail→default==default)
     AND break product float reads; the canary makes it loud [R8].
8. **Standing gates**: ValidateRows additions = compile-time (desc non-empty; gatedBy coherent;
   NEW: string/enum defaults contain no ';'/edge-whitespace/CRLF — the inline-comment strip can
   never mangle a default [R10]). Release-time: docs/RELEASE.md step 0 gains the NAMED requirement
   "smoke with VOTVCOOP_RUN_CONFIG_SELFTEST=1; `config-selftest: DONE fail=0` in the host log";
   machine-asserted at the smoke layer — mp.py's monitor folds the line into its printed VERDICT
   when the selftest env is set (honest locality: mp.py is the never-commit rig tool; the rig is
   where releases are minted) [R10/R11].
9. **Named residuals / accepted costs**: (a) gatedBy consultation depth stays prose (anchored at
   measured UseCustomNetMaster→rows::net_master_custom, config.cpp:352; a behavioral unread-assert
   needs read-path instrumentation — disproportionate for 2 rows) [R2]; (b) desc SEMANTIC rot
   (mitigation: desc lives in the same .inc row a key edit must touch + per-launch regen) [R5];
   (c) a future non-dyadic float default renders ugly-but-exact ("0.100000001") — cosmetic; none
   today [R7]; (d) header env prose is variable-set-INVARIANT wording (no per-var content → nothing
   to rot; no detector needed) [R8].
10. **Enumerated worst-case user errors**: uncommenting an identity line → present-but-empty →
    the mint re-mints (guid hex check config.cpp:559-563; skin validity :531 — measured safe) [R2];
    hand-writing `VOTVCOOP_X=1` into the LIVE ini → the T10 unknown-key report fires (complement of
    IsKnownKey, measured arc-2) [R7].

## Build order

C1: .inc desc+gatedBy columns + Row fields + ValidateRows additions (the 108-description bulk).
C2: config_example.cpp generator + ExampleGenStatus + internal AtomicWriteLines seam + boot call.
C3: SelftestCatalogRoundTrip seam + the arc-4 drill in autotest_config.cpp (detectors + controls +
canary) + mp.py verdict fold (local) + RELEASE.md step-0 row.
Then: build, deploy, smoke-with-selftest (the generator's first-ever run = the drill's evidence),
LOC caps, audits, and the b127-dev release closes the whole ini workstream.

## /qf ledger (12 rounds)

R1 bare-headers-by-measurement / matched-controls / structured-gatedBy / fail-soft+tri-state+no-
timestamp. R2 controls match detectors; identity-empty measured safe; consultation residual;
columns-not-hand. R3 pattern-not-list; seam-not-handles; floats measured; regeneration-canonical.
R4 banner-by-name; tri-directional exactly-once; drill-vs-parser ownership. R5 `;;`/`;` grammar;
env-name one-owner; real-bytes uncomment; desc-rot residual. R6 FOUND+equal; section detector;
%.9g guarantee; triple flips. R7 zero-direction control; prose-no-equals; def-column types
measured. R8 locale pin + product-path canary; prose set-invariance; evidence path named. R9
field observability; drill on live boot bytes; all-rows walk. R10 release standing row; string-
default ValidateRows guard. R11 status-coupled drill (stale-green closed); ordering by
construction; smoke-layer machine assert. R12 "that holds — build it".
