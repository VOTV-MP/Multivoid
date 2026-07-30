# Hands-on runbook — ImGui 1.92.9, C1 + C2a + THE FLIP (commit 1), 2026-07-30

**Status: SMOKE-MEASURED on both RHIs, NOT hands-on.** No human has looked at a frame drawn by this
build. Everything below is what a hands-on pass should confirm or falsify.

- Artifact: `multivoid-0.9.0n-133.dll`. **Protocol unchanged (133)** — no wire-format change, so no
  version gate moves. Submodule `third_party/imgui` = **v1.92.9** (`01380c579`).
- Deployed to all four installs by `tools/deploy-all.ps1`.
- Design of record + as-built: `research/findings/tooling/votv-imgui-192-upgrade-DESIGN-2026-07-30.md`
  (read the AS-BUILT box at the top of §7 first).

## THE ONE THING TO LOOK AT

**Characters that used to draw as boxes now draw.** That is the whole deliverable, and it is visible
without any special scenario:

- **The em dash `—`, both curly quote pairs `“” ‘’`, the ellipsis `…`, the ruble sign `₽`.** These
  are the ones this user types daily. They were fallback boxes in b132 and every build before it.
- **Whole scripts the shipped faces always carried and the atlas simply never asked for**: Hebrew
  (`שלום`), Thai (`ไทย`), Arabic (`العربية`), Greek (`Ω`), all of Latin Extended.
- **CJK and Hangul are STILL boxes**, and correctly so — no embedded face or the emoji donor
  contains them. That needs C3 (a font source), not this commit. If hanzi renders, something is
  wrong: the font selftest's RED case exists to catch exactly that.

Measured, for scale: **+4,741 codepoints** moved from box to glyph, for **zero new DLL bytes** — the
glyphs were already inside the fonts we ship.

Type an em dash and a Hebrew word into chat. If they draw, the commit did its job.

## The lines to read in `multivoid.log`

**The old signature is gone.** There is no more `fonts: atlas baked in %.1f ms (2048x2048)` and no
more "two bake+selftest pairs per peer" — the atlas is lazy, so there is no single bake to time.
Expect instead:

```
repertoire selftest: PASS (22/22) -- 441 fold ranges, 67 exclude ranges
novelty ledger selftest: PASS (6/6) -- budget 512 per 10000 ms per peer
fonts: atlas geometry 512x128 RGBA32 (texid=1, packed 8618 px, discarded 0 px, 1 bakeds) -- lazy atlas...
font selftest: DONE fail=0 (12/12) -- atlas 512x128 RGBA32 texid=1, 348 colour texels in one emoji ...
```

- **`font selftest: DONE fail=0 (12/12)`** — a POSITIVE line, and `tools/mp.py` asserts its presence
  **on every smoke** (not only `smoke_i18n`, which was the case until 2026-07-30 and let a build with
  `fail=2` print PASS). The old negative grep (`"selftest: FAIL" not in log`) was sound only while the
  selftest ran unconditionally at boot; it is conditional now (a texture-id edge), so silence would
  have read as success.
- **12 rows, not 8.** Four landed 2026-07-30: three negative controls (U+00AD / U+E0B0 / U+E0067 —
  each carried by a different subset of faces, each excluded) plus a per-source census asserting every
  `ImFontConfig` actually received the generated exclude table. The census is the one that catches a
  config nobody's text exercises, without waiting for a draw.
- **The atlas STARTS SMALL and GROWS.** `512x128` at boot is correct, not a defect — it holds only
  what has actually been drawn. Earlier runbooks expected `2048x2048`; that was the eager regime.
  The geometry line reappears whenever it changes.
- **`novelty ledger: REFUSED ...` at boot is EXPECTED, once.** It is the selftest proving its own
  burst check fires. A refusal during *play* is not expected — see below.
- **Zero `atlas watch:` lines.** Two detectors live there and both are silent on a healthy build:
  - `N glyph(s) baked OUTSIDE the repertoire` — a font source lost its exclude list, or a Windows
    fallback face is supplying scripts the fold table sentinels. Names can then collide invisibly.
  - `N codepoint(s) a source HAS and we did NOT exclude failed to bake` — the packer ran out of
    room; those codepoints draw as boxes for the life of that baked. This is the saga's own symptom
    arriving through the new mechanism, which is why the detector is load-bearing.
- **Zero `Backend does not support ImGuiBackendFlags_RendererHasTextures`.** One means something
  queried a glyph outside a frame.
- **Zero `dx12 SRV pool exhausted`.**

## DX12 specifically

Launch with `-dx12` (or `VOTVCOOP_RHI=dx12` via `mp.py`) and confirm the geometry matches the DX11
run exactly — that is the "one binary, one drawable repertoire" property, and it was measured
(`512x128`, texid 1 then 2, 348 then 156 colour texels, identical on both RHIs).

New on DX12 only, the upload probe:

```
imgui_overlay: dx12 texture upload -- 1 texture(s), 1.29 ms total, dirty box up to 346x33 (INFINITE fence wait; worst so far 1.29 ms)
```

First real numbers: **1.29 / 1.92 / 3.96 ms**, boxes 346×33 → 95×10 → 12×13. Note the 3.96 ms was
the *smallest* box — the cost is the fence wait, not the copy. Watch for a worst-case that keeps
climbing during play; that is the input to whether we ever write our own servicing (§7.6).

## What is NOT covered by any smoke

- **P0 (`af234c08`)** — the `g_pending` unbounded queue is a **DX12-only** path. Exercise it by
  opening/closing the skins panel repeatedly on DX12.
- **A stutter while text with fresh codepoints appears.** The atlas now rasterises during play. If
  you see a hitch when a multilingual name or a new emoji first appears, look for
  `fonts: N glyphs rasterised in one frame`.
- **The three `dev_menu.cpp` clock `InputInt`s** changed behaviour with no code edit: 1.92.9 defaults
  `ImGuiItemFlags_LiveEditOnInputScalar` OFF, so typing "12" applies 12 on validation instead of
  writing 1 then 12 on the way.
- **Chat word-wrap positions.** 1.92.9 refined `CalcTextSize` width rounding, so pre-upgrade chat
  screenshots are not pixel-comparable.
- **The novelty cap under real multilingual play.** The budget is 512 first-sight codepoints per peer
  per 10 s (`net.novelty_budget` / `net.novelty_window_ms` in `multivoid.ini`). A legitimate lobby
  should never reach it; if a real conversation gets a message refused, that is a product bug and the
  number should move.

## The drills, if you want to see the instruments work

Both detectors have positive controls that run from a launch, no rebuild:

```
VOTVCOOP_ATLAS_NO_EXCLUDE=1   # every source bakes its whole cmap -> superset invariant goes RED
VOTVCOOP_ATLAS_TEXMAX=64      # starve the packer -> pack-failure detector goes RED (needs text load)
python tools/text/build_repertoire_drill.py     # the generator's 4 hard-fails, 4/4 RED
pwsh -File tools/text/atlas_regime_gate.ps1 -Drill   # the CI gate's 5 checks, 5/5 RED
```

`VOTVCOOP_ATLAS_TEXMAX=32` is degenerate — a single glyph rect exceeds the whole texture and the
process does not survive it. 64 is the smallest useful value.

## If it goes wrong

The flip is one commit. Reverting it restores the eager atlas and the b132 repertoire; the 1.92.9
upgrade itself (`b33aae30` + the submodule gitlink), `af234c08` (P0) and `c142d077` are independent
and should stay.
