# ImGui overlay DX12 render path + RHI indicator — design of record (2026-07-26)

**Status: AS-BUILT + VERIFIED-BY-DRILL (not hands-on). Commits `f5018ff8` (DX11
extraction), `4c325ea5` (texture-leak fix), `63693526` (capture gate),
`e3a53fe1` (renderer + RHI indicator), `027a2110` (both audits folded).**

> **SUPERSEDING ARC (2026-08-22) — read `docs/OVERLAY_CAPTURE_COEXIST.md` before changing where the
> overlay DRAWS.** This doc's architecture hangs off our inline hook on `IDXGISwapChain::Present`.
> That seam is the measured ROOT of two user-reported failures: the overlay is invisible when
> RivaTuner/MSI-Afterburner is on (RTSS's hook-integrity control restores its own bytes and unlinks
> ours), and OBS game-capture cannot see it (OBS copies the backbuffer at the top of its own Present
> detour, before ours draws). The converged fix moves the draw to `FD3D11Viewport::PresentChecked` —
> upstream of the whole external hook chain — and RETIRES the `Present` + `ResizeBuffers` inline
> hooks (RULE 2). **The DX12 half of THIS doc is directly affected:** RTSS defends
> `ExecuteCommandLists` too, so our persistent ECL hook is a second S1 surface and must become a
> TRANSIENT discovery (hook, capture the queue, uninstall). Everything below remains accurate as the
> description of what is BUILT today; the coexistence arc is what changes it, and its DX12 phase
> reuses this doc's `-dx12` rig recipe and its 600/600 ECL finding.

## AS-BUILT (2026-07-26)

**The user's ask is answered:** launching with `-dx12` brings the overlay up and
draws it; the old "menu will not draw" WARN is gone (RULE 2).

**Gate results (rig CLIENT_3, real `-dx12` run) -- every inferred pillar measured:**
- P1 `GetDevice(ID3D12Device)` hr=0 TRUE; P3 `QI(IDXGISwapChain3)` hr=0 TRUE.
- swapchain: 3 buffers, FLIP_DISCARD, **format 24 = R10G10B10A2_UNORM** -- the RTV
  format MUST come from the desc; an RGBA8 literal would have been wrong.
- queues: ONE DIRECT device-matched queue (2999 ECL calls, last-before-Present
  600/600 = 100%); one COPY queue (5 calls) = the known-positive proving the
  instrument sees foreign traffic. No ambiguity, no HALT.
- **creation probe armed but NEVER fired** -> our boot does NOT precede the game's
  swapchain creation, so the factory-hook route (zero-ambiguity IF preceding) is
  NOT available; the ECL capture is what ships, exactly as the data-picks-the-
  mechanism rule required. `hook::Enable` is used by the recreation re-arm.
- confirmation window as shipped: 30 presents, >=90% agreement (anchored on the
  measured 100%).

**Drills:** `-dx12` -> "queue CONFIRMED 30/30" -> "renderer up (3 buffers,
rtvFormat=24)" -> "first frame rendered (794 verts, 3 draw lists)"; PrintWindow
screenshot shows the full F1 menu incl. the "Graphics API: DX12" line; TWO live
window resizes -> "render target rebuilt on DX12" x2 with no timeout/SEH/disable
lines; process alive after the drill. DX11 regression: LAN smoke PASS,
"DX11 bring-up OK" unchanged. NOT hands-on (no human has played on DX12 yet).

**Audits (both agents, post-ship):** 3 CRITICAL + 5 IMPORTANT + 5 minor, ALL fixed
in `027a2110` -- texture-slot recycling under an in-flight draw, null frame-context
allocator when the buffer count grows, unlatched confirmation re-baking the font
atlas per frame, InitRenderer leaking on every failure path, the recreation branch
missing the desc-change re-init AND the queue re-arm (its "stale frame, not UB"
comment was false), a silent capture-arming failure, `WaitFence` reporting success
when it could not wait, and a QueueSlot payload published before it was written.

**Known residuals (named, not hidden):** the mid-detour teardown window during the
capture/confirmation seconds and each re-arm window (same pre-existing class as the
Present detour); `imgui_overlay::Shutdown()` has no caller today, so the ordered
teardown path is code-complete but unexercised; DX11 `EnsureTarget` still has no
swapchain-identity check (pre-existing, unchanged by this arc); DX12 texture uploads
serialize one GPU round-trip each when many previews decode in one frame (batching
is the follow-up).

---

