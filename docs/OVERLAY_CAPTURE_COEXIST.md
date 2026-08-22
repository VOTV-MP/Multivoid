# Overlay coexistence — RTSS / MSI Afterburner / OBS capture (SOLE live-tracked doc)

**This is the single owner document for: "why is the Multivoid ImGui overlay invisible under
RivaTuner (RTSS/MSI Afterburner OSD), and why can't OBS capture it — and what we do about it."**
Any finding, decision, or status on overlay-vs-third-party-hooker coexistence lives HERE. If another
doc touches the subject, it links here; it does not re-state.

- **Status (2026-08-23):** design converged for DX11 (7-round /qf), then **materially REVISED by
  measurement on 2026-08-23** — DX12 moved from "Phase 2, needs a rig" to **in scope for v1 with zero
  hooks** (§6c). **Increment 1 is BUILT and now LIVE-VERIFIED**; the seam-move itself is NOT built.
  - **AS-BUILT + LIVE-VERIFIED (deployed, solo boot, NOT hands-on RTSS/OBS):** the AOB
    `kSigD3D11ViewportPresentChecked` + `kD3D11Viewport_SwapChain = 0x70` in `sdk_profile.h`, and a
    **log-only** boot resolve-probe in `ui/imgui_overlay.cpp::Init`. **Verify-before-retire PASSED on a
    running game** (2026-08-23 22:36/22:40, DLL `335AC774544E17AB`): the seam resolved at
    `image+0x16F4BA0` — exactly the IDA-predicted address — and a live byte read at that address
    returned the signature's bytes UNPATCHED (§6c).
  - **NOT BUILT:** the seam-move itself (hook `PresentChecked`/`PresentInternal`, draw there, retire
    the swapchain-`Present` + `ResizeBuffers` + `ExecuteCommandLists` hooks, RTV-on-`GetBuffer(0)`
    change, fail-closed path). **The live overlay is UNCHANGED — S1 and S2 are both still present.**
  - The design is §9 (DX11) + §9b (DX12); the IDA/live measurements backing it are §6b + §6c.
    Acceptance is a hands-on RTSS+OBS screenshot — nothing here is VERIFIED until that runs.
- **NOTE — this box does NOT currently reproduce S1.** [MEASURED 2026-08-23 + USER-CONFIRMED] the user
  set RTSS's detection level to **None globally**, so `RTSSHooks64.dll` still injects (it is loaded at
  `0x180000000`) but hooks nothing. An earlier revision of this doc claimed "this box can reproduce
  both" — that was **wrong** and is retracted. **The acceptance test therefore requires the user to
  re-enable RTSS detection first**, or it is vacuous (see `[[lesson-an-instrument-blind-to-the-
  phenomenon-always-passes]]`).
- **HEAD at authoring:** `01a43486` · proto 134 (a render-path change; **no wire format touched → no
  proto bump** expected).
- **Owner subtree:** `src/votv-coop/src/ui/` (the overlay), `ue_wrap/core/hook.*` (the detour engine),
  `ue_wrap/core/sdk_profile.h` (AOB signatures: **6 shipped**, 7 after the DX12 seam lands).
- **Provenance tags** (used throughout, per OPUS §2 / verify-handed-down-measurements): `[SRC]` read
  from a component's source; `[AUTH]` the component author's own statement; `[COMM]` community-reported;
  `[MEASURED]` measured on THIS box / in THIS repo this session; `[DERIV]` derived by reasoning from
  tagged facts; `[?]` asserted, not yet verified.

---

## 1. The two symptoms (user's words, 2026-08-22)

> "Our mods imgui is not visible when users have riva tuner overlay (fps, cpu core etc) enabled, and
> also obs can't capture our imgui properly."

Two distinct user-visible failures:
- **S1 — invisible under RTSS:** with the RivaTuner Statistics Server OSD enabled (the FPS/CPU overlay
  that ships with MSI Afterburner), our ImGui overlay does not appear at all.
- **S2 — not captured by OBS:** OBS game-capture records the game but our ImGui is missing from the
  capture (the player sees it on their own screen, the stream/recording does not).

