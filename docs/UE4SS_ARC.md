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
| **WP-2** | The loader cut: delete `xinput_proxy.cpp` + the proxy deploy path (RULE 2); `cppmod_entry.cpp` in; predecessor detection + mutex; keep EVERYTHING else. | **IN PROGRESS.** Pre-cut LANDED (§2). Fix (**B**, §4) is BUILT + default-ON and its compose is **VERIFIED** (2026-08-22 16:02 real-env byte decode + 16:25 DEV `POLYHOOK-COMPOSED` boot, §4 Proof status). The IsLive/VEH arc is BUILT (D1, 2026-08-22 night, §4 -- run B pending the user). Remaining before the proxy deletion: symbolize the 19:17 real-env EXEC-at-NULL dump (§4 residuals), B's teardown leak-at-death residual, then commit 3 itself. |
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
- **OPEN (2026-08-22 eve): two intermittent client boot fatals on the coop rig WITH the ArmPE
  fixture enabled.** CLIENT_1 (fix B active, ArmPE forcing UE4SS's PE hook at boot) showed a
  `Fatal Error!` dialog during asset load twice in ~8 boots (18:11, 19:31); no UE4CC dump, no WER
  record (killed with the box up), mod log ends clean at the dispatch census both times.
  Interleaved boots with the SAME bytes + fixture passed, incl. a full join. NOT correlated with
  the D1 conversions (first fatal predates them). Hypothesis: a residual boot-time compose race —
  PolyHook writing the relay prologue while another thread executes it — which the same-day
  compose verification (small n) would not catch; or an unrelated UE/UE4SS boot fatal. The ArmPE
  fixture is now DISABLED on the coop rig (HOST+CLIENT_1 `enabled.txt` → `.off`) per the test-rig
  topology (the deliberate double-detour belongs in the r2modman repro rig); post-disable smoke
  PASSED. If it reproduces in the r2modman rig, capture the DIALOG TEXT (it is the diagnosis; no
  dump gets written) before dismissing it.
- **OPEN (2026-08-22 19:17, REAL ENV, dump analyzed): boot crash = EXEC-at-NULL with OUR frames
  on the faulting thread.** The user's real game (`Desktop\a09n\...\Win64`, profile build
  `F71621E0`, fix B ON, experimental UE4SS + VoidFax/CrashContext/Fusion stack) crashed ~7 s
  after launch; `crash_2026_08_22_19_17_22*.dmp` (37 MB) parsed by hand
  (`tools/debug/parse_dump.py`): exception `0xC0000005` DEP-EXEC of address **0x0** (RIP=0 — a call through a
  NULLed function pointer), and the faulting thread's stack resolves into **our main.dll**
  (base sz 0x11A9000 = the 17.5 MB Multivoid module, many frames) interleaved with
  **chrome_elf.dll** (VoidFax's CEF — its own hooker), **dxgi.dll**, win32u/dwmapi — the shape of
  the DXGI/Present seam, NOT the PE trampoline (that class was #GP at a noncanonical address,
  not exec-at-0). Working hypothesis: a multi-hooker collision on the Present chain (our overlay
  hook + CEF/FusionFix) nulling a chain pointer — the same coexistence CLASS as the PE
  double-detour, on a different seam. NEXT for this thread: rebuild commit `275e0f67` to
  regenerate `F71621E0`'s PDB and symbolize the stack offsets (+0x360E55/+0x308FD0/+0x11FFE2 …);
  the naive scan is return-address-noisy — symbolization decides. Also note the real profile
  root is **`C:\r2modman\r2modmanPlus-local\...`** (not AppData) — recorded so the next deploy
  doesn't hunt for it.
  **CROSS-LINK (2026-08-22 night, NOT a merge of roots): `docs/OVERLAY_CAPTURE_COEXIST.md`** opened a
  separate arc on exactly this seam — our ImGui draws from an inline hook on
  `IDXGISwapChain::Present`, which is the function RTSS/OBS/CEF-class hookers also patch. That arc's
  converged fix RETIRES our `Present` + `ResizeBuffers` inline patches (drawing instead from
  `FD3D11Viewport::PresentChecked`, upstream of the whole chain), which **reduces our footprint on
  the exact chain this crash implicates** — so it can only help here. Do NOT fold the two: this dump
  is unsymbolized and the coexistence arc is unbuilt; if the fix lands first, re-test this crash and
  record whether it survives. Whoever symbolizes the dump should read that doc's §3/§4 first — the
  hooker mechanics (who patches what, in what order) are already written up there.

### As-built (2026-08-22 — baseline REPRODUCED in the real modded env; compose VERIFIED same day, see Proof status)

- `ue_wrap/core/hook.{h,cpp}` — `Install(..., bool followJmpImmune=false)`; the relay rewrite
  (`MakeRelayFollowJmpImmune`) runs between `MH_CreateHook` and `MH_EnableHook` (target unpatched →
  thread-safe), fail-closed if the `FF25` relay signature is not found.
- `ue_wrap/core/pe_detour.cpp` — the immune relay is now **default ON** (`immuneRelay=true`;
  `VOTVCOOP_PE_IMMUNE_RELAY=0` forces the LEGACY corruptible relay for an A/B baseline repro — a
  RULE-2-exempt diagnostic escape, retired at commit 3). Boot logs `PE relay followJmp-immune (fix ON)`.
  The `VOTVCOOP_PE_DIAG` probe classifies the relay form (LEGACY-INTACT / LEGACY-CORRUPT / IMMUNE-INTACT
  / POLYHOOK-COMPOSED).
- Committed build `0e14a2ca` = flag-gated **default OFF** (`multivoid-0.9.0n-134.dll` sha `76a8d200`);
  `bd617056` flipped the source default ON. The current build (classifier fix included, see below) is
  sha `93AC315B` — deployed to all 4 installs + the r2modman test profile 2026-08-22.

**PROOF STATUS — both legs VERIFIED (2026-08-22):**
- **Baseline crash REPRODUCED in a REAL modded environment** [VERIFIED, matching real log/crash,
  2026-08-22 15:42]: r2modman `Default` profile (`unreal_shimloader` + **experimental** UE4SS +
  DebugMod + CrashContext + PBMovement + Fusion + FusionFix + VoidFax) + a 5-line `ArmPE` Lua fixture
  (`ExecuteInGameThread(fn, ProcessEvent)` — the only thing that arms UE4SS's PE hook) + Multivoid with
  the fix OFF → boot crash. Evidence: `multivoid.baseline-1542.log` in the a09n install =
  `RELAY: LEGACY-RELAY CORRUPT(double-detour hit)` + `WHO-FIRST: WE-FIRST`; `UE4SS.log`
  `ProcessEvent address 0x7ff64fc00fda` (= our trampoline **+0x1A**, the relay pointer slot); CrashContext
  + a UE fatal. So the mechanism holds identically on experimental UE4SS + shimloader.
- **Fix compose VERIFIED** [2026-08-22, twice, independently]:
  1. **Real env (user relaunch, 16:02, a09n + ArmPE, fix ON):** the trampoline byte dumps prove it —
     at install the relay is the immune form `48 B8 <&detour> FF E0`; at post-init that slot holds a
     foreign `FF 25` (PolyHook in-place-hooked our relay, i.e. UE4SS's PE hook DID arm mid-session —
     the exact race that crashed the 15:42 baseline). No crash; the session ran ~80 s (server browser,
     join attempt, clean shutdown). Log: `multivoid.log` 16:02 in the a09n install.
  2. **DEV copy (autonomous boot, 16:25, UE4SS 3.0.1 stable, no ArmPE):** classifier printed
     `IMMUNE-RELAY INTACT` at install → `POLYHOOK-COMPOSED(fix working)` + `WE-FIRST` at post-init,
     no crash. (Side datum: 3.0.1 in the DEV stack armed its PE hook within 10 s with NO ArmPE
     fixture — the earlier "lazy, 0/15 solo boots" measurement does not generalize to this stack;
     trigger unidentified, harmless now that the compose holds.)
  The 16:02 run also exposed a **classifier bug, fixed same day**: the diag's first-match scan read
  MinHook's own jump-back stub (`FF25 00000000` + abs64 → PE+6, which precedes the relay in the
  trampoline slot) as "the relay" and printed `LEGACY-RELAY CORRUPT` on every boot regardless of
  reality. The scan now locates the relay once at the install snapshot (only the relay's payload
  equals `&detour`) and classifies that remembered offset thereafter. The verdict strings above are
  from the FIXED classifier (DEV boot); the 16:02 real-env proof rests on the raw byte dumps, which
  were always trustworthy.

### Realistic-stack coexistence — MEASURED (2026-08-22)

The double-detour crash is **config-dependent, not universal.** Measured in the r2modman stack:
- **No mod arms UE4SS's ProcessEvent inline detour on its own.** All three C++ mods (DebugMod,
  CrashContext, PBMovement) import `UE4SS.dll` and use UE4SS's OWN API — `RegisterHook`/`ProcessEvent`
  (per-function) / `AddVectoredExceptionHandler` — **none ships its own inline-hook engine**; the Lua
  mods (Fusion/FusionFix/VoidFax) ride UE4SS too. UE4SS hooks ProcessInternal / ProcessLocalScriptFunction
  / BeginPlay / CallFunctionByName, and RESOLVES ProcessEvent's address, but installs **no PE detour**
  unless something calls `RegisterProcessEventPreCallback` — which no stock mod does (verified: not even
  jsbLuaProfiler). So the common stack **coexists with Multivoid with no crash.** The crash needs a
  PE-callback mod OR Multivoid's own multiplayer/join path (the unknown ~2/10 trigger).