**Original design (kept for the record):**
12-round /qf, "that holds" at R12 (spine unchanged across R10-R12). User ask (verbatim):
"imgui in our MOD doesn't support DX12... menu will not draw. DX11 is working properly.
CAN WE FIX AND MAKE IT WORK WITH DX12 PER RULE 1?" + "new dev feature - overlay telling
the dev which directx he's using".

## Measured fact base

- The Present hook (shared-DXGI-vtable technique, MinHook on vtbl[8]/[13]) FIRES on the
  DX12 swapchain — the user's WARN line is printed from inside our detour. Reproduced on
  the rig: CLIENT_3 (b127) launched with `-dx12` prints the exact WARN (14:30:01).
  `-dx12` is the verification path; the source of the USER's DX12 (their personal
  install) remains unknown — the RHI indicator makes it visible operationally.
- Hook install 14:29:51 vs first Present 14:30:01: install-before-first-present proven;
  install-before-swapchain-CREATION unproven (our Init is an async BootThread; a
  fast-machine race is possible in principle).
- `imgui_impl_dx12.cpp` is already vendored + compiled (CMakeLists:93). ImGui 1.91.5,
  legacy Init API (device, num_frames_in_flight, rtv_format, srv_heap, font cpu/gpu
  handles). **SUPERSEDED 2026-07-30 (`780a93af`): the submodule is v1.92.9 and this TU
  now inits through `ImGui_ImplDX12_InitInfo` with our own `SrvDescriptorAllocFn`/
  `FreeFn` over a UNIFIED slot pool — the "slot 0 = the font" reservation described
  below is retired, because 1.92 keeps up to two atlas textures alive across a repack.
  See `votv-imgui-192-upgrade-DESIGN-2026-07-30.md` §4 C2a.**
  ImTextureID = D3D12_GPU_DESCRIPTOR_HANDLE (u64; fits the existing void*
  cast in skins_panel). The vendored backend stores the provided SRV heap (:725) and
  creates ONLY the font SRV into the given cpu handle (:443) — slots beyond the font
  are wholly caller-managed (measured).
- ui/fonts.cpp is backend-agnostic (zero DX11 refs). imgui_overlay.cpp = 772 LOC (soft
  cap) -> the render path extraction is mandatory before any DX12 code.
- skins_panel is the SOLE texture consumer; TODAY its "Refresh list" leaks one
  texture+SRV per preview per refresh (g_previews.clear() without Release). 10 previews
  on the rig install.
- hook facade Shutdown = MH_DisableHook(ALL) + MH_Uninitialize (hook.cpp:69-70, called
  from shutdown.cpp:211); a mid-detour-body thread at teardown is a PRE-EXISTING risk
  class (the Present detour has it today; g_inFrame bounded spin narrows it).
- Screenshot tool = tools/capture_window.ps1 PrintWindow(PW_RENDERFULLCONTENT) + BitBlt
  fallback; desktop-level (captures our ImGui layer). Under a FLIP-model chain it is
  unmeasured -> it is the SECONDARY visual check only; the machine gate is a log line.
- No DXGI API exposes the swapchain's creating queue -> capture required.

## Inferred pillars, gated (stage 1 must measure TRUE before the renderer is built)

- P1: `sc->GetDevice(IID_ID3D12Device)` succeeds on the DX12 swapchain.
- P3: the DX12 swapchain QIs to IDXGISwapChain3 (GetCurrentBackBufferIndex).
- (P2 "one DIRECT queue presents" is NOT load-bearing any more — replaced by the
  candidate+confirmation mechanism; stage 1 still measures the real queue set.)
- (P4 "-dx12 flips the RHI" was measured this session — closed.)

## The design (v12)

**Commit 1 — pure extraction.** The RHI-specific rendering moves behind
`ui/overlay_backend.h`: BringUp(sc), NewFrame(), RenderDrawData(sc), OnResizeRelease(),
OnResizeRecreate(sc), InvalidateDeviceObjects(), CreateTextureFromImageFile(),
Shutdown(), Kind(). `overlay_backend_dx11.cpp` = a literal move of today's DX11 code;
imgui_overlay.cpp keeps hooks/WndProc/surface compositing; the WIC decode becomes a
shared helper. Kind() (a one-line constant) is the only new code — whitelisted glue
(s24b seam precedent). Equivalence: literal body-diff instrument + mutate control +
DX11 smoke BEFORE any DX12 code.

**Commit 1b — the leak fix, whole.** DestroyTexture (interface declaration + DX11
Release body + the skins_panel Refresh caller) lands here, NOT in commit 1 — the
extraction instrument stays byte-honest.