The user posed two solution options and asked for RE → doc → plan → /qf → implement, per RULE 1:
- **Option 1 (user's favorite, "but not sure"):** make ImGui stop fighting RTSS/Afterburner and be
  captured by OBS in all modes.
- **Option 2:** drop ImGui entirely, move the whole overlay to native UE widgets.

**This box can reproduce both:** [MEASURED] RTSS (pid 29516) + MSIAfterburner (pid 11036) are running;
OBS Studio is installed at `C:\Program Files\obs-studio`. RTSS `Profiles\Global` has `EnableOSD=1`.

---

## 2. How our overlay draws today (the thing both symptoms are ABOUT)

[SRC, `src/votv-coop/src/ui/imgui_overlay.cpp`] We use the textbook "ImGui over a DXGI swapchain" technique:

1. `Init()` (`imgui_overlay.cpp:614`) spins up a throwaway DX11 device+swapchain on a hidden window
   ONLY to read the `IDXGISwapChain` vtable, then **MinHook inline-detours `Present` (vtbl[8]) and
   `ResizeBuffers` (vtbl[13])** (`:597-624`). The vtable is shared by every swapchain, so hooking the
   dummy hooks the game's real one.
2. `PresentDetour` (`:503`) runs each frame; on the first real present it brings up ImGui + the RHI
   backend, then **draws our UI into the backbuffer and AFTER that calls the real present**:
   `RenderFrameGuarded(sc)` at `:540`, then `return g_origPresent(sc, sync, flags)` at `:543`.
3. Both RHIs draw: DX11 (`overlay_backend_dx11.cpp`) and DX12 (`overlay_backend_dx12.cpp` +
   `_dx12_capture.cpp`, which hooks `ID3D12CommandQueue::ExecuteCommandLists` to find the presenting
   queue). Selected at first present.

**The one architectural fact that causes both symptoms:** our draw is an **inline prologue patch ON
`IDXGISwapChain::Present`**, and it runs INSIDE that detour. That is exactly the seam RTSS defends and
exactly the point OBS captures around. Everything below follows from this.

---

## 3. RTSS — why S1 (invisible) happens

[AUTH — Unwinder / Alexey Nicolaychuk, RTSS author, guru3d dev-news thread #412822 p.217 + OBS forum
thread #149496; [COMM] for the failure fingerprint]:

- RTSS injects **system-wide and early** (RTSSHooks64.dll enters every process; per-profile it decides
  whether to hook the 3D API). [MEASURED] our box's `RTSS\Profiles\Config` shows RTSS resolving
  `IDXGISwapChain::Present`, `Present1`, `ResizeBuffers`, and `ID3D12CommandQueue::ExecuteCommandLists`
  by DLL-relative offset — i.e. it targets exactly the functions we (and OBS) target.
- RTSS's hook engine does NOT overwrite the function prologue the way MinHook/Detours do. [AUTH,
  verbatim] it *"unwind[s] the whole JMP chain and inject[s] the hook in the body of the function after
  the very last jump ... you NEVER overwrite a jump previously installed by any other hook"* — so RTSS
  deliberately places itself LAST in the chain, and a hook installed BEFORE it is chained-after safely.
- **The killer: hook-integrity control.** [AUTH] RTSS runtime-disassembles to verify its hook is still
  in the chain and **restores its own bytes** if a later hooker overwrites the region — Unwinder:
  *"it will effectively kick [a Detours hook] out of hook chain, if you inject it after RTSS."*
- **[DERIV, high]** Our MinHook prologue patch on `IDXGISwapChain::Present` lands AFTER RTSS has already
  hooked (RTSS is early/system-wide; we hook only once we can read a live swapchain vtable). MinHook
  overwrites the prologue region RTSS's integrity check watches → RTSS restores it → **our detour is
  silently unlinked; ImGui draws once or never, then never again.**
- **[COMM] exact fingerprint match:** hudhook #196 ("RivaTuner overlay conflict"): *"The Present hook
  ... is only called once ... but ImGui never shows up."* Same class across kiero/UniversalHookX ImGui
  overlays. Community "fix" is per-app RTSS detection-level = None — **not acceptable to us** (we can't
  require users to configure RTSS).

**S1 root:** we own a competing inline patch on the exact function RTSS defends; RTSS wins by design.

---

## 4. OBS game capture — why S2 (not captured) happens

[SRC, obs-studio `plugins/win-capture/graphics-hook/dxgi-capture.cpp`, `game-capture.c`]:

- OBS's graphics-hook uses **Microsoft Detours** (inline prologue patch) on `Present`/`Present1`/
  `ResizeBuffers`, injected **at capture start** — i.e. after game boot, normally after our hook. So
  OBS's detour is the OUTERMOST hook; our detour runs inside OBS's call to its trampoline.
- **The capture point depends on the "Capture third-party overlays" flag** (`capture_overlay`,
  default OFF):
  - **OFF (default):** OBS copies the backbuffer at the TOP of its Present detour, **before** calling
    the real present → **before our draw for that frame** → our overlay is NOT in the copy.
  - **ON:** OBS moves the copy to **after** the real present returns; by then our detour (running inside
    RealPresent) has drawn → our overlay IS captured (at the cost of flip-model one-frame staleness).
- [DERIV, high] So with default OBS settings our overlay is missing; the fix by config would be to tell
  users to enable "Capture third-party overlays" — again **not acceptable** (config dependence).

**S2 root:** our draw happens below OBS's default capture point (inside RealPresent, after OBS already
copied the backbuffer).

---

## 5. The precedents — how everyone who solved this solved it

- **MTA:SA** [SRC, `reference/mtasa-blue/Client/core/DXHook/`]: draws its GUI inside
  `CProxyDirect3DDevice9::Present` (`CProxyDirect3DDevice9.cpp:1064` OnPresent) **BEFORE** the real
  present (`:1074` PresentGuarded). Crucially it reaches that seam via a **COM proxy object** (returned
  from a Detours hook of the `Direct3DCreate9` export, `CDirect3DHook9.cpp:34`), NOT an inline patch on
  Present — so MTA is upstream of any inline Present hooker by construction. MTA even has explicit
  RTSS-coexistence code: `CreateDeviceSecondCallCheck` (`CProxyDirect3D9.cpp:985-1028`) passes RTSS's
  own second `CreateDevice` straight through un-proxied, and `FilterException`
  (`CDirect3DEvents9.cpp:1427`) swallows "corrupted vtable, overlay hooks" faults. **Its screenshots
  read the backbuffer before the real present** (`CScreenShot.cpp:186`) → MTA's own capture is
  deterministic for the same reason OBS's would be: the composed frame exists before Present.
- **ReShade** [SRC, `crosire/reshade source/dxgi/dxgi_swapchain.cpp`]: returns the game a **wrapper
  swapchain object** (`_orig->Present` inside its own `Present`), hooking only at swapchain CREATION.
  RTSS's integrity check never sees it (no prologue to stomp); OBS captures it with the option OFF
  (drawn upstream of every inline hook). This is the canonical clean escape.
- **Special-K** [AUTH/COMM]: the most aggressive interop engineer in this space treats "RTSS hooked
  first + I inline-hook after" as **unwinnable**, and either hooks EARLIER (CBT hooks to be first) or
  drops RTSS support. Confirms: you don't out-fight RTSS on the same seam; you get off the seam.

**Common thread:** everyone who is visible under RTSS and captured by OBS draws **UPSTREAM of the
inline Present-hook chain** — inside the game's own frame, before `IDXGISwapChain::Present` is called.

