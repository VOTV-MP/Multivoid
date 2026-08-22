# UE4SS transition arc — Multivoid becomes a UE4SS mod (D-3 slim contract)

> **Canonical LIVING doc for the arc.** This tracks WHAT the arc is, the work-package
> breakdown, and the current state. It is the entry point; the deeper records are:
> - **Design of record (point-in-time):** `research/findings/tooling/votv-ue4ss-f2-migration-DESIGN-2026-08-21.md`
>   (the full D-3 mechanism, WP definitions, HALT gates, rejected forks).
> - **Decision ledger + tripwires:** `docs/VERSION_MIGRATION.md` §11 (why F2, the re-open
>   trip-wires, the dated execution notes; machine-checked per release by `tools/release/tripwires.ps1`).
> - **Coexistence facts:** `research/findings/tooling/votv-ue4ss-coexistence-FACTS-2026-07-26.md`
>   (what UE4SS + our mod actually do in one process; §2 = the ProcessEvent double-detour).
> - **The crash-fix design (WP-2):** the `/qf`-converged decision is recorded in §4 below and in
>   `scratchpad/qf_wp2/qf_fix_thread.md` (root cause) + `qf_fix_brief_r4.md` (the B-vs-C decision).
>
> Status tags: **DECIDED** (ratified), **AS-BUILT** (shipped + in tree), **PENDING** (built, not
> yet proven), **PARKED** (deferred by the user for now), **DEFERRED** (later phase by design).
> Keep this current when a WP moves; do not let a status label rot (the `/documentize` rule).

---

## 0. What the arc IS (DECIDED 2026-08-21)

Multivoid stops shipping its **own loader** (the `xinput1_3.dll` proxy) and instead ships as a
**real UE4SS mod** — `Mods/Multivoid/dlls/main.dll` + `enabled.txt` — that speaks ONLY the UE4SS
**C-ABI loading contract** (`start_mod` / `uninstall_mod`). This is the **D-3 "SLIM CONTRACT"**.

Three load-bearing choices, all DECIDED:

1. **C-ABI only, not the C++ `CppUserModBase` vtable.** The C++ mod-base vtable is ABI-unstable
   across UE4SS builds (measured vtable drift in the WP-1 spike). We expose the two C entry points
   UE4SS calls, plus 256 no-op vtable stubs for the slots UE4SS may call, and NOTHING else crosses
   the C++ boundary. Lua-injection (the earlier idea) is dead; the engine bridge is L-4 (below).
2. **The WHOLE substrate is KEPT.** Our AOB reflection, our MinHook UFunction/PE hooks, the coop
   network layer, the overlay — all of it stays and runs unchanged inside the mod DLL. UE4SS
   replaces only the *loader*, not the engine access.
