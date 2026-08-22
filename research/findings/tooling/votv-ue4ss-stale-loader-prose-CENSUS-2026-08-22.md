# Stale loader/install prose census — every surface still telling the xinput-proxy story

**Date:** 2026-08-22 (WP-2 day — the proxy deletion commit). **Status:** CENSUS, deliberately
NOT fixed here. **User directive (verbatim, 2026-08-22):** *"Все места со старой инфой о том
как работает или устанавливается мод, включая сайт, надо записать и исправить позже."* —
record now, fix later. **Consumers:** WP-4 (release/distribution/INSTALL surfaces) and WP-6
(README/RULE-3/docs/site sweeps) of the D-3 migration
(`votv-ue4ss-f2-migration-DESIGN-2026-08-21.md` §3). This doc is the WRITTEN inheritance list
so those WPs get a checklist, not a hunt.

**The new truth these surfaces must converge on:** the mod is a UE4SS-ecosystem mod folder —
`Win64\Mods\Multivoid\dlls\main.dll` + `enabled.txt`, started via the C-ABI `start_mod()`
contract; loaders: UE4SS 3.0.1+ (manual) or unreal_shimloader (r2modman/Thunderstore). The
xinput1_3.dll proxy + the drop-two-DLLs-beside-the-exe story are RETIRED (WP-2, this day).
Runtime artifacts (log/ini/marker/banlist/players/report/screenshots) anchor on the EXE dir
(commit `1d153d98`). The versioned `multivoid-<game>-<build>.dll` name survives ONLY as the
build/release artifact identity until WP-4 re-homes distribution (mod-folder zip).

Produced by a read-only sweep agent 2026-08-22 (whole docs tree + site + release lane + CI +
in-code strings; memory/ and dated research session records excluded as historical).
~139 rows across 24 files; counts per file at the bottom.

## Mechanical welds (fix-together sets — breaking one alone breaks CI or the release lane)