---

## 6. The root cause, stated once

Both symptoms are the SAME defect: **we draw from an inline hook ON `IDXGISwapChain::Present`.** That
seam is (a) the exact byte region RTSS defends and restores (→ S1), and (b) below OBS's default capture
point (→ S2). The RULE-1 root fix is not to fight RTSS or to ask users to reconfigure OBS — it is to
**move our draw one level UP, into the engine's own present path, before the swapchain Present call**, so
we are upstream of the entire external inline-hook chain (exactly where the engine's own UI, ReShade,
and MTA draw). Then RTSS has nothing of ours to unlink (we no longer patch the function it defends), and
OBS captures us by default (our pixels are in the backbuffer when the game calls Present).

---

## 6b. Measured facts (IDA headless on the shipping exe, 2026-08-22)

All measured against `VotV-Win64-Shipping.exe.i64` (the shipping exe, 84,751,360 bytes =
`kExpectedExeSize`), IDA Professional 9.2 headless. Scripts in the session scratchpad
(`rtss_probe/ida_*.py`); this is the fact base the converged design (§9) rests on.

- **`[MEASURED]` the D3D11 present path is DISCRETE functions, not an inlined monolith.** UE's own
  `__FILE__` strings anchor them: `D3D11Viewport.cpp` (`0x143CA01D0`), `WindowsD3D11Viewport.cpp`
  (`0x143CA14E0`), and the D3D12 twins. The device-lost HRESULTs (0x887A0005/6/7) resolve to the
  discrete `VerifyD3D11Result` family + `GetD3D11ErrorString` + the device-lost terminator — separate
  bodies, refuting "it might all be inlined into one Present blob."
- **`[MEASURED]` the D3D11 swapchain-present call graph (the choke-point census):**
  - `FD3D11Viewport::Present` = **`sub_1416F4A00`** (does the fullscreen reconcile, then branches).
  - `FD3D11Viewport::PresentWithVsyncDWM` = **`sub_1416F4D50`** (the DWM-vsync path).
  - `FD3D11Viewport::PresentChecked` = **`sub_1416F4BA0`** — reads the swapchain at **`viewport+0x70`**
    and calls **`IDXGISwapChain::Present` at vtbl[8]** (`vtbl+64`), then retry / `VerifyD3D11Result`.
  - **Census result:** `PresentChecked`'s only callers are `Present` (normal path) and
    `PresentWithVsyncDWM` (which itself calls `PresentChecked` at its end). So **both present sub-paths
    funnel through `PresentChecked` exactly once per frame — it is THE single, complete choke point;
    no present bypasses it.**
- **`[MEASURED]` `sub_1416F4BA0`'s 24-byte prologue is UNIQUE in the image** (occurrences = 1), so it is
  a derivable, unique AOB signature (rip-relative displacement wildcarded, exactly like the 5 existing
  signatures). `Present` (`sub_1416F4A00`) is also unique (occ=1) if a higher point is ever wanted;
  `RHIEndDrawingViewport` (`sub_1416FD160`) prologue is NOT unique (occ=5).
- **`[MEASURED]` this box presents with DX11** — `multivoid.log` reads `DX11 bring-up OK` across every
  session (14:25 / 14:56 / 19:34). VOTV defaults to DX11 (SM5); DX12 is opt-in via VOTV's own
  in-game `setting_rhi` / `setting_rhiTest` widgets.
- **`[MEASURED]` the D3D12 present** lives in `WindowsD3D12Viewport.cpp` (`sub_14177D800` /
  `sub_14177E8B0` / `sub_141770420`); those bodies manipulate the D3D12 device/queue/command-list
  structs directly. Not exercised at runtime (box is DX11) — see the Phase-2 caveat in §9.
- **`[MEASURED]` adding one AOB-resolved native hook is routine here:** `sig_scan.cpp:59` FindPattern,
  the 5-signature table at `sdk_profile.h:38-93`, `hook.cpp` Install; the exact precedent is
  `save_block.cpp:131-156` (FindPattern → hook::Install, stale-signature fail-soft).

---

## 6c. Measured 2026-08-23 — the live present-chain census + the DX12 answer

Two measurement passes ran this session: headless IDA on the shipping `.i64`, and a **live solo boot**
(DLL `335AC774544E17AB`, `mp.py menushot` + `mp.py host`, DX11, RTSS detection = None) probed with
**`tools/debug/present_hook_census.py`** (`census` + `follow` modes; promoted out of the scratchpad so
these citations survive — the AOB tool beside it was promoted for the same reason).

### (a) The DX11 seam, confirmed on a RUNNING process — verify-before-retire PASSED

- `[MEASURED]` the boot probe logged `FD3D11Viewport::PresentChecked draw-seam resolved
  @00007FF7D0604BA0 (image+0x16F4BA0)` — **the exact address IDA predicted.** The signature is not a
  static artefact; it resolves on the real exe.
- `[MEASURED]` a live byte read at that address returned
  `48 89 5C 24 18 55 56 57 48 81 EC B0 00 00 00 48` — **the signature's own bytes, UNPATCHED.**
  Nobody else is hooking the seam we intend to draw from. (Caveat: RTSS was at detection None for this
  run, so this measures "no one *else* today", and must be re-read with RTSS armed.)
- `[MEASURED]` re-decompiling `sub_1416F4BA0` confirms the swapchain read is `*(this+112)` = **`+0x70`**
  and the present call is `vtbl[8]`. It also shows a **CustomPresent at `viewport+0xB0`** whose
  `NeedsNativePresent()` gates the present — VOTV has no VR so it is null, but the seam design must not
  assume the present always happens (§9).

### (b) WHO ELSE patches the present chain in our process — a third hooker nobody knew about

