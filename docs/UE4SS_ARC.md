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
| **WP-2** | The loader cut: delete `xinput_proxy.cpp` + the proxy deploy path (RULE 2); `cppmod_entry.cpp` in; predecessor detection + mutex; keep EVERYTHING else. | **DONE 2026-08-28.** Pre-cut LANDED (§2). Fix (**B**, §4) BUILT + compose VERIFIED (2026-08-22). IsLive/VEH arc BUILT (D1). ~~Symbolize the 19:17 dump~~ RETIRED by hash census; ~~teardown residual~~ CLOSED as the §4c use-after-free fix. **Commit 3 LANDED (`1912d229`)**: proxy source + CMake target + dllmain proxy lane + dup-feeder + inject.ps1 deleted; OUTPUT_NAME → `main`; identity moved into a generated VERSIONINFO with fail-closed deploy/publish compares. The §7.3a anchor weld landed the same day as **C3.3 (`d693609b`)** — publish/ledger_lib/ledger_lint/tag_regex_selftest/notes_regen inverted to the one-zip shape, INSTALL.md flipped with its anchors. Two post-ship audits folded (0 CRIT/0 IMPORTANT + a 6-miss doc census, all fixed). |
| **WP-4** | Fix the stale install/update/uninstall prose + the site + installer for the UE4SS lane. | **DONE in the tree 2026-08-28** (C3.3 `d693609b` rewrote INSTALL.md whole with the re-shaped anchors; C3.4 `8eeda065` swept the census's doc rows — README/BUILDING/ARCHITECTURE/RE_WORKFLOW/ROADMAP/FEASIBILITY/VERSION_MIGRATION/RELEASE and the small rows; site templates flipped in the site repo `70cfd6a` with **deploy GATED on the first one-zip release**). §7.3a item 5's allowance covered the early local flip (nothing pushed). **Residuals:** the site `public/` upload (gated); **§7.0's GitHub repo DESCRIPTION/topics live OUTSIDE the tree and are still stale** (`description` says "a standalone C++ DLL"; `dll-injection` topic; `homepageUrl` empty). |
| **WP-6** | Distribution re-home (the `multivoid-<game>-<build>.dll` filename + master + release flow onto the mod-folder shape). | **DONE 2026-08-28 except the release itself** — the filename re-home is commit 3 (`1912d229`: OUTPUT_NAME `main`, identity in VERSIONINFO) and the release flow is C3.3 (`d693609b`: one zip end-to-end, era-aware body writer, fixtures). The master needed no change (it serves the pair as strings, no filename). The first release in the new shape is §6 step 5. |
| **WP-9** | **Thunderstore publication** (USER 2026-08-23: "надо нам бы стать официальным модом и попасть в магазин thunderstore ... чтобы обычный юзер смог поставить нативно"). Ship Multivoid as a Thunderstore package so r2modman / Thunderstore Mod Manager installs it natively. | **NEW, SPECIFIED, NOT BUILT** — §7. The payload shape is ALREADY correct; what is missing is package metadata, a GENERATED manifest, and a publish step. **The version mapping is DECIDED, not owed** (§7.3, user 2026-08-23: `<game-major>.<game-minor>.<build>`); this row said "a version mapping decision" was missing after §7.3 had already made it. **§7.3a (2026-08-24, user-raised) measures what the versioned DLL name costs today** — it moves on every proto bump including security-only ones (`0.9.135` as of `ca3943e9`), its CMake justification expires with WP-2 commit 3, `deploy-mod.ps1` picks the payload by mtime out of 14 artifacts, and the six anchor sites that must move in one commit are tabulated. **2026-08-25 (user-raised, five real Thunderstore packages): §7.2 was measuring the extracted PROFILE and calling it the ZIP — the real zip has a `mod/` wrapper, and §7.2's tree would have installed cleanly and never loaded. §7.2a is now the authoritative routing rule (from Thunderstore's own ecosystem schema + r2modman's rule engine and test spec), §7.2b is the field survey + the measurement that shows what D-3 bought (field mods import 32/40/130 mangled C++ symbols from `UE4SS.dll`; we import 0), and §7.9 answers "can GitHub produce the package" — yes for everything except the pak, whose blocker is its inputs.** **THE PACKAGING HALF IS BUILT 2026-08-26** (`2a223362` + `3dd546dd`): `tools/release/package.ps1` assembles the §7.2a zip, `Test-PackageZip` is the fail-closed tree check, the drill is 14 arms all passing, and the Team `Multivoid` now exists. The zip was **hand-installed from the extracted artifact and booted** -- rule B's manual half, evidenced (§7.8). **NOT done (re-verified 2026-08-28):** ~~the `publish.ps1` asset-shape inversion~~ **C3.3 LANDED (`d693609b`)** -- what remains is the r2modman managed-import control (needs the rig mutation: this box carries UE4SS's dwmapi, not shimloader) and the upload itself, which the user has explicitly deferred (*"пока не обязательно грузить, сначала проверки локальной установки zip"*). |
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

All four installs are CONVERTED: UE4SS 3.0.1 (pinned) + `Mods\Multivoid` mod folder.
**COMMIT 3 LANDED 2026-08-28 (`1912d229`): the proxy SOURCE + loader lane + dup-dialog feeder +
inject.ps1 are DELETED** (the dialog itself survives on `server_browser_native`'s missing-donor
feeder); OUTPUT_NAME is the contract name `main` and the Paper pair moved INTO the bytes (a
generated VERSIONINFO resource, verified fail-closed by `deploy-mod.ps1` and `publish.ps1`).
C3.3 (`d693609b`) inverted the release pipeline to the one-zip shape the same day.

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

- ~~**Teardown:**~~ **CLOSED 2026-08-26 — and the residual as written understated it. See §4c.**
  It said PolyHook holds a restore-pointer into our slot and `MH_Uninitialize` frees it. The real
  defect needed no third party at all: `MH_RemoveHook` CORRUPTS the trampoline in place before any
  unmap, and the RED arm crashed reading the trampoline's own address. Fixed in `42af8cc0`.
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
  double-detour, on a different seam.
  **NEW DATA POINT (2026-08-25, and it WEAKENS the CEF hypothesis): the same shape occurred in the
  CLEAN LAB env, with no VoidFax/CEF/FusionFix present at all.** `mp.py smoke` on b143 bytes
  `c81f836e`: the HOST launched at 20:48:54, logged normally for ~10 s, and was found at kill time
  with the GAME's own `Fatal Error!` window and no UDP bind (`FAIL: host did NOT bind UDP within
  30s`). Both logs end CLEANLY — `multivoid.log`'s last lines are the `pe_diag[post-init]` block
  (`RELAY: POLYHOOK-COMPOSED(immune relay in-place hooked -- fix working)` / `WHO-FIRST: WE-FIRST`),
  and `UE4SS.log` ends at `Event loop start`. No crash dump was produced. **Frequency: 1 of 2
  consecutive boots on IDENTICAL bytes** — the immediate re-run PASSED (both peers stable, client
  connected, ledger selftest ALL PASS), so it is not deterministic and it is NOT caused by that
  session's diff (the only delta from the previous PASSING build was a one-line comment). `[?]`
  Unattributed: with no dump there is no callstack, so this is a LEAD, not a second root — but it
  means the class is reachable WITHOUT a third-party hooker, which is what the row above assumed.
  Cheapest next step: re-run the boot in a loop and capture a dump when it fires.
  NEXT for this thread: rebuild commit `275e0f67` to
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

### 4a. THE TEARDOWN PATH, MEASURED — the rig could not walk it, and now it can (2026-08-26)

B's honest residual (above) is *"leak the PE hook at process-close"*, because PolyHook holds a
restore-pointer into our MinHook slot and `MH_Uninitialize` frees it. That fix could not be
validated, and the reason was not subtle:

`[V]` `tools/mp.py` `kill_all()` is `Get-Process VotV-Win64-Shipping | Stop-Process -Force`. That is
`TerminateProcess`. No `WM_CLOSE` is ever sent, so our wndproc never runs, and a forced kill does not
deliver `DLL_PROCESS_DETACH` either. Therefore `ue_wrap/core/hook.cpp Shutdown()` →
`MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize()` — the path the residual lives on — **had never
executed under an automated scenario**, on any build.

> **CORRECTION 2026-08-26, same day, and it matters.** An earlier draft of this section said the path
> had "never executed in this rig's history, on any build, in any scenario", and the commit message
> of `fe474b86` repeats that. **Too strong.** `[V]` `docs/piles/test-evidence/handson-s31-doom-HOST.log`
> and the `handson-s32-strip` pair both contain `hook: MinHook shut down`; their banner reads
> `votv-coop 0.0.1`, i.e. pre-b122, i.e. **the proxy lane**. So the teardown HAS run — under a human
> close, before UE4SS was in our runtime. The true statement is narrower and more useful: **it had
> never run under an automated scenario, and it had never once run with a PolyHook composition on our
> relay until 2026-08-26 16:14.** The overstatement came from censusing `src/` and not `tools/` or
> `docs/` — the same alias-vocabulary lesson this project has now paid for three times.

**THE INSTRUMENT IS BUILT (`fe474b86`, additive, `kill_all` untouched).** `python tools/mp.py
gracefulexit` launches a solo host, waits for the UDP bind, settles, then posts
`WM_SYSCOMMAND`/`SC_CLOSE` — what an X-click and Alt+F4 actually generate, per `shutdown.cpp`'s own
hands-on note that UE4.27 acts on `SC_CLOSE` and bypasses `WM_CLOSE` entirely — and reads the log
written after the signal. It gates on the invariants that survive the coming fix (process exits,
`BEGIN` and `END cleanup` both present, no new crash report, no `[Error]`) and deliberately NOT on
which sub-steps the teardown performs, because that is exactly what the fix changes.
`--control-terminate` is its RED arm.

**Both arms ran 2026-08-26. Four things they measured:**

1. **RED control:** `Stop-Process -Force` produced **0 bytes** of teardown log. Not a partial trail —
   nothing. So the markers discriminate the close path.
2. **GREEN:** the full trail in order, process gone in 6.5 s, no crash report:
   `shutdown: close-signal received on HWND=... msg=0x112 wp=0xF060` → `BEGIN cleanup` →
   `net: session stopped` → `hook: MinHook shut down` → `END cleanup` → (3 s later)
   `cppmod: final dispatch tally ... (start_mod x1, uninstall_mod x0)`.
3. **THIS RIG IS THE COMPOSED CASE, WITH NO ArmPE FIXTURE.** `[V]` same run:
   `pe_diag[install] RELAY: IMMUNE-RELAY INTACT(UE4SS not armed on it)` at 16:14:21, then
   `pe_diag[post-init] RELAY: POLYHOOK-COMPOSED(immune relay in-place hooked -- fix working)` at
   16:14:31. UE4SS 3.0.1 armed its own PolyHook PE detour within 10 s **unprompted**. §5's note that
   ArmPE is disabled on these installs reads as though the rig does not compose. **It does.**
4. **There is a 3-second window.** 16:14:54 (`hook: MinHook shut down`) → 16:14:57
   (`DLL_PROCESS_DETACH`). Everything `MH_Uninitialize` unmaps stays unmapped while the process is
   still running, and `uninstall_mod x0` confirms UE4SS never tears its own mods down, so our
   teardown is entirely wndproc- and DETACH-driven exactly as `loader/cppmod_entry.cpp` assumes.

**Consequence for "USER run B":** its acceptance was *one ordinary real-env exit*. That is now a
scenario, not a request — and per the user's 2026-08-25 ruling that hands-on is closed, it had to
become one or it was a shelf.

### 4c. THE TEARDOWN USE-AFTER-FREE — found, measured, fixed (2026-08-26, `42af8cc0` + `eafb2207`)

§4a built the instrument. The instrument found something bigger than the residual it was built
for. **This section is cited from `pe_detour.cpp`, `imgui_overlay.cpp` and `hook.cpp`** — it is the
long-form account those comments point at.

#### What the code said

`[V]` `ue_wrap/core/pe_detour.cpp`, `Uninstall()`, Audit C3, dated 2026-05-27:

> *"Leaving the pointer non-null is harmless (UAF is not possible because `g_originalPE` points at
> the engine's PE, a process-lifetime entry point that is never unloaded)."*

#### Why that is false, twice

**The object.** `[V]` `third_party/minhook/src/hook.c:634` — `*ppOriginal = pHook->pTrampoline`.
The out-param of `hook::Install` is MinHook's 64-byte **trampoline slot**, not the engine's
function. `[V]` Our own install log has always printed it as `trampoline %p`. The audit read the
variable's NAME — `g_originalPE` — and the name was the evidence.

**The mechanism and the timeline.** Removing a hook does not merely schedule a later unmap.
`[V]` `buffer.c:43-50` — `MEMORY_SLOT` is a **union** of a free-list `pNext` and the trampoline
bytes. `[V]` `buffer.c:282` — `FreeBuffer` does `pSlot->pNext = pBlock->pFree`, writing eight bytes
**at offset 0 of the trampoline**, over the stolen prologue. `[V]` `hook.c:702` — its caller is
`MH_RemoveHook`, which `hook::Uninstall` called at `pe_detour.cpp:741`, **one line above** the
`Sleep(50)` at `:757` that the comment offered as its mitigation. The window was not the three
seconds §4a measured to `DLL_PROCESS_DETACH`. **It was zero**, and `[V]` ProcessEvent dispatches at
~250,000/s in normal play.

**And this is how the header could contradict itself unnoticed.** `[V]` `hook.h:45-48` called
`Disable` *"the ONLY safe retirement for a detour other threads may be entering concurrently (an
inflight counter cannot prove absence)"*; `[V]` `hook.h:56`, five lines later, called
remove-and-uninitialize *"Safe to call once at shutdown"*. Both shipped for four months. An audit
had already looked at the site and cleared it, so nobody re-derived it.

#### The measurement (RED / GREEN, same scenario, same rig)

`ue_wrap/core/hook_drill` samples the trampoline's first eight bytes either side of the teardown.
It asserts **those bytes, not a crash** — `[V]` the eight bytes written are a heap pointer that may
or may not fault when executed, and `[V]` `buffer.c:288-296` only `VirtualFree`s at `usedCount == 0`,
so neither a fault nor an unmap is a guaranteed signal. A control keyed on a crash is a coin flip.

| arm | build | result |
|---|---|---|
| **RED** — old `hook::Uninstall` + the drill | `b064a4e1` | baseline sample `trampoline 00007FF6E7CF0FC0 first8=25FF544157565540`, then **no post-disable sample, no `END cleanup`**, and a crash report: `EXCEPTION_ACCESS_VIOLATION reading address 0x00007ff6e7cf0fc0` |
| **GREEN** — `hook::Disable` + the drill | `3c14bccc` | same address, `25FF544157565540` **before and after**, full trail to `END cleanup`, exit 3.9 s, no crash report |

**The faulting address IS the trampoline, byte for byte.** `25FF544157565540` little-endian is
`40 55 56 57 41 54 FF 25` — ProcessEvent's stolen prologue, so the drill was reading the right
memory.

**A correction to my own reasoning, recorded because it was load-bearing while I believed it:** I
argued the `VirtualFree` would not fire because `usedCount` would not reach 0 with ~12 live hooks.
It did. MinHook allocates blocks near their target; ProcessEvent lives in the game exe while the
overlay hooks live near `dxgi.dll`, so the PE trampoline had its **own block** and removing that one
hook released the whole page. "~12 hooks" was never the relevant number.

#### The fix

- **`hook::Uninstall` deleted.** Its remove IS the corruption. Its four call sites became `Disable`
  or vanished with the dead function that held them.
- **`hook::Shutdown` keeps `MH_DisableHook(MH_ALL_HOOKS)`, drops `MH_Uninitialize`.** The blanket is
  not redundant: `[V]` `ui::imgui_overlay::Shutdown()` had zero callers tree-wide, so the blanket is
  measurably the ONLY thing that has ever lifted the overlay's three patches.
- **`Enable` re-reads the live flag AFTER `MH_EnableHook` returns.** The guard alone is
  check-then-act and `[V]` `overlay_backend_dx12.cpp:506` → `dx12_capture::Rearm()` reaches it from
  the render thread while the game thread is in `Shutdown`. Lock-free and with **no second flag**:
  `[V]` `dllmain.cpp:53-60` — `DoShutdown` is reachable from `DLL_PROCESS_DETACH` under the loader
  lock, where a mutex owned by a thread Windows already terminated never unlocks; and two flags that
  can disagree leave neither as authority.
- **The 11 lying identifiers renamed**, censused **by assignment site**: those receiving
  `hook::Install`'s out-param are now `*Trampoline`. `g_origVirtual`/`g_origFinal` (GNatives table
  reads) and `g_origProc`/`g_origWndProc` (`SetWindowLongPtr`) keep theirs — renaming those would
  author the inverse lie.
- **`tools/hooks/minhook_free_gate.ps1`** (the 7th gate, wired into `build-core.yml`): no
  `MH_RemoveHook`/`MH_Uninitialize` in `src/` outside one allowlisted line — `hook.cpp`'s
  enable-failure path, where enable just failed so the target was never patched. Matched as **calls,
  not prose**; shown RED by injection AND shown not to fire on the allowlisted shape. A gate cannot
  catch a lying comment, which is why the rename carries that half.
- **`eafb2207`** then deleted the orphaned subtree (RULE 2): `console::Shutdown`,
  `overlay_backend::Shutdown`, `dx11`/`dx12::Shutdown`, `dx12_capture::Shutdown` — each reachable
  only from the one above, none ever executed. Note the sting: `dx12_capture::Shutdown` was the ONE
  function implementing the Disable-only doctrine correctly, with the rationale written out — and
  being unreachable it had never protected anything, while the undocumented blanket did the real work.

#### NOT fixed — filed here so it is not mistaken for closed

1. **W1.** Lifting the PE patch restores ProcessEvent's prologue, which a composed PolyHook sits
   downstream of — so UE4SS's PE dispatch plausibly goes dark for the ~3 s to `DLL_PROCESS_DETACH`.
   **Unmeasured.** This change stops the corruption and the free; it does not answer that.
2. **The same question on Present.** Whether restoring a prologue at death harms a co-hooker there
   is order-dependent, and `pe_diag`'s `WHO-FIRST` line measures ProcessEvent ONLY. `[V]`
   `NahimicOSD.dll` is a measured co-hooker on this box's present chain (§4). Cheap instrument named:
   a byte dump at Present install, mirroring `pe_diag`.
3. **`MH_DisableHook(MH_ALL_HOOKS)` on the loader-lock path.** `[V]` `hook.c:267,348` — it reaches
   `CreateToolhelp32Snapshot` + `SuspendThread`, and a toolhelp snapshot inside `DllMain` is a
   documented deadlock risk. **Pre-existing and unchanged by this commit**, and the graceful-close
   path does not exercise it: `[V]` the wndproc latches `g_shuttingDown` first (`close-signal` at
   16:14:54, DETACH tally at 16:14:57), so DllMain's call is the idempotent no-op. **No test in the
   tree has ever run `hook::Shutdown()` under the loader lock.** A `gracefulexit` arm that suppresses
   the wndproc path would be the first.

### 4b. THE 19:17 SYMBOLIZATION IS RETIRED — a hash census answered it for free (2026-08-26)

§1's WP-2 row listed *"symbolize the 19:17 real-env EXEC-at-NULL dump"* as blocking commit 3. It does
not, and the discrimination it was meant to buy was available all along:

`[V]` Census of all **102** dumps in `%LOCALAPPDATA%\VotV\Saved\Crashes` by `PCallStackHash`:

- **The largest "cluster" is not one.** 47 dumps share `DA39A3EE5E6B4B0D3255BFEF95601890AFD80709`,
  which is **SHA-1 of the empty string** (verified: `printf '' | sha1sum`). That is the ABSENCE of a
  walkable callstack, not 47 identical crashes. Instrument caveat first, conclusion second.
- **The double-detour cohort is exactly 7 dumps**, hash `3E0EBD39…`, every one reading
  `EXCEPTION_ACCESS_VIOLATION reading address 0xffffffffffffffff` — precisely the signature §3
  predicts (non-canonical jump → `#GP` → no CR2 → Windows reports "AV read 0xffff…ffff").
  **CORRECTED 2026-08-28 (§4d's recount): "exactly 7" was the 08-21/22 WINDOW, not the hash's whole
  population** — the same hash + error string also sits on **5 dumps from 2026-05-25/30, the proxy
  era**, when no PolyHook existed in-process. The signature is the CLASS "non-canonical jump/read",
  not a double-detour fingerprint; the cohort's attribution rests on the timing bracket + §3's
  decode + §4d's on-demand knob repro (whose fresh dump carries the identical hash).
- **They span 08-21 22:44 → 08-22 13:13 and STOP.** Fix B went default-ON in `bd617056` on 08-22;
  compose was verified at 16:02 and 16:25 the same day. **Zero recurrence in four days.**
- Only two dumps exist after the fix, both 08-23, both DIFFERENT hashes; one reads `0x000000…`.

**So the two crash families are discriminable by ERROR STRING alone** — `0xffffffffffffffff` for the
double-detour class, `0x000000…` for the EXEC/read-at-NULL Present-chain family — which is exactly what
symbolizing one dump was supposed to establish. (Per the 2026-08-28 correction above: the string
SEPARATES the two families from each other, but `0xffff…ffff` alone does not PROVE double-detour —
the May proxy-era members are the counter-example. Separation was all the retirement needed.)

**And the proxy's independence is now measured, not argued.** `[V]` `src/loader/xinput_proxy.cpp`
has ZERO DXGI/Present surface: it is `ParseBuildNumber` → `LoadPayload` → `DllMain`. A grep for
"Present" returns three hits and all three are the local `legacyPresent` (= "is `votv-coop.dll` on
disk"), a name collision. Deleting the proxy cannot affect a Present-chain crash.

**Two residuals the census does NOT close, stated so the retirement is not overread:** `[V]` the
19:17 dump is not in this Crashes directory at all — it came from the separate `Desktop09n`
r2modman install, so a local census structurally cannot see it. And the 2026-08-25 clean-lab boot
fatal produced **no dump**, so it is invisible here too. **Note the logical hole this leaves, named
rather than papered over:** the discriminator lives inside a dump, and the reason the 08-25 fatal
looks benign is that it produced none — so "not that family" and "that family with an unwalkable
callstack" are not distinguished for it. It stays unattributed. The right place for its assertion is
the install tests §7.4's B-gate already requires, which ARE realistic-stack boots: they should assert
`POLYHOOK-COMPOSED` + `WE-FIRST` per boot (`pe_diag.cpp`'s RELAY/WHO-FIRST verdict lines — the
2026-08-28 extraction moved them out of `pe_detour.cpp`), because on a rig where ArmPE is
disabled a green boot rate is an instrument blind to the phenomenon.

### 4d. THE FIX-B RED TABLE (2026-08-28) — the A/B record; the escape knob RETIRED with it

`docs/LESSONS.md` (*"build the knob that FORCES the field's condition"*): the knob retires with the
mechanism (RULE 2); **the RED table is the durable artifact.** This is that table. Both fresh arms
ran on the SHIPPED b143 bytes (`main.dll` md5 `71410E028036D7F2`, all four installs byte-identical),
same rig, same scenario (`python tools/mp.py gracefulexit --no-deploy`), same hour, ONE variable.

| arm | forcing | relay verdicts (`pe_diag`, real log) | outcome |
|---|---|---|---|
| **RED** 2026-08-28 23:46 | `VOTVCOOP_PE_IMMUNE_RELAY=0` (legacy `FF25` relay) | `[install] LEGACY-RELAY INTACT` — tramp+0x14 holds `ff 25 00 00 00 00` + abs64 `&ProcessEventDetour` (`00007FFB7EA51FD0`, matches the detour line); `[post-init +10s] LEGACY-RELAY CORRUPT(double-detour hit)` — the abs64 POINTER slot at +0x1A now holds an INSTRUCTION (`ff 25 20 f0 07 80 …`): PolyHook's `followJmp` resolved our `FF25`'s operand EA and wrote its patch THERE — §3 step 3, byte for byte | **HOST DIED before binding UDP** (seconds after the corrupt snapshot). New dump `UE4CC-Windows-AFE4129E…`: `EXCEPTION_ACCESS_VIOLATION reading address 0xffffffffffffffff`, `PCallStackHash` **`3E0EBD39…` — byte-identical to the organic field cohort's hash** |
| **GREEN** 2026-08-28 23:47 | none (immune relay — the shipped default) | `[install] IMMUNE-RELAY INTACT(UE4SS not armed on it)`; `[post-init +10s] POLYHOOK-COMPOSED(immune relay in-place hooked -- fix working)` | **PASS** — graceful exit 3.9 s, teardown to `END cleanup`, `final dispatch tally` present, no crash report |

Historical arms (2026-08-22; the prose is scattered through §4 — consolidated here):

| arm | evidence |
|---|---|
| RED, organic (field) | 7 dumps, hash `3E0EBD39…`, spanning 08-21 22:44 → 08-22 13:13 — the UE4SS-lane window — STOPPING at fix-B default-ON (`bd617056`); zero recurrence in six days until today's deliberate knob repro |
| RED, real env 15:42 | r2modman + experimental UE4SS + ArmPE + fix OFF: `RELAY: LEGACY-RELAY CORRUPT` + `UE4SS.log` reporting "ProcessEvent address" = our trampoline **+0x1A** (the pointer slot!) + UE fatal (`multivoid.baseline-1542.log`) |
| GREEN, real env 16:02 | fix ON, same stack: raw byte dumps — install `48 B8 <&detour> FF E0`, post-init a foreign `FF 25` in-place hook on our relay; ~80 s session, clean shutdown |
| GREEN, DEV 16:25 → every rig boot since | `IMMUNE-RELAY INTACT` → `POLYHOOK-COMPOSED` + `WE-FIRST`; UE4SS 3.0.1 arms unprompted within 10 s on this rig (§4a item 3), so **every green boot exercises the compose**, incl. both b143 smokes and the C3 gracefulexit |

**What the fresh RED run additionally bought — a census correction (§4b overclaimed, fixed in
place):** filing today's dump forced a recount of the WHOLE `Crashes` directory, and the
hash+error-string pair has **5 additional members from 2026-05-25/30 — the PROXY era**, months
before any PolyHook existed in-process. So `0xffff…ffff` + `3E0EBD39…` is the **CLASS** "jump/read
through a non-canonical address → `#GP` → no CR2" — not a fingerprint unique to the double-detour.
Attribution of the 08-21/22 cohort rests on the timing bracket (starts with the UE4SS lane, stops
at fix-B default-ON), §3's byte-level decode, and this table's on-demand repro — not on the hash
alone.

**The knob is RETIRED (same commit, RULE 2).** `pe_detour.cpp` no longer reads
`VOTVCOOP_PE_IMMUNE_RELAY`; the PE hook installs `followJmpImmune=true` unconditionally and the
boot line is the fix-on form only. `hook::Install`'s `followJmpImmune` parameter STAYS — it is the
per-hook mechanism selector (other MinHook sites keep the standard relay), not a legacy escape.
Reproducing the RED arm now requires checking out a pre-retirement commit; that is the point.

### As-built (2026-08-22 — baseline REPRODUCED in the real modded env; compose VERIFIED same day, see Proof status)

- `ue_wrap/core/hook.{h,cpp}` — `Install(..., bool followJmpImmune=false)`; the relay rewrite
  (`MakeRelayFollowJmpImmune`) runs between `MH_CreateHook` and `MH_EnableHook` (target unpatched →
  thread-safe), fail-closed if the `FF25` relay signature is not found.
- `ue_wrap/core/pe_detour.cpp` — the immune relay is **UNCONDITIONAL since 2026-08-28** (the
  `VOTVCOOP_PE_IMMUNE_RELAY=0` A/B escape retired with the §4d RED table per RULE 2; it outlived
  the "retired at commit 3" schedule this line used to carry because no RED table existed yet —
  §6 step 3). Boot logs `PE relay followJmp-immune`.
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
  installs carried the D1 build `95B02A826950DDC4` when that line was written (2026-08-22 night).
  **DO NOT TRUST ANY SHA WRITTEN HERE.** `[V]` This figure has now gone stale three times, and on
  2026-08-26 a PARALLEL session redeployed between one session's smoke and its own report. The rule
  that replaces it: **`md5sum` the four `Mods/Multivoid/dlls/main.dll` before trusting any run**, and
  rebuild before trusting a deploy. (2026-08-26 reading, for scale only: `6b170c7c9023ef3f`.). Multivoid drops as
  `shimloader/mod/multivoid/dlls/main.dll` + `enabled.txt`; the game is a separate `Desktop\a09n`
  install whose Win64 has the shimloader `dwmapi.dll` + `ue4ss.dll`, launched via r2modman. The ArmPE
  fixture stays in the PROFILE (the repro rig) but is **DISABLED on the four coop-rig installs**
  (`enabled.txt` → `.off` on HOST/CLIENT_1) — two intermittent client boot fatals rode it (see §4
  residuals).
  **Do NOT read that as "the coop rig does not compose" — it does.** `[V]` 2026-08-26: with ArmPE off,
  UE4SS 3.0.1 armed its own PolyHook PE detour within 10 s unprompted and `pe_diag[post-init]` printed
  `POLYHOOK-COMPOSED` (§4a). The fixture FORCES the compose early; its absence does not prevent it. Run A was attempted (no repro — see §4); run B awaits the user on the D1 build.
- Rollback to the proxy lane if needed: copy `build/votv-coop/Release/xinput1_3.dll` + the versioned
  DLL beside the exe + delete `Mods\Multivoid\enabled.txt` (3 ops).
- Nothing is pushed; commits are local pending the user's word + the five-axis leak audit.

## 6. Next steps (in order)

1. ~~Confirm B in the real env~~ **DONE 2026-08-22** — see §4 Proof status (real-env byte decode +
   DEV `POLYHOOK-COMPOSED` boot, both crash-free).
2. ~~Build the IsLive zero-AV arc~~ **DONE 2026-08-22 night** (all 78 sites converted; gate/drill/
   smoke/differential evidence in §4). Remaining from that arc: ~~**USER run B**~~ — **its acceptance
   is now a SCENARIO, not a request** (`python tools/mp.py gracefulexit`, `fe474b86`): one ordinary
   close, asserting no crash report and a teardown that reaches `END cleanup`. Per the user's
   2026-08-25 ruling that hands-on is closed, a step whose last line was "the user will exit the
   game" was a shelf; it is now automated (§4a). Also remaining: the ad-hoc `{ptr,idx}` pair
   migration scope (pending user decision, design doc §6; partially done en route — local_streams +
   daynightcycle pairs retired).
2b. ~~**Symbolize the 19:17 real-env EXEC-at-NULL dump**~~ **RETIRED 2026-08-26 — §4b.** A hash
   census of all 102 dumps bought the discrimination this was meant to buy, for free: the two crash
   families separate by ERROR STRING alone. (This row survived one sweep after §4b retired it; it is
   the same stale-open class §1's WP-2 row had.)
3. ~~Add B's teardown leak-at-death~~ **DONE 2026-08-26 (`42af8cc0` + `eafb2207`) and it was
   BIGGER than a leak — see §4c.** The residual said "leak the PE hook at process-close";
   the measurement found a live use-after-free whose RED arm crashed reading the trampoline's
   own address. **The second half — the `VOTVCOOP_PE_IMMUNE_RELAY=0` escape — is DONE
   2026-08-28: the RED table was written FIRST (§4d), then the knob retired in the same
   commit**, per `docs/LESSONS.md` *"build the knob that FORCES the field's condition"* (the
   knob retires with the mechanism; the RED table is the durable artifact). The fresh RED arm
   reproduced the field crash on demand on b143 bytes — dump hash byte-identical to the organic
   cohort — and its recount corrected §4b's "exactly 7" overcount in place.
4. ~~**Commit 3** — the proxy deletion (RULE 2): `xinput_proxy.cpp` + the loader lane + dup-dialog +
   `inject.ps1` go, fully. Then WP-2 is DONE.~~ **DONE 2026-08-28 (`1912d229`) — WP-2 IS DONE.**
   The RULE-2 chain resolved as the second box below demanded: OUTPUT_NAME → `main`, the identity
   moved INTO the bytes (generated VERSIONINFO, `version.rc.in`), `deploy-mod.ps1`'s pick-by-build
   selector dissolved into a fail-closed VERSIONINFO-vs-tree compare (RED/GREEN drilled), and the
   dup-dialog's proxy feeder died while the dialog survives on `server_browser_native`'s feeder.
   Evidence: gracefulexit PASS, LAN smoke PASS (0 [ERROR] both peers), abi_gate PASS, package_drill
   14/14; two post-ship audits (0 CRITICAL/0 IMPORTANT + a 6-miss census sweep, all folded).

   > **THE ORDER IN THIS LIST IS WRONG AND WAS CORRECTED 2026-08-26 — the zip is a PRECONDITION of
   > step 4, not a follow-on to it.** The design pass that preceded commit 3 tried the obvious
   > reading (delete the proxy, ship a loose `main.dll`, zip later) and a critic killed it on a
   > measurement: `[V]` §7.8 + `THUNDERSTORE.md`'s checklist make rule B's acceptance test
   > r2modman's **"Import local mod"**, which consumes a **ZIP**. With no zip that test can never
   > run, so the 18 unpushed commits could never unblock — and hands-on is closed, so no human path
   > exists either. **A plan that builds the zip last cannot satisfy the gate that unblocks it.**
   > `[V]` §7.4c also already records the user choosing the zip as THE artifact, and names §7.4b's
   > loose-file release body as *"the proxy lane"* that retires with it. The packaging step was
   > therefore built FIRST (`2a223362`); step 4 is still open.
   >
   > **A second RULE-2 consequence of step 4, measured and not yet acted on:** `[V]`
   > `CMakeLists.txt` (grep `load-bearing, not cosmetic`) justifies the versioned
   > `multivoid-<game>-<build>.dll` OUTPUT_NAME **by the proxy's scan** — delete the scanner and the
   > justification is gone, which is §7.3a item 2. That retirement also dissolves ~30 lines of
   > sort-and-guard in `deploy-mod.ps1`, and note `[V]` the filename identity is ALREADY destroyed at
   > deploy time (it installs **as** `main.dll`), while `[V]` the DLL carries **no VERSIONINFO
   > resource** — so nothing out-of-band identifies a built or installed DLL today. Whatever replaces
   > that guard must be chosen with step 4, not after it.
5. **Release a UE4SS-lane build.** STILL OPEN as of 2026-08-28 -- no tag cut. (Its rationale
   sentence is OVERTAKEN: the prose already flipped in the unpushed tree per §7.3a item 5's
   allowance, with the LIVE surfaces -- the site, and GitHub once pushed -- gated on this release.)
6. **WP-4 + WP-6 + WP-9 as ONE welded change** (§7.4): ~~`docs/INSTALL.md` (both lanes, manager
   first) + `README.md` + the site templates & built `public/` + `ledger_lib.ps1` anchors and
   release-body block + `ledger_lint.ps1` checks + `publish.ps1` asset shape~~ **ALL LANDED
   2026-08-28 (C3.3 `d693609b` + C3.4 `8eeda065`; site `70cfd6a` staged, upload gated)** → and the
   Thunderstore package published (DEFERRED by the user -- the one leg still open). ~~Blocked on the user's §7.3 `version_number` call~~ **NOT BLOCKED (stale-open, corrected
   2026-08-26).** `[V]` §7.3's own heading reads *"DECIDED (USER, 2026-08-23)"* and the mapping is
   `<game-major>.<game-minor>.<build>` -- the block was lifted three days before this line was read,
   and the `0.0.<build>` "recommendation" it named is not what was chosen.
7. WP-7 (native debug subsystem) and WP-8 (hygiene split) stay parked.

---

## 7. How a VOTV player installs a mod NATIVELY — MEASURED 2026-08-23 (WP-4 / WP-6 / WP-9 input)

> **THE PROCEDURE MOVED OUT 2026-08-25 → `docs/THUNDERSTORE.md`.** This section keeps what it is good
> at — the *decisions*, the *measurements*, and the record of what was overturned. The repeatable
> *how* (preconditions, the manifest field-by-field, first upload, the immutability rules that make a
> botched upload unrepairable, why a package may be invisible, the moderation rules that bite
> `scientists.pak`, "player says it doesn't work" triage, and the pre-flight checklist) is now its
> own doc, written from the official wiki. **Read `THUNDERSTORE.md` before uploading anything; read
> §7.2a here before building the zip.**

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

**The mental model, first, because every mistake in this area is the same mistake: THE MANAGER WRITES
TWO PATH SEGMENTS THAT THE ZIP NEVER CONTAINS.** The profile tree you can see on disk
(`…\profiles\Default\shimloader\…`) is the install's **output**; the zip is its **input**; and the
manager inserts `shimloader\` (the profile root) and `<Author>-<Name>\` (from `mod.getName()`)
between them. Authoring either one into the zip, or reading the profile and writing it down as the
package shape, is the same error — and it is the error §7.2 made.

Side by side, one real package, measured on this box:

```
ZIP  (what we build)                 ->   PROFILE  (what you see on disk)
acitulen-DebugMod-5.0.3.zip               ...\profiles\Default\shimloader\
    manifest.json                             mod\acitulen-DebugMod\manifest.json
    icon.png                                  mod\acitulen-DebugMod\icon.png
    README.md                                 mod\acitulen-DebugMod\README.md
    mod\enabled.txt                           mod\acitulen-DebugMod\enabled.txt
    mod\dlls\main.dll                         mod\acitulen-DebugMod\dlls\main.dll
    pak\DebugMod.pak                          pak\acitulen-DebugMod\DebugMod.pak
```

Neither `shimloader\` nor `acitulen-DebugMod\` appears anywhere in the zip. **None of the five field
packages in §7.2b contains a `shimloader\` folder.** And a zip that *did* carry `shimloader\mod\…`
would hit a top-level directory named `shimloader`, which matches no route, so the rule engine would
recurse into it and *might* then match the `mod\` one level down by accident — **untested, and not
something to rely on instead of the convention every shipped package follows.**

The **manager** routes each top-level entry according to a per-game rule set that Thunderstore
publishes as machine-readable data, and VOTV's rule set is this — fetched live from
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
| `Z` | **OURS** — `kProtocolVersion` (`src/votv-coop/include/coop/net/protocol.h:709`), the Paper pair's build half | **`143`** as of 2026-08-26 (was `140` when this cell was last written) (was `134` when this table was written, `135` on the WP-6 A5 retirement, then 136 A34 / 137 A37-A38 / 138 B1 / 139 B2 / 140 A50 — see §7.3a item 1; the line number moves with it, so re-grep rather than trusting `:709`). **The rate this moves at is itself the §7.3a argument**: six bumps in two days, every one of them security work with no player-visible feature, each silently moving the Thunderstore release identity |

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

**2. The name's stated justification expires with commit 3.** `src/votv-coop/CMakeLists.txt:642-645`
(the `OUTPUT_NAME`/`multivoid-` comment block; **re-cited 2026-08-25 — it was `:636-638`, which the file
outgrew. Grep `load-bearing, not cosmetic` rather than trusting the number.**)
declares *"the filename is load-bearing, not cosmetic"* and gives the reason: **the xinput proxy
scans for `multivoid-*.dll`** (loads the highest build, flags duplicates for the in-game popup). WP-2
commit 3 deletes that scanner, at which point the sentence is false and the versioned filename has no
consumer inside the mod folder at all — UE4SS's contract name is the fixed `dlls\main.dll`. The
identity does not disappear; it moves to the zip name, the generated `manifest.json`, and the
in-game banner (`coop/version.h` + `kProtocolVersion`), none of which need it in the DLL filename.

**3. ~~A live defect~~ -- CLOSED 2026-08-25, verified 2026-08-26 at CODE level (not by its comment).**
`[V]` `deploy-mod.ps1:57-63` now sorts by the PARSED BUILD NUMBER descending and `:87-89` throws on a
payload/source mismatch against the name the tree declares -- the fail-closed check this item asked for.
It also handles a case this item never reached: `multivoid-0.9.0n-141.dll` and `multivoid-0.9.0o-141.dll`
both parse to 141. The original text is kept below because the REASONING is still the standing rule
("derive it, never guess it"); only its status changed.

**Original (now historical):** `tools/deploy-mod.ps1:38-43` picks the payload by
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
| `src/votv-coop/CMakeLists.txt` (`add_library(xinput1_3 SHARED)`; **`:684-737` / `:688` as of 2026-08-26 -- the third re-cite. STOP WRITING THE NUMBER: grep `add_library(xinput1_3`**) | ~~the `xinput1_3` target still BUILDS today; it retires with commit 3~~ **RETIRED at commit 3 (`1912d229`) — the grep now finds nothing, which is the point** |

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

> **PARTLY SUPERSEDED 2026-08-25 by §7.4c — read that first.** The *channel* half of this section
> stands unchanged: Thunderstore is primary, GitHub is the manual lane, the site keeps both. What is
> retired is the **second artifact** — the separate `*_release.zip` in game-folder hierarchy. There
> is now ONE zip in Thunderstore/r2modman layout serving both lanes, and the manual lane gets a
> documented relocation step instead of a pre-shaped tree. Every bullet below about *"the GitHub
> artifact becomes a single archive"*, its naming, and its internal tree is therefore obsolete;
> the bullets about the proxy-lane anchors and `publish.ps1` inverting are still live, restated in
> §7.4c.

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

### 7.4c ONE ARTIFACT, r2modman-shaped — DECIDED (USER 2026-08-25). Supersedes §7.4b's second row.

Verbatim: *"Я хочу чтобы в zip лежал сам мод с нужной иерархией под r2modman и pak тоже там был под
иерархию r2modman уже в готовом правильном месте лежал. Кто захочет установить не для r2modman, а
вручную для ue4ss без shimloader то сам разберется и закинет куда надо файлы из нашего собранного zip
release. Можем подсказать в install.md и в других местах как устанавливать для тех кто на что ставит."*

**What changed.** §7.4b (2026-08-23) specified **two** artifacts: the Thunderstore package, and a
separate GitHub `*_release.zip` whose internal tree was *already the game-folder hierarchy*, so that
a hand-installer could unpack it over their install with no decisions to make — the hierarchy WAS the
instruction. **That second artifact is now retired before it was ever built.** There is ONE zip, in
the §7.2a Thunderstore/r2modman layout, and both lanes take the same file:

```
manifest.json   icon.png   README.md   [CHANGELOG.md]
mod\enabled.txt
mod\dlls\main.dll
pak\scientists.pak      (+ the preview tiles the F1 browser reads)
```

**What was traded, stated once so nobody re-derives §7.4b's rationale as if it still stood:** the
manual lane loses "no decisions to make" and gains a relocation step it must be *told* about. The
user accepted that explicitly and asked for it to be documented — so the mapping below is not a
nice-to-have, it is the thing that replaces the property we gave up. It belongs in `docs/INSTALL.md`
(the single owner of install prose, §4i), in the release-body template, and on the site.

**The mapping, for INSTALL.md's manual/UE4SS-without-shimloader lane:**

| in the zip | goes to | note |
|---|---|---|
| `mod\dlls\main.dll` + `mod\enabled.txt` | `<Game>\VotV\Binaries\Win64\Mods\Multivoid\` | i.e. **the CONTENTS of `mod\`**, into a folder you name `Multivoid` |
| `pak\*` | `<Game>\VotV\Content\Paks\LogicMods\Multivoid\` | any subfolder name works once §7.7 lands |
| `manifest.json`, `icon.png`, `README.md` | nowhere — Thunderstore metadata, inert in a manual install | harmless if copied |

**Three measured facts that make this work, and one that makes it fail:**

- `[V]` **Nothing in our code depends on the mod folder's name.** `ue_wrap/core/paths.h` is the sole
  owner of the install anchor and deliberately anchors every runtime artifact (`multivoid.log`,
  `multivoid.ini`, `multivoid-players.txt`, …) on the **game EXE directory**, not the module's — and
  a grep for `Mods\` / `Mods/` across `src/votv-coop/` returns **zero** hits. So the manual installer
  may call the folder anything; the only cost of copying `mod\` verbatim is UE4SS listing a mod
  named "mod".
- `[V]` The Thunderstore root files are inert. UE4SS reads `Mods/<name>/enabled.txt` and
  `Mods/<name>/dlls/main.dll` and ignores everything else in the folder.
- **THE FAILURE MODE TO DOCUMENT AGAINST, because it is the one a careful person makes:** copying the
  *whole zip* into `Mods\Multivoid\` yields `Mods\Multivoid\mod\dlls\main.dll`, which UE4SS does not
  load — same silent non-load as §7.2a trap 1, one level out. The instruction must say **contents of
  `mod\`**, not `mod\`.
- **§7.7 is a precondition here too.** The pak arrives under a subfolder whose name differs between
  lanes (`<Author>-Multivoid` from r2modman, whatever the human typed manually), and
  `skin_registry.cpp:114` hardcodes `LogicMods/multivoid`. Until the scan walks `LogicMods/`
  subdirectories, **both** lanes ship a pak the game cannot see.

**Consequences for the release machinery**, all in the same welded commit as §7.3a item 4:
`publish.ps1:24-28`'s assertion inverts from *exactly one `multivoid-*.dll` + one `xinput1_3.dll`* to
**exactly one `.zip`**, and — because a human now builds it (§7.9 candidate (c)) — it must verify the
zip's **internal tree**, not just its name: an empty or mis-rooted zip is a silently broken release,
and hand assembly is precisely the step that produces one. `ledger_lib.ps1:149-151`'s verbatim
INSTALL anchors and `ledger_lint.ps1:64-77`'s checks are proxy-lane text and must be re-minted
against the new INSTALL.md in that same commit or CI fails on the spot.

### 7.6 The pak's CONTENT — DECIDED (USER 2026-08-23): the HL skins ship

**USER DECISION:** the HL scientist skins ship with the mod. Rationale (user's): character-swap mods
built on assets from other games are ubiquitous across Steam Workshop and the modding scene at large,
and Valve has never been aggressive about its own assets in that context. **Recorded as settled — do
not re-litigate it.** One correction to how it was first put to the user: the three `.gitignore` rules
below are a *"what do we commit to a public git repo"* triage (binaries, heavy, regenerable), not a
considered decision about what to ship to players — they were presented as stronger than they are.

The residual risk is not legal but **availability**: Thunderstore is a third party with its own
content policy, and a takedown of the package would remove the *whole mod*, not just the skins.

> **SIZED BY MEASUREMENT 2026-08-25 (USER-RAISED) — it is smaller than this paragraph implies.** The
> live VOTV catalog (185 packages) carries cross-property asset replacement openly and
> un-deprecated: `Hirokhai-MinecraftBeehive`, `forder-Kerfur_Kurobara`, `Yojimo-Kerfuro_Snickers`,
> `AmariMakes-NSFW_Loona_3d_prints`. Third-party character assets are **accepted practice here**, not
> a live hazard. The one leg with no observed precedent is the cooked template extracted from VOTV's
> **own** paks, which the platform rule names explicitly. Full measurement + the two rule texts:
> `docs/THUNDERSTORE.md` §7 / §7a.

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
- **~~STILL OWED~~ HALF DONE 2026-08-26 (USER):** the Thunderstore **Team `Multivoid` EXISTS** —
  the user created it and said so verbatim (*"Команду я создал уже Multivoid"*). Two consequences to
  carry, neither reversible: the package identity is fixed at **`Multivoid-Multivoid`**, so §7.2a
  trap 4's pak path is `shimloader/pak/Multivoid-Multivoid/` and the `dependencies` string bakes the
  same pair; and changing either half later creates a SECOND package rather than updating this one
  (`THUNDERSTORE.md` §5). **Still owed:** the service-account API token §7.9 needs — and per the user
  2026-08-26 it is not needed yet (*"пока не обязательно грузить, сначала проверки локальной
  установки zip и тд"*), so the automated-publish job (C3.5) is DEFERRED, not blocked.
- **PARTIAL 2026-08-26 — the MANUAL half of this control has now RUN; the managed half has not.**
  `[V]` The zip was assembled by the real generator, extracted, and hand-installed into
  `Game_0.9.0n_HOST/…/Mods/Multivoid/` after wiping that folder entirely — then booted with
  `mp.py gracefulexit --no-deploy`, i.e. with nothing re-deploying over it: `boot: Multivoid 0.9.0n
  b143`, `entry=cppmod` at 49 ms, both predecessor legs clean, `POLYHOOK-COMPOSED`, 0 `[Error]`,
  graceful exit 3.9 s with the full teardown trail. That is rule B's *"ставится вручную"* half,
  evidenced. **The r2modman half is still owed and is BOUNDED BY A STRUCTURAL LIMIT, not just by
  effort:** `[V]` `THUNDERSTORE.md` §3 — `author` is not a manifest field and the namespace comes
  from the Team **at upload time**, so a LOCAL import has no Team and cannot produce the
  `<Team>-<Name>/` folder. A local-import control therefore CANNOT observe trap 4; that assertion
  belongs to the first real managed install. `[V]` And reproducing the managed RUNTIME lane means
  mutating a rig copy — this box's game folders carry UE4SS's own 58,368-byte `dwmapi.dll`, not the
  profile's 700,488-byte shimloader. Original text follows.
- **NEWLY OWED 2026-08-25 — the negative control §7.2a could not run.** Everything in §7.2a about
  where files land is `[V]` from three independent sources (the live ecosystem schema, r2modman's
  rule engine + its own test spec, and five real packages measured against this box's profile) — but
  **our own package has never been installed by a manager**, and trap 1 ("a root-level `dlls/`
  silently never loads") is read off the rule engine, not observed. r2modman can install a **local
  zip** ("Import local mod"), so the whole thing is testable before anything is published: build a
  candidate package from the current DLL + `assets/branding/icon.png`, import it, and diff the
  resulting profile tree against §7.2a's prediction. **Do this before the first Thunderstore upload,
  not after** — a wrong layout is invisible until a player reports the mod doing nothing. Note it
  touches the shared r2modman profile, so it needs a window when no other session is using it.

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
packaging script exists at all.

> **HALF OF THIS IS NOW STALE — BUILT 2026-08-26 (`2a223362`, audited + hardened in `3dd546dd`).**
> `tools/release/package.ps1` assembles the §7.2a zip and `ledger_lib.ps1` gained the five packaging
> functions; the tracked count is now **2**, not 0. Identity is READ from the tree
> (`Get-GameTargetFromCMake` + a new `Get-ProtoFromWorktree`), both throwing labeled `UNREADABLE`
> rather than defaulting, so §7.3's generated-never-typed requirement is satisfied structurally.
> `Test-PackageZip` is the fail-closed tree check §7.4b demanded, and it lives in `ledger_lib.ps1`
> precisely so `publish.ps1` can re-run the identical predicate on the artifact it downloads back.
> **What is still true in the paragraph above is the `publish.ps1` half** — it still asserts two loose
> DLLs and still never calls `Test-PackageZip`. That inversion is C3.3, §6 step 4's welded commit.
> Drill: `tools/release/package_drill.ps1`, **14 arms, all pass** (1 GREEN + 13 RED, one per trap).

Meanwhile
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

   > **USER DIRECTION 2026-08-25, and it MOVED within the day — read both, the second supersedes:**
   >
   > **First: `scientists.pak` goes IN THE PUBLIC REPO** ("скорее всего") — candidate (a). It is the
   > mod's default skin pack and ships with the mod, so it is a build input like any other, and
   > committing it is what makes the whole publish hands-off.
   >
   > **Then, on being shown that a public blob costs its size × every future recook, forever:
   > "ну мы можем вручную тогда релиз собирать и zip релиза будет содержать мод и пак"** — i.e.
   > candidate **(c)**, assemble by hand, pak never enters git. **This supersedes (a).**
   >
   > **The earlier line calling (c) "dead" was wrong and is retracted.** It read the choice as
   > all-or-nothing: manual *assembly* versus automated *everything*. Those are separable, and only
   > one of the two seams actually matters (below). (c) costs one human step per release in a ritual
   > that is already human-driven end to end (`docs/RELEASE.md`: local build + LAN smoke gate,
   > authoring `notes/b<N>.md`, tagging, the ledger bump, the push, the published row).

   **What (c) does and does not cost, measured — this is the part worth not re-deriving:**
   - **Nothing structural is lost or forked.** The zip's internal tree is identical whether a human
     or a runner writes it. Starting manual is a *sequencing* choice, not an architecture choice, and
     (a) or (b) stay available later at the cost of the packaging step alone.
   - **THE ONE SEAM THAT MUST NOT GO MANUAL: which DLL bytes go in the zip.** The whole
     `judge.ps1` → cacheless rebuild from the tag → `publish.ps1` download-back-and-sha256-verify
     chain exists for exactly one guarantee — *the published bytes are the tagged source*. A hand-
     assembled zip that scoops a DLL out of a local `build/` directory silently voids it, and this
     project has already shipped wrong bytes from precisely that shape of mistake (`deploy-mod.ps1`
     picking its payload by mtime, which put a broken b137 into four installs). **The rule to write
     into RELEASE.md: the DLL in a hand-made zip is DOWNLOADED FROM THE CI ARTIFACT of the tagged
     run, never taken from a local build.** The pak, by contrast, is fine to add by hand — it has no
     equivalent provenance chain and cannot be rebuilt by CI anyway.
   - **It is TWO zips by hand, not one.** §7.4b already separates them and warns that conflating
     them is the obvious trap: the **Thunderstore** package is `manifest.json` + `icon.png` +
     `README.md` + `mod/` + `pak/` (§7.2a), while the **GitHub** `*_release.zip` is the game-folder
     tree a hand-installer unpacks over their install
     (`VotV/Binaries/Win64/Mods/Multivoid/dlls/main.dll` + `VotV/Content/Paks/LogicMods/...`).
     Same two payloads, two different layouts, and a human doing this twice per release is exactly
     where a mis-rooted zip gets shipped — so `publish.ps1`'s asset assertion should still verify the
     zip's internal tree even when a human built it.

   **What (a) WOULD require — SUPERSEDED by (c) the same day, kept because the measurements stand
   and (a) is the fallback if the hand-assembly step ever proves too error-prone.** Under (c) none of
   this applies: the pak stays exactly where it is, in the ignored trees, and never enters git.
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

## 8. What happens to the GitHub workflow (USER-RAISED 2026-08-26)

The user asked directly: *"Что будет со старым github workflow тоже в ue4ss арку надо задокументить."*
It is a fair question and the arc did not answer it anywhere — §7.4c decided the ARTIFACT, §7.9
decided that CI CAN produce it, and nothing said what happens to the four YAML files in between.
Measured 2026-08-26.

### 8.1 What the chain IS today `[V]`

| file | role | trigger |
|---|---|---|
| `release-trampoline.yml` | the only entry point | `push: tags: v*` |
| `release-core.yml` | reusable; judge → cacheless rebuild → publish | `workflow_call` |
| `build-core.yml` | reusable; compile + collect + upload artifact | `workflow_call` |
| `build.yml` | manual wrapper for a cacheless build smoke | `workflow_dispatch` |

`[V]` `build-core.yml:208` is a plain `cmake --build --config Release` — **it never names a target**,
so it builds whatever `CMakeLists.txt` declares. `[V]` `:216` then collects `Release/*.dll` by GLOB,
which today sweeps up **both** the payload and `xinput1_3.dll` and uploads them as one artifact.

### 8.2 What commit 3 does to it — and the important part is what does NOT break

**The two workflow files need no change at all, and that is a measured claim, not an assumption.**
`[V]` Nothing in `build-core.yml` names the proxy: the build step names no target and the collect
step is a glob. Delete `add_library(xinput1_3)` and the same YAML produces a one-DLL artifact with no
edit. The `if ($dlls.Count -eq 0) { throw 'no DLLs produced' }` guard still holds.

**What breaks is entirely in the PowerShell the workflows call** — which is why round 1's census hole
mattered and why "the workflow" is the wrong unit to reason about. **(EXECUTED 2026-08-28: every row
below landed at C3.3 `d693609b` — publish assembles + uploads the ONE zip, the body writer is
era-aware by sha-map data, the anchors re-shaped, the fixtures cover both eras. The table stays as
the record of what the weld had to move.)**

| site | today | after commit 3 |
|---|---|---|
| `publish.ps1:25,27` | `throw "expected xinput1_3.dll in $ArtifactDir"` | **HARD FAILURE.** The release cannot be cut at all. |
| `publish.ps1:24,26,35` | demands exactly one `multivoid-<game>-<N>.dll` | becomes a demand for **one zip**, per §7.4c |
| `ledger_lib.ps1:231` | *"You need **both** files below … + `xinput1_3.dll` (the loader)"* in EVERY release body | a false sentence in every future release |
| `ledger_lib.ps1:149` | `$InstallFolderAnchor = 'WindowsNoEditor\VotV\Binaries\Win64'` | **has no true value** — two lanes, two paths (§8.4) |
| `ledger_lint.ps1:64-65` | asserts both anchors appear VERBATIM in `docs/INSTALL.md` | keeps CI **GREEN over the false body**, because it checks that two documents AGREE, never that either is TRUE |
| `tag_regex_selftest.ps1:58` | fixture map contains `xinput1_3.dll` | stale fixture |

### 8.3 What it BECOMES

§7.4c already decided the output: **ONE zip**, in the §7.2a r2modman layout, taken by both lanes. So
the release lane's shape changes from *"upload two loose DLLs"* to *"assemble one zip, upload it"*.

`[V]` **The precedent is already vendored in our own tree, written by the people whose loader we
target:** `reference/unreal-shimloader/.github/workflows/release.yml:79-97` stages `icon.png` +
`README.md` + the payload, heredocs `manifest.json` **with the version interpolated** (which is
exactly §7.3's "generated, never hand-edited" requirement, satisfied structurally), runs
`7z a -tzip`, and calls `gh release create` with the zip. Entirely on `windows-latest`, no
maintainer machine. We do not need to invent this step.

**The one genuinely open input is the `.pak`** (§7.9 item 1): `[V]` zero `.pak` files are tracked and
`.gitignore:6` excludes them, so a zip assembled on the runner cannot contain one today. Note the
scope precisely — this is about how the bytes REACH the runner, not about whether CI can zip them.
And per the user 2026-08-26 the bundled **`scientists.pak` is a DEBT** and out of this pass, which
means the first zip can ship without it and the pak lane rejoins later.

### 8.4 The `$InstallFolderAnchor` gate does not survive, and re-typing it is not the fix

This is round 2's residual and §8.2's fifth row, and it is the one place where the honest answer is
that we do not yet have the replacement. After the weld there are two install lanes with two
different destinations — `Binaries\Win64\Mods\Multivoid\` for the manual lane, and whatever
r2modman's shimloader VFS decides for the managed one — so **a single anchor string has no true
value**, and updating it to a new literal reproduces the same defect one release later.

The defect is the gate's SHAPE: it certifies that the release body and `INSTALL.md` agree. Two
documents can agree perfectly and both be wrong, which is exactly what would ship the day after
commit 3. §7.4b specifies a zip-tree fail-closed check as the replacement — ~~but that is a PLAN,
not code~~ **ANSWERED at C3.3 (`d693609b`, 2026-08-28): `publish.ps1` now runs `Test-PackageZip`
on the finished zip with the exact payload sha, plus a THREE-leg identity agreement (tag ==
artifact VERSIONINFO == tree) before assembly. The anchor was re-SHAPED, not re-typed: it is now
explicitly the MANUAL lane's mod-folder destination (`...\Binaries\Win64\Mods\Multivoid`) — a
path with a true value — while the managed lane's path belongs to r2modman's VFS and is typed
nowhere. The truth of the ARTIFACT is certified by the zip-tree check; the anchor gate's residual
job is only that the body and INSTALL.md name the same manual path.**

### 8.5 `wire-d` / `wire-e` — cited as live, and they do not exist

`[V]` `UE4SS_ARC.md:667` refers to "tripwire wire-e" as if it were a live runtime watch.
`[V]` `VERSION_MIGRATION.md:473` says wire-d (the C loading contract) and wire-e (the safety
premises) **"remain OWED at WP-6"**. Both statements are in this project's own docs and they
contradict each other. Marked here as DEBT rather than quietly built, and the `:667` citation is the
kind of sentence a future session will read as evidence.

---

Related: `[[project-wp2-realistic-env-test-2026-08-22]]`,
`[[project-wp2-precut-and-trampoline-crash-2026-08-22]]`,
`[[project-f2-ue4ss-switch-decision-2026-08-21]]`,
`[[lesson-veh-crash-reporter-preempts-our-seh-guard]]`,
`[[lesson-double-detour-crash-is-config-dependent-needs-pe-callback-arm]]`.