1. **INSTALL.md <-> ledger anchors:** `tools/release/ledger_lint.ps1:64-65` FAILS unless the
   anchor strings from `ledger_lib.ps1:149-150` (`WindowsNoEditor\VotV\Binaries\Win64`,
   ``delete the old `multivoid-*.dll` ``) appear verbatim in `docs/INSTALL.md`. The doc, the
   anchors, and the release-body template (`ledger_lib.ps1:231-233` "You need **both** files
   ... xinput1_3.dll (the loader)") must move in ONE commit.
2. **publish.ps1 hard-throws:** `tools/release/publish.ps1:25-27` requires exactly one
   `xinput1_3.dll` in the artifact dir; `ledger_lib.ps1:219-220` requires exactly one
   `multivoid-*.dll`. The release lane cannot publish the new asset shape until WP-4 rewrites
   both (it is ALREADY blocked by the b133 fingerprint debt).
3. **site source + built output:** `site/templates/*.html` AND the built `site/public/index.html`
   (live on the Worker) carry the same claims — edit templates, `zola build`, re-upload
   `public/` manually (site/ is its own remote-less git; a stale `public/` shipped once before,
   `site/NOTES.md:16-20`).

## Per-file rows (`path:line | claim | axis`)

### README.md (root) — 15 rows
- 4 | "A standalone mod that adds drop-in co-op" | RULE-3-claim
- 9 | "the newest multivoid-<game>-<N>.dll ... the DLL filename carries the identity" | artifact-naming
- 96-97 | "Standalone loader — xinput1_3.dll proxy + the versioned payload" | loader-mechanics
- 106-110 | "The mod is a single DLL pair ... thin proxy loader ... highest build wins" | loader-mechanics
- 116 | "no asset edits, no .pak repacks, no UE4SS at runtime" | RULE-3-claim
- 139 | artifact-name explainer | artifact-naming
- 166-171 | "Download both files ... drop them next to the game executable ... uninstall: delete the two DLLs" | install-story
- 232 | reference/ list implies UE4SS reference-only | RULE-3-claim (borderline)
- 246 | "deep sync ... on the standalone substrate" | RULE-3-claim
- 279 | "RULE 3 — Standalone mod. UE4SS is a dev tool only" (headline; 280-284 already carries the REVERSED note) | RULE-3-claim
- 300 | "It does not ship and is not required to play." | RULE-3-claim
- 307 | "This is a hook-only standalone mod." | RULE-3-claim

### docs/INSTALL.md — 10 rows + 1 gap (SINGLE OWNER of install prose; 100% stale)
- 12-13 | anchor-phrase note (weld #1) | install-story
- 34-35 | "it is two DLLs you drop next to the game's executable" | install-story
- 39-43 | filename identity + "the two files" | artifact-naming / install-story
- 45-50 | "Download both ... put them into ...Binaries\Win64" | install-story
- 54-61 | update flow: "delete the old multivoid-*.dll ... MOD INSTALL PROBLEM notice ... xinput1_3.dll almost never changes" | update-flow / loader-mechanics
- 71-74 | uninstall: "Delete the two DLLs" | install-story
- 85-91 | troubleshooting rows (dup notice; "DLLs must sit next to the exe") | loader-mechanics
- GAP | zero mention of UE4SS / r2modman / Mods folder; `docs/LESSONS.md:2670` already cites
  "INSTALL.md's r2modman upgrade language" which does NOT exist yet (forward-reference to satisfy;
  the upgrade language is written in `lesson-shimloader-owns-the-xinput-error-surface`) | install-story

### BUILDING.md — 11 rows
- 3-5 | "ships as a single standalone DLL plus a proxy loader"; "seven architectural principles" (there are 8) | loader-mechanics
- 7-11 | "the filename is load-bearing ... proxy scans ... Do not rename the output." | artifact-naming
- 16-18 | "A protocol bump therefore renames the artifact" | artifact-naming (stays true until WP-4 renames the artifact shape)
- 30-33 | CI artifact contains "the versioned payload DLL, xinput1_3.dll" (already false since WP-2 commit 3 — the proxy target is gone) | artifact-naming
- 92 / 114 / 165-166 | RULE-3 phrasings on ImGui/vcpkg/CRT | RULE-3-claim
- 191-194 | outputs list incl. xinput1_3.dll | artifact-naming
- 206 / 213-215 | "copies the proxy + payload DLLs into four local game copies"; manual install = two DLLs beside exe | install-story

### CLAUDE.md — 11 rows (gitignored working copy; WP-6 owns the RULE-3 rewrite)
- 72-97 | RULE 3 heading + body ("must run with no UE4SS present"; the INVERTING note 74-84 already present) | RULE-3-claim
- 155-156 / 177 | "resolve primitives standalone ... no UE4SS at runtime"; "our own standalone substrate" | RULE-3-claim
- 159-165 | "Injection / hooking is OUR xinput1_3.dll proxy" | loader-mechanics
- 217-219 | Paper-pair section: "the filename is load-bearing — the xinput proxy scans ..." | artifact-naming (the load-bearing HALF died at WP-2; the identity pair itself stays)
- 326 | ImGui note "violates RULE 3" | RULE-3-claim
- 743-744 | reading-order s29 entry re proxy scan (historical narration inside a dated entry — LOW priority) | loader-mechanics
- 924-929 | reading-order 4h: "decision F1 keep RULE 3 ... read §11 before answering why-not-UE4SS" | RULE-3-claim (superseded by the 2026-08-21 F2 decision; §11's latest entry is current)
- 930-934 | 4i INSTALL.md pointer (pointer valid; content behind it stale) | install-story

### docs/ARCHITECTURE.md — 8 rows
- 8-16 | "A standalone hook-only mod ... loads via our own proxy ... no UE4SS at runtime" | loader-mechanics
- 45-46 | layer diagram row `loader/ (xinput_proxy.cpp -> xinput1_3.dll)` (file deleted at WP-2) | loader-mechanics
- 61-68 | "loaded ... via our own xinput1_3.dll proxy ... No injection, no third-party loader." | loader-mechanics
- 90-104 | "Substrate: standalone (RULE 3)" section + the UE4SS-vs-ours table | RULE-3-claim
- 112-115 | "no UE4SS-named symbols appear in the shipping module" — ALREADY factually false
  (cppmod_entry.cpp ships the UE4SS C-ABI shim) | RULE-3-claim (highest-priority single line here)

### docs/RELEASE.md — 4 rows
- 16 | release page carries "multivoid-<game>-<N>.dll + xinput1_3.dll + SHA256" | install-story
- 21-22 | release-body Install block shape | install-story
- 51-55 | trip-wires: "a FIRED wire re-opens the UE4SS-switch DECISION" — the decision was
  re-opened and TAKEN (F2) 2026-08-21; the tripwire framing needs the WP-6 repurpose the design
  already names | RULE-3-claim
- 128-129 | "code-enforced invariant: the loader scans multivoid-*.dll ..." (the enforcing code
  is deleted at WP-2 commit 3) | loader-mechanics

### docs/RE_WORKFLOW.md — 7 rows
- 3-5 | RULE-3 framing + "three game copies" standalone rationale | RULE-3-claim
- 15-17 | "HOST + CLIENT copies have only xinput1_3.dll + multivoid-*.dll"; deploy-loader -Standalone cleanout | loader-mechanics (now false: all four carry UE4SS)
- 86 | "no UE4SS types/headers" adaptation rule (still true for ue_wrap style; reword) | RULE-3-claim (borderline)
- 94 | "we don't need scripting in production ... keeps the standalone DLL self-contained" —
  contradicts the L-4 modder-API lean recorded in VERSION_MIGRATION §11 | RULE-3-claim
- 99 | deploy-all description (proxy pair) | loader-mechanics

### docs/ROADMAP.md — 7 rows
- 148 | `WINEDLLOVERRIDES="xinput1_3=n,b"` Linux/Proton note (becomes a UE4SS/shimloader question) | loader-mechanics
- 216-227 | "Standalone shipping vehicle (RULE 3) — DONE" block (proxy loader / standalone reflection checkboxes) | RULE-3-claim (historical DONE section — annotate superseded, don't erase history)
- 367 | "same-box xinput1_3 loading" | loader-mechanics

### docs/FEASIBILITY.md — 4 rows
- 64-65 / 73 / 93 / 104 | AS-BUILT annotations phrased as "RULE 3: no UE4SS at runtime" | RULE-3-claim
  (the as-built FACTS stay true — own ImGui, own hooks, own input; only the RULE-3 justification
  prose needs the reframe)

### docs/VERSION_MIGRATION.md — 6 rows
- 110-111 | INSTALL.md game-target-line instruction (survives; the doc behind it changes) | install-story
- 142-144 | "a stranger does NOT need ... UE4SS at runtime" | RULE-3-claim
- 184-194 | §7 appendix's live argument "the shipping mod must not require a second loader" | RULE-3-claim
- 292 | **§11 heading still reads "DECIDED 2026-07-26: F1, keep RULE 3" while the section's own
  2026-08-21 entry records F2 taken — the single most confusing line in the repo** | RULE-3-claim
- 301-302 | "the decision, one sentence: the substrate stays standalone" | RULE-3-claim
- 335-336 | coexistence mechanics note (measurement still true; reword context) | borderline

### docs/MULTIPLAYER_UI.md — 1 row
- 344-347 | Boot-warning modal section: armed from MULTIVOID_DUP_FILES by the proxy — the whole
  arming chain + the modal are DELETED at WP-2 commit 3 (`ui/boot_warning_dialog`); the surviving
  refuse surface is cppmod_entry's native MessageBoxW | loader-mechanics

### docs/OPUS_48_DISCIPLINE.md — 1 row
- 53 | "RULE 3 standalone (UE4SS/IDA are dev tools; nothing of them ships)." | RULE-3-claim

### docs/COOP_SCOPE.md — 1 row (borderline)
- 304 | "All shipping behaviour rides through the standalone DLL" | RULE-3-claim (still one DLL; drop "standalone")

### docs/AUTONOMOUS_TESTING.md — 2 rows
- 3 | "current standalone-C++ harness" | borderline
- 148-150 | probe-terminals description (disables the proxy / -Restore re-enables) — the TOOL was
  re-pointed at WP-2 commit 2 (enabled.txt parking); the doc row must follow | loader-mechanics

### docs/LESSONS.md — 1 flagged row
- 2670-2671 | cites "INSTALL.md's r2modman upgrade language" that does not exist yet —
  forward-reference satisfied by WP-4's INSTALL.md rewrite | install-story (gap, not stale-old)

### site/ (own git, no remote; deploy = manual `public/` upload) — 13 rows
- `templates/index.html:18` | hero meta "standalone loader" | RULE-3-claim
- `templates/index.html:112-113` | RE-UE4SS credit "not required to play" | RULE-3-claim
- `templates/index.html:121-129` | "A proxy DLL loads the mod ... remove the two DLLs"; antivirus
  Q&A "ships as a proxy xinput1_3.dll" | install-story / loader-mechanics
- `templates/index.html:163-166` | Download: "drop the two DLLs into ...Binaries\Win64" | install-story
- `templates/base.html:46` | footer (every page): "a standalone runtime layer" | RULE-3-claim
- `public/index.html` (built, minified line 1) | ALL six of the above live on the Worker | (weld #3)
- `NOTES.md:74-76, 99-101` | pre-deploy check "exactly two DLL assets on the release" | artifact-naming
- `NOTES.md:164, 292-293` | "стандалон DLL-лоадер (UE4SS не нужен)" | RULE-3-claim
- `NOTES.md:197-221` | "Why not just build on UE4SS?" Q&A — the whole answer is inverted by the
  2026-08-21 decision | RULE-3-claim

### tools/release/ — 14 rows (partly weld #1/#2; the rest)
- `ledger_lib.ps1:146-150` | anchor-phrase constants | weld #1
- `ledger_lib.ps1:219-220, 231-233` | asset-shape throw + release-body Install block | weld #1/#2
- `ledger_lint.ps1:54-56, 64-65, 75` | INSTALL_CONSISTENT + INSTALL_STALENESS gates | weld #1
- `publish.ps1:11, 25-27` | artifact expectations incl. the hard-throw | weld #2
- `tag_regex_selftest.ps1:58, 84` | fixtures name the two-DLL shape (update with the lane) | artifact-naming
- `notes/b126.md:5` | "Settings live in one file next to the game executable" — STILL TRUE after
  the ExeDir re-anchor (kept deliberately); changelogs are write-once anyway | no action
- `LEDGER.tsv` | clean | —

### .github/workflows/ — 2 rows
- `build-core.yml:229` | comment "the DLL filename inside stays canonical (loader contract)" —
  the loader-contract half is dead; the name is now the release identity only | artifact-naming
- `build-core.yml:230` | artifact naming `multivoid-ci-<sha>` | borderline, fine

### tools/README.md + src/votv-coop/README.md
- `tools/README.md` rows for deploy/stop/mp_host_game were FIXED at WP-2 commit 2 (they document
  tools changed in that commit). Remaining: 76-77 "The standalone proxy is the production load
  path" (inject.ps1 row — dies with inject.ps1 at commit 3) | loader-mechanics
- `src/votv-coop/README.md:3-11, 26, 57, 59-60, 64-68` | "Two binaries" / proxy loader / output
  list / deploy-loader / "What's NOT in this tree: UE4SS" — 7 rows, all stale | loader-mechanics
  / RULE-3-claim (WP-6; the `loader/` dir row must also gain cppmod_entry)

### In-code player-facing strings — resolved at WP-2 commit 3
- `boot.cpp:160-164` dup-popup body + `ui/boot_warning_dialog` "MOD INSTALL PROBLEM" modal —
  DELETED with the chain (the env feeding them is never set again).
- `cppmod_entry.cpp:369-386` refuse dialogs — CURRENT (the new story, correct).
- `session_manager.cpp:222-225, 456-459` update-check/mismatch popups — loader-neutral, no action.

## Counts

| group | rows | hard |
|---|---|---|
| README.md | 15 | 15 |
| docs/INSTALL.md | 10+gap | 10 |
| BUILDING.md | 11 | 10 |
| CLAUDE.md | 11 | 10 |
| docs/ARCHITECTURE.md | 8 | 8 |
| docs/RELEASE.md | 4 | 3 |
| docs/RE_WORKFLOW.md | 7 | 7 |
| docs/ROADMAP.md | 7 | 7 |
| docs/FEASIBILITY.md | 4 | 3 |
| docs/VERSION_MIGRATION.md | 6 | 5 |
| docs/MULTIPLAYER_UI.md / OPUS_48 / COOP_SCOPE / AUTONOMOUS_TESTING / LESSONS | 6 | 4 |
| site/ (templates + built + NOTES) | 13 | 12 |
| tools/release/ | 14 | 14 |
| .github/workflows/ | 2 | 1 |
| tools/README.md + src README | 8 | 8 |
| in-code strings | 4 | 4 (resolved at commit 3) |
| **TOTAL** | **~139** | **~121** |

## Ownership split (who fixes what)

- **WP-4 (distribution/release re-home):** INSTALL.md rewrite (+ the r2modman upgrade language
  from the shimloader lesson), ledger_lib/ledger_lint anchors + release-body template,
  publish.ps1 asset shape, tag_regex_selftest fixtures, RELEASE.md 16/21-22/128-129,
  BUILDING.md artifact rows (7-11, 16-18, 30-33, 191-194, 206, 213-215), build-core.yml:229
  comment, VERSION_MIGRATION:110-111.
- **WP-6 (docs/repo sweeps):** README.md whole, CLAUDE.md RULE-3 rewrite + reading-order rows,
  ARCHITECTURE.md (incl. the already-false 112-115), RE_WORKFLOW.md, ROADMAP.md (annotate the
  DONE block as superseded), FEASIBILITY.md reframes, VERSION_MIGRATION §11 heading + §7
  appendix, OPUS_48:53, COOP_SCOPE:304, AUTONOMOUS_TESTING:148-150, MULTIPLAYER_UI:344-347,
  LESSONS:2670 forward-ref, src/votv-coop/README.md, tools/README.md:76-77, **the site whole**
  (templates + NOTES Q&A + rebuild + manual `public/` upload).