- **Consequence for B (good):** the whole realistic stack has **no second independent inline PE hooker**
  → the §4 Q2 residual does not exist in a normal modded setup; B fully covers it.
- **The ExeDir anchor works under the shimloader VFS** (our `multivoid.log`/`.ini` land in the real
  a09n exe dir, not lost in the VFS) — but one boot failed to rotate the log (stale-log caveat).

### The IsLive / VEH exit-to-menu FALSE-CRASH — measured, designed, **D1 BUILT 2026-08-22 night**

**Re-scoped by measurement: it is NOT a crash.** Exiting to the menu (the user was HOSTING) produced two
CrashContext reports 9 s apart at `main.dll+0x11CC78` = `ue_wrap::reflection::IsLive` — but CrashContext
**cannot terminate anything** (no TerminateProcess/ExitProcess/MiniDump/`__fastfail` imports; it is a
VEH + `MessageBoxW`), the process survived (second report; no UE dump in the window), and the user saw a
POPUP over a fault our SEH absorbed by contract. **VEH fires before frame-based SEH**, so any VEH crash
reporter turns our first-chance probe AV into a user-visible "crash". Faults manifest only when the
freed page is DECOMMITTED — nondeterministic (two DEV menutravel runs silently clean).

**Root + design (10-round `/qf`, "that holds"): the ratified cached-pointer discipline (OPUS §3:59) is
violated at 78 censused call sites** — bare `IsLive` on cross-tick caches (prime suspect for this exact
symptom: `multiplayer_menu.cpp` `g_button`/`g_versionText`, freed all session, probed per menu tick on
RETURN to menu). Fix = `CachedObjRef {ptr, idx, serial}` (ue_wrap/core) + staged conversion of all 78 +
deterministic decommit drill + tripwire gate; acceptance = ZERO first-chance AVs from our probes; the
IsLive fault WARN now attributes its CALLER module-relative (`_ReturnAddress`, shipped in `F71621E0`).
**Design of record: `research/findings/tooling/votv-islive-zeroav-cachedobjref-DESIGN-2026-08-22.md`**
(census appendix, serial semantics, the filed ABA residual, the D2 purge-blind world-gate deferral +
its wire-window probe). **Run A (pre-fix attribution repro) was ATTEMPTED same evening: NO repro** —
zero probe faults that run (pages stayed mapped; the decommit nondeterminism measured in the real env
too), so run A is downgraded to opportunistic (the tripwire persists post-fix; attribution is never
lost) and the 15:45 caller stays formally unnamed. Run B (post-fix exit) = acceptance (no report
resolving into main.dll, no popup) — necessary-not-sufficient; the deterministic drill carries the
zero-AV proof.