3. **L-4 (engine access via UE4SS's own APIs) is DEFERRED** — and, as of 2026-08-22, known to be
   **permanently PARTIAL** (see §4: our ProcessEvent detour can never move onto UE4SS's PE callback,
   because that callback cannot intercept).

**Why the switch.** Triggered by VOTV dev SentientYeet's public critique of a standalone-loader mod.
A 5-round `/qf` re-audit broke the previous "keep RULE 3 / stay standalone" record (F1) twice — its
LOC premise and the F2 blocker's "public-clone reproducibility" leg both turned out Claude-authored
and were dropped per `[[feedback-drop-my-requirement-if-it-blocks-rule-1]]`. The user took F2 on
2026-08-21. Full record: VERSION_MIGRATION §11, the 2026-08-21 entry.

**RULE 3 is inverting, not violated.** The old rule ("UE4SS is a dev tool, never a runtime
dependency") is being deliberately retired for this arc. When the arc ships, the standalone loader /
proxy / dup-dialog retire WHOLE per RULE 2 — no standalone-and-UE4SS dual path.

---

## 1. Work-package breakdown + status

| WP | What | Status |
|----|------|--------|
| **WP-1** | Spike: prove the C-ABI shim boots the one binary as a UE4SS mod; measure the double-PE-detour survivability. | **AS-BUILT** — commit `cddb116c` (2026-08-21 eve). Matrix green ~110 ms; LAN join worked; double-detour "alive" on a SMALL sample (later found to crash ~2/10, see §3). WP-4 spike findings: ini err=3 under VFS; shimloader panics on `xinput1_3.dll`. |
| **WP-2** | The loader cut: delete `xinput_proxy.cpp` + the proxy deploy path (RULE 2); `cppmod_entry.cpp` in; predecessor detection + mutex; keep EVERYTHING else. | **IN PROGRESS.** Pre-cut LANDED (§2). Proxy DELETION is **HELD** behind the boot crash (§3), whose fix (**B**, §4) is **PENDING** (built + deployed, runtime proof not yet run). |
| **WP-4** | Fix the stale install/update/uninstall prose + the site + installer for the UE4SS lane. | **PARKED** — census written (`votv-ue4ss-stale-loader-prose-CENSUS-2026-08-22.md`, ~139 rows); fix deferred until the arc ships. |
| **WP-6** | Distribution re-home (the `multivoid-<game>-<build>.dll` filename + master + release flow onto the mod-folder shape). | **PARKED.** |
| **WP-7** | The native DEBUG subsystem (USER 2026-08-21: adopt UE4SS's debug tooling / DebugMod ideas). | **PARKED** — scoped in the design finding §3c. |
| **WP-8** | The hygiene split (USER 2026-08-21: "everything that is a tool, not the mod" moves out). | **PARKED** — scoped in the design finding §3d. |
| **L-4** | Engine access via UE4SS's own APIs (the "bridge"). | **DEFERRED**, and **permanently PARTIAL** — the ProcessEvent interception path stays ours forever (§4). |

Parking of WP-4/6/7/8 is the user's call, until the arc's blocking crash is closed and WP-2 ships.

---

## 2. WP-2 pre-cut — what LANDED (AS-BUILT, committed, NOT pushed)

The pre-cut (deliberately sequenced BEFORE the proxy deletion, so the deletion lands on a proven
substrate) is in tree, all authored per a 9-round `/qf`:

- `1d153d98` **ExeDir re-anchor** — one owner `ue_wrap::paths::ExeDir()`; every per-install artifact
  (log/ini/marker/banlist/players/screenshots) anchors on the game EXE dir, loader-independent
  (under UE4SS the DLL lives in `Mods\Multivoid\dlls\`, VFS'd under r2modman).
- `a767e1e7` **start_mod started-legs flush** — the boot-lane evidence line survives TerminateProcess
  teardown.
- `1f762fa2` **whole dev workflow onto the UE4SS lane** — `deploy-mod.ps1` (replaces deploy-loader),
  `deploy-all.ps1` rewrite, `install-ue4ss.ps1` (per-copy substrate owner), `mp.py` `_lane_check`
  (cppmod entry required, proxy line forbidden).
- `fd4a5b71` installer staging-path fix. `fe6ab1a7` the ~139-row stale-prose census (WP-4 input).

All four installs are CONVERTED: UE4SS 3.0.1 (pinned) + `Mods\Multivoid` mod folder. The proxy files
were removed from beside the exes, but the proxy SOURCE + loader lane + dup-dialog + inject.ps1 are
STILL IN TREE — the deletion is commit 3, held until §4's fix is proven.

---

## 3. The blocker — the UE4SS-lane boot crash, ROOT-CAUSED (2026-08-22)

An intermittent (~2/11 modded boots, 0 mod-free) `EXCEPTION_ACCESS_VIOLATION reading -1` during boot,
ONLY on the new UE4SS lane. **PROVEN** from a full `-fullcrashdump` UE4Minidump decode:

**It is a ProcessEvent DOUBLE-DETOUR, corrupting via PolyHook's `followJmp`.** Chain, every step
measured:

1. We MinHook `UObject::ProcessEvent` (exe+0x1465930): the target is patched `E9` → our MinHook
   **relay** (which, on x64, MinHook ALWAYS builds — `hook.c:607`), an indirect
   `FF 25 [rip+0]` + abs64 `&ProcessEventDetour`.
2. UE4SS 3.0.1 also detours PE, but **LAZILY** — the first `RegisterProcessEventPreCallback`
   (`LuaMod.cpp:3847` etc.) arms a PolyHook `x64Detour`. The *capability* defaults on
   (`HookUObjectProcessEvent{true}`), but the PLH hook installs only on first registration; ~80% of
   boots never arm it, which is why the crash is intermittent (0/15 solo, ~2/10 two-peer).
3. When UE4SS's `x64Detour::hook()` runs AFTER us, `followJmp()` (`ADetour.cpp`) follows our `E9`
   into our relay, sees the indirect `FF 25` (a branch WITH displacement), and resolves
   `getDestination()` to the OPERAND effective address — the relay's abs64 POINTER slot — then writes
   its own target-patch THERE, **clobbering `&ProcessEventDetour`** with a thunk into PolyHook's
   VALLOC2 holder region.
4. The next engine PE call runs our relay `jmp qword [rip]` through the now-garbage pointer → a
   **non-canonical** jump → `#GP` (which sets no CR2, so Windows reports "AV read `0xffff...ffff`",
   RIP at the relay). All symptoms match the dump.

**Measured, and it kills the obvious "wrong" fix:** the who-first probe shows we are ALWAYS
install-first (**20/20** — our trampoline holds the real PE prologue). So install-ORDER is not the
variable, and "install after UE4SS" (candidate A) is DEAD (we cannot be second — UE4SS is lazy, and
we are structurally first). The proxy lane never had this (no PolyHook in-process — months clean).

Lesson: `[[lesson-two-inline-hook-engines-collide-via-followjmp]]`,
`[[lesson-votv-crash-dumps-live-in-localappdata]]`.

---

## 4. The fix — DECIDED **B** (followJmp-immune relay); **C** ruled out architecturally

Converged over four `/qf` rounds (2026-08-22). The live fork was:

- **B — followJmp-immune relay (local, keeps the substrate).** Rewrite MinHook's relay for the PE
  hook from the indirect `FF 25 [rip]; abs64` form to a **non-branching-led** `MOV RAX, imm64; JMP RAX`
  form (same absolute-jump semantics; different encoding). PolyHook's `followJmp` STOPS on the `MOV`
  (`ADetour.cpp:66` — `if (!front().isBranching()) return true;`), so it does a **clean in-place hook
  of our relay** instead of corrupting the pointer, and **both detours chain**
  (PE → our E9 → relay → PolyHook jmp → UE4SS dispatch → PolyHook trampoline `mov rax,&ourDetour;jmp rax`
  → our detour → our MinHook trampoline → real PE). Source-traced end-to-end through PolyHook's
  VALLOC2 path; safe in the INPLACE-fallthrough path too (PolyHook's `hook()` fails cleanly, writing
  nothing, before any corruption).
- **C — observe PE via UE4SS's own `RegisterProcessEventPreCallback` (the deferred L-4 slice).**

### Why C is ruled OUT — the permanent constraint (MEASURED)

UE4SS's PE pre-callback is `void(TCallbackIterationData<void>&, UObject*, UFunction*, void*)` — it
returns **void and has no skip/cancel mechanism; the original ProcessEvent ALWAYS runs.** But our
substrate has **~20 INTERCEPTORS** that *cancel* the native call by returning true and NOT calling the
trampoline (`FireInterceptors`): trash-grab suppression, npc, desk_input, garbage, serverbox,
event_dispatch_world, kerfur, and more. UE4SS's callback **cannot host interception**. So C would
either (a) keep our own inline PE hook just for interception — the double-detour comes right back, C
fixes nothing — or (b) re-plumb every interceptor onto a different skip-capable mechanism UE4SS 3.0.1
does not clearly offer. Either way C is not the clean seam-swap it looked like.

**Consequence — a durable architectural fact worth carrying forward:** because interception requires
owning the PE hook, **Multivoid will ALWAYS run its own ProcessEvent detour.** L-4 may move *observation*
onto UE4SS callbacks someday, but the PE *interception* path stays ours. Therefore **B is not a
transitional crutch awaiting C — it is the permanent, correct way for our PE hook to coexist with
UE4SS's.** (This corrects an earlier framing that called C "B's eventual retirement.")

### Residuals of B (honest)

- **Teardown:** with B, PolyHook holds a restore-pointer INTO our 64-byte MinHook slot. Today
  `DoShutdown` → `MH_Uninitialize` frees that slot; if it frees before PolyHook restores, that is a
  restore-into-freed-page the proxy lane never had. Fix: **leak the PE hook at process-close** (never
  free it — the process is dying). This touches `MH_Uninitialize`'s all-or-nothing behavior (small
  blast radius to overlay teardown) — real work, folded into commit 3.
- **Second independent inline PE hooker:** another C++ mod that inline-hooks PE with a jmp-following
  engine would still corrupt the chain — but that is an ecosystem property that hits C identically,
  is unobserved, and B makes us strictly better than today (we stop corrupting UE4SS).
- **The `DIAG` probe** keys on the `FF25` relay signature B overwrites, so it was updated to
  recognize the `MOV`-led relay and the "PolyHook-composed" success case.

### As-built (PENDING proof)

The fix is implemented behind a proof flag (`VOTVCOOP_PE_IMMUNE_RELAY=1`, default OFF so the same DLL
reproduces the baseline crash for the A/B):

- `ue_wrap/core/hook.{h,cpp}` — `Install(..., bool followJmpImmune=false)`; the relay rewrite
  (`MakeRelayFollowJmpImmune`) runs between `MH_CreateHook` and `MH_EnableHook` (target unpatched →
  thread-safe), fail-closed if the `FF25` relay signature is not found.
- `ue_wrap/core/pe_detour.cpp` — reads the env, passes the flag; the `VOTVCOOP_PE_DIAG` probe now
  classifies the relay form (LEGACY-INTACT / LEGACY-CORRUPT / IMMUNE-INTACT / POLYHOOK-COMPOSED).

Built as `multivoid-0.9.0n-134.dll` (sha `76a8d200…`), deployed to all four installs. **Runtime proof
NOT yet run** — the gate before this becomes unconditional and commit 3 lands: build behind the flag
(done), run the crash-repro with flag-OFF (baseline still crashes) vs flag-ON (0 crashes + LAN join
clean + the probe shows POLYHOOK-COMPOSED on armed boots). Once proven, the env gate is removed (B
becomes unconditional; the flag was the proof scaffold, RULE-2 diagnostic) and the proxy deletion
(commit 3) proceeds.

---

## 5. State / hands-on warning

- The four installs are LIVE on the UE4SS lane. With `VOTVCOOP_PE_IMMUNE_RELAY` UNSET, a hands-on
  launch can still hit the ~20% boot crash; relaunching is safe (pre-gameplay, no corruption). To run
  the FIXED path, set `VOTVCOOP_PE_IMMUNE_RELAY=1` in the environment before launch.
- Rollback to the proxy lane if needed: copy `build/votv-coop/Release/xinput1_3.dll` + the versioned
  DLL beside the exe + delete `Mods\Multivoid\enabled.txt` (3 ops).
- Nothing here is pushed; commits are local pending the user's word + the five-axis leak audit.

## 6. Next steps (in order)

1. Run the B runtime proof (§4 "As-built"). Flag-OFF baseline crash reproduces; flag-ON 0/N + join
   clean + POLYHOOK-COMPOSED observed.
2. Make B unconditional (drop the env gate), add the teardown leak-at-death, re-run the pre-cut gate.
3. **Commit 3** — the proxy deletion (RULE 2): `xinput_proxy.cpp` + the loader lane + dup-dialog +
   `inject.ps1` go, fully. Then WP-2 is DONE.
4. Un-park WP-4 (stale prose + site + installer), then WP-6 (distribution), per the user's sequencing.

Related: `[[project-wp2-precut-and-trampoline-crash-2026-08-22]]`,
`[[project-f2-ue4ss-switch-decision-2026-08-21]]`.