Every hooked DXGI entry was followed through its jmp chain to the owning module:

| function (offset from RTSS's own cache) | patch | jmp chain resolves to | owner |
|---|---|---|---|
| `IDXGISwapChain::Present` | `E9` rel32 | trampoline `0x7FF923800FCE` → `main.dll +0x30BC10` | **us** |
| `IDXGISwapChain::ResizeBuffers` | `E9` rel32 | trampoline `0x7FF923800F8E` → `main.dll +0x30C010` | **us** |
| `IDXGISwapChain1::Present1` | `E9` rel32 | trampoline `0x7FF9257B0E93` → **`NahimicOSD.dll +0x17670`** | **third party** |
| `ID3D12CommandQueue::ExecuteCommandLists` | — | `d3d12core.dll` NOT LOADED (DX11 run) | — |

- `[MEASURED]` **`RTSSHooks64.dll` IS loaded (base `0x180000000`) even at detection level None** — it
  injects system-wide unconditionally and only *decides* whether to hook the 3D API. This confirms the
  `[AUTH]` injection claim in §3 directly, on this box.
- `[MEASURED]` **`NahimicOSD.dll` (the A-Volute/Nahimic audio-driver overlay that ships with many
  MSI/ASUS/Dell audio stacks) inline-hooks `IDXGISwapChain1::Present1`.** We never knew it was there.
  It is a well-known cause of crashes and overlay conflicts in games, and it is a **new suspect for the
  open 19:17 exec-at-NULL crash in `docs/UE4SS_ARC.md`** (that dump was described as "a DXGI/CEF-shaped
  thread"). **This is a LEAD, not a diagnosis** — it is recorded here so the next dump symbolization
  starts with the right module list.
- **Design consequence:** the present chain is *more* crowded than the 7-round /qf assumed (us + RTSS
  when armed + OBS when capturing + Nahimic). That strengthens the root fix rather than weakening it —
  every one of those lives on the DXGI entry points, and none on the engine-private functions we are
  moving to.
- `[MEASURED]` RTSS's own offset cache contains `IDXGISwapChain1::m_pCommandQueue=00000118`, i.e.
  **RTSS gets the D3D12 presenting queue by reading it out of the DXGI swapchain object, not by hooking
  `ExecuteCommandLists`.** Independent precedent that struct-reading the queue is the normal shape —
  though we will read it from the ENGINE (version-pinned by our own migration process) rather than from
  an undocumented DXGI internal that Windows Update can move.

### (c) DX12: the seam AND the presenting queue, both measured — no rig needed to DECIDE

- `[MEASURED]` **`FD3D12Viewport::PresentInternal` = `sub_14177E0E0`** (image+0x177E0E0), 109 bytes,
  **exactly one caller** (`sub_141770420` = `FD3D12Viewport::Present`, which reaches it only after the
  `CustomPresent->NeedsNativePresent()` gate). It reads the swapchain at **`viewport+0x60`** and
  **tail-jumps** to `IDXGISwapChain::Present` at `vtbl[8]` (`48 FF 60 40`). It is the DX12 twin of the
  DX11 seam.
- `[MEASURED]` its AOB is **unique at 24 bytes with NO wildcards** (occ=1; 190 at 16 bytes). Shipping
  length 32 for margin:
  ```
  48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 33 DB 8B F2 48 8B F9 85 D2 75 10 38 59 54 75 0B 38
  ```
- `[MEASURED]` **the presenting `ID3D12CommandQueue` is reachable from the viewport by four constant
  dereferences, no hook at all:**
  ```
  viewport + 0x18   -> FD3D12Adapter*
  adapter  + 0x988  -> FD3D12Device*             (Devices[0])
  device   + 0x38   -> FD3D12CommandListManager* (queue type 0 = Direct)
  manager  + 0x28   -> ID3D12CommandQueue*       <- THE PRESENTING QUEUE
  ```
  This is not a heuristic: it is **the same pointer the engine itself passes as `pCommandQueue` to
  `CreateSwapChainForHwnd`**, proven by the VERIFY string in `sub_14177D800`
  (`WindowsD3D12Viewport.cpp:175`) whose argument is `*(sub_141720E30(Devices[0], 0) + 40)`.
  `sub_141720E30` decompiles to a 32-byte switch returning `dev[7]/dev[8]/dev[9]` for queue type
  0/1/2 — a plain offset, so no call is needed. Multi-GPU is safe by construction: the engine uses
  `Devices[0]` unconditionally at swapchain creation, so `Devices[0]`'s Direct queue *is* the presenting
  queue regardless of node count.
- **Consequence:** the ECL capture is no longer the only way to learn the queue, and the new way is
  **zero-ambiguity** (the ECL route was a statistical "last DIRECT submitter before Present wins", a
  600/600 measurement — strong, but still a heuristic). See §9b.

---

## 7. Candidate seams for "draw upstream of Present" (UE4.27)

> **WINNER (see §9): candidate #1, `FD3D11Viewport::PresentChecked` (`sub_1416F4BA0`).** The table below
> is the pre-measurement analysis; §6b measured the target and §9 is the converged decision.

[Engine-source knowledge, UE4.27; repo-infra facts cited. Full call path:
`FSlateRHIRenderer::DrawWindow_RenderThread` → RHI `EndDrawingViewport` → `FD3D11Viewport::Present` /
`FD3D12Viewport::Present` → `IDXGISwapChain::Present`.]

| # | Seam | How | Pros | Cons / risk |
|---|------|-----|------|-------------|
| **1** | **`FD3D11Viewport::Present` + `FD3D12Viewport::Present`** (the direct caller of swapchain Present) | AOB-resolve + MinHook; draw, then call original | Same thread + GPU-state context as today's detour → existing DX11/DX12 backends transfer nearly unchanged; RTSS/OBS hook the DXGI vtable BELOW us → they compose/capture us regardless of order (the goal) | +2 AOB signatures (surface 5→7; recompile-fragile); `PresentChecked`/`PresentInternal` may inline into `Present` (identity question — the return-address bootstrap below sidesteps it); DX12: draws before the frame's final present but command-list ordering needs care |
| 2 | `FD3D11DynamicRHI::RHIEndDrawingViewport` (+D3D12 twin) | AOB + MinHook | **Virtual → never inlined**, reliably a discrete body; one choke point per RHI | body also runs on `bPresent=false` (movie capture/resize) — detour must honor the flag; +2 AOB; one step further from the backbuffer |
| 3 | `FSlateRenderer::OnBackBufferReadyToPresent` delegate (exists in 4.27, fires render-thread before EndDrawingViewport) | register a hand-built 4.27 `TMulticastDelegate` binding, or hook `DrawWindow_RenderThread` | engine-sanctioned, RHI-agnostic (one registration), gives `SWindow&` + backbuffer ref | **no reflection route** (Slate isn't UObject — confirmed absent from the SDK dump); delegate ABI hand-built (crash-on-mistake, render thread); **on DX12 broadcast thread ≠ present thread, frame CLs may be unexecuted → raw ImGui draw races** |
| 4 | `FRHICustomPresent` (`Viewport->SetCustomPresent`) | build an `FRHICustomPresent` vtable, install on the live viewport | engine's DESIGNED present-interception point (VR uses it); no inline hook at all | need the live viewport pointer (heap-scan/bootstrap); 4.27 vtable ABI hand-built; clobbers other CustomPresent users (none in VOTV — no VR) |
| 5 | `AHUD::ReceiveDrawHUD` via reflection (PE-visible; `bShowHUD` at `sdk_profile.h:564`) | already-owned PE machinery, zero new natives | only reflection-reachable per-frame draw; asset-free | **wrong layer** — UCanvas API, not an ImGui surface; renders UNDER all UMG; cannot host our overlay |

[MEASURED, repo] Adding one AOB-resolved native hook is ROUTINE here: `sig_scan.cpp:59` FindPattern,
`sdk_profile.h:38-93` the 5-signature table, `hook.cpp` Install (+ followJmp-immune relay), and the
exact precedent `save_block.cpp:131-156` (FindPattern kSigSaveGameToSlot → hook::Install, stale-sig
fail-soft). **[MEASURED] the DX12 creation-probe already proved our boot does NOT precede the game's
swapchain creation** (`overlay_backend_internal.h:80-85`; the 2026-07-26 DX12 finding) — so a
ReShade-style creation-time wrapper (candidate not in the table) is UNAVAILABLE to us, the same reason
DX12 shipped ECL capture instead of a factory hook. That leaves the caller-side native seams (#1/#2).

**[DERIV] The return-address bootstrap (elegant cross-check, not the whole answer):** our EXISTING
Present detour can capture `_ReturnAddress()` on first fire → that lands inside `FD3D11Viewport::Present`
/`PresentChecked`; `RtlLookupFunctionEntry` gives the containing function base → the hook target with NO
signature, surviving recompiles. **Caveat that makes it insufficient alone:** in the RTSS case our
Present detour is unlinked and may never fire, so we can't rely on it to discover the target. We likely
need the AOB (IDA-derived) as primary and the bootstrap as a same-build ground-truth cross-check.

---

## 8. The two options the user posed

**Option 1 — make ImGui coexist + be captured (KEEP ImGui, move the draw seam).** This IS the root fix
in §6. Cost: re-point ~200 lines of hook/render glue from the swapchain-Present seam to a native
viewport-present seam (candidate #1/#2), derive 1–2 new AOB signatures (IDA lane + return-address
cross-check), and RULE-2-retire the old `IDXGISwapChain::Present` inline hook whole (no dual path). The
9,600 LOC of actual UI (`ui/*.cpp`) is UNTOUCHED — it just gets called from a different seam.

**Option 2 — native UE widgets (drop ImGui).** To be asset-free (principle 1 / anti-pattern A6 forbids
adding cooked UMG assets) we would have to construct UWidget trees at RUNTIME via reflection
(`CreateWidget` + reflected property sets) for chat, scoreboard, server browser, dev menu, voice panel,
HUD, loading screen — every surface. That is a multi-month rewrite that DELETES all of `ui/` (RULE 2),
throws away a mature feature-rich overlay, and — note — native UMG widgets ARE drawn by the engine's own
Slate pass, which is upstream of Present, so they WOULD coexist/capture. But option 2 buys the SAME
coexistence outcome as option 1 at 50× the cost and risk.

**Recommendation (DESIGN, pre-/qf): Option 1.** It is the user's preference, the RULE-1 root fix, and
by far the smaller change. Option 2 solves the same problem by demolishing and rebuilding a working
subsystem — that is migration-scale churn to reach an outcome option 1 reaches by moving where ~200
lines draw. Both draw upstream of Present in the end; option 1 gets there without discarding the UI.

---

## 9. The converged design (build-ready) — 7-round /qf, "that holds"

Chosen seam: **`FD3D11Viewport::PresentChecked` (`sub_1416F4BA0`)** — census-proven single once/frame
choke point (§6b), unique AOB. Draw there, before the original runs. Everything below is the D3D11
**Phase 1** (the box's RHI + the common case); DX12 is Phase 2 (caveat at the end).

**The change (all under `src/votv-coop/src/ui/` + `ue_wrap/core/`):**

1. **New AOB signature** `kSigD3D11ViewportPresentChecked` in `sdk_profile.h` (surface 5 → 6).
   **Measured, proven unique (occ=1) with the GS rip-displacement wildcarded** (48 bytes):
   ```
   48 89 5C 24 18 55 56 57 48 81 EC B0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33
   C4 48 89 84 24 A0 00 00 00 33 F6 89 54 24 30 48 8B D9 40 B5 01 48 8B 89
   ```
   Decoded: `mov [rsp+18],rbx; push rbp/rsi/rdi; sub rsp,0B0h; mov rax,[rip+GS]` (wildcarded) `;
   xor rax,rsp; mov [rsp+0A0h],rax; xor esi,esi; mov [rsp+30h],edx; mov rbx,rcx; mov bpl,1;
   mov rcx,[rcx+0B0h]` — the tail is all stable (register moves + struct-offset reads: `+0xB0` is
   the `*(this+176)` field the decompile reads); only the GS displacement is build-variable, hence
   wildcarded. Uniqueness measured: the wildcarded window is ambiguous (occ=2) at 24/32 bytes and
   becomes unique at 40; 48 is used for margin. `HealthCheck` exe-fingerprint WARN covers it like
   every other signature.
2. **Hook it** via `ue_wrap::hook::Install` (MinHook). The detour: on first call, read the swapchain at
   `this+0x70` and **validate it** via `QueryInterface(IID_IDXGISwapChain)` — success caches it and arms
   the overlay; failure (offset drifted) → **fail-CLOSED** (log + native Win32 `MessageBox`, overlay
   disabled — no draw into a garbage pointer). Each frame: bring up ImGui + the RHI backend lazily
   (as today), draw our existing surfaces into the backbuffer, then call the original PresentChecked.
   This is the exact ImGui pass that lives in `RenderFrameGuarded` today — only the *seam it hangs off*
   changes.
3. **Retire WHOLE (RULE 2)** the `IDXGISwapChain::Present` (vtbl[8]) **and** `ResizeBuffers` (vtbl[13])
   inline hooks + the dummy-swapchain vtable resolve that feeds them. Both are RTSS-defended; keeping
   either leaves an S1 surface. No dual draw path — the ImGui pass runs only at the new seam.
4. **RTV lifecycle without the ResizeBuffers hook:** at the draw seam, `GetBuffer(0)` and compare the
   backbuffer resource pointer to the cached one; recreate the render target on change. A resize leaves
   `viewport+0x70` (the swapchain) unchanged but replaces the backbuffers, and our seam runs *after* the
   engine rendered the new backbuffer, so `GetBuffer(0)` already returns the new one — no stale-RTV
   frame. Cost: one COM `AddRef`/`Release` + compare per frame (no allocation, no array walk — not a
   per-tick violation).
5. **Unchanged:** the `WndProc` hook (`SetWindowLongPtrW`) and the `user32!SetCursorPos` hook — neither
   is on an RTSS-defended function; input capture is unaffected by the seam move. The whole `ui/*.cpp`
   surface layer (~9,600 LOC) is untouched.

**Why this cures both symptoms (hook-order, measured mechanics §3/§4/§6b):** our draw now happens inside
`PresentChecked`, *before* it calls `SwapChain->Present`. RTSS defends `IDXGISwapChain::Present` (a
system-DLL entry it resolves by offset) — it has no knowledge of the private `PresentChecked`, so it
never touches our hook (S1 gone); RTSS's OSD still draws, on top, when `PresentChecked` calls the real
Present. OBS's default game-capture copies the backbuffer at the top of *its* `Present` detour — by then
our pixels are already in it (S2 gone). Thread is unchanged (`PresentChecked` is the immediate caller of
the swapchain Present we hook today). Layering is unchanged (frame-end seam, after `DrawWindow`
composited game + UMG + tonemap → we draw on top).

**Discovery + failure policy:** AOB-only (the return-address bootstrap was considered and DROPPED per
OPUS §1 — the AOB is the only discovery that works under RTSS, so it is mandatory and covers all cases;
a second mechanism for the already-covered no-RTSS case is not load-bearing). Fail-CLOSED on a stale AOB
or failed swapchain validation: overlay does not come up, a native `MessageBox` says so; **no fallback to
the old swapchain-Present draw** (that is RULE-2 baggage and would reintroduce S1/S2). This is the same
version-surface contract the existing 5 signatures carry (re-derived per `docs/VERSION_MIGRATION.md` on
a game recook).

**Two acceptance-gated inferences (named, not hidden):** that S1's *exact* mechanism is RTSS's
integrity-restore, and that RTSS leaves the private `PresentChecked` untouched, are `[inferred]` (strong:
RTSS author + hudhook #196 fingerprint + RTSS resolves the Present fns on this box) — both are measurable
ONLY by running the game under RTSS, which IS the acceptance gate. The fix is **class-robust**: drawing
upstream cures unlink / z-order / chain-clobber alike, so it does not hinge on which one S1 is. Optional
pre-build de-risk: a frame-counter in the *current* build under RTSS (if it stops incrementing → unlink
confirmed) — one user run.

**Verification (acceptance = VERIFIED gate, hands-on per OPUS §4):**
- **S1** — launch with RTSS OSD ON; our overlay (F1 menu / tilde scoreboard) is visible. (Auto-checkable
  too: a DWM window grab shows the overlay present; `capture_window.ps1` suffices for S1.)
- **S2** — OBS **game-capture** with **"Capture third-party overlays" OFF** shows our overlay in the
  capture. **This is provable only with a real Present-hooking capturer** — `capture_window.ps1`
  (PrintWindow/BitBlt = the DWM path) is BLIND to it and must not be used as the S2 gate.
- Screenshots per `[[feedback-show-screens]]`.

**DX12 — SUPERSEDED 2026-08-23, see §9b.** This paragraph originally phased DX12 to a later pass
("needs the presenting command queue... read the queue from the engine viewport struct (unmeasured —
needs a DX12 rig)"). That unmeasured option is now **measured** (§6c.c): the queue is four constant
offsets from the viewport. DX12 is in scope for v1 and needs no ECL hook at all. The paragraph is kept
only so the change of decision is visible; §9b is the design of record.

---

## 9b. DX12 — in scope for v1 (design of record, 2026-08-23)

Symmetric to §9, on the facts in §6c.c:

1. **New AOB** `kSigD3D12ViewportPresentInternal` (32 bytes, no wildcards, occ=1) + the offsets
   `kD3D12Viewport_SwapChain = 0x60`, `kD3D12Viewport_Adapter = 0x18`, `kD3D12Adapter_Devices = 0x988`,
   `kD3D12Device_DirectQueueMgr = 0x38`, `kD3D12CmdListMgr_Queue = 0x28`. Signature surface 6 → 7.
2. **Hook `FD3D12Viewport::PresentInternal`**; draw before calling the original, exactly as DX11.
3. **Get the presenting queue by walking the four offsets** — no `ExecuteCommandLists` hook, no tally,
   no confirmation window, no swapchain-creation probe.
4. **RULE 2 — `overlay_backend_dx12_capture.cpp` retires WHOLE** (~400 LOC): the ECL hook, the per-queue
   tally, the candidate/confirmation state machine, `Rearm()`, and the creation probe. Its public seam
   (`Device()` / `TryConfirmQueue()` / `InstallCreationProbe()` / `Rearm()` / `Shutdown()` in
   `overlay_backend_internal.h`) collapses to a single "resolve the queue from the viewport" call. No
   dual path: the ECL route does not survive behind a flag.
5. **Fail-CLOSED validation** before first use, SEH-guarded: the swapchain pointer must
   `QueryInterface(IID_IDXGISwapChain)`; the queue pointer must `QueryInterface(IID_ID3D12CommandQueue)`
   **and** report `GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT`. Any failure disables the overlay
   with a native `MessageBox` — it never draws through an unvalidated pointer.

**Why the ordering is correct (and better than today):** at `PresentInternal` the engine has already
submitted the frame's command lists to *that same queue*. A D3D12 command queue is serial, so our
submission after theirs is correctly ordered by construction — today's design submits to the same queue
from inside the `Present` detour and relies on the same property.

**What this buys beyond DX12 support:** after §9 + §9b, Multivoid owns **zero inline hooks on any
function RTSS, OBS, or Nahimic target** — not `Present`, not `Present1`, not `ResizeBuffers`, not
`ExecuteCommandLists`. Only two engine-private functions, which §6c.b measured nobody else touches.

**Residual, named:** the queue walk is five new struct offsets, i.e. new version-migration surface
(`docs/VERSION_MIGRATION.md`). It is fail-closed and QI-validated, so a drift disables the overlay
rather than corrupting a frame. And **a `-dx12` run is still owed to VERIFY** the walk yields a live,
QI-valid DIRECT queue — the rig is cheap (a `-dx12` launch flag) but it has not been run. The DECISION
no longer waits on it; the VERIFICATION does.

---

**Cross-link (NOT folded into this root):** `docs/UE4SS_ARC.md`'s open 19:17 real-env exec-at-NULL crash
is the same multi-hooker-on-Present class (our overlay hook + CEF/FusionFix). Retiring our
`IDXGISwapChain::Present` + `ResizeBuffers` inline patches *reduces* our Present-chain footprint, so it
can only help that crash — but this is a plausible side-effect to VERIFY when that dump is symbolized,
not a claimed cure. Keep the two investigations separate.

---

## 10a. CURRENT STATE + NEXT (2026-08-23)

**Where the arc stands right now:**

| item | state |
|---|---|
| Root cause (S1 + S2, one cause) | **MEASURED / settled** (§3, §4, §6, §6c.b) |
| DX11 seam chosen + AOB | **MEASURED, live-verified on a running game** (§6b, §6c.a) |
| DX12 seam + presenting queue | **MEASURED statically**; `-dx12` run owed to verify (§6c.c, §9b) |
| DX12 scope decision | **DECIDED: in v1** (was "Phase 2") — the deferral was not free, see below |
| The seam-move implementation | **NOT BUILT** — S1 and S2 are both still live |
| Acceptance (RTSS + OBS hands-on) | **NOT RUN**; blocked on the user re-enabling RTSS detection |

**Why DX12 stopped being deferrable.** §9 retires the `IDXGISwapChain::Present` + `ResizeBuffers`
hooks whole. If DX12 stayed on the old seam we would have to KEEP those hooks — a parallel old+new
path (RULE 2) that also keeps the RTSS-defended surface in the process, i.e. it would not actually
fix S1. Retiring them anyway would leave DX12 users with **no overlay at all**. So "Phase 2" was a
choice between a RULE-2 violation and a regression — and the measurement removed the need for either.

**NEXT, in order:**
1. **`/qf` on the revised design** (§9 + §9b). The 7-round convergence predates §6c; DX12-in-v1, a
   whole-TU retirement, and five new offsets are a material reframe, so the convergence does not carry.
2. **Implement** — staged: build → autonomous smoke proving the overlay still draws from the new seam
   (a non-regression check, NOT an S1/S2 proof) → deploy.
3. **Hand off for acceptance** (user's, hands-on). **Precondition: RTSS detection must be re-enabled**
   — it is currently None, so the test would otherwise pass vacuously.
4. **Owed, cheap:** a `-dx12` launch to verify the queue walk resolves a QI-valid DIRECT queue.

---

## 10b. NEXT SESSION — the agreed order (user, 2026-08-22) — SUPERSEDED by §10a

The user's words: *"In the next session we will tackle the two left things you mentioned and then
implementation."* The two "left things" are the two items surfaced at the end of §9:

1. **DX12 — decide its fate for v1.** Phase 1 as designed fixes DX11 only (the box's measured RHI and
   VOTV's default). To do DX12 in the same pass we need a DX12 rig — and **one already exists and is
   cheap: launch the game with `-dx12`** (the flag the 2026-07-26 DX12 overlay bring-up used on rig
   CLIENT_3; see `research/findings/tooling/votv-imgui-dx12-overlay-DESIGN-2026-07-26.md`, whose gate
   results record a real `-dx12` run: 3 buffers, FLIP_DISCARD, format 24 R10G10B10A2_UNORM). That is
   a launch flag, not the in-game `setting_rhi` toggle, so it needs no save/settings churn. Then
   measure (a)
   whether the presenting queue is reachable from `FD3D12Viewport` without the ECL hook, and (b) the
   transient-ECL-vs-RTSS timing race. Until that rig exists, DX12 is Phase 2 — that is the honest
   narrowing, restated so it is not discovered later.
2. **The acceptance test is the user's to run** (hands-on, per OPUS §4 / user-at-PC-tests-himself):
   RTSS OSD ON + our overlay visible **and** OBS **game-capture** with "Capture third-party overlays"
   **OFF** showing our overlay. `capture_window.ps1` CANNOT substitute for the OBS half — it is the
   DWM/GDI path and is blind to what OBS's Present-hook capture sees.
   *Optional pre-build de-risk, one run, on the CURRENT build:* launch with RTSS on and watch whether
   our per-frame counter keeps incrementing; if it stops, S1's unlink mechanism is confirmed measured
   rather than inferred (see §9's "acceptance-gated inferences").

3. **Then implementation** (the seam-move, §9), staged: build → autonomous smoke proving the overlay
   still draws via the new seam (a non-regression check, NOT an S1/S2 proof) → deploy → hand off for
   the hands-on acceptance test above.

---

## 10. Decision record / changelog

- **2026-08-23 — increment 1 LIVE-VERIFIED; DX12 promoted to v1; a third hooker found; one claim
  RETRACTED.** Live solo boot (DLL `335AC774544E17AB`, DX11): the seam AOB resolved at the exact
  IDA-predicted `image+0x16F4BA0` and its live bytes are unpatched (§6c.a) — verify-before-retire
  PASSED. Present-chain census (§6c.b) attributed every patch: `Present` + `ResizeBuffers` are OURS,
  and **`IDXGISwapChain1::Present1` is hooked by `NahimicOSD.dll`** — a third-party overlay we did not
  know was in the process, and a new (unproven) lead for the UE4SS-arc 19:17 crash. `RTSSHooks64.dll`
  is loaded even at detection None. **RETRACTED: "this box can reproduce both"** — the user has RTSS
  detection set to None globally, so this box reproduces NEITHER symptom today; the acceptance test
  requires re-enabling it. DX12 (§6c.c, §9b): `FD3D12Viewport::PresentInternal = sub_14177E0E0`
  (109 B, one caller, swapchain at `+0x60`, AOB unique at 24 B with no wildcards) and the presenting
  queue is four constant offsets from the viewport — provably the `pCommandQueue` the engine hands to
  `CreateSwapChainForHwnd`. So DX12 ships in v1 with **no ECL hook**, and
  `overlay_backend_dx12_capture.cpp` retires whole (RULE 2). Deferring DX12 was measured to be a choice
  between a RULE-2 dual path and a DX12-user regression (§10a). The 7-round convergence does NOT carry
  over this reframe — a fresh `/qf` is owed before building.
- **2026-08-22 (session end) — increment 1 BUILT (compiles only): the AOB + the log-only resolve
  probe.** `sdk_profile.h` gains `kSigD3D11ViewportPresentChecked` (measured unique, occ=1, 48 bytes,
  GS-displacement wildcarded) + `kD3D11Viewport_SwapChain = 0x70`; `ui/imgui_overlay.cpp::Init` logs
  where the seam resolves (expected `image+0x16F4BA0`). Verify-before-retire: nothing retired, the live
  overlay untouched, S1/S2 still present. Signature surface 5 → 6 (`docs/VERSION_MIGRATION.md` updated
  in the same pass — it claimed 5 in six places).
- **2026-08-22 (later) — DESIGN CONVERGED (7-round /qf, "that holds").** Seam chosen + measured:
  `FD3D11Viewport::PresentChecked = sub_1416F4BA0` (unique prologue, census-proven single once/frame
  choke point; swapchain at `viewport+0x70`; `IDXGISwapChain::Present` at vtbl[8]). Bootstrap dropped
  (not load-bearing). ResizeBuffers hook retired too (also RTSS-defended); RTV via per-frame
  `GetBuffer(0)` compare. Fail-CLOSED incl. `+0x70` QI-validation. DX12 phased to Phase 2 (transient-ECL
  queue discovery; needs a DX12 rig). Two acceptance-gated inferences named (S1 exact mechanism;
  RTSS-ignores-PresentChecked) — class-robust fix, hands-on gate confirms. IDA measurements in §6b;
  /qf transcript in the session scratchpad (`qf_thread.md`). NEXT: implement Phase 1, build + deploy,
  hand off for the hands-on RTSS+OBS acceptance test.
- **2026-08-22 — doc created (DESIGN, pre-/qf).** RE complete via three parallel research agents
  (OBS/RTSS source mechanics; MTA GUI-seam precedent; UE4.27 pre-Present seams), cross-checked against
  the repo (5 AOB signatures confirmed `sdk_profile.h:38-93`; PresentDetour draws-before-original
  confirmed `imgui_overlay.cpp:540/543`; DX12 creation-probe "boot does not precede creation" confirmed
  `overlay_backend_internal.h:80-85`). Root cause identified (inline hook on the swapchain-Present seam
  = both symptoms). Recommendation: Option 1 (move the draw to a native viewport-present seam, keep
  ImGui).
