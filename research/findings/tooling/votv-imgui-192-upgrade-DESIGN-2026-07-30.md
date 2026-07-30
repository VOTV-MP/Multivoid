# ImGui 1.91.5 -> 1.92.9: the measured migration, and the plan the `/qf` left standing

**Status: MEASURED (compile + link, by a reverted spike). NOTHING BUILT, NOTHING COMMITTED.**
HEAD at write time `a0a9bb70`; submodule still pinned `v1.91.5` (`f401021d5`); tree clean.
Logs: `build/imgui1929_pricing.log` (pre-port errors), `build/imgui1929_pricing2.log` (clean link).
Snapshot used: `build/imgui1929/` (a 1.92.9 checkout with `.git` stripped).

**Why this document exists.** The glyph thread (see §1) converged on a single root: CJK is
unaffordable in ImGui 1.91.5 *whatever the glyph source and whatever the bake trigger*, because
1.91.5 has no dynamic atlas. The user then asked *"Do we need to make our own CEGUI?"* (no),
*"Ok, measure"* (this document's §2), and *"Okay, upgrade"* (§4's plan). Read it before touching
the imgui submodule pin, `ui/fonts.cpp`, `ui/overlay_backend_dx12.cpp`, or
`tools/text/build_repertoire.py`.

**It supersedes RF3's framing** (`votv-arc-d-gate-measurements-2026-07-28.md` §top box), which
recorded the 1.92 upgrade as gated on a "structural" DX12 descriptor-allocator migration. That
migration is largely already built — see §2.4.

---

## 1. The originating ask, and what the 8-round `/qf` settled

The user's own words, in order: *"so we're not supporting any glyphs at all?"* -> *"how much will
baking all, including hieroglyphs, actually cost"* -> *"lets not ship them in dll, lets ship them as
mta does"* -> *"I actually want glyphs supported in chat too"* -> *"We started the chat saga when I
wanted it to support glyphs, remember?"* -> *"Will chat support glyphs, cyrillic and emojis?"*

**The answer to the last one, as measured:** Cyrillic YES, emoji YES, Latin Ext-A/B + Greek YES
(shipped `9b4286f1`). **CJK/Japanese NO.** Tagged `inferred` for the **Chat face specifically** —
the boot selftest reads `Role::Nameplate` (`fonts.cpp:255`) and the Chat face (18 px, bold) is the
one role that by the `{fam, px, bold}` dedup key can never share a face with any surface yet
measured.

Two things the pass killed, both mine:

- **The "two layers" claim was false.** `nickname_arbiter.cpp:117-133` `AssignAgainst` returns
  `Candidate(requested, n)` — **the original string plus a numeric suffix**; the fold sentinel never
  leaves `FoldKey`'s comparison, and `player_handshake_nick.cpp:218-231` is *persist*-suppression, not
  pixels. So `research/nickarb_shots/host.png` is drawing the real `张伟明` / `さくら田中` and those
  boxes ARE ImGui's `FallbackGlyph`, one per codepoint (3 and 5, counts matching exactly).
- **Five separate objections collapse to one root.** §7a of the bake doc listed five counts against
  on-demand OS fonts. Count 4 (DirectWrite) does not survive substituting MTA's real mechanism
  (`AddFontResourceEx` + `D3DXCreateFont` — a FILE handed to the OS; `MapCharacters` is a discovery
  API MTA never touches). Count 6 (the one-generator break) is an invariant to preserve, not an
  impossibility. Count 1 alone is survivable when rare. **Only count 3 kills — and it kills because
  1.91.5 has no partial update.** MTA survives arbitrary remote text because CEGUI rasterises per
  PAGE and evicts (`CEGUIFont.cpp:1510-1527`); one remote codepoint costs it a page, ours costs a
  whole atlas.

