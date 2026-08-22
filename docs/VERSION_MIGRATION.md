# Porting Multivoid to a new VOTV version — the living playbook

**Status: LIVING DOC. Written 2026-07-26, BEFORE the first real migration.**
Everything below is measured against the tree as of that date. **Nothing here is
battle-tested yet** — Multivoid has only ever run against VOTV `0.9.0-n`. The
first time a new game version ships, this doc gets rewritten with what the work
ACTUALLY cost, and the estimates get replaced by facts. Until then, treat every
duration as unknown, not as small.

**PENDING ADVERSARIAL REVIEW (user, 2026-07-26): the PLAYBOOK half of this doc
(§1-§6, §10) has NOT been through a `/qf` pass.** It was written in one pass from
measurement; nobody has yet tried to break it. Candidate blind spots are listed
in §9 — read them before trusting any claim here as complete. (**Exception:** the
UE4SS-switch decision, §11, DID go through a 26-round /qf that converged
2026-07-26 — that section is the audited part.)

Why it exists: "what happens when the game updates" is the single most common
and most legitimate question about a hook-based mod. Answering it with a shrug is
how a project dies quietly. Answering it with a measured surface, a runbook and a
set of gates is how it survives an author who is busy, ill, or gone.

---

## 1. The version surface — what actually breaks (MEASURED 2026-07-26)

Almost all of the mod is version-agnostic. The version-specific knowledge is
deliberately concentrated in two files, and the mod is ~146k lines of first-party
code across 734 files, so the ratio matters:

