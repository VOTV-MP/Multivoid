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
| **WP-4** | Fix the stale install/update/uninstall prose + the site + installer for the UE4SS lane. | **PARKED, but now SPECIFIED** — census written (`votv-ue4ss-stale-loader-prose-CENSUS-2026-08-22.md`, ~139 rows); §7 measures the target shape and §7.3 fixes the sequencing (it must not flip before a UE4SS-lane build is released). **+ §7.0 (USER 2026-08-24): the GitHub repo DESCRIPTION and topics are stale prose too and are invisible to the census — they live outside the tree. `description` still says "a standalone C++ DLL"; the `dll-injection` topic goes; `homepageUrl` is empty and is fixable NOW.** |
| **WP-6** | Distribution re-home (the `multivoid-<game>-<build>.dll` filename + master + release flow onto the mod-folder shape). | **PARKED, now SPECIFIED** — §7.2 + §7.4. |
| **WP-9** | **Thunderstore publication** (USER 2026-08-23: "надо нам бы стать официальным модом и попасть в магазин thunderstore ... чтобы обычный юзер смог поставить нативно"). Ship Multivoid as a Thunderstore package so r2modman / Thunderstore Mod Manager installs it natively. | **NEW, SPECIFIED, NOT BUILT** — §7. The payload shape is ALREADY correct; what is missing is package metadata, a GENERATED manifest, and a publish step. **The version mapping is DECIDED, not owed** (§7.3, user 2026-08-23: `<game-major>.<game-minor>.<build>`); this row said "a version mapping decision" was missing after §7.3 had already made it. **§7.3a (2026-08-24, user-raised) measures what the versioned DLL name costs today** — it moves on every proto bump including security-only ones (`0.9.135` as of `ca3943e9`), its CMake justification expires with WP-2 commit 3, `deploy-mod.ps1` picks the payload by mtime out of 14 artifacts, and the six anchor sites that must move in one commit are tabulated. **2026-08-25 (user-raised, five real Thunderstore packages): §7.2 was measuring the extracted PROFILE and calling it the ZIP — the real zip has a `mod/` wrapper, and §7.2's tree would have installed cleanly and never loaded. §7.2a is now the authoritative routing rule (from Thunderstore's own ecosystem schema + r2modman's rule engine and test spec), §7.2b is the field survey + the measurement that shows what D-3 bought (field mods import 32/40/130 mangled C++ symbols from `UE4SS.dll`; we import 0), and §7.9 answers "can GitHub produce the package" — yes for everything except the pak, whose blocker is its inputs.** |
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
  **UPDATE 2026-08-23 — the Present-seam hypothesis is now much better supported, still not proven.**
  Re-parsed the same dump plus a live census of a running game
  (`tools/debug/present_hook_census.py`); four new measured facts:
  1. The crashing thread's stack holds **`dxgi.dll+0x18C0` three times** — and `dxgi+0x18C0` is
     exactly **`IDXGISwapChain::Present`** (RTSS's own resolved-offset cache, cross-checked live).
     So the thread is unambiguously ON the Present chain, interleaved with our `main.dll` and
     `chrome_elf.dll` frames.
  2. **`NahimicOSD.dll` was LOADED in this dump** (`0x7FF8D8950000`, the A-Volute audio-driver
     overlay) — and a live probe the next day measured it **inline-hooking
     `IDXGISwapChain1::Present1`** at that same base. It is a third independent present-chain hooker
     nobody had accounted for. It was invisible until now only because `parse_dump.py`'s module
     filter was loader-shaped and did not match it (filter widened in the same commit).
  3. `RTSSHooks64.dll` was loaded too, and **RTSS's `Profiles\Global` was written at 19:18:16 — 54
     seconds AFTER the 19:17:22 crash**, which is consistent with RTSS having been ARMED at crash
     time and the user turning detection off in reaction. (Circumstantial; the user has since
     confirmed detection is now None, but not when it changed.)
  4. So the 19:17 process plausibly had **four** parties on the Present chain: us + CEF + Nahimic
     (+ RTSS). The exception is EXEC at address 0 — a call through a NULLed function pointer, which
     is exactly what a clobbered hook-chain pointer looks like.
  **Still a hypothesis:** WHO nulled the pointer is not proven, and symbolization remains the
  decider. But the coexistence class is now measured rather than assumed, and the fix in
  `OVERLAY_CAPTURE_COEXIST.md` removes OUR two patches from that chain.

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
5. **Release a UE4SS-lane build.** This is the gate §7.4 identifies: until a released build IS the
   mod-folder shape, the player-facing install prose must keep describing the proxy.
6. **WP-4 + WP-6 + WP-9 as ONE welded change** (§7.4): `docs/INSTALL.md` (both lanes, manager
   first) + `README.md` + the site templates & built `public/` + `ledger_lib.ps1` anchors and
   release-body block + `ledger_lint.ps1` checks + `publish.ps1` asset shape → and the Thunderstore
   package published. **Blocked on the user's §7.3 `version_number` call** (recommendation: `0.0.<build>`).
7. WP-7 (native debug subsystem) and WP-8 (hygiene split) stay parked.

---

## 7. How a VOTV player installs a mod NATIVELY — MEASURED 2026-08-23 (WP-4 / WP-6 / WP-9 input)

Measured on this box from the real r2modman profile
(`C:\r2modman\r2modmanPlus-local\VotV\profiles\Default`) and the vendored
`reference/unreal-shimloader` + `reference/voidmod-extracted`. This replaces guesswork about
"what the new install is" — the question WP-4 was parked without an answer to.

### 7.0 The GitHub repo's own DESCRIPTION is stale prose too (USER 2026-08-24)

The WP-4 census counted install/update/uninstall prose in files. **The repository's GitHub "About"
blurb and topics are the same surface and are not in any file**, so no census, grep or CI gate can
ever see them — they have to be changed by hand, in the GitHub UI or via `gh`.

**Measured 2026-08-24 `[V]`** (`gh repo view VOTV-MP/Multivoid --json description,homepageUrl,repositoryTopics`):

| field | current value | after the D-3 migration |
|---|---|---|
| `description` | *"Multiplayer co-op mod for Voices of the Void — **a standalone C++ DLL** layered on UE4.27 that syncs full game state between host and clients without modifying any game files."* | **FALSE at the load-bearing word.** Multivoid stops being standalone: it ships as `Mods/Multivoid/dlls/main.dll` and is loaded by UE4SS (§0). "without modifying any game files" stays true and stays in — it is principle 1 and the thing that distinguishes us |
| topic `dll-injection` | present | **wrong lane** once the proxy is deleted — we are loaded by a mod framework, not injected. Candidates to add: `ue4ss`, `thunderstore` |
| `homepageUrl` | **empty** | `https://multivoid.dev` — this one is stale *today*, independent of the migration, and is a free fix |

**Sequencing — the same rule as §7.4a.** The description must NOT flip to the UE4SS lane before a
UE4SS-lane build is actually released, or the front page will tell a player to install a thing that
does not exist yet. `homepageUrl` is exempt from that ordering: it is wrong now and can be set now.

**Why this is filed here and not "just done":** it is a public-facing statement about what the mod
IS, on the surface most people read first. It belongs to the WP-4 flip, with the release, not to a
tidy-up commit. Add it to the WP-4 checklist rather than treating it as done because it is small.

**Generalise the miss:** any project statement that lives OUTSIDE the repo tree is invisible to every
mechanism this project trusts. The known set, so the next stale-prose sweep starts from a list
instead of a memory: the GitHub description + topics + homepage, the Thunderstore package
description (WP-9, not created yet — it will be written from this same prose and must be written
*correct*, not migrated later), the site's own copy (`site/`, deployed by hand), the Discord channel
topic/pins, and the release-body template in `tools/release/notes/`.

### 7.1 The two install lanes

1. **Mod manager (r2modman / Thunderstore Mod Manager) — this IS "natively, the way they do it".**
   The manager downloads a Thunderstore zip and ~~extracts it **whole** into
   `<profile>\shimloader\mod\<Author>-<Name>\`~~ — **WRONG, corrected 2026-08-25: it ROUTES each
   top-level entry to a different profile directory per a published per-game rule set, and strips
   the matched folder's own name. See §7.2a, which is authoritative.** It then launches the game
   through `unreal_shimloader`, which VFS-maps `--mod-dir` → `GAME\Binaries\Win64\Mods`,
   `--pak-dir` → `Content\Paks\LogicMods`, `--cfg-dir` → `Config`, and
   `--overlay-dir` → `GAME\Binaries\Win64` itself
   (`reference/unreal-shimloader/README.md:21-31`). Nothing is written into the game folder.
2. **Manual UE4SS** — install UE4SS per the upstream guide, then drop
   `GAME\VotV\Binaries\Win64\Mods\Multivoid\dlls\main.dll` + `enabled.txt`.
   This is what `tools/deploy-mod.ps1` already does for our four dev installs.

`docs/INSTALL.md` must document BOTH, with lane 1 first (it is what most players use).

### 7.2 The package shape — measured from a real VOTV UE4SS C++ mod

> **CORRECTED 2026-08-25 — THIS SECTION DESCRIBED THE WRONG TREE, AND THE ERROR WAS SHIP-BREAKING.**
> Everything below was measured from the **extracted r2modman profile**, i.e. the *output* of the
> install, and was then written down as if it were the **zip**. It is not: the zip carries a
> top-level **`mod/`** wrapper that the manager strips. A package built to the tree below would
> install "successfully", show up in the manager, and **never load** (§7.2a proves why, from
> r2modman's own rule engine). **Read §7.2a before building any package.** The section is kept —
> not rewritten — because everything it says about the *profile* layout is still true, and because
> the failure mode it would have caused is the thing worth remembering.

`acitulen-DebugMod` 5.0.3 (and `Moddy-CrashContext`, `Flyingcoyote-VoidFax`) unpack to:

```
<profile>\shimloader\mod\acitulen-DebugMod\
    manifest.json
    icon.png            (Thunderstore requires 256x256)
    README.md
    CHANGELOG.md        (optional)
    enabled.txt         (empty file -- UE4SS's per-mod enable flag)
    dlls\main.dll       (the mod binary; `dlls/main.dll` is UE4SS's FIXED contract)
```

`manifest.json`, measured verbatim:

```json
{
    "name": "DebugMod",
    "author": "Acitulen",
    "version_number": "5.0.3",
    "website_url": "https://github.com/Acitulen/DebugMod",
    "description": "This mod adds a multifunctional console menu ...",
    "dependencies": ["Thunderstore-unreal_shimloader-1.1.7"]
}
```

**The good news: our payload is ALREADY in exactly this shape.** The r2modman profile carries
`shimloader\mod\Multivoid\dlls\main.dll` + `enabled.txt` today. WP-9 is therefore **metadata +
a zip + a publish step**, not a re-architecture. The folder name becomes `<Author>-Multivoid`
once it comes from Thunderstore rather than our hand-install.

**A package can ship a `.pak` TOO, in the same zip — measured 2026-08-23.** `acitulen-DebugMod` is the
exact precedent for what we need: one Thunderstore package that carries **both** a C++ DLL mod and a
blueprint pak. On disk it lands in two places at once:

```
<profile>\shimloader\mod\acitulen-DebugMod\dlls\main.dll     <- the --mod-dir lane
<profile>\shimloader\pak\acitulen-DebugMod\DebugMod.pak      <- the --pak-dir lane
```

So the package holds a root-level **`pak/`** folder beside ~~`dlls/`~~ **`mod/`** (the pak half of this
sentence is right; the `dlls/` half is the §7.2 error — corrected 2026-08-25), and the manager routes
each to its own shimloader directory (`--pak-dir` VFS-maps to `Content\Paks\LogicMods`).
`NynrahGhost-Fusion` does the same. Multivoid's target package is therefore:

```
  DO NOT COPY THIS BLOCK -- it is the §7.2 error, kept only so the correction has a subject.
  The buildable tree is in §7.2a. What is wrong here: `enabled.txt` and `dlls/` are NOT at the
  zip root; they live under `mod/`, and a root-level `dlls/` silently never loads.

manifest.json  icon.png  README.md  CHANGELOG.md  enabled.txt      <- enabled.txt belongs in mod/
dlls\main.dll                                                      <- belongs at mod\dlls\main.dll
pak\<model>.pak        (+ its <model>.png preview tile, which the F1 skin browser reads)   <- correct
```

This matches what `tools/deploy-all.ps1` already does for the four dev installs (it copies the pak to
`Content\Paks\LogicMods\multivoid\` plus the preview `.png`) — the mechanism is built and shipping
locally; only the packaging wrapper is missing. **But WHICH model may go in that pak is an open
question — see §7.6.**

### 7.2a The routing rule — AUTHORITATIVE `[V]` 2026-08-25, and §7.2's tree would NOT have loaded

A Thunderstore zip is not extracted "whole" anywhere. The **manager** routes each top-level entry
according to a per-game rule set that Thunderstore publishes as machine-readable data, and VOTV's
rule set is this — fetched live from
`https://thunderstore.io/api/experimental/schema/dev/latest/`,
`games["voices-of-the-void"].r2modman[0]` (326 games in that document):

| zip folder | → profile route | `isDefaultLocation` | `defaultFileExtensions` | `trackingMethod` |
|---|---|---|---|---|
| `mod/` | `shimloader/mod/<pkg>/` | **true** | `[]` | `subdir` |
| `pak/` | `shimloader/pak/<pkg>/` | false | **`[]`** | `subdir` |
| `cfg/` | `shimloader/cfg/` | false | `[]` | **`none`** |
| `overlay/` | `shimloader/overlay/<pkg>/` | false | `[]` | `subdir` |

Same object: `packageLoader: "shimloader"`, `internalFolderName`/`dataFolderName`/`settingsIdentifier`
`"VotV"`, `exeNames: ["VotV.exe"]`, community label **`voices-of-the-void`** (the `--community` a
publish step needs), `wikiUrl: https://questwalker.github.io/votv-modding-wiki/`. Two packages are
registered as the loader for this community — `Thunderstore-unreal_shimloader` **and**
`0xFFF7-votv_shimloader` — so a player may arrive with either.

**The algorithm, which is what makes §7.2 wrong** (`ebkr/r2modmanPlus`,
`src/installers/InstallRulePluginInstaller.ts`, `buildInstallForRuleSubtype`):

- A top-level **directory** matches a rule when its name equals `basename(rule.route)` — literally
  `mod`, `pak`, `cfg`, `overlay`. A directory that matches **nothing is recursed into**, and the
  files inside are classified individually — the folder is *not* carried along as a unit.
- A **file** matches by extension; with no extension rule it falls to the `isDefaultLocation` rule,
  i.e. `shimloader/mod`.
- `installSubDir` then copies each matched source's **children** into `<route>/<pkg>/`. So the
  matched folder's own name is stripped and everything beneath it is preserved.

r2modman's own test spec pins the mapping (`test/vitest/tests/unit/Installers/ModLoader/Shimloader.Tests.spec.ts`):

```
README.md            -> shimloader/mod/<pkg>/README.md
manifest.json        -> shimloader/mod/<pkg>/manifest.json
icon.png             -> shimloader/mod/<pkg>/icon.png
mod/scripts/main.lua -> shimloader/mod/<pkg>/scripts/main.lua
mod/dll/mod.dll      -> shimloader/mod/<pkg>/dll/mod.dll     <- the `mod/` level is STRIPPED
pak/blueprint.pak    -> shimloader/pak/<pkg>/blueprint.pak
cfg/package.cfg      -> shimloader/cfg/package.cfg           <- FLAT, no <pkg>
```

Confirmed independently on this box against the real profile: `acitulen-DebugMod`'s zip is
`mod/dlls/main.dll` + `mod/enabled.txt` + `pak/DebugMod.pak` + four root metadata files, and it
lands as `shimloader/mod/acitulen-DebugMod/{dlls/main.dll, enabled.txt, + the metadata}` and
`shimloader/pak/acitulen-DebugMod/DebugMod.pak`. `forder-FusionFix` is the clean control (no runtime
writer): zip `mod/Scripts/*.lua` → profile `.../Scripts/*.lua`. `NynrahGhost-Fusion` is **not**
usable as evidence — its `Fusion.exe` writes into its own mod folder at runtime, which is why that
folder looks flattened.

**So the shape to build is:**

```
manifest.json          icon.png (EXACTLY 256x256)   README.md   [CHANGELOG.md]
mod\enabled.txt
mod\dlls\main.dll
pak\<model>.pak        (+ its <model>.png preview tile)
```

Four traps, each of which produces a package that installs cleanly and then misbehaves silently:

1. **`dlls/` at the zip root does not work.** Its name matches no route, so it is recursed into;
   `main.dll` matches no extension rule; it falls to the default location; `installSubDir` copies it
   **by basename** → `Mods/<pkg>/main.dll`. UE4SS scans `Mods/<name>/dlls/main.dll` (measured across
   all three UE4SS eras — `src/loader/cppmod_entry.cpp:5-9`), so the mod is simply never loaded, with
   no error anywhere. `[V]` by rule-engine read; not run as a negative control.
2. **VOTV's `pak` route declares NO extension rule** (`defaultFileExtensions: []`). A loose `.pak`
   at the zip root therefore goes to `shimloader/mod/<pkg>/`, not to the pak dir — r2modman has a
   named test for exactly this (*"Loose .pak files route to schema default location when no
   extension rule exists"*). The pak **must** be inside `pak/`.
3. **`cfg/` is a shared, un-owned namespace.** `trackingMethod: "none"` means no per-package subdir
   *and* no removal on uninstall — r2modman's test asserts `shimloader/cfg/package.cfg` still exists
   after the package is uninstalled. Anything we put there collides across mods by filename and
   outlives us. (`multivoid.ini` already lives beside the game exe / under `SHIMLOADER_CFG_DIR`;
   this is a reason not to move it into the package.)
4. **`<pkg>` is `<Author>-<Name>`, not `<Name>`** (`installSubDir` uses `mod.getName()`; measured on
   disk as `acitulen-DebugMod`). Our hand-installs are `Multivoid`; a Thunderstore install is
   `<Author>-Multivoid`. This is the same fact §7.7 hits from the other side — `skin_registry.cpp:114`
   hardcodes `LogicMods/multivoid`, and the pak will arrive at `LogicMods/<Author>-Multivoid/`.

**`shimloader/overlay` is new information for this doc, and it is upstream, not ours** `[V]`. It maps
a package subtree onto `GAME/Binaries/Win64/`, and shimloader publishes `SHIMLOADER_MOD_DIR` /
`_PAK_DIR` / `_CFG_DIR` / `_OVERLAY_DIR` into the environment *before* `ue4ss.dll` loads so an
overlay-loaded wrapper can resolve the profile from `DllMain`
(`reference/unreal-shimloader/README.md:30,40-42`; `src/hooks.rs:117`, `src/lib.rs:128-195`,
`src/paths/registry.rs:70`). Provenance checked because the vendored copy is gitignored and could
have been a local fork: `git log -- reference/unreal-shimloader` is exactly two commits, both the
vendoring drop and its later un-tracking, **no authored source change**, and the same text is live at
`raw.githubusercontent.com/thunderstore-io/unreal-shimloader/master/README.md`.
Consequence to record and *not* act on: the mod-manager lane **does** have a supported way to ship a
DLL that must live in `Binaries/Win64/` — so WP-2 commit 3 deleting the proxy does not burn that
bridge, and the existence of this route is **not** an argument for keeping the proxy.

### 7.2b What five real VOTV packages actually ship — field survey `[V]` 2026-08-25

Downloaded by the user to `ignore_folder/thunderstore_mod_examples/` and measured from the **zips**,
not the profile:

| package | kind | zip tree beyond the 3 required root files | manifest `dependencies` |
|---|---|---|---|
| `acitulen-DebugMod` 5.0.3 | C++ **+ pak** | `CHANGELOG.md`, `mod/enabled.txt`, `mod/dlls/main.dll` (814,592 B), `pak/DebugMod.pak` (1,337,587 B) | `Thunderstore-unreal_shimloader-1.1.7` |
| `Moddy-PBMovement` 1.0.1 | C++ **+ ini** | `CHANGELOG.md`, `mod/enabled.txt`, `mod/dlls/main.dll` (523,776 B), **`mod/dlls/PBMovement.ini`** (4,414 B) | same |
| `Moddy-CrashContext` 1.0.0 | C++ | `mod/enabled.txt`, `mod/dlls/main.dll` (46,080 B) — no CHANGELOG | same |
| `Flyingcoyote-VoidFax` 1.0.7 | **Lua** | `CHANGELOG.md`, `mod/enabled.txt`, `mod/Scripts/main.lua`, `mod/Scripts/config.lua` | same |
| `SquishEk-BlyatErrorReplacement` 0.0.0 | **pak only** | `CHANGELOG.md`, `pak/BlyatErrorReplacement_P.pak` — **no `mod/`, no `enabled.txt`** | `[]` |

Every icon is exactly 256×256. Three of five omit `author` from the manifest entirely — the
namespace comes from the uploading **team**, not the file. Four of five declare the single dependency
`Thunderstore-unreal_shimloader-1.1.7`; the pak-only package declares none. **`PBMovement` puts its
config file next to the DLL** (`mod/dlls/PBMovement.ini`) rather than in `cfg/` — consistent with
trap 3 above, and the closest field precedent for where `multivoid.ini` would go if it ever moved
into the package.

**The linkage measurement — this is D-3's slim contract shown against the field.** PE import/export
tables, parsed directly:

| binary | imports from `UE4SS.dll` | CRT | exports |
|---|---|---|---|
| `Moddy-CrashContext` | **32** | dynamic (`MSVCP140`, `VCRUNTIME140`, `api-ms-win-crt-*`) | `start_mod`, `uninstall_mod` |
| `Moddy-PBMovement` | **40** | dynamic | same two |
| `acitulen-DebugMod` | **130** | dynamic | same two |
| **`multivoid-0.9.0n-141.dll`** | **0** | **static** (`CMakeLists.txt:186,691`) | same two, plus 18 leaked `SteamNetworking*` from static GNS |

Every one of those imports is an **MSVC-mangled C++ symbol carrying `std::` types across the DLL
boundary** — e.g.
`?on_dll_load@CppUserModBase@RC@@UEAAXV?$basic_string_view@_WU?$char_traits@_W@std@@@std@@@Z`,
`?RegisterProcessInternalPreCallback@Hook@Unreal@RC@@YAXV?$function@...@Z`. Two things follow, and
they are the concrete payoff of the D-3 choice:

- **Their** ABI surface is (this UE4SS build) × (this MSVC STL). A signature change upstream is a
  *missing import* — the Windows loader fails the DLL outright; there is no degraded mode. That is
  why the whole cohort pins `unreal_shimloader-1.1.7`. **Our** surface is two `extern "C"` symbols
  plus a vtable of no-op stubs whose slot count is watched at runtime
  (`cppmod_entry.cpp`, tripwire wire-e).
- All three field mods **require the VC++ redistributable**; Multivoid does not. One fewer
  precondition in the install prose, and one fewer support class.

The size gap is real and worth naming before WP-9: **17,688,064 B vs 814,592 B** — 21× the largest
VOTV C++ mod. Thunderstore's documented ceiling is ~5 GB, so this is not a store problem; it is a
*download-on-every-update* problem. ~~and it is the strongest practical argument for §7.6's
"skins as a separate package" conclusion.~~ **Corrected 2026-08-25 within a day of being written: that
last clause cited a recommendation the user had already overturned on 2026-08-23 (§7.7c part 1 — one
package, base pak inside), re-confirmed 2026-08-25. The size fact stands and is the reason §7.7c caps
the base pak at ~4 skins rather than all 14; it is not an argument for splitting the package.**

**Two of §7.5's owed measurements are closed by this pass** — see §7.5.

### 7.3 `version_number` — DECIDED (USER, 2026-08-23): **`<game-major>.<game-minor>.<build>`**

Thunderstore **requires** `version_number` to be semver `X.Y.Z` and orders updates by it. Multivoid
deliberately **deleted mod semver** (USER DECISION 2026-07-19: the identity is the Paper pair — game
target + build number, `Multivoid 0.9.0n b134`). The user chose the mapping that keeps the Paper pair
visible: **`0.9.134`** for game target `0.9.0n` + build `134`.

**The derivation (exact, so WP-9 does not re-derive it):**

**`X.Y` comes from the GAME target; `Z` is OURS** (the user's own phrasing, 2026-08-23).

| component | source | today |
|---|---|---|
| `X.Y` | **THEIRS** — the first two dot-separated fields of `VOTVCOOP_GAME_TARGET` (`src/votv-coop/CMakeLists.txt:23`, read via the ONE existing parser `Get-GameTargetFromCMake`, `tools/release/ledger_lib.ps1:160`), with any non-digit characters stripped from each field | `0.9` (from `0.9.0n`) |
| `Z` | **OURS** — `kProtocolVersion` (`src/votv-coop/include/coop/net/protocol.h:709`), the Paper pair's build half | **`140`** as of 2026-08-25 (was `134` when this table was written, `135` on the WP-6 A5 retirement, then 136 A34 / 137 A37-A38 / 138 B1 / 139 B2 / 140 A50 — see §7.3a item 1; the line number moves with it, so re-grep rather than trusting `:709`). **The rate this moves at is itself the §7.3a argument**: six bumps in two days, every one of them security work with no player-visible feature, each silently moving the Thunderstore release identity |

Parse rule, stated so it cannot be misread: split the game target on `.`, take fields 1 and 2, strip
non-digits from each (so a hypothetical `0.9n` still yields `0.9`), and fail closed if either field is
empty after stripping. The game target's THIRD field and letter suffix are deliberately not used.

**Monotonicity holds** — and this corrects a weaker caveat written earlier the same day. Semver
compares components numerically, `kProtocolVersion` never resets and only increases, and a game
version's numeric prefix never decreases; so `0.9.134` -> `0.10.135` -> `1.0.140` all order correctly.
The only information lost is the game target's letter suffix (`0.9.0n` and a later `0.9.1a` both map
to `0.9`), which the build number already disambiguates and which the full Paper pair — displayed in
the package `description` and README — still states exactly.

**HARD REQUIREMENT: the manifest is GENERATED, never hand-edited.** A hand-kept version string that
rots unbumped is precisely the failure that got mod semver deleted in the first place (2026-07-19);
re-introducing a hand-typed `version_number` in `manifest.json` would recreate it one layer out. So
`manifest.json` is emitted at package time from the two sources above, and the packaging step fails
closed if either parse misses. Do not check a literal version into the repo's manifest template.

### 7.3a What the artifact name costs TODAY — measured 2026-08-24 (USER-RAISED)

The user asked mid-WP-6: *"Why do we still produce `multivoid-0.9.0n-135.dll` outputs? We have to
re-work semver according to UE4SS arc."* The mapping itself is NOT open — §7.3 above decided it and
the user decided it. What the question exposed is a set of facts nobody had written down, recorded
here so WP-4/WP-9 do not re-derive them.

**1. `version_number` moves on EVERY protocol bump, including a security-only one.** This session's
WP-6 fix retired the `BalanceDelta` lane, which is a real parse change, so `kProtocolVersion` went
134 -> 135 (`ca3943e9`). By §7.3's own rule that silently moved the release identity:
`multivoid-0.9.0n-135.dll`, Thunderstore `version_number` **`0.9.135`**. This is the first time the
pair has moved for a reason that is neither a release nor a game recook. **Consequence:** any
release-notes ledger row, INSTALL anchor or draft body still keyed to `134` is stale, and the
packaging step must read the number rather than carry one.

**2. The name's stated justification expires with commit 3.** `src/votv-coop/CMakeLists.txt:636-638`
declares *"the filename is load-bearing, not cosmetic"* and gives the reason: **the xinput proxy
scans for `multivoid-*.dll`** (loads the highest build, flags duplicates for the in-game popup). WP-2
commit 3 deletes that scanner, at which point the sentence is false and the versioned filename has no
consumer inside the mod folder at all — UE4SS's contract name is the fixed `dlls\main.dll`. The
identity does not disappear; it moves to the zip name, the generated `manifest.json`, and the
in-game banner (`coop/version.h` + `kProtocolVersion`), none of which need it in the DLL filename.

**3. A live defect, independent of the cutover.** `tools/deploy-mod.ps1:38-43` picks the payload by
globbing `multivoid-*.dll` and taking `Sort-Object LastWriteTime -Descending | Select -First 1`. The
build directory currently holds **14** such artifacts (`122` through `135`), so the deploy is one
stale rebuild away from shipping the wrong payload while reporting success. It should compute the
exact expected filename from the two sources §7.3 already names and fail closed if it is absent —
the same "derive it, never guess it" rule the manifest is held to.

**4. The anchors that must move in the SAME commit** (measured; this is the weld's real edge, and
`ledger_lint` fails on the spot if they drift):

| site | what it asserts |
|---|---|
| `tools/release/publish.ps1:24-36` | exactly one `multivoid-*.dll` in the artifact dir, and its name equals `multivoid-<game>-<N>.dll` |
| `tools/release/ledger_lib.ps1:219-220` | the release body is built from exactly one `multivoid-*.dll` key in the sha map |
| `tools/release/ledger_lib.ps1:149-150` | the verbatim INSTALL anchor `delete the old ` + backtick-`multivoid-*.dll` |
| `tools/release/ledger_lint.ps1:74-77` | INSTALL.md carries no literal build filename, only the placeholder |
| `tools/release/tag_regex_selftest.ps1:58,78` | fixtures `multivoid-0.9.0n-999.dll` + `xinput1_3.dll` |
| `src/votv-coop/CMakeLists.txt:675-679` | the `xinput1_3` target still BUILDS today; it retires with commit 3 |

**5. Sequencing is unchanged and already answered by 7.4a** — the flip is allowed locally right now
because nothing is being pushed; what is forbidden is flipping the prose without re-minting the CI
anchors in the same commit. The user's 2026-08-24 ruling on ordering: **this work happens AFTER the
security holes are closed** (WP-6), together with the release-pipeline fix in item 3.

### 7.4 Sequencing — SUPERSEDED 2026-08-23 by the user's no-push ruling (read 7.4a first)

> **7.4a — USER RULING 2026-08-23, and it dissolves this section's blocker.** Verbatim: *"We don't
> break anything if we flip now, since I'm against pushing commits for now. We will push when
> everything is ready, so changes are allowed, we don't confuse anyone is it sits locally."*
>
> The rule below existed for exactly ONE reason — prose describing a build the public does not have
> would break the instructions for every current user. Nothing is published until the whole weld is
> ready, so that reason no longer applies and **the prose may be flipped NOW, locally.**
>
> What does NOT change, because it was never about publication timing:
> - **The weld is still a weld.** `ledger_lint.ps1:64-66` FAILS unless `docs/INSTALL.md` carries the
>   verbatim anchors defined at `ledger_lib.ps1:149-150`, which are proxy-lane text. Flipping
>   INSTALL.md without re-minting those anchors in the SAME commit breaks CI on the spot — a local
>   self-inflicted break, not a user-facing one, but a break.
> - **The prose must be TRUE of the build in the tree**, which it now is: the payload is already the
>   `Mods/Multivoid/dlls/main.dll` shape, the r2modman profile runs it, and WP-2's fix B is built.
> - **Commit 3 (proxy deletion) still owns the retirement**, and the release-body/publish.ps1 asset
>   shape still moves with it (7.4b). Flipping the prose early just means the docs land first and
>   wait for it, instead of the other way round.
>
> The rest of this section is kept for its measured content (what the weld consists of); its
> "must NOT flip yet" instruction is the part that is superseded.

**Original section (2026-08-23) — the weld inventory, still accurate:**

`docs/INSTALL.md` is the single owner of install prose **for players**, and the current PUBLIC
release is still the xinput-proxy build. Flipping it to the UE4SS story before a UE4SS-lane build
is actually released would break the instructions for **every current user**. So the order is:

> **commit 3 (proxy deletion) → release a UE4SS-lane build → flip INSTALL/README/site/release lane
> AND publish to Thunderstore, as one welded change.**

The weld is real and was re-verified verbatim this session:

- `tools/release/ledger_lint.ps1:64-66` FAILS unless `docs/INSTALL.md` contains, verbatim,
  `WindowsNoEditor\VotV\Binaries\Win64` and ``delete the old `multivoid-*.dll` `` (defined at
  `ledger_lib.ps1:149-150`), plus the current game target.
- `tools/release/ledger_lib.ps1:231-234` emits the release-body Install block
  *"You need **both** files ... `xinput1_3.dll` (the loader)"*.
- `tools/release/publish.ps1:24-27` **throws** unless the artifact dir holds exactly one
  `multivoid-*.dll` **and** one `xinput1_3.dll`.

So INSTALL.md, README.md, the site templates + built `public/`, all three release-lane scripts, and
the new Thunderstore packaging move together. Retiring the proxy also makes the
`multivoid-*.dll` "highest build wins" scan and the "MOD INSTALL PROBLEM" duplicate dialog
meaningless (the mod manager owns installation) — they retire WHOLE per RULE 2.

### 7.4b DISTRIBUTION MODEL — DECIDED (USER, 2026-08-23): Thunderstore is PRIMARY, GitHub is the manual lane

Verbatim: *"Я решил что thunderstore mod manager/r2modman будет основным распространением нашего мода.
А релиз на гитхабе будет name_release.zip архив с иерархией такой что уже готова к ручной установке
мода, это для ручных любителей установки. На сайте тоже инфу поменять, пусть будет гитхаб ссылка как
раньше, но еще и на thunderstore сделаем кнопки/инфу."*

This settles the ordering §7.1 could only guess at, and it changes the GitHub asset shape:

| Lane | Channel | Artifact | Audience |
|---|---|---|---|
| **PRIMARY** | Thunderstore (r2modman / TMM) | the package of §7.2, `version_number` per §7.3 | the ordinary player — one click, no file handling |
| **SECONDARY** | GitHub release | **ONE `.zip` whose internal hierarchy is already the on-disk layout** | people who install by hand |

**The GitHub artifact becomes a single archive, not loose files.** Today `publish.ps1:24-27` throws
unless the artifact dir holds exactly one `multivoid-*.dll` **and** one `xinput1_3.dll`, and
`ledger_lib.ps1:231-234` writes a release body telling the player to place *both files* by hand. Both
are the proxy lane and both retire with it. The replacement is one zip the user unpacks **over the
game folder** with no decisions to make — the hierarchy IS the instruction, which is the whole point
of the user's phrasing *"иерархией такой что уже готова к ручной установке"*.

Consequences to carry into the WP-4/6/9 weld, so they are not re-derived:

- The zip's internal tree must mirror the MANUAL UE4SS lane of §7.1 exactly
  (`VotV/Binaries/Win64/Mods/Multivoid/dlls/main.dll` + `enabled.txt`, and the pak under
  `VotV/Content/Paks/LogicMods/...`), because that lane is what a hand-installer is doing.
  It does NOT mirror the Thunderstore package shape — those are different layouts for different
  extractors, and conflating them is the obvious trap.
- **UE4SS itself is a PREREQUISITE, not payload, in the manual lane** — the Thunderstore lane gets
  `unreal_shimloader` via `dependencies` (§7.2), and the manual lane has no equivalent, so the
  release body + INSTALL.md must state the UE4SS install step for the zip and only for the zip.
- Naming: the user wrote `name_release.zip`. Concretely `multivoid-<game>-<build>_release.zip`
  (e.g. `multivoid-0.9.0n-134_release.zip`) so the Paper pair stays on the filename, matching the
  DLL-naming rule that is already load-bearing elsewhere.
- `publish.ps1`'s asset assertion inverts: exactly ONE `*_release.zip`, and it must FAIL CLOSED if
  the zip does not contain the expected tree (an empty or mis-rooted zip is a silently broken
  release, and this project has shipped one silently-broken artifact before).
- **The site keeps its GitHub link AND gains Thunderstore buttons/info** — the user was explicit
  that GitHub does not go away. Two buttons, Thunderstore first (it is the primary lane).
- `ledger_lint.ps1:64-66`'s verbatim anchor phrases (`WindowsNoEditor\VotV\Binaries\Win64`,
  ``delete the old `multivoid-*.dll` ``) are proxy-lane text and must be re-minted against the new
  INSTALL.md in the SAME commit, or CI fails the release.

### 7.6 The pak's CONTENT — DECIDED (USER 2026-08-23): the HL skins ship

**USER DECISION:** the HL scientist skins ship with the mod. Rationale (user's): character-swap mods
built on assets from other games are ubiquitous across Steam Workshop and the modding scene at large,
and Valve has never been aggressive about its own assets in that context. **Recorded as settled — do
not re-litigate it.** One correction to how it was first put to the user: the three `.gitignore` rules
below are a *"what do we commit to a public git repo"* triage (binaries, heavy, regenerable), not a
considered decision about what to ship to players — they were presented as stronger than they are.

The residual risk is not legal but **availability**: Thunderstore is a third party with its own
content policy, and a takedown of the package would remove the *whole mod*, not just the skins.

> **SUPERSEDED 2026-08-23 by §7.7c part 1, re-confirmed by the user 2026-08-25** (*"пусть всё внутри
> одного zip будет — сам мод и pak скинов дефолтных"*). The paragraph below argued from that residual
> risk toward a **separate** skins package. **The user decided the opposite: ONE package, base pak
> inside.** Kept because the availability risk it names is real and unchanged — it is a risk we
> accepted, not one that went away. Do not re-derive the split from it.

~~That argues for shipping skins as a **separate package** from the mod (see §7.7), which is better packaging
anyway — skins are optional, bulky, and should not force a re-download on every mod update.~~

### 7.7 THE BLOCKER NOBODY WOULD HAVE PREDICTED — the skin scan is pinned to one folder name

`[MEASURED 2026-08-23]` **`skin_registry.cpp:114-124` hardcodes the scan directory to
`<game>/VotV/Content/Paks/LogicMods/`*`multivoid`*`/`**, and `Entries()` (`:153`) runs a FLAT,
non-recursive `directory_iterator` over exactly that folder looking for `*.pak` (+ a `<stem>.png|.bmp`
preview sidecar; the skin's display name is the pak's **stem**, `:159`).

Thunderstore does not use that folder. A package's pak lands in
`shimloader\pak\`**`<Author>-<Name>`**`\` -> `Content\Paks\LogicMods\<Author>-<Name>\`. So:

- **Our own package's pak would NOT be listed in the F1 skin browser.** UE auto-mounts any `.pak`
  under `Content/Paks/`, so the mesh would be loadable — but the registry never sees the file, the
  browser shows `dr_kel + builtins only`, and the skin cannot be picked. It works today ONLY because
  `tools/deploy-all.ps1` hand-copies into `LogicMods/multivoid/`.
- A **separate** skins package is impossible for the same reason.

**Requirement for WP-9 (RULE 1 — fix the scan, do not special-case a folder name):** `PakDir()` must
stop being one pinned path. Scan `Content\Paks\LogicMods\` **and its immediate subdirectories** for
`*.pak` + sidecars. That single change simultaneously (a) makes our Thunderstore package work whatever
`<Author>-Name` resolves to, (b) makes independent third-party Multivoid skin packs work — anyone can
publish one, (c) keeps the existing dev-deploy path working unchanged. Needs a name-collision rule,
since the display name is the pak stem and two packages may ship the same stem.

**This is a hard precondition: without it, shipping the pak from Thunderstore silently produces a mod
with no selectable skins.**

### 7.7b TASK (USER 2026-08-23): one `scientists.pak` holding every scientist skin + previews

*"Будет задача все скины ученых собрать и их превью и затолкать в один .pak и назвать scientists.pak"* —
collapse the per-skin paks into ONE `scientists.pak`. Measured against the code, this is **much smaller
than it looks**, because one half of it already works:

**LOADING already works, zero code change.** `[MEASURED]` `client_model.cpp:75-84` resolves a skin by
ASSET PATH, never by pak filename:
```
mesh    -> /Game/Mods/VOTVCoop/<name>.kerfurOmega_KelSkin
texture -> /Game/Mods/VOTVCoop/tex_<name>.tex_<name>
```
UE mounts any `.pak` under `Content/Paks/` and resolves those paths regardless of which archive the
packages came from. So N packages inside one `scientists.pak` load exactly as N separate paks do.
(Every converted model keeps the same export name `kerfurOmega_KelSkin`; the **package** name is the
skin identity — `docs/COOP_CLIENT_MODEL.md` §6a.)

**ENUMERATION is the part that breaks.** `[MEASURED]` `skin_registry.cpp:153-159` builds the skin list
from pak **filenames** (`p.stem()`), one pak = one skin. Given a single `scientists.pak` it would offer
exactly one entry named `scientists`, which then fails to load (`/Game/Mods/VOTVCoop/scientists` does
not exist). The list must instead come from the pak's CONTENTS. Options, in preference order:

- **(ii) enumerate the mounted packages under `/Game/Mods/VOTVCoop/`** (asset registry / object walk).
  The RULE-1 answer — cannot drift from what the pak actually contains, and third-party skin packs
  work automatically. Feasibility not yet measured; needs the mount to precede enumeration.
- **(i) a manifest sidecar** (`scientists.txt`, one skin name per line) beside the pak. Trivial, no
  reflection — but it is a second source of truth that can drift from the pak.
- (iii) a hardcoded list like `kBuiltinSkins` — works for OUR pak, but then nobody else can ship a
  skin pack. Rejected unless (i)/(ii) both fail.

**PREVIEWS are nearly free if they stay sidecars.** `[MEASURED]` the preview lookup is ALREADY keyed on
the skin NAME, not the pak stem — `skin_registry.cpp:137-147` does exactly that for the builtin kerfur
skins (`<dir>/<name>.png|.bmp`). So dropping `dr_x.png` beside `scientists.pak` needs **no new code**.
Putting the previews INSIDE the pak instead means loading a cooked `UTexture2D` and getting its mip
pixels into an ImGui texture — a genuinely new path, versus today's WIC decode of a loose PNG
(`DecodeImageFileBgra`). **Worth confirming with the user which they meant**: "затолкать в один .pak"
reads as inside, but the sidecar route costs nothing and the in-pak route is real work for no
user-visible difference.

**Depends on §7.7** — whichever enumeration wins, the scan directory must also stop being pinned to
`LogicMods/multivoid/`, or none of it is reachable from a Thunderstore install.

### 7.7c The skin DISTRIBUTION model (USER 2026-08-23) — base pak + user packs + a missing-pack notice

**USER DECISION, three parts:**
1. **`scientists.pak` is the BASE pak and ships INSIDE the mod package** (not separate — this
   supersedes the "ship skins separately" suggestion in §7.6).
2. **Users may publish their OWN skin packages**, and those must work for everyone who installed them:
   the skins appear in the browser and are selectable.
3. **A peer missing a pack gets told, in chat:** if someone is wearing a custom skin you do not have,
   you get a line saying so — rather than silently seeing the fallback body.

**Sizes, measured** (`research/pak_re/`, 14 scientist paks): `570 KB … 4,522 KB` each, **≈32 MB
total**. The mod DLL is ~18.5 MB, so a bundled package lands near **50 MB**, and Thunderstore/r2modman
fetch a whole package per version — every build bump re-downloads all of it. Stated as a fact for the
size budget, not as an argument against the decision; several paks are suspiciously equal at ~4.28 MB,
so a single archive may dedupe/compress meaningfully better than the sum.

> **RE-MEASURED 2026-08-25 `[V]` — the estimate above verifies, and the "~4 skins" set now has a
> concrete candidate.** The 14 paks live in `models/` (13) + `research/pak_re/` (1) and total
> **33,884,614 B = 32.3 MB**; the extremes are `ship_dr_freemanw_v2sc` 579,540 B and `skeleton2_v1sc`
> 4,630,346 B, so this section's `570 KB … 4,522 KB / ≈32 MB` was right.
> **What is new: the HOST install already carries a chosen set of FIVE**, hand-placed rather than
> deployed (`deploy-all.ps1:48` copies only `hl_einstein_v1sc.pak`) — `hl_einstein_v1sc`,
> `luther_v1sc`, `rvi_scientist_v1sc`, `sci_v1sc`, `walter_v1sc` = **14,929,010 B = 14.2 MB** as
> five separate paks, plus ~119 KB of `<name>.png` preview tiles and the 342 KB `dr_kel.png`
> native-kel tile. That is the best available evidence for which skins the base pack is drawn from.
> Note `rvi_scientist_v1sc.pak` is **not** in `models/` — it comes from
> `tools/client_model/_rvi_scientist_v1sc/`, so the base pack draws from two source trees today.
>
> **USER 2026-08-25, and it is a SHAPE decision, not just a count: "4 скина в итоге будут внутри
> `scientists.pak`" — FOUR skins inside ONE pak file.** Two consequences that must not be lost:
> - **The size cannot be predicted by summing four of the five above.** Each standalone pak carries
>   its own UE4 pak header + index, and the shared/duplicated assets across scientist skins are
>   exactly what a single archive would collapse — which is the "may dedupe meaningfully better than
>   the sum" hypothesis this section already raised, now load-bearing. The sum of any four of the
>   measured five (**~10.1 MB … ~13.8 MB** depending on the choice) is therefore an **upper bound,
>   not an estimate**. The real number is unmeasured until `scientists.pak` is built, and it should
>   be measured before it is quoted anywhere.
> - **One pak is precisely what §7.7 says the registry cannot read.** `skin_registry.cpp:159` derives
>   the skin's display name from the pak's **stem**, so a single `scientists.pak` enumerates as ONE
>   entry called "scientists" no matter how many skins are inside. This decision does not merely
>   *benefit* from the §7.7/§7.7b rework — it is **unusable without it**. That makes §7.7 a hard
>   precondition of WP-9, not a parallel task.

**Part 2 is mostly free after §7.7.** Once the scan walks `LogicMods/` subdirectories, a user pack
installed by r2modman lands in its own `<Author>-<Name>/` folder and is enumerated automatically. The
subdirectory name is then also the **package identity** — useful for part 3.

**Part 3 maps onto machinery that already exists:**
- `[MEASURED]` the skin name is **already on the wire** — `SkinChange` reliable kind **82**, defined at
  `protocol.h:2181` (`:939` is the changelog note), plus the skin field on Join + RosterRow; every
  player carries a persisted `player_skin=` choice in `multivoid.ini`.
- `[MEASURED]` the failure is **already detected**: `client_model.cpp:52-53` logs
  *"skin '%s' %s NOT loadable (pak absent on this machine?) -- native kel fallback"* and falls back to
  the game's own kel body. Today it is log-only; part 3 is surfacing it.
- The surfacing grammar exists too: the device-busy local chat line (`<HolderNick> is using <unit>`)
  went through `AnnounceDirect`. **Standing rule to honour: the feed never renders "You" — always the
  nickname.**
- **Dedup:** `ResolveCached`'s `tried` latch is per skin NAME, not per peer, so the chat line needs its
  own per-`(peer, skin)` latch or a respawn will repeat it.
- **Mid-join (principle 8) is satisfied naturally** — the notice fires at puppet skin-resolve, which a
  joiner performs on adoption, so a late joiner is told about skins already in use.
- **The base pak never triggers it:** the join gate is byte-equality on the Paper pair, so every peer
  in a lobby runs the same build and therefore the same bundled `scientists.pak`. Only CUSTOM packs can
  produce the notice — which is exactly the intent.

**RESOLVED (USER 2026-08-23): ship (a) first, then (b).** Rationale below stands as recorded.

**RESOLVED (USER 2026-08-23): the base pak holds ~4 skins, not all 14 — the user picks which.** That
cuts the bundle from ~32 MB to roughly 2-16 MB depending on the choice. It also creates a constraint
set that must be honoured or the out-of-box experience breaks:

- **`kDefaultSkinName` MUST be one of the chosen ~4.** `[MEASURED]` it is `"hl_einstein_v1sc"`, defined
  at **`include/coop/player/skin_registry.h:36`** (the `multivoid.ini player_skin=` default; the
  `protocol.h:942` mention is a changelog comment, not the definition). If the default is not in the base pak, every fresh
  install defaults to a body nobody can load — so every peer would fire the §7.7c notice about every
  other peer on first join. This is the single most likely way to ship this feature broken.
- **The base pak defines the LOBBY-SAFE set.** Because the Paper-pair join gate guarantees one build
  per lobby, exactly the bundled skins are the ones every peer is certain to have. Everything else is
  optional-by-construction.
- **The starter roll should therefore prefer base-pak skins.** After §7.7 the registry also lists
  user-pack skins; rolling a NEW identity onto one of those would hand a first-time player a body most
  of the lobby cannot see. Roll among the guaranteed set.

**A THIRD site is pinned to one-pak-per-skin — `PickRandomStarterSkin()`.** `[MEASURED]`
`skin_registry.cpp:84-112` curates six starter names (`walter_v1sc`, `sci_v1sc`, `rvi_scientist_v1sc`,
`luther_v1sc`, `twhl_scientist2_v1sc`, `twhl_scientist3_v1sc`) and tests presence by asking the
filesystem **whether `<dir>/<name>.pak` is a regular file** (`:97-99`). With a single `scientists.pak`
none of those files exist, `present` is empty, and **every new identity silently falls back to
`kDefaultSkinName`** — the curated roll quietly dies. The six names must also be reconciled with
whichever ~4 actually ship.

**FULL CENSUS — 11 surfaces, not 3.** An earlier revision of this section said "three sites"; a
tree-wide census of `.pak` / `LogicMods` / `PakDir` corrects that. Fixing only the logic would ship a
build whose own UI tells players the wrong thing.

| # | surface | assumption | breaks as |
|---|---|---|---|
| **LOGIC (3)** | | | |
| 1 | `skin_registry.cpp:114-121` `PakDir()` | skins live in exactly `LogicMods/multivoid/` | nothing found from a Thunderstore install (§7.7) |
| 2 | `skin_registry.cpp:126-159` `Entries()` | skin name = pak file **stem** (`:159`) | one shared pak = one bogus skin named `scientists` |
| 3 | `skin_registry.cpp:84-99` `PickRandomStarterSkin()` | presence = `<name>.pak` **is a file** (`:98-99`) | `present` empty -> every new identity silently gets `kDefaultSkinName` |
| **PLAYER-FACING TEXT (2) — becomes FALSE** | | | |
| 4 | `local_body.cpp:127` | *"(drop the pak into LogicMods/multivoid and re-pick)"* | tells the player the wrong folder |
| 5 | `skins_panel.cpp:52` | *"A skin = a converter .pak in Content/Paks/LogicMods/multivoid"* | states the retired rule as the rule, in the F1 browser itself |
| **CONTRACT COMMENTS (6) — the header IS the spec** | | | |
| 6-9 | `skin_registry.h:8, 40, 62, 68` | four blocks describing one-pak-per-skin (`:62` *"per `*.pak` in the LogicMods multivoid folder"*) | the next reader implements the old shape from the header |
| 10-11 | `protocol.h:944, 2182` | *"skins = converter paks in LogicMods/multivoid/, name = ..."* | the wire doc describes a dead layout |

**RULE-1 fix, one root: presence must be asked of the REGISTRY ("is this skin name available?"), never
of the filesystem ("does `<name>.pak` exist?").** One authority for what exists; `PakDir`, `Entries`
and the starter roll all consume it. Fixing them piecemeal leaves the next pak-shape change to break
whichever survived — and leaves surfaces 4-11 lying.

`[MEASURED]` the fact the whole migration rests on is stated in our own header,
`ue_wrap/core/asset_load.h:5-6`: *"UE4 auto-mounts every `.pak` under `Content/Paks/` at startup"* —
which is why loading is pak-shape-agnostic and only the presence/enumeration layer has to change.

**THE ONE OPEN FORK — what does the message NAME?** The user's wording is *"нету у вас этого пакета"*
(you don't have this PACKAGE), but only the SKIN NAME is on the wire today:
- **(a) name the skin only** — *"Pelmentor is wearing 'walter_v1sc', which you don't have."* Zero wire
  change, ships with part 3 immediately, and the player can search that name.
- **(b) name the package** — *"…install 'CoolSkins' to see it."* Better UX and it becomes natural once
  §7.7 lands (the registry then knows each skin's containing folder = the Thunderstore package name),
  but it puts a pack identifier on the wire = **a protocol bump**.

Recommend shipping (a) with part 3 and adding (b) when the wire is next bumped for another reason, so
the notice is not gated on a protocol change. **User's call.**

### 7.8 The asset-provenance record (kept for context; the decision is §7.6)

The **mechanism** is settled (§7.2). On the asset, this repo had a position that predates the question:

- `[MEASURED]` the pak we deploy today, `research/pak_re/hl_einstein_v1sc.pak`, is **derived from
  Valve's Half-Life scientist model**, and it has **never been in git** — three independent
  `.gitignore` rules keep it and its inputs out: `research/pak_re/` (:144, *"extracted copyrighted
  game content — dev/RE only, never shipped"*), `tools/hl_einstein_v1sc/` (:169, *"third-party model
  assets (Valve/COF), local only — never commit"*), and `models/` (:174), whose comment says it
  verbatim: **"distribution-unsafe, deploy reads it from disk, git never carries it"** (2026-07-02).
- So "our mod ships the scientist model" has only ever been true of **local dev installs**.
  `tools/deploy-all.ps1` copies it from disk; the public repo has never carried a byte of it.
  Publishing it in a Thunderstore package would be **public redistribution of a Valve asset** — a
  different act from a local dev copy, and the one the gitignore comment was written about.
- `[MEASURED]` **nothing breaks without it.** `coop/player/client_model.cpp:52-53` logs
  *"skin ... NOT loadable (pak absent on this machine?) — native kel fallback"* and puppets fall back
  to `kerfurOmega_KelSkin` — **the game's own skin**, already on every player's disk, nothing
  redistributed. A pak-less package is a fully working mod.

**Resolved by the user 2026-08-23 (§7.6): the HL skins ship.** The alternative that was on the table —
sourcing a CC0/CC-BY or commissioned scientist mesh — remains cheap if it is ever wanted, because the
conversion chain in `docs/COOP_CLIENT_MODEL.md` (`mdl -> psk -> repose -> ue_cook -> repak`) is
**model-agnostic**: the work would be sourcing a mesh, not rebuilding tooling. Noted only so that
option is not re-derived from scratch later.

### 7.5 Owed measurements before WP-9 ships

- ~~Which UE4SS build `Thunderstore-unreal_shimloader-<ver>` bundles~~ **CLOSED `[V]` 2026-08-25.**
  The shimloader package **is** the UE4SS delivery: `Thunderstore-unreal_shimloader-1.1.7` ships
  `dwmapi.dll` (700,488 B, FileVersion 1.1.7 — shimloader itself) plus a whole `UE4SS/` drop —
  `UE4SS/UE4SS.dll` (16,228,864 B, md5 `8A78269B`, **no version resource at all**),
  `UE4SS/dwmapi.dll` (61,952 B — UE4SS's own proxy), `UE4SS-settings.ini`, and the eight built-in Lua
  mods. `ShimloaderInstaller.ts` copies exactly `dwmapi.dll`, `UE4SS/ue4ss.dll`,
  `UE4SS/UE4SS-settings.ini` to the profile root and `UE4SS/Mods/**` → `shimloader/mod/**`.
  **Era, by exported symbol set rather than by a version string** (there is none): this build exports
  `on_program_start`, `on_unreal_init`, `on_ui_init`, `on_dll_load`, `on_update`, `on_cpp_mods_loaded`,
  `render_tab`, `register_tab`, `register_keydown_event` (2 overloads) and 4+4 `on_lua_start`/`on_lua_stop`
  overloads — i.e. the **wide, post-3.0.1 `CppUserModBase`**, matching the vendored
  `reference/RE-UE4SS/UE4SS/include/Mod/CppUserModBase.hpp`, not the narrow v3.0.1 surface.
  **Our contract is unaffected either way** — §7.2b measured that we import **zero** UE4SS symbols,
  and `cppmod_entry.cpp` already sizes its stub table for the wide era (slots 0..15) with a runtime
  WARN above it. The `UE4SS-settings.ini` this package ships also has `GraphicsAPI = opengl` and
  `GuiConsoleEnabled = 0`, which is worth knowing before blaming our overlay for anything.
- ~~Whether the VOTV community requires listing approval~~ **PARTLY CLOSED `[V]` 2026-08-25.** The
  ecosystem schema lists the community as `voices-of-the-void`, `listed: true`, with 15 categories
  (`mods`, `tools`, `libraries`, `tweaks`, `misc`, `audio`, `items`, `language`, `console`, `kerfur`,
  `signals`, `crafts`, `placeables`, `modpacks`, `nsfw` — **no multiplayer/co-op category**), two
  sections (`mods` excluding `modpacks`; `modpacks` requiring it), `wikiUrl`
  `https://questwalker.github.io/votv-modding-wiki/` and a Discord invite. Whether uploads pass
  through moderation is **not** expressible in that schema, so it stays open — but it is a question
  for the community's own channels, not a measurement.
- **STILL OWED, and it is an account action, not a measurement:** Thunderstore team/namespace
  creation (the namespace becomes the `<Author>` half of `<Author>-Multivoid`, which §7.2a trap 4
  shows is load-bearing for the pak path), plus the service-account API token that §7.9 needs.

### 7.9 Can GitHub produce the ready-to-install package, or must it come from the maintainer's PC? (USER 2026-08-25)

**Short answer: GitHub can do all of it except the `.pak`, and the missing piece is a zip step, not
a capability.** The blocker is one unshipped asset's *inputs*, not CI.

**What GitHub already does, proven by green runs and not by reading YAML** `[V]` 2026-08-25:

| step | on GitHub? | evidence |
|---|---|---|
| Fetch every dependency | yes — all 6 submodules public; vcpkg pinned by `builtin-baseline` and bootstrapped by the workflow itself | `.gitmodules`; `build-core.yml:70,158-174` |
| Compile the payload DLL | yes, `windows-latest`, ~34 min wall clock | `build-core.yml:37,208`; runs `2026-07-31 06:43→07:18`, `07-27`, `07-25` all `success` |
| Needs UE4SS headers/libs | **no** — D-3 means we link nothing of UE4SS; the contract is our own `cppmod_entry.cpp` + `cppmod_stubs.asm` | `CMakeLists.txt:219-220` |
| Needs the game install / a dumped SDK header | **no** — `sdk_profile.h`/`sdk_profile_names.h` are committed, fonts are committed, `version.h` is `configure_file`d | tracked in git; `CMakeLists.txt:41-44` |
| Tag → judge → cacheless rebuild → publish + sha256 verify | yes, entirely on the runner with `GH_TOKEN: ${{ github.token }}` | `release-core.yml`; the `release` workflow has completed `success` (e.g. 2026-07-26) and the ledger carries published builds |

**What is missing is one step, and its precedent is already sitting in our own tree.**
`publish.ps1:24-28` asserts *exactly two loose DLLs* and uploads them individually — there is no zip
anywhere, and `git ls-files | grep -icE 'thunderstore|manifest\.json|icon\.png'` → **0**, i.e. no
packaging script exists at all. Meanwhile
`reference/unreal-shimloader/.github/workflows/release.yml:79-97` — written by the people whose
loader we are targeting — does the whole thing on `windows-latest` with no maintainer machine: stage
`icon.png` + `README.md` + the payload, heredoc `manifest.json` **with the version interpolated**
(exactly §7.3's "generated, never hand-edited" requirement), `7z a -tzip`, then `gh release create`
with the zip. That is a complete working answer to the question, and it is upstream's, not ours.

Publishing onward to Thunderstore from CI is a first-party path: a **service account** on the team
issues an API token, it goes in a repo secret, and `tcli publish` reads it as `TCLI_AUTH_TOKEN`.
Marketplace actions wrap this and accept a **pre-built zip** via a `file:` input, so we would not
have to hand our packaging to a third-party action to use one — we build the zip, it uploads it.

**What genuinely cannot come from GitHub today, and why each is a different kind of problem:**

1. **The `.pak` — an INPUT problem, not a tooling problem, and now the ONLY unanswered one.** Zero
   `.pak` files are tracked (`git ls-files | grep -c '\.pak$'` → 0; `.gitignore:6` `*.pak`), and the
   one we deploy, `research/pak_re/hl_einstein_v1sc.pak`, sits under three independent ignore rules
   (`.gitignore:6`, `:144`, `:273`). The *chain* is deliberately editor-free Python + `repak`
   (`tools/client_model/README.md`), so nothing about it needs the Unreal Editor — but its inputs are
   a cooked template extracted from the game's own paks and a Valve source model, neither of which has
   ever been in git (§7.8), and neither of which can be.
   **The DISTRIBUTION half is decided** (§7.7c part 1, re-confirmed by the user 2026-08-25: one zip,
   base `scientists.pak` inside, **four skins in one pak**). The remaining question was purely
   mechanical — how the pak's bytes reach a CI runner that cannot rebuild them — and the user
   answered it the same day.

   > **USER DIRECTION 2026-08-25 (stated as a leaning, "скорее всего", so treat it as the working
   > answer rather than a closed record): `scientists.pak` goes IN THE PUBLIC REPO.** It is the
   > mod's default skin pack and ships with the mod, so it is a build input like any other, and
   > committing it is what makes the whole publish hands-off. Of the three candidates that were on
   > the table — (a) commit it, (b) attach it to a release and fetch by pinned sha256, (c) keep it
   > maintainer-side — this is **(a)**. (c) is dead: it forfeits everything else §7.9 establishes.

   **What (a) mechanically requires, measured 2026-08-25** — none of it is hard, but all of it is
   easy to discover too late:
   - **The bytes must move to a tracked path first.** Every current source folder is ignored:
     `models/` (`.gitignore:174`), `research/pak_re/` (`:144` and `:273`), and
     `tools/client_model/_*/` (`:200`, where `rvi_scientist_v1sc` lives). The pak's new home has to
     be somewhere none of those cover.
   - **`.gitignore:6` is a blanket `*.pak`** and needs exactly one negation, scoped to this file,
     with the reason written next to it — the rule's own header says it exists to keep copyrighted
     binaries out, so a silent exception is the wrong shape.
   - **The preview tiles come too.** §7.2a's package shape carries `<model>.png` beside each pak (the
     F1 browser reads them), and those live in the same ignored folders.
   - **Git history is additive and permanent, and this is the one cost worth stating once.** A blob
     committed to a public `main` is in every clone forever, and `docs/DOCS_ARC.md` records the
     user's ruling that **history is not rewritten**. So the number that matters is not the pak's
     size but its size **× the number of times it is ever rebuilt** — and
     `docs/VERSION_MIGRATION.md` says a game recook is a recurring event. If the pak turns out to be
     ~10 MB, five recooks over the mod's life is ~50 MB that every future clone pays for. Candidate
     (b) exists specifically to avoid that and stays available if the number comes in high — which is
     the reason §7.7c's "measure it before quoting it" line is now load-bearing.
2. **`fingerprint.json`** — a human commits the toolchain dump from a cacheless run
   (`docs/RELEASE.md:99-104`); a mismatch is a hard refusal (`fingerprint.ps1:56-60`). By design.
3. **The build number, `LEDGER.tsv`, `notes/b<N>.md`, the tag push** — human by design; `LEDGER.tsv`
   calls itself the single mint authority and append-only.
4. **The Thunderstore team + service-account token** — an account action (§7.5), one time.

**One honest caveat on item 1's opposite side:** the GNS submodule pulls nested submodules including
`webrtc.googlesource.com` (~263 MB), which is public but is *not* GitHub. CI has been green with this
repeatedly, so it is a proven path rather than a hypothetical, but it is the one dependency whose
availability we do not control and it is worth naming before anyone calls the CI build hermetic.

**The icon — RESOLVED, USER-SUPPLIED 2026-08-25.** Thunderstore requires `icon.png` at the zip root,
**exactly 256×256** (§7.2b: all five field packages comply). When this section was first written the
repo tracked **zero image files of any kind**, and the only candidate on disk was
`site/public/favicon.svg` inside an untracked `site/` (`.gitignore:205`) that CI cannot reach. The
user then supplied the art. It now lives in the repo, and it is the **first tracked binary art asset
the project owns**:

- `assets/branding/icon-512.png` — the master, verbatim as supplied (512×512, PNG, 32bpp alpha).
- `assets/branding/icon.png` — **256×256, GENERATED** from the master by the one-line HighQualityBicubic
  downscale recorded in `assets/branding/README.md`. Alpha survives (the art has rounded corners, and
  the Thunderstore spec explicitly supports transparency). Never hand-edit it; re-run the line.
- Provenance, stated once so it is not re-litigated: the art is a Multivoid screenshot showing the
  HL-derived client model. That is the **same asset class §7.6 already settled** — it ships.

**Where this lands the WP-9 estimate:** unchanged in kind, sharper in shape. WP-9 is a `7z`/`Compress-Archive`
step plus a generated `manifest.json`, with the `icon.png` now in hand — bolted onto a release lane
that already builds, verifies and publishes without a maintainer's PC. **The base `scientists.pak`
rides in the same zip** (§7.7c part 1), so item 1 above — how its bytes reach the runner — is the one
remaining question standing between WP-9 and a fully hands-off publish.

---

Related: `[[project-wp2-realistic-env-test-2026-08-22]]`,
`[[project-wp2-precut-and-trampoline-crash-2026-08-22]]`,
`[[project-f2-ue4ss-switch-decision-2026-08-21]]`,
`[[lesson-veh-crash-reporter-preempts-our-seh-guard]]`,
`[[lesson-double-detour-crash-is-config-dependent-needs-pe-callback-arm]]`.