**Commit 2 — detection + capture slice (the stage-1 gate).** On ID3D11Device failure:
try ID3D12Device (hr LOGGED, P1), QI IDXGISwapChain3 (hr LOGGED, P3), lazy
LoadLibrary("d3d12.dll"). BOTH capture mechanisms measured this stage:
- factory hook (CreateSwapChainForHwnd — the presenting queue is its 1st param; exact
  IF we precede creation, which is unproven -> never trusted from a timing sample);
- ECL hook (ID3D12CommandQueue::ExecuteCommandLists via dummy device+queue vtable;
  timing-independent). The stage-1 detour logs per-caller (queue ptr first-seen,
  Desc.Type, thread id, calls/frame) across ALL queue types and tallies
  "candidate == last DIRECT ECL before Present" agreement over ~600 frames.
- Corpus rule (negative-grep lesson): the run covers menu + a loading transition +
  IN-WORLD (texture streaming guarantees foreign copy-queue traffic). Nonzero
  non-candidate ECL counts in the same log are the KNOWN-POSITIVE proving the
  instrument can see disagreement; zero foreign traffic -> extend the run; the gate is
  not accepted on a corpus that couldn't disagree.
- The confirmation constants ship DATA-ANCHORED from this tally. A pathological set
  (>=2 DIRECT queues without a clear nearest-before-Present winner) = HALT + its own
  /qf on the measured data (candidates there: IAT patch of CreateDXGIFactory at attach
  = structural creation-precede without D3D objects in DllMain).

**Prod capture = candidate + confirmation.** First device-matched DIRECT queue is a
CANDIDATE; the detour stays armed for the confirmation window (constants from stage 1)
verifying candidate == last-DIRECT-ECL-before-Present; accept -> hook::Disable(ECL)
from the NEXT PresentDetour (an owned moment; never from inside the ECL detour).
Until confirmed: no rendering (today's behavior; the initial blink is invisible —
nothing has drawn yet). Foreign same-device DIRECT writers cannot steal the capture;
different-device overlays (OBS/RTSS) are excluded by device-match.

**ECL detour discipline.** Pure capture/confirm bookkeeping; NO inflight counter (a
counter that cannot prove absence — pre-inc preemption — is RULE-2 ballast). Retirement
= MH_DisableHook ONLY, never Remove: the ~64-byte trampoline slot stays allocated for
the process lifetime so a preempted thread finishes through a live trampoline (no
race, no grace heuristic). hook facade gains an Enable/Disable pair. ACCEPTED RESIDUAL
(named): a thread can be mid-detour-body at MH_Uninitialize teardown during (a) the
initial capture/confirmation seconds and (b) every re-arm window after a swapchain
recreation — the same pre-existing class as the Present detour today, strictly
narrower. MinHook freeze-alloc-safety is checked against vendored hook.c at impl time
(if it allocates while threads are frozen, Disable moves to the game-thread pump).

**Commit 3 — the DX12 renderer.**
- Bring-up (once queue confirmed): RTV descriptor heap (BufferCount) + per-buffer
  RTVs; ONE shader-visible SRV heap: slot 0 = font, slots 1..256 = textures
  (free-list; exhaustion = WARN naming counts, imageless tiles — visible, not silent);
  fence; FrameContext shape VERBATIM from the vendored example_win32_directx12
  (per-context fenceValue, WAIT before allocator Reset, Signal after Execute).
  ImGui_ImplDX12_Init(device, BufferCount, desc format, srvHeap, slot0 handles).
- Per frame in the Present detour: sc POINTER CHECK first (backend state is bound to
  the sc it brought up on; a recreation that never passed our ResizeBuffers hook ->
  bounded WaitGpuIdle + rebuild all swapchain-derived state + RE-ARM capture with the
  prior confirmed queue SEEDED as candidate — rendering continues immediately, demote
  on disagreement; seeded-window damage bound = <=confirm-window frames of possible
  visual artifact, no UB, named residual); then ctx wait -> reset -> barrier
  PRESENT->RT -> OMSet+SetDescriptorHeaps -> RenderDrawData -> barrier back -> Close ->
  confirmed-queue ExecuteCommandLists -> Signal.
- ALL fence waits bounded (event + ~2s timeout) + GetDeviceRemovedReason on timeout ->
  one-shot log, overlay rendering disabled for the run, Present passes through — never
  hang the render thread or shutdown.
- ResizeBuffers: bounded wait-last-signal -> release OUR backbuffer state BEFORE the
  original call; FAILED hr tolerated (log + null state, DX11-symmetric). Format or
  BufferCount change -> WaitGpuIdle + backend-only Shutdown/re-Init (format is baked
  into the backend PSO at Init; ImGui context + Win32 backend survive).