| Kind of knowledge | Where | Count | Breaks when | How it is re-derived |
|---|---|---|---|---|
| **AOB signatures** | `include/ue_wrap/core/sdk_profile.h` | **5** (`kSigFNameToString`, `kSigGUObjectArray`, `kSigProcessEvent`, `kSigFMemoryRealloc`, `kSigSaveGameToSlot`) | ANY recompile of the exe — even a patch | Real RE work: UE4SS log for ground-truth addresses, then IDA to confirm the RVA and derive a unique AOB (workflow written in that file's header) |
| **Engine struct offsets** | same file | **41** (`UObject_*`, `AActor_*`, `FUObjectArray_*`, `FMalloc*`, …) | Engine version bump (UE4.27 → something else). NOT by a game recook | Public UE4.27 layout / the SDK dump |
| **Game (blueprint) offsets** | same file | **29** (`AmainPlayer_*`, `mainGamemode_*`, `mainGameInstance_*`, …) | A VOTV recook CAN move these — a blueprint gaining or losing a property shifts everything after it | Mechanically, from a fresh UE4SS CXX header dump (each constant's comment cites its `*.hpp:line`) — **no IDA needed** |
| **Content names** | `include/ue_wrap/core/sdk_profile_names.h` | **235** constants (229 string names) | The game renames/removes a class, function, property, level or asset | Grep the fresh dump / the cooked assets |
| **Everything else** | the whole tree | **1,141** name-driven reflection lookups (`FindObject/FindClass/FindFunction`, property-by-name) | Only if the NAME changes | Nothing — they resolve at runtime by name |

Two consequences worth stating plainly:
- The recook-fragile set is **5 signatures + 29 game offsets + whatever names
  moved**. That is one file plus a name file — not a codebase-wide sweep.
- The mod does NOT hardcode addresses into gameplay code. If a signature fails,
  the mod says so at boot instead of corrupting anything (see §3).

**Exe fingerprint:** `kExpectedExeSize` (currently 84 751 360) + the exe's file
version are logged at boot and WARN on mismatch — that line is the first thing to
read after a game update, because it tells you the signatures are now suspect.

## 2. Failure modes, in the order you will meet them

| Symptom | Almost certainly | Where to look |
|---|---|---|
| Boot log: `[FAIL] GUObjectArray signature` / `FName::ToString` / `ProcessEvent` | The 5 AOBs, or 1-2 of them | `sdk_profile.h` §"AOB signatures"; §4 step 3 |
| Signatures OK but `NumObjects()` tiny / `[FAIL] object array populated` | Engine struct offsets (an engine bump, not a recook) | `sdk_profile.h` §"struct offsets" |
| Name round-trip or `FindClass(Actor/World)` fails | `sdk_profile_names.h`, or FName layout | health check output |
| Everything resolves, but one system is dead / reads garbage | A **game blueprint offset** moved (the 29) or a name changed | `tools/sdk_diff.py` against the previous dump |
| A hooked BP function never fires | The function was renamed, or its dispatch changed | `docs/COOP_DISPATCH_VISIBILITY.md` + the fresh dump |
| Save load/transfer misbehaves | The game's own save format changed | `docs/` save-transfer docs; both peers must run the same game version anyway |

## 3. The instrument that tells you: the boot HealthCheck

`ue_wrap::reflection::RunHealthCheck()` (`src/ue_wrap/core/reflection.cpp:585`,
called from `src/bootstrap/dllmain.cpp:110`) runs on every launch and prints a
`---- SDK health check ----` block. It does two things worth knowing:

1. **Resolution:** logs each resolved address AND its RVA, then `[ OK ]`/`[FAIL]`
   per signature. The RVA is what you carry into IDA.
2. **Functional validation, not just "an AOB matched":** it round-trips a known
   engine name (`object[1]` must be `"Object"`), resolves `Actor`/`World` classes
   and a known function. This is deliberate — a signature can match the WRONG
   site and still "succeed"; the round-trip catches that.

**Rule: the health-check block is the first artifact of any migration.** Paste it
into the migration note (§6) before changing a single constant.

## 4. The runbook

Assume: a new VOTV version shipped, the mod loads and either fails the health
check or misbehaves.

**Step 0 — freeze a baseline.** Keep the old game install. You need the old
CXXHeaderDump to diff against; if it is gone, the migration gets much harder.
(Dumps live outside the repo — `research/bp_reflection/` holds the Blueprint side.)

**Step 1 — record the boot artifact.** Launch once, save the health-check block,
the exe size/version WARN, and the first 200 log lines. This is the evidence base
for everything that follows.

**Step 2 — take a fresh SDK dump.** Install UE4SS into a COPY of the new game
build (`tools/install-ue4ss.ps1`), launch, press **CTRL+H** for the C++ header
dump (CXXHeaderDump/) and **CTRL+J** for the object dump. UE4SS is a development
tool here — it does not ship, and this is one of the two places it earns its keep.

**Step 3 — diff the dumps.** `python tools/sdk_diff.py <old_dump> <new_dump>
--out report.md`. It reports added/removed classes, renamed functions, **changed
property offsets per class**, and K2Node ordinal shifts — each annotated with the
corresponding `sdk_profile.h` constant. This is what converts the 29 game offsets
from "RE work" into "mechanical transcription".

**Step 4 — re-derive the signatures (only if the health check failed).** Per the
workflow in `sdk_profile.h`'s header: UE4SS's log prints ground-truth addresses →
compute RVAs → confirm each in IDA → derive a unique AOB (wildcard rip
displacements) → verify uniqueness. For `ProcessEvent`, dump a UObject vtable at
runtime and find the un-overridden slot. Update the constants; re-run the health
check until it is all `[ OK ]`.

**Step 5 — update the version identity.** `VOTVCOOP_GAME_TARGET` in
`src/votv-coop/CMakeLists.txt`, `kExpectedExeSize` in `sdk_profile.h`, and the
build number (`kProtocolVersion`). Join compatibility is byte-equality on the
pair, so an old cohort keeps playing among themselves — see `docs/RELEASE.md`.
Also update the game-target line in `docs/INSTALL.md` — ledger_lint's
INSTALL_STALENESS check mechanically fails every CI build and release until the
doc names the new target.

**Step 6 — run the gates, in this order.** Each one catches a different class:
- boot health check: all `[ OK ]`;
- `config-selftest: DONE fail=0` (env-gated; catches config/lexer regressions);
- the autonomous LAN smoke (`python tools/mp.py smoke`) — both peers stable,
  client connected, no RAM breach;
- the differential/verdict scenarios in `tools/mp.py` for the systems the diff
  said moved (containers, weather, desk, drives …);
- a hands-on take by a human. **Nothing is called "working" without it.**

**Step 7 — write the migration note.** A dated file in `research/findings/` with:
the health-check before/after, the sdk_diff report, every constant changed and
why, what broke that this playbook did not predict, and **how long it actually
took**. Then update §1 and §8 of THIS doc with the real numbers.

## 5. What makes this survivable by someone who is not the author

The honest bus-factor answer, in the order a stranger would need it:

1. `CLAUDE.md` — the rules and the reading order (start here).
2. This doc — what breaks and what to do.
3. `sdk_profile.h` / `sdk_profile_names.h` — the two files that hold the
   version-specific knowledge, each constant commented with its provenance
   (`mainPlayer.hpp:13`, an RE finding, an IDA address).
4. `docs/LESSONS.md` — the categorized ledger of everything the project learned
   the hard way, each row pointing at the file to read first.
5. `research/findings/` — dated, append-only RE and design log.
6. `tools/` — build, deploy, launch, autonomous tests, `sdk_diff.py`.

What a stranger does NOT need: any part of the author's setup, an AI tool, or
UE4SS at runtime. What they DO need: a Windows box with the game, Visual Studio,
CMake, and (for signature work only) IDA.

## 6. Standing risks, stated honestly

- **The 5 signatures are the real bill.** They need someone who can read a
  disassembler. Everything else in a migration is mechanical.
- **A UE version bump (not just a recook) is a bigger event** — the 41 engine
  offsets move together and the reflection primitives may change shape. That has
  never happened to this project.
- **This playbook is untested.** Written from measurement, not from experience.
  Its first contact with a real migration will change it.
- **The estimate trap:** do not publish a duration for a migration you have not
  done. Post what it cost afterwards.

## 7. Appendix — the maintenance critique, and the measured answers

A public exchange in the VOTV modding community (2026-07-26) put the maintenance
question sharply. It is recorded here without names because the ARGUMENTS are
worth keeping and the personalities are not. The critic was right about several
things; where the answer is a measurement, the measurement is given.

**Claim: "the thing being owned is ~144k lines directed but not written."**
Measured: 146,347 lines of first-party code, 734 files. The line count is
accurate. Authorship is stated in the README's Credits section and on the site:
one person directing, heavy AI use, fully public commit history.

**Claim: "when the signatures break, owning it means re-deriving offsets from
IDA."** Partly right, and the split matters: **5 AOB signatures** do need IDA
(and UE4SS for ground truth — step 1 of our own workflow). The **29 game
blueprint offsets** come out of a fresh SDK dump mechanically, not out of IDA.
The **41 engine offsets** do not move on a game recook at all. And 1,141
gameplay lookups resolve by name through reflection and survive untouched.

**Claim: "I question whether it works at month 18, or if you step away."** Fair,
and unanswerable by assertion. The structural answer is this doc plus §5: the
version-specific surface is two files with commented provenance, the failure is
loud (health check) rather than silent, and every gate is automated except the
final hands-on. The empirical answer only arrives after the first real migration
— which is why §4 step 7 exists.

**Claim: "switch to UE4SS, it does 99% of what you're doing, and it is maintained
by a team."** Measured: the part UE4SS could replace — loader, reflection, hook
engine, AOB scan (`ue_wrap/core`) — is **7,174 lines of 146,347, about 5%**, and
that is an upper bound (not all of `ue_wrap/core` is UE4SS-shaped). The other 95%
is co-op logic, per-class VOTV wrappers, UI and the test harness, none of which
any framework ships. The reason we do not take the 5% is stated in RULE No.3:
the shipping mod must not require players to install and version-match a second
loader. That is a deliberate trade — it costs us that 5% and buys install
simplicity and independence from another project's release cadence. UE4SS remains
a development dependency we actively use (SDK dumps, Blueprint dumps, ground-truth
addresses for signature work) — the refusal is about the RUNTIME, not the tool.
(This claim later went through a full 26-round adversarial pass and became a
recorded decision with re-open trip-wires — see §11.)

**Claim: "VoidTogether deserves credit."** Agreed and done (README Credits + the
site Q&A), stated accurately: no VoidTogether code is in Multivoid — it is a JS
server, this is a C++ in-process mod — and the two idea-level borrowings (the
nickname sanitizer approach, widget-styling comparisons) are each cited in the
source file that uses them.

**What the exchange actually produced:** two stale documentation claims were
found and fixed the same day — `docs/FEASIBILITY.md` still announced "Chosen
approach: UE4SS + reflection" (reversed the next day by RULE No.3) and still
described the overlay as riding "UE4SS's built-in ImGui" months after the mod
hand-rolled its own DXGI present hook. Hostile review is cheap QA; treat it that
way. See `memory/lesson_stale_planning_docs_are_public_ammunition.md`.

## 8. Migration history

| Game version | Date | Health check before | What moved | What it cost | Note |
|---|---|---|---|---|---|
| `0.9.0-n` | 2026-05-21 → present | n/a (bootstrap) | n/a | n/a | The build everything was derived against |
| _(next)_ | — | — | — | — | Fill this row from §4 step 7. Replace the estimates in §1 with what actually happened. |

## 9. Known-unknowns for the pending /qf pass

Seeded 2026-07-26 while the measurements were fresh. These are the places the
author already suspects are thin — the review should NOT stop at them:

- ~~**Is the surface really only those two files?**~~ **ANSWERED 2026-07-26
  (measured):** the "~136 `+ 0x..`" figure was wrong. The real census is **26
  occurrences in `coop/` + 8 in `ue_wrap/`**, and they are wire-struct / protocol
  byte math (e.g. `coop/creatures/npc_sync.cpp:357-364` parses our own packet
  layout; `coop/net/signaling_client.cpp:37-38`), not a third copy of a game
  offset. The premise "game offsets live in the two `sdk_profile` files" HOLDS.
- **The 1,141 "survive by name" lookups are asserted, not tested.** A renamed
  class/function fails at runtime, not at compile time. Is there any gate that
  would catch a name that vanished, short of the feature silently dying?
- **Blueprint bytecode / dispatch assumptions.** `COOP_DISPATCH_VISIBILITY.md`
  encodes which verbs are visible to our hooks and which need the VM path. A
  recook can change dispatch shape (EX_* opcodes, K2Node ordinals) without
  changing a single name or offset. This doc does not mention that class at all.
- **The save format.** §2 says "both peers run the same game version anyway" —
  but save_transfer ships the host's save blob, and an old save loaded by a new
  game build is the user's normal case. Unexamined.
- **The gates' coverage.** Step 6 lists health check / config-selftest / smoke /
  differential scenarios / hands-on. Nobody has asked which failure modes from §2
  those gates would actually catch, and which would pass all of them and still be
  broken.
- **The "mechanical" claim for the 29 game offsets.** It rests on each constant's
  comment citing an `*.hpp:line`. Spot-checked, not audited: if some of those
  comments are stale or absent, part of that work is RE, not transcription.
- ~~**Nothing about mods coexisting**~~ **PARTLY ANSWERED 2026-07-26:** the
  coexistence question (UE4SS + UE4SS mods beside Multivoid) now has a measured
  fact base — `research/findings/tooling/votv-ue4ss-coexistence-FACTS-2026-07-26.md`
  (no proxy-filename collision on any current channel; one ProcessEvent
  double-detour surface for the UE4SS 3.0.1 cohort; the dominant risk is semantic,
  not mechanical). Still unexamined here: OTHER VOTV mods (non-UE4SS), and a game
  update that changes the RHI or engine build mid-line.

## 10. Design rules that make this survivable (absorbed from VERSION_PORTABILITY.md, 2026-07-26)

These are the standing invariants that keep a migration to one file instead of a
codebase sweep. They predate this doc (written 2026-05-25) and are why §1's
surface is as small as it is.

1. **One porting surface.** All version-specific knowledge lives in
   `sdk_profile.h` + `sdk_profile_names.h`, nowhere else. Porting = review/
   re-derive those files. Logic files reference them via `profile::...`; no
   version constant may leak into logic (the `ue_wrap` / `coop` split,
   principle 7). VERIFIED 2026-07-26: the only raw `+ 0x..` literals outside them
   are 26 in `coop/` + 8 in `ue_wrap/`, all wire-struct/protocol byte math.
2. **Fail loud, never silent.** Every resolve is checked and logged; nothing
   reads an offset off an unresolved pointer.
3. **Functional validation, not just "matched".** The health check proves each
   primitive *works* (round-trips a known name, finds known classes) — this
   catches an AOB that matched the WRONG site, the nastiest silent failure.
4. **Detect + announce the build.** The health check logs the exe FileVersion +
   size and WARNs when they differ from `kExpectedExeSize`. First line of triage:
   "is this even the build we built against?"
5. **Logging.** `ue_wrap/log` writes `multivoid.log` next to the mod — levelled,
   timestamped, the primary diagnosis tool. (INFO lines are buffered until a WARN;
   a killed process loses them — see `docs/LESSONS.md`.)

### The adaptation toolchain (shipped 2026-05-25)

Besides the boot health check, two artifacts ease cross-version porting:

- **`multivoid-compat-report.txt`** — written next to the DLL on every boot by
  `harness::sdk_check::Run` (`src/harness/sdk_check.cpp:123`). Captures the exe
  FileVersion + size, every resolved AOB address with its computed displacement,
  every reflection-resolved class / UFunction / property offset, and the
  PASS/FAIL verdict per primitive: a snapshot of "what the mod sees right now".
  (Renamed from `votv-coop-compat-report.txt` at the 2026-07-19 rebrand.)
- **`tools/sdk_diff.py <old.txt> <new.txt>`** — diffs two compat reports (or two
  SDK dumps) and reports what moved, annotated with the `sdk_profile.h` constant
  each change corresponds to. This is §4 step 3's instrument.

## 11. The UE4SS-switch decision record (DECIDED 2026-07-26: F1, keep RULE 3)

A public critique (§7) argued the substrate should move onto UE4SS's C++ API. The
question went through a 26-round adversarial /qf pass (fact base:
`research/findings/tooling/votv-ue4ss-switch-decision-QF-WIP-2026-07-26.md` §3+§5,
which remain the cited measurements; its §4 draft conclusion and §6 residual plan
are superseded by THIS section. Coexistence fact base:
`votv-ue4ss-coexistence-FACTS-2026-07-26.md`). **The user took F1 on 2026-07-26.**

**The decision, one sentence:** the substrate stays standalone (RULE 3) — the
"99% of what you're doing" is measurably a **1.6% pillar** (2,404 of 146,347 LOC)
whose lifetime cost was **5 repair commits in 1,282** (2 of which UE4SS's API
would have absorbed), whose framework value targets engine-version churn this
game has never had (UE4.27 its whole life, zero recooks), and whose C++ path is
**blocked for outsiders today** — while the migration playbook above is untested
and the game has never been re-cooked in the mod's life.

**Why F2 (their API) and F3 (vendor their engine source) fail — CORRECTED
2026-07-26, same day (the user asked "check their wiki"):** UE4SS's C++ engine
core (UEPseudo) is a private, **Epic-access-gated** repo. Anonymous measurement
(all still true): `git ls-remote https://github.com/Re-UE4SS/UEPseudo` → 404
under BOTH org spellings while the same transport against `UE4SS-RE/RE-UE4SS`
returns refs (positive control); `gh search repos UEPseudo` → **zero** public
mirrors/forks on all of GitHub; the release channel's `zDEV-UE4SS_v3.0.1.zip`
(166 entries) ships **zero** `.h/.hpp`, zero `.lib`; our vendored
`deps/first/Unreal` gitlink (→ `d72d2f38`) is present but empty. **However** —
the first record OVERCLAIMED "un-buildable for outsiders": UE4SS's own README
(vendored copy, lines 80-82) and docs + issue #577 document a self-service
access path — link a GitHub account to an Epic Games account (the same free
gate as Unreal Engine source itself) to pull the submodule. So a C++ mod IS
buildable by any individual who passes the Epic linkage. **The leg that
survives, and it is structural, not access:** UEPseudo is Epic-derived code
behind Epic's source-access terms — it can be neither vendored nor made a
dependency of a public repo. Under F2/F3, Multivoid would stop building from a
plain `git clone --recursive` (today it does — a measured bus-factor virtue CI
re-proves every push), and every contributor would need the Epic linkage. F3
("vendor it") is dead outright; F2 trades our reproducible public build for an
EULA-gated one.

**What switching would NOT buy** (each measured):
- *Mod compatibility:* the incompatibility with other mods is SEMANTIC (a
  world-mutating mod on one peer is adopted+amplified / fought / drifts — the
  coexistence FACTS doc), identical under both substrates; UE4SS has no
  multiplayer. Mechanically the standalone mod already coexists (no filename
  collision; the PE detour stacks).
- *The overlay:* as measured 2026-07-26, UE4SS's GUI is its own OS window
  (D3D11/OpenGL swapchain, **no DX12 backend**, never hooks the game's Present);
  our in-game DX11+DX12 overlay stays ours under any fork.
- *The recook work:* the game half (5 AOBs + 29 BP offsets + 235 content names +
  the 1,141 name-anchors) is ours under any substrate — it is the part that
  actually breaks (§1), and no framework covers it.

**Honest residuals the decision does NOT fix:** the untested playbook (F4: no old
VOTV build is on disk, so the migration drill cannot run — an ACCEPTED RISK; the
pre-named threshold is **>3 working days back to a green smoke after a recook =
unbearable**, which re-opens this record); the bus-factor question (measured half:
a fresh `--recursive` clone resolves and CI fresh-builds every push; unmeasured
half: a successor carrying the game half — this doc is the mitigation); our own
`FindFunction` superclass-chain gap (`reflection.cpp:427`) — queued fix, home =
the auto-memory backlog, precondition: call-site census.

### The trip-wires (each one re-opens this decision, or it does not belong)

`tools/release/tripwires.ps1` — run from the RELEASE.md step-0 bullet; output is
pasted into the written release handoff. ADVISORY (always exit 0): a FIRED wire
re-opens the DECISION, never blocks a release. Detection latency = the release
cadence (rare, end-of-session by standing policy) — acceptable for a months-scale
decision, and stated here rather than implied. Verdicts per wire: QUIET / FIRED /
CHECK-UNREACHABLE (+ OVERDUE-DECISION, the mechanical no-wallpaper detector
backed by the committed `tripwires_state.json`).

- **wire-a (machine):** `git ls-remote https://github.com/Re-UE4SS/UEPseudo`
  SUCCEEDS → the F2/F3 blocker fell; re-open both forks.
- **wire-b (machine):** a non-prerelease UE4SS release newer than the frozen
  decision baseline **v3.0.1** exists → the "stable is 2.5 years old" leg fell.
  The check enumerates + filters `prerelease==false` and prints the newest
  SKIPPED prerelease each run (the live feed carries `experimental-latest`,
  2024-12-29, newer-dated than the stable — a standing positive control of the
  filter) plus a repo-health line (`archived`, `pushed_at`).
- **wire-c (monitor-less, by design):** the game leaves UE4.27 / gets re-cooked.
  No monitor exists or is pretended: a recook breaks the mod LOUDLY (boot health
  check, §3), and the forced migration-playbook run is where this fork re-opens.
- **Human-carried doors (named, not machine-watched):** the zDEV release asset
  starts shipping headers/libs (a C++ SDK — needs asset inspection, not a cheap
  probe); a successor fork becomes the community's live line (the API follows
  renames, and an archived repo serves its old releases green forever).

**Re-quiet / no-wallpaper rule:** a FIRED (or twice-consecutive UNREACHABLE)
wire's disposition is a dated `TRIPWIRE-DECISION <wire> <YYYY-MM-DD>: <text>`
line appended to the ledger below PLUS the matching constant update in
`tripwires.ps1`, in the SAME commit. The script detects a repeat-FIRED with no
newer decision line mechanically (OVERDUE-DECISION); only the disposition stays
human.

**Drill evidence (run on commit day, 2026-07-26, against the committed bytes;
pass criterion = verdict matches measured reality that day):**

```
DRILL fired-shape:  PASS -- control repo produces the FIRED shape
DRILL stable-floor: PASS -- floor 2.0.0 fires on the real feed
                    (newest stable v3.0.1 2024-02-14; newest SKIPPED prerelease
                     experimental-latest 2024-12-29; archived=False)
DRILL offline:      PASS -- .invalid targets -> CHECK-UNREACHABLE on both wires
                    (network-down never reads as not-fired)
DRILL overdue:      PASS -- seeded prior-FIRED + unrelated dated line stays
                    OVERDUE; a matching TRIPWIRE-DECISION line clears it
REAL RUN:           wire-a QUIET (404-class, control answers);
                    wire-b QUIET (v3.0.1 == baseline); state file written
```

### TRIPWIRE-DECISION ledger (append-only; the grep anchor is the line format)

TRIPWIRE-DECISION wire-b 2026-07-26: baseline frozen at v3.0.1 — the newest
stable at decision time; F1 taken, record created.
TRIPWIRE-DECISION wire-a 2026-07-26: same-day correction — the "un-buildable
for outsiders" leg was overclaimed; UEPseudo access is Epic-linkage-gated
(self-service), per UE4SS's own README/docs/issue #577. The blocker demotes to
the structural leg (EULA-gated dependency = non-vendorable, kills public-clone
reproducibility). wire-a still watches the repo going fully PUBLIC (that would
remove the structural leg too). F1 RE-CONFIRMED by the user the same day on the
corrected fact base, after probing the F2 cost themselves (verbatim: "Я Только
за F1") — the enumerated price: two-component install with CRT/ABI
version-matching (CppUserModBase.hpp:29), public-clone reproducibility lost for
every contributor, dependence on a revocable third-party access grant, a
2,404-LOC rewrite that still would not cover GNatives interception, DX12, or
the recook-fragile game half.

TRIPWIRE-DECISION human-door 2026-08-21: an UNLISTED door fired — the game's own
developers publicly critiqued the substrate choice (SentientYeet; the door list
named "a successor fork becomes the live line" but never "the game's devs reject
the approach"). A 5-round /qf re-audit then broke this record's fact base twice:
(1) the LOC premise — the real tracked count is 515,392 (`cat|wc -l`,
parts-sum-checked 341,054 + 174,338; the 146,347 above came from the xargs-batch
instrument `docs/LESSONS.md` already records), and (2) the F2 blocker's surviving
leg ("public-clone reproducibility") was Claude-authored inside this record,
never a user requirement — DROPPED per the drop-my-requirement rule. The terms
leg (`RE-UE4SS/docs/contributing.md:131`, Epic licensing) kills F3 (vendoring)
only. New facts from the pass: the public UE4SS C++ mod lane is thin (~8 repos,
two query shapes, closed-source invisible); CI under F2 needs an Epic-linked
token and fork PRs cannot build; the recook-fragile game half is ours under any
substrate; `types.h` verified zero-re-plumb; the one behavioral seam is the
game-thread pump under 3.0.1's eager PE detour (spike = HALT gate).
**USER DECISION 2026-08-21: F2 TAKEN — the substrate moves onto the UE4SS C++
API, pinned to stable v3.0.1. RULE 3 inverts; the standalone loader / AOB
reflection / PE detour retire whole per RULE 2 when the migration ships. The
double-Lua question resolves toward injecting the mod's API into UE4SS's own Lua
states (measured available: `on_lua_start` + `LuaMadeSimple::register_function`)
— ROADMAP phases 4-6 to be rewritten by the migration design.** Design of
record: `research/findings/tooling/votv-ue4ss-f2-migration-DESIGN-2026-08-21.md`.
Wire repurposing: wire-b now watches the PIN (a newer stable = an upgrade
decision, not a switch trigger); wire-a now affects contributor convenience
only. Both tripwires keep running until the migration ships, then retire with
the record they guard.

2026-08-22 (assistant, execution note): **WP-1 spike RUN AND PASSED** (commit
`cddb116c`, built 2026-08-21 evening) — the C-ABI shim boots the one binary on
all three live UE4SS eras (3.0.1 / experimental / shimloader profile, ~110 ms
start each), the PE double-detour stack ran LIVE under 3.0.1's eager detour,
and a LAN smoke completed a full join with one peer per loader lane. Evidence:
the AS-RUN block in the design of record §3. Two WP-4 findings: ini
atomic-swap writes fail under the shimloader VFS (home moves to
SHIMLOADER_CFG_DIR); shimloader panics on `xinput1_3.dll` by filename, so the
r2modman upgrade-error surface is shimloader's own log, not our dialog. wire-d
(the C loading contract) and wire-e (the safety premises) remain OWED at WP-6.

2026-08-22 (assistant, execution note 2): **WP-2 PRE-CUT LANDED; the proxy
DELETION is HELD on a measured blocker.** Landed: the ExeDir re-anchor
(`1d153d98` — runtime artifacts anchor on the game exe dir via one
`ue_wrap::paths::ExeDir()` owner), the start_mod boot-evidence flush
(`a767e1e7`), the whole dev-tooling move to the UE4SS lane (`1f762fa2` —
deploy-mod.ps1 replaces deploy-loader.ps1, install-ue4ss.ps1 owns per-copy
substrate presence, mp.py gains a boot-lane assertion and retires
set_dev_ue4ss), the ~139-row stale-prose census (`fe6ab1a7`) for WP-4/WP-6,
and the conversion of all four dev installs to UE4SS 3.0.1 + the mod folder.
Blocker ROOT-CAUSED 2026-08-22 (full `-fullcrashdump` decode): a **ProcessEvent
double-detour with UE4SS, corrupting via PolyHook's `followJmp`**. We MinHook PE
(E9 → an indirect `ff25[rip]` relay → our detour); UE4SS 3.0.1 hooks PE too, but
**LAZILY** (first `RegisterProcessEventPreCallback`, its PE dispatcher
`UE4SS.dll+0x554da0`); when its PolyHook `x64Detour::hook()` runs after us, its
`followJmp` follows our E9 into our relay and — because the relay is indirect —
patches the relay's POINTER slot, clobbering `&ProcessEventDetour` with a thunk to
a non-canonical address → our relay `jmp [rip]` → `#GP` → the `AV read -1`. The
variable is NOT install order (measured 14/14 boots WE-FIRST) but whether UE4SS's
lazy PE hook arms (0/15 solo, ~2/7 two-peer runs). **FIX DECIDED 2026-08-22 (`/qf`,
4 rounds) = B** (followJmp-immune relay: `ff25[rip]` → `mov rax,imm64; jmp rax`, so
followJmp stops on the `mov` and PolyHook cleanly in-place-hooks our relay → both
detours chain; source-confirmed via PolyHook's VALLOC2 path). **C ruled out** —
UE4SS's PE PreCallback returns `void` with no skip, so it cannot host our ~20
native-call interceptors; Multivoid must always own its own PE detour, and B is the
PERMANENT coexistence, not a stopgap. BUILT (`fac0c293` default-ON); baseline crash
**REPRODUCED in the real modded env** (r2modman/shimloader + experimental UE4SS +
DebugMod/CrashContext/PBMovement + an `ArmPE` fixture), fix compose PENDING hands-on.
Two coexistence findings surfaced: the crash is config-dependent (no stock mod arms
PE) and a separate exit-to-menu `IsLive`/VEH crash. The proxy stays in-tree until the
fix is hands-on-confirmed and commit 3 lands. **Canonical arc doc now
`docs/UE4SS_ARC.md`.** Record: design of record §3; FACTS doc §2; memory
`project-wp2-realistic-env-test-2026-08-22`.
