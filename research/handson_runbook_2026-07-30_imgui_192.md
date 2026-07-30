# Hands-on runbook — ImGui 1.92.9 upgrade (C1 + C2a), 2026-07-30

**Status: SMOKE-MEASURED on both RHIs, NOT hands-on.** No human has looked at a frame drawn by this
build. Everything below is what a hands-on pass should confirm or falsify.

- HEAD at write time: `246b50f3` + this docs commit. **Commits since are DOCS ONLY** (`1b95b71d`,
  `683f8214`, `7230408a`, + this sweep) — **the artifact under test is unchanged**, so everything below
  still applies verbatim. The FLIP is designed but **not built**; see that design's §7.
- Artifact: `multivoid-0.9.0n-133.dll`, **17,424,896 B** (baseline 1.91.5 was 17,366,016 → **+58,880**).
- **Protocol unchanged (133).** No wire-format change in this work, so no version gate moves.
- Submodule `third_party/imgui` = **v1.92.9** (`01380c579`).
- Deployed to all four installs by `tools/deploy-all.ps1` during the session.

## What changed that a human can actually see

Nothing should look different. That is the claim under test: **`RendererHasTextures` is cleared on both
RHIs**, so the drawable repertoire is byte-for-byte the old one. Concretely expect:

1. **Text renders as before.** Cyrillic draws; CJK/Japanese names draw as fallback boxes, one box per
   codepoint (the smoke's player list showed **3** boxes for `张伟明` and **5** for `さくら田中`, matching
   the 1.91.5 baseline exactly).
2. **Emoji are still COLOURED.** The atlas is RGBA32 and the boot selftest counts colour texels.
3. **The F1 menu, chat, nameplates, net-stats and the skins previews all draw**, at both the boot
   resolution and after a window resize (the resize path re-bakes the atlas).

## The lines to read in `multivoid.log`

Per peer, expect **two** bake+selftest pairs — one at boot scale, one after `UI re-scaled`:

```
fonts: atlas baked in 82.5 ms (2048x2048 RGBA32)
font selftest: PASS (8/8) -- atlas 2048x2048 RGBA32, 348 colour texels in one emoji (...)
fonts: atlas baked in 72.7 ms (1024x2048 RGBA32)
font selftest: PASS (8/8) -- atlas 1024x2048 RGBA32, 156 colour texels in one emoji (...)
```

- **`8/8`, not `6/6`.** Two checks are new: the donor supplies `U+1F600` via `IsGlyphInFont`, and the RED
  case `U+4E00` must be ABSENT. A `7/8` naming `U+4E00` would mean the repertoire table and the shipped
  fonts have diverged.
- **TWO selftest lines per peer.** Under 1.91.5 there was only one — a `static bool done` latch meant the
  atlas the game actually ran on was never checked. One line only = the latch came back.
- **Zero lines containing `Backend does not support ImGuiBackendFlags_RendererHasTextures`.** Even one
  means something queried a glyph outside a frame and poisoned `TexIsBuilt` — the §3.2 defect.
- **Zero `dx12 SRV pool exhausted`** (C2a's new allocator; would alias a texture visibly).

## DX12 specifically

The overlay has a separate render half per RHI, and C2a rewrote DX12's init. Launch with `-dx12`
(or `VOTVCOOP_RHI=dx12` via `mp.py`) and confirm:

```
imgui_overlay: RHI = DX12 -- capturing the presenting queue (...)
imgui_overlay: dx12 renderer up -- 3 buffers, rtvFormat=24, queue=...
imgui_overlay: DX12 bring-up OK (...)
```

**The bake sizes must match the DX11 run** (`2048x2048` then `1024x2048`). A second bake logged as
`512x128` in `0.0 ms` is the signature of the dynamic atlas being ON for DX12 only — the defect a smoke
caught mid-session, and the reason the flag is cleared in `InitImguiBackend`.

Then press F1 and check the menu reports `Graphics API: DX12`, and open the skins panel so a UI texture
goes through the shared descriptor pool alongside ImGui's atlas.

## What is NOT covered by any smoke

- **P0 (`af234c08`)** — the `g_pending` unbounded queue is a **DX12-only** path; a default smoke launches
  DX11, so it has never executed. Exercising it wants many preview textures created and destroyed inside
  one fence window (open/close the skins panel repeatedly on DX12).
- **The `ImGui_ImplDX12_UpdateTexture` INFINITE wait** is unreachable while the flag is off. It becomes
  reachable in C2b, which is why C2b replaces it.
- **The three `dev_menu.cpp` clock `InputInt`s** (`:100/103/106`) changed behaviour with no code edit:
  1.92.9 defaults `ImGuiItemFlags_LiveEditOnInputScalar` OFF, so typing "12" now applies 12 on
  validation instead of writing 1 then 12 on the way. Worth a deliberate look — it is an improvement for a
  world-clock setter, but it is a real UI change no compile could show.
- **Chat word-wrap positions.** 1.92.9 refined `CalcTextSize` width rounding, and chat wraps through
  `CalcWordWrapPositionA`, so pre-upgrade chat screenshots are not pixel-comparable.

## If it goes wrong

The upgrade is one commit (`b33aae30`) plus the submodule gitlink. Reverting that commit and the
gitlink restores 1.91.5; `af234c08` (P0) and `c142d077` (the smoke fix) are independent of it and should
stay.
