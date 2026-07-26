# Hands-on runbook — DX12 overlay + Tidy-up fix (2026-07-26)

**Why this exists:** everything below was verified AUTONOMOUSLY (rig launches, log
assertions, screenshots, LAN smoke). No human has played on the DX12 path. This is
the short list of what a human should actually look at.

**Deployed:** `multivoid-0.9.0n-128.dll` built 2026-07-26 ~17:33 from `027a2110`,
deployed to all four rig installs via `tools/deploy-all.ps1`. Proto **128**
(UNCHANGED — no wire format was touched this session, so a mismatched-peer test is
not needed). HEAD at hand-off: the documentize commit over `dd3711c4`; origin is
still `04a5d129` (8 local commits, not pushed).

---

## A. DX12 overlay (the session's main ask)

**How to get there:** launch the game with `-dx12` (the rig shortcut has no args;
add it, or launch the exe directly). On the user's personal install DX12 may
already be the active RHI — that is where the original bug report came from.

1. **Press F1.** The mod menu must draw. Before this session it did not draw at
   all on DX12; the log said "menu will not draw".
2. **Read the new line in the menu's right pane: "Graphics API: DX11" or "DX12".**
   That is the dev feature you asked for. It should say DX12 on a `-dx12` launch
   and DX11 otherwise.
3. **Exercise the surfaces, not just the menu:** tilde (player list), T (chat),
   V (voice panel), the server browser, F1 > Cosmetics > Skins (the skin preview
   TILES are the DX12 texture path — the one place a D3D12-specific bug would show
   as a wrong/blank image). Press "Refresh list" a few times: previews must keep
   rendering correctly and RAM must not climb.
4. **Resize the window / toggle fullscreen a few times.** The overlay must keep
   drawing at the new size. (Two window resizes were drilled autonomously and the
   render target rebuilt correctly; fullscreen was NOT exercised — Alt+Enter did
   not reach the game from a script.)
5. **Then play normally for a few minutes on DX12** — nameplates, chat feed, the
   join flow. The overlay draws every frame the HUD is active, so a DX12-only
   stability problem would surface as a stutter or a vanished overlay.

**What to read in `multivoid.log` if something looks wrong:**
- `imgui_overlay: RHI = DX12 -- capturing the presenting queue (P1 ... P3 ...)` —
  detection; the hr values must be 0.
- `dx12: presenting queue CONFIRMED <ptr> (N/30 frames)` then
  `dx12 renderer up -- N buffers, rtvFormat=...` then
  `dx12 first frame rendered (N vertices, ...)` — the bring-up chain.
- `swapchain resized (WxH, N buffer(s), fmt=...) -- render target rebuilt on DX12`
  — one per resize.
- Anything with `GPU wait timed out` / `overlay stays down this run` /
  `no presenting queue could be confirmed` = the failure paths, each self-explaining.

## B. Tidy up (the settings-check panel)

The button used to do nothing visible and the panel returned every launch.
1. Launch with an ini that has a stale key (any old `multivoid.ini`; `posinfo=1` is
   the canonical one) — the SETTINGS CHECK panel appears.
2. Press **"Tidy up multivoid.ini"**. Expect: a green line naming what changed
   ("N duplicate line(s) merged, N setting(s) placed, N unknown/invalid line(s)
   commented out"), the offending rows GONE, and the panel closing itself if
   nothing else is left. Open the ini: the retired lines are still there as
   `; unknown key (tidy): posinfo=1` — data kept, just disabled.
3. Relaunch: the panel must NOT come back for those keys.

## C. What was already proven autonomously (no need to re-check)

- DX12: queue confirmed 30/30 frames, first frame 794 vertices, F1 menu screenshot
  correct, two live resizes rebuilt the render target, 30 s soak alive.
- DX11 regression: LAN smoke PASS on the same bytes, `DX11 bring-up OK`.
- Tidy up: config-selftest Drill H 6/6 (the literal `posinfo` case) with the smoke's
  machine assertion `config-selftest: DONE fail=0`.

## D. Known residuals (do not report these as new bugs)

- `imgui_overlay::Shutdown()` is never called today — the ordered DX12 teardown
  exists but is unexercised (the process exits instead).
- Many skin previews decoding in one frame serialize a GPU round-trip each on DX12
  (a brief hitch the first time the Skins tab opens with a large pack set).
- Vulkan is not covered at all — there is no DXGI swapchain to hook there.