**D1 BUILT (2026-08-22 night, commits `f675de11`..`712fa33b`): all 78 census sites converted.**
Evidence: `tools/reflection/islive_gate.ps1` CI-mode PASS (0 bare-IsLive-on-static tree-wide); the
deterministic decommit drill (`VOTVCOOP_RUN_ISLIVE_DRILL=1`) PASS on pre- AND post-conversion bytes
(legacy = exactly 1 absorbed AV with caller attribution; `CachedObjRef::Alive()` = 0 AVs); the
differential no-bypass menutravel bracket PASS around the prime-suspect commit; LAN smoke PASS on the
final bytes with zero IsLive WARNs. New one-root accessors: `Element::LiveActor()`,
`ActiveDrive::LiveActor()`; `SavedMaterial` carries refs; reflection's ANY-THREAD class cache
converted. The D2 wire-window probe RAN: **zero reliable leakage** (the exit window is ~1 s, closed by
the existing gameplay→MENU session-stop edge, not the 4 s flee poll) → D2 stays deferred; instrument
permanent (`mp.py wirewindow` + `coop/dev/wire_census`). NOT hands-on — run B pending the user.

---

## 5. State / hands-on warning

- **The r2modman test profile** (`C:\r2modman\...\VotV\profiles\Default`) AND all four `Game_0.9.0n_*`
  installs carry the D1 build `95B02A826950DDC4` (immune relay + all 78 CachedObjRef conversions +
  wire_census + the drill; 2026-08-22 night). Multivoid drops as
  `shimloader/mod/multivoid/dlls/main.dll` + `enabled.txt`; the game is a separate `Desktop\a09n`
  install whose Win64 has the shimloader `dwmapi.dll` + `ue4ss.dll`, launched via r2modman. The ArmPE
  fixture stays in the PROFILE (the repro rig) but is **DISABLED on the four coop-rig installs**
  (`enabled.txt` → `.off` on HOST/CLIENT_1) — two intermittent client boot fatals rode it (see §4
  residuals). Run A was attempted (no repro — see §4); run B awaits the user on the D1 build.