- InvalidateDeviceObjects (rescale path) = WaitGpuIdle FIRST (in-flight lists may
  reference the font texture; DX11 hid this via driver refcounts).
- Textures: shared WIC decode -> default-heap tex + upload staging + CopyTextureRegion
  on an upload list submitted to the confirmed queue BEFORE the frame's draw list
  (queue order replaces any CPU fence stall — zero wait); staging + destroyed textures
  go to a deferred-release list {resource, slot, fenceValue}, freed and slot recycled
  once the fence passed. DestroyTexture on DX12 = deferred entry, never immediate.
- Mod-Shutdown sequence: uninstall Present/Resize hooks -> g_inFrame spin -> bounded
  WaitGpuIdle (the last submitted list may still read the SRV heap) -> drain
  deferred-release -> release FrameContexts/heaps/fence -> ImGui_ImplDX12_Shutdown ->
  context teardown.

**RHI indicator (user feature).** LOG-primary: ONE one-shot line carrying Kind +
outcome + reason ("RHI = DX12: bring-up OK" / "RHI = DX12: bring-up FAILED (hr=...,
removed-reason=...) — menu will not draw"); it REPLACES today's WARN (RULE 2). The F1
menu line (reads Kind() live, "pending" before ready) is secondary — the log survives
exactly the failure it reports (lesson: diagnostic surface must not be gated by what
it diagnoses). Stage gates assert the LOG line, not the menu.

**Verification.** Stage-1 log gate (-dx12, measured launch path) -> build -> machine
gate = "first DX12 frame rendered (N vertices)" log line -> PrintWindow screenshot
(secondary; flip-model capture gets measured here; black through both paths -> demoted
to n/a, the log carries the verdict) -> a REAL swapchain-recreation drill (fullscreen
toggle under -dx12) exercising the re-arm branch -> DX11 regression smoke.

## qf round digest (what forced each shape)

R1 AOB queue read rejected (brittle non-reflected structs); vendored FrameContext
adopted over "Present chain is the fence". R2 the ECL vtable hook fires for ALL queues
from other threads (copy/compute) — the same-thread uninstall argument died; resize
must re-derive desc state; extraction = its own commit. R3 the drain counter is
defeated by pre-inc preemption -> Disable-without-Remove + permanent trampoline;
DestroyTexture + deferred release + free-list; queue-ordered upload replaces the fence
stall; IDXGISwapChain3 QI became pillar P3. R4 the leak fix would blind the extraction
instrument -> split to 1b; MH_Uninitialize teardown measured (pre-existing class);
>=2-DIRECT ambiguity = HALT; 256 slots + named-counts WARN. R5 swapchain recreation
bypasses our ResizeBuffers hook -> per-frame sc pointer check; ALL fence waits bounded
+ device-removed; ECL OFF-edge positive in both branches; -dx12 measured on the rig.
R6 the factory-hook alternative surfaced -> stage 1 measures both, data picks prod;
screenshot mechanism identified (desktop PrintWindow, not HighResShot); resize release
sequenced before the original. R7 the RHI indicator must survive its own failure ->
log-primary; factory-in-prod requires a STRUCTURAL creation-precede guarantee (a
timing sample does not generalize); Disable deferred to PresentDetour. R8 the
shutdown-spin contradiction -> the inflight counter deleted entirely, residual stated;
"queue outlives swapchains" inference -> re-arm mechanism; shutdown GPU-idle sequence.
R9 the residual statement must name re-arm windows too; wrong-queue capture on overlay-
infested machines -> candidate+confirmation; vendored backend SRV handling measured.
R10 flip-model PrintWindow unmeasured -> log-primary gate; re-arm blink -> seed the
prior queue; confirm constants must be data-anchored. R11 the tally corpus must be able
to disagree (known-positive = foreign copy-queue traffic, in-world coverage); the
recreation drill enters the verification plan; DestroyTexture moved whole to 1b.
R12 "that holds".

## File plan

- `include/ui/overlay_backend.h` (NEW) — the interface; no RHI types leak out.
- `src/ui/overlay_backend_dx11.cpp` (NEW, commit 1) — moved DX11 path + WIC helper home.
- `src/ui/overlay_backend_dx12.cpp` (NEW, commits 2-3) — capture + renderer.
- `src/ui/imgui_overlay.cpp` — hooks/WndProc/surfaces only; ~500 LOC after the cut.
- `include/ue_wrap/core/hook.h` / `hook.cpp` — Enable/Disable pair added.
- `src/ui/dev_menu.cpp` — the RHI line.