**And the axis conflation that had the pass killing the wrong half in the user's name:** SOURCING
(DLL donor vs the player's OS fonts) and TRIGGERING (eager vs on-first-sight) are orthogonal. Count 3
kills triggering. Utterance (3) is about sourcing.

---

## 2. The measurement

### 2.1 It compiles and it links

Whole mod built against the 1.92.9 snapshot: **400 TUs compiled, errors in exactly 3 UI files.**
After a **+18/-14** port across those three, `multivoid-0.9.0n-133.dll` **linked with 0 errors**.
Link warnings are the pre-existing opus CRT-mismatch set (present in >=5 historical 1.91.5 build
logs). **DLL size 17,364,480 -> 17,435,648 = +71,168 B (+0.41 %).**

The spike was then reverted: submodule back to `v1.91.5`, all three files restored.

### 2.2 The breaks, each verified against 1.92.9's header

| symbol | 1.92.9 | our sites |
|---|---|---|
| `ImFont::FontSize` | -> `LegacySize` (`imgui.h:3941`) | `fonts.cpp:395`, `hud.cpp:125`, `chat_view.cpp:210` |
| `ImFontConfig::FontBuilderFlags` | -> `FontLoaderFlags` (`imgui.h:3633`) | `fonts.cpp:188` |
| `ImFont::FindGlyphNoFallback` | **MOVED to `ImFontBaked`** (`imgui.h:3910`), via `font->GetFontBaked(px)` | `fonts.cpp:265,285,287` |
| `ImFontAtlas::TexPixelsRGBA32` / `TexWidth` / `TexHeight` | -> `atlas->TexData->Pixels/Width/Height` (`imgui.h:3838`, `ImTextureData` at `:3564`) | `fonts.cpp:271,272,277,290,354` (10 refs) |

**The `ImFont` / `ImFontBaked` split IS the dynamic atlas** — "baked data for a ImFont at a given
size" (`imgui.h:3881`). Everything else in the port is a rename.

### 2.3 What did NOT break, and why that is a warning not a relief

**No backend break at all**: 1.92.9 retains the legacy `ImGui_ImplDX12_Init` /
`ImGui_ImplDX11_Init` signatures, so `overlay_backend_dx11.cpp` and `overlay_backend_dx12.cpp`
compiled unmodified.

But three call sites compile only because 1.92.9 keeps an **obsolete shim**, and leaving them there
is RULE-2 migration baggage rather than a completed port:

- `imgui.h:4133` — `inline void PushFont(ImFont* font) { PushFont(font, font ? font->LegacySize : 0.0f); }`
  is why `chat_input.cpp:89` and `net_stats_panel.cpp:178` produced no error.
- `SetWindowFontScale` survives (`loading_screen.cpp:74,78`).
- `CalcWordWrapPositionA` survives as an inline forward (`imgui.h:3968`).

### 2.4 The capability strip — the reason a clean compile is not a working upgrade

`imgui_impl_dx12.cpp:987` does `io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;` on the
legacy init path, with the reason in the source: *"Using legacy ImGui_ImplDX12_Init() call with 1 SRV
descriptor we cannot support multiple textures."* `imgui_impl_dx12.cpp:894` additionally asserts
*"Only 1 simultaneous texture allowed with legacy ImGui_ImplDX12_Init() signature!"*.
**DX11's only init sets the flag unconditionally** (`imgui_impl_dx11.cpp:632`).

So the spike build has **no dynamic atlas on DX12 and a dynamic atlas on DX11** — see §3.1, this is
the finding that re-ordered the whole plan.

`ImGui_ImplDX12_InitInfo` (`imgui_impl_dx12.h:45-50`) wants `SrvDescriptorAllocFn` /
`SrvDescriptorFreeFn` over an app-owned heap. **We already own that machinery**, built in July for
the UI-texture feature: a 256-slot SRV heap (`overlay_backend_dx12.cpp:36`, `:258`), per-slot CPU/GPU
handle math (`:527-538`), and fence-gated slot recycling (`:88-113`, `:547-558`). What is missing is
wrapping it in the two callback signatures. **This is why RF3's "structural" framing is superseded.**

### 2.5 `GlyphRanges` is dead input — the finding that dissolves arc D2's construction

`ImFontConfig::GlyphRanges` is marked `*LEGACY*` (`imgui.h:3625`) and is read in exactly ONE
function, `ImFontAtlasBuildLegacyPreloadAllGlyphRanges` (`imgui_draw.cpp:3540`), called only under
`if (atlas->RendererHasTextures == false)` (`:3498-3499`).

**Once the dynamic atlas is on, our repertoire ranges bound nothing.** The atlas draws whatever the
face cmap carries — 8,148 codepoints today against the 2,517 we fold. Arc D2's load-bearing property
("one generator mints both what FOLDS and what BAKES, so they cannot drift") is not *violated* by the
upgrade; it is **dissolved**, because the bake side stops being a set.

**And on-demand baking is NOT gated on the flag.** `ImFontBaked_BuildLoadGlyph`
(`imgui_draw.cpp:4562-4571`) checks only `atlas->Locked` and `ImFontFlags_NoLoadGlyphs`; the
per-source accept test uses `GlyphExcludeRanges`, never `GlyphRanges`. So a codepoint outside the
preload bakes on demand **even in the legacy regime** — which is why §3.2 exists.

### 2.6 Other measured bounds

- **At most TWO atlas textures live at once.** `ImFontAtlasTextureAdd` (`imgui_draw.cpp:4085-4113`)
  creates a new texture and marks the old `WantDestroyNextFrame`; `UnusedFrames` (`:2886`) exists so a
  backend can DEFER destruction — compatible with our fence-gated release.
- **`FindGlyphNoFallback` now BAKES on miss** (`imgui_draw.cpp:5361-5373`: sets `LoadNoFallback = true`,
  then `ImFontBaked_BuildLoadGlyph`). Asking is baking. The font selftest can therefore no longer ask
  "is this in the atlas" — only "can this face produce this", which is a cmap fact
  `build_repertoire.py` already knows at generate time. It still discriminates (a codepoint no face
  carries returns NULL, so `U+4E00` is a valid RED), but **its claim must be relabelled**.
- **Weight asymmetry is ZERO.** `regular_union ^ bold_union` = 0; both are 8,148. The generator's
  per-weight gate (`build_repertoire.py:197`) is a real guard for a future face and costs nothing today.
- **`build_repertoire.py:221`** builds `drawable = (base_ask - gap) | kept_cmap` — bounded by a
  HAND-WRITTEN `BASE_RANGES`, while the generator already computes and prints `w_union` = 9,478
  codepoints across the 8 faces, of which `base_ask` reaches only 1,127.

### 2.7 The free widening this exposes (measured, unbuilt)

Asking for what both weights can actually draw, minus the no-pixel categories, adds **4,772
codepoints** with **zero donor bytes**; max codepoint moves `U+1FAF6 -> U+1FBF9` (+259 index entries,
~2 KB/face). **Zero CJK Unified and zero kana** — our faces have neither, so this cannot accidentally
answer the CJK question. What it buys: Latin Extended Additional 256 (Vietnamese), Greek Extended 233,
Arabic 210, Armenian 86, Georgian 83, Hebrew 42, Braille 256, box-drawing 128, math 120, General
Punctuation 71, Currency 32.

Per-block coverage by BOTH weights, measured:

| block (minus `Default_Ignorable`) | covered | gap |
|---|---|---|
| General Punctuation `2000-205F` | 85 / 86 | `{U+2029}` |
| **Currency `20A0-20BF`** | **32 / 32** | none |
| Letterlike `2100-214F` | 63 / 80 | 17 |
| Arrows `2190-21FF` | 74 / 112 | 38 |
| Math Ops `2200-22FF` | 120 / 256 | 136 |

The first two matter for a Russian-speaking user more than the Greek that shipped: **today an em
dash, every curly quote, the ellipsis and the ruble sign fold to `U+FFFD` in a name and draw as a
fallback box in chat**, while all seven text faces carry them.

---

## 3. Live defects this pass found (neither is caused by the upgrade)

### 3.1 One build, two drawable repertoires, chosen by the player's RHI

Because DX11's init sets `RendererHasTextures` unconditionally and DX12's legacy init strips it
(§2.4), a straight port ships **DX11 with the dynamic atlas** (ranges dead, draws all 8,148) and
**DX12 without it** (preload, draws 2,517). The selftest's verdict would mean different things per
RHI, and the commit would not be bisectable. The plan's C1 therefore clears the flag explicitly on
DX11 as a named transitional gate, retired in C2.

### 3.2 The "behaviour-identical" claim for a straight port is NOT established

Under 1.91.5 a codepoint absent from the atlas drew as `FallbackGlyph` and nothing else happened.
Under 1.92-legacy it **bakes** (§2.5), which grows the atlas while a legacy backend uploads only once
via `GetTexDataAsRGBA32`. Whether that yields a correct draw, a stale texture, or a broken one is
**not decidable by reading** — `PreloadedAllGlyphsRanges` and the `IM_ASSERT_USER_ERROR` pair at
`imgui_draw.cpp:2814-2817` clearly anticipate the situation but do not state the outcome.
**This is the first thing the next session must RUN.**

### 3.3 33 shipped invisible, uniqueness-bearing codepoints

`repertoire_ranges.inc` ships `{ 0x00020, 0x000AC }, { 0x000AE, 0x0024F }` — the only carve-out is
`U+00AD`. So **`U+0080-U+009F` and `U+00A0` are in the repertoire today**. A nickname of twenty
non-breaking spaces folds to itself, counts as unique, and shows nothing: the same defect
`repertoire.h:57` records as measured-and-closed for `U+034F`, reached from a direction arc D2 never
checked. `player_handshake_nick.cpp:119` `denied()` already blocks `c < 0x20 || c == 0x7F`, so the
nickname-path hole is 33, not 34.

`IsDefaultIgnorable` structurally cannot see it — `Cc`/`Zs` are a different Unicode property from
`Default_Ignorable`. **Both halves are needed**: subtracting them from the repertoire only makes such
a name *collide* (by C15 the arbiter still returns the original string plus a suffix); stripping them
from the DISPLAY requires `denied()`. Putting "renders no ink" in two owners with two vocabularies is
`[[lesson-one-capacity-expressed-in-three-places-will-disagree]]` again, so the invariant's home is
**one generated table** consumed by both — exactly the move `player_handshake_nick.cpp:110-117`
documents for `Default_Ignorable`.

**Measured, zero-build:** the four installs' persisted `net.nick` values are `Пельмень`,
`Пельменьмень2`, 19x`U+1F600`+`2`, `Пельменьмень4` — **no no-ink codepoints anywhere**, three
legitimate suffixes, so the new fold orphans none of them. (Four installs is not the user population.
The migration behaviour to document: a persisted nick containing a no-ink codepoint would be
sanitised on load — benign, since it had no pixels, but a visible rename.)

### 3.4 `g_pending[64]` — a live latent use-after-free

`overlay_backend_dx12.cpp:85` sizes the deferred-release table at 64, justified by a comment —
*"never seen: 64 entries vs a ~10-preview UI"* — and its overflow path (`:96-101`) `Release()`s
immediately, which the comment itself names as a use-after-free risk. The unbounded producer is not
the slot table: **every `CreateTexture` queues a staging buffer with `slot = 0` (`:525`)**, and
staging entries are not bounded by slot count at all, so N creates inside one fence window is N
entries. It is the fifth-site capacity pattern — one set bounded in two units in two places
(`kTextureSlots = 256` vs `g_pending[64]`). **The fix is to remove the fixedness, not to pick a bigger
number**, and it is its own commit with its own attribution, independent of the upgrade.

---

## 4. The plan (DESIGN; nothing built)

Six `/qf` rounds, every one of which corrected something. Ordering is load-bearing.

- **P0 — `g_pending` loses its fixed size.** §3.4. Live latent defect, own commit, own attribution.
  Not an upgrade precondition.
- **C1 — pin -> `v1.92.9` + the 3-file port + explicitly clear `RendererHasTextures` on DX11**
  (named transitional, retired in C2), so one build has ONE repertoire regardless of RHI. **The
  behaviour-identity claim must be MEASURED, not asserted** (§3.2); the DX11 gate stands or falls on
  that run. Also verify the pre-`NewFrame` ordering — `BringUp` calls `fonts::Load()` before
  `ImGui_ImplWin32_Init` (`imgui_overlay.cpp:295`).
- **C2 — DX12 `InitInfo` + our alloc/free callbacks + ONE unified free list** (the hard-coded
  "slot 0 = ImGui font" reservation retires), ImGui's `Free` routed through `QueuePendingRelease`,
  flag ON for both RHIs, **TOGETHER WITH** the regenerated fold table (+4,772; `BASE_RANGES` retires
  per RULE 2; the no-ink subtraction closing §3.3; the `Default_Ignorable` justification rewritten)
  **and** the provenance assertion of §5. Merged deliberately: in the flag-off regime the atlas
  preloads the fold table, so **fold == render at every commit boundary and the eager +4,772 bake
  cost never exists in any commit.**
- **C3 — the per-size mechanism swap.** `PushFont(font, px)` / `style.FontSizeBase` REPLACING the
  scale-change `Clear()`+rebake. **This is not a deletion.** Our roles bake via
  `AddFontFromMemoryTTF(..., px, ...)` and the UI pushes fonts, never sizes, so removing the rebake
  would leave every surface drawing at the OLD pixel size. Only then retire `io.Fonts->Build()`
  (`fonts.cpp:350`) and its timing log, against evidence. Retire the three obsolete-shim call sites
  of §2.3 here too (RULE 2).
- **Instrument** — the font selftest ported to the baked lookup, **relabelled** per §2.6, looped over
  roles (dedup-aware, reporting DISTINCT faces), RED = `U+4E00`, the per-process `static bool done`
  latch fixed to follow a rebake instead of describing only the boot atlas, and its log line stating
  that asking MUTATES the atlas.

### The confirmation instrument for C3

Launch at two resolutions, log each role's BAKED px + atlas identity/dimensions, screenshot. Same
drawn size across resolutions => the rebake was load-bearing; correctly scaled text with no `Clear()`
=> genuinely dead. `mp.py` already does windowed launches and `_capture_window`. **This is a
CONFIRMATION instrument, not a decision gate** — C3's mechanism swap is already settled by code — and
**the 1.91.5 baseline must be captured BEFORE C1** or there is no control.

---

## 5. The fold's expiry, and why it must be machine-enforced

Under OS fonts, two visibly different CJK names would fold to one sentinel and one would take a
numeric suffix — **arc D2's invariant inverted** ("the sentinel must BE the character the pixels
show"). The root: **the sentinel mapping is a function of coverage, and coverage stops being a build
constant.** At that arc the fold has to move off coverage entirely, onto a machine-independent
normalisation, and the sentinel mechanism retires.

A prose note is the wrong enforcement — this repo already turns this class of claim into a build
failure (`EXPECTED_BASE_GAP` refuses a silently-shrunk table, which is how the Greek gap got caught).
The trip-wire: **every `AddFont*` goes through one wrapper recording provenance
(`Embedded | SystemFallback | OsSupplied`), and an assertion fires if any `OsSupplied` source exists
while the fold is coverage-based.** A naive "sources == embedded set" assertion would hard-fail the
legitimate RCDATA-failure fallback at `fonts.cpp:369-390`, which `:243`/`:377` already accept as a
logged fold != render divergence.

---

## 6. Still unmeasured, knowingly

All runtime behaviour. Specifically: §3.2's legacy-regime on-demand bake outcome; whether
`ImGuiFreeTypeBuilderFlags_LoadColor` still yields COLOURED emoji under per-size baking; which
texture FORMAT 1.92 picks (our emoji need RGBA32, not Alpha8); whether
`ImGui_ImplDX12_UpdateTexture` contends with our own `CreateTexture` upload (own command list +
fence, `overlay_backend_dx12.cpp:505-525`); the per-size bake cost when five roles at different px
each demand their own `ImFontBaked`; and `g_pending`'s steady state once ImGui is a second producer.
**The ported selftest was compiled and linked, never run** — which texture a glyph's UV addresses
after a repack, with two textures live, is exactly the sort of thing not to infer.

**What this upgrade delivers: no CJK and no OS fonts.** It is the precondition. The prior session's
own dynamic-atlas probe (`votv-arc-d-gate-measurements-2026-07-28.md` M2) priced 3,000 drawn hanzi at
**8 MB / 82 ms at x1.0** against the eager path's 64-256 MB — that is the size of the prize, and it is
one arc away, not in this one.

Related: `[[project-imgui-192-upgrade-measured-2026-07-30]]`,
`votv-bake-everything-atlas-cost-2026-07-29.md` (the eager price list + the MTA font architecture),
`votv-arc-d-gate-measurements-2026-07-28.md` (RF3, superseded on the "structural" framing),
`votv-nickname-arbitration-roster-id-DESIGN-2026-07-27.md` (arc D2 + the fold),
`docs/VERSION_MIGRATION.md` §11 (the UE4SS decision + tripwire ledger).
