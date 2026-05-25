# Version portability — adapting the mod across game versions

The mod is tagged to mirror the game version (currently `0.9.0-n`; versioning
rule in `CLAUDE.md`). When VOTV ships a new build (e.g. `1.0.0`), parts of our
standalone engine access can break. This document is how we keep that **cheap to
detect and quick to fix**, by design.

## What is version-specific, and how it breaks

The mod couples to a build through three kinds of knowledge — all isolated in
**`src/votv-coop/include/ue_wrap/sdk_profile.h`** (the porting surface):

| Knowledge | Breaks when | Symptom |
|---|---|---|
| **AOB signatures** (GUObjectArray, FName::ToString, ProcessEvent) | any engine recompile — even a patch | resolve returns 0 |
| **Struct offsets** (UObject / FUObjectArray / FUObjectItem) | engine-version change (4.27 → 5.x); stable within a version | garbage reads / crash |
| **Content names** (`mainPlayer_C`, `untitled_1`, `K2_SetActorLocation`) | game blueprints/levels change | lookups return null |

Everything else (the walk, the lookups, `CallFunction`, the coop logic) is
version-agnostic and should not need touching.

## Design rules that make porting fast

1. **One porting surface.** All of the above lives in `sdk_profile.h`, nowhere
   else. Porting = review/re-derive that one file. Logic files reference it via
   `profile::...`; no version constants are allowed to leak into logic (keep the
   `ue_wrap` engine layer / `coop` logic split, principle 7).
2. **Fail loud, never silent.** Every resolve is checked and logged. Nothing
   reads an offset off an unresolved pointer.
3. **Functional validation, not just "matched".** The boot health check proves
   each primitive *works* (round-trips a known name, finds known classes) — this
   catches an AOB that matched the *wrong* site, the nastiest silent failure.
4. **Detect + announce the build.** The health check logs the exe FileVersion +
   size and WARNS when they differ from the build the profile targets
   (`kExpectedExeSize`). First line of triage: "is this even the build we built
   against?"
5. **Logging.** `ue_wrap/log` writes `votv-coop.log` next to the mod. Levelled,
   timestamped. This is the primary diagnosis tool.

## Boot health check (`reflection::RunHealthCheck`)

Runs on load, writes `votv-coop.log`. Sample (0.9.0-n, PASS):

```
target build: game=Alpha 0.9.0-n engine=UE4.27
exe fileversion=4.27.2.0  size=84751360 bytes
resolve: GUObjectArray=... (rva 0x4d8f910)
resolve: FName::ToString=... (rva 0x127d870)
resolve: ProcessEvent=... (rva 0x1465930)
  [ OK ] GUObjectArray / FName::ToString / ProcessEvent signature
  [ OK ] object array populated (offsets sane)
  [ OK ] FName::ToString round-trip   (object[1] == 'Object')
  [ OK ] FindClass(Object/Actor/World), FindFunction(Actor, K2_SetActorLocation)
==== HEALTH: PASS ====
```

On a new build, a `[FAIL]` line names the broken primitive and the verdict says
`sdk_profile.h likely needs re-derivation`.

## Re-derivation workflow (when health check FAILs on a new build)

1. **Confirm the build changed**: the health check's exe size/version vs the
   profile (the WARN line). Update `kTargetGameVersion` / `kExpectedExeSize`.
2. **Engine version**: if the exe FileVersion major/minor changed (e.g. 4.27 →
   5.x), the **struct offsets** in `profile::off` must be re-checked — a runtime
   header dump of `FUObjectArray` and a UObject vtable confirms them (the method
   used originally; see the finding below). Within the same engine version,
   offsets usually hold.
3. **Re-derive AOBs** for whatever `[FAIL]`s: launch once with UE4SS (dev tool)
   to get ground-truth addresses from its log, confirm in IDA, derive a unique
   AOB (wildcard rip displacements; verify with `find_bytes`), update the
   `kSig*` constants. ProcessEvent: dump a vtable, find the un-overridden slot.
4. **Content names**: if `FindClass(mainPlayer_C)` etc. fail in gameplay, the
   game renamed the class/function — update `profile::name`.
5. Re-run; iterate until `HEALTH: PASS`.

Reference: `research/findings/ue-wrap-reflection-2026-05-22.md` (AOBs, RVAs,
offsets, and the discovery method for all three primitives).

## Adaptation toolchain (shipped 2026-05-25)

In addition to the boot health check, two artifacts ease cross-version
porting:

- **`votv-coop-compat-report.txt`** — written next to the DLL on every
  boot by `harness::sdk_check::Run` (in `src/votv-coop/src/harness/`).
  Captures: exe FileVersion + size, every resolved AOB address with
  its computed displacement, every reflection-resolved class /
  UFunction / property offset, and the PASS/FAIL verdict per
  primitive. A snapshot of "what the mod sees right now".

- **`tools/sdk_diff.py <old.txt> <new.txt>`** — diffs two compat
  reports across versions. Flags AOB drift, offset shifts, missing
  classes / UFunctions. First-pass triage of "what changed in the new
  build" before reaching for IDA / UE4SS.

Many BP-cooked field offsets (~22 of them — `playerTransform`,
`save_gameInst`, `takeObj` params, etc.) that were previously
hardcoded in `sdk_profile.h` are now **reflection-resolved at runtime**
via `FindPropertyOffset`. This keeps `sdk_profile.h` focused on
engine-level invariants (UObject layout, AOB sigs, UFunction names)
that don't move within a UE4.27 patch series, while BP-cooked offsets
self-heal across game recooks.

See `memory/project_adaptation_strategy.md` for the patch-day workflow
and the deferred items (JSON override, engine UPROPERTY reflection,
patternsleuth integration).