- Rollback to the proxy lane if needed: copy `build/votv-coop/Release/xinput1_3.dll` + the versioned
  DLL beside the exe + delete `Mods\Multivoid\enabled.txt` (3 ops).
- Nothing is pushed; commits are local pending the user's word + the five-axis leak audit.

## 6. Next steps (in order)

1. ~~Confirm B in the real env~~ **DONE 2026-08-22** — see §4 Proof status (real-env byte decode +
   DEV `POLYHOOK-COMPOSED` boot, both crash-free).
2. ~~Build the IsLive zero-AV arc~~ **DONE 2026-08-22 night** (all 78 sites converted; gate/drill/
   smoke/differential evidence in §4). Remaining from that arc: **USER run B** (one ordinary real-env
   exit; acceptance = no CrashContext report resolving into main.dll) and the ad-hoc `{ptr,idx}` pair
   migration scope (pending user decision, design doc §6; partially done en route — local_streams +
   daynightcycle pairs retired).
2b. **Symbolize the 19:17 real-env EXEC-at-NULL dump** (§4 residuals): rebuild `275e0f67` for
   `F71621E0`'s PDB, map the stack offsets, decide the Present-seam multi-hooker hypothesis.
3. Add B's teardown leak-at-death (§4 residual), drop the `VOTVCOOP_PE_IMMUNE_RELAY=0` diagnostic escape.
4. **Commit 3** — the proxy deletion (RULE 2): `xinput_proxy.cpp` + the loader lane + dup-dialog +
   `inject.ps1` go, fully. Then WP-2 is DONE.
5. Un-park WP-4 (stale prose + site + installer), then WP-6 (distribution), per the user's sequencing.

Related: `[[project-wp2-realistic-env-test-2026-08-22]]`,
`[[project-wp2-precut-and-trampoline-crash-2026-08-22]]`,
`[[project-f2-ue4ss-switch-decision-2026-08-21]]`,
`[[lesson-veh-crash-reporter-preempts-our-seh-guard]]`,
`[[lesson-double-detour-crash-is-config-dependent-needs-pe-callback-arm]]`.
