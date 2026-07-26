# Porting Multivoid to a new VOTV version — the living playbook

**Status: LIVING DOC. Written 2026-07-26, BEFORE the first real migration.**
Everything below is measured against the tree as of that date. **Nothing here is
battle-tested yet** — Multivoid has only ever run against VOTV `0.9.0-n`. The
first time a new game version ships, this doc gets rewritten with what the work
ACTUALLY cost, and the estimates get replaced by facts. Until then, treat every
duration as unknown, not as small.

**PENDING ADVERSARIAL REVIEW (user, 2026-07-26): this doc has NOT been through a
`/qf` pass.** It was written in one pass from measurement; nobody has yet tried
to break it. Candidate blind spots to aim that pass at are listed in §9 — read
them before trusting any claim here as complete.

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

## 9. Known-unknowns for the pending /qf pass

Seeded 2026-07-26 while the measurements were fresh. These are the places the
author already suspects are thin — the review should NOT stop at them:

- **Is the surface really only those two files?** The 5/41/29/235 counts came
  from `sdk_profile.h` + `sdk_profile_names.h`. Nothing verified that no OTHER
  file hardcodes a game-version fact — an earlier grep found ~136 `+ 0x..`
  occurrences across `coop/` + `ue_wrap/`, and it is unknown how many are engine
  layout, how many are local struct math, and how many are a third copy of a
  game offset that this doc claims lives in one place.
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
- **Nothing about mods coexisting** (other VOTV mods, a different loader present)
  or about a game update that changes the RHI/engine build mid-line.

## 8. Migration history

| Game version | Date | Health check before | What moved | What it cost | Note |
|---|---|---|---|---|---|
| `0.9.0-n` | 2026-05-21 → present | n/a (bootstrap) | n/a | n/a | The build everything was derived against |
| _(next)_ | — | — | — | — | Fill this row from §4 step 7. Replace the estimates in §1 with what actually happened. |
