# Arc D — the five gate measurements, RUN (2026-07-28)

**Status: MEASURED.** These are the five read-only measurements
`votv-nickname-arbitration-roster-id-DESIGN-2026-07-27.md` §9b.7 named as the gate on building
arcs B+D. Four are complete; one (M1) is complete for the question it had to answer and
explicitly incomplete for a question that no longer needs answering — said plainly in M1 below.

Every number here came off this machine on 2026-07-28. Nothing is carried forward from the
design pass. Three of the design's own claims are **falsified** and are marked as such.

Instruments, both committed and re-runnable: `tools/probes/atlas_probe/` — a standalone
imgui+freetype harness that never touches the shipping build tree (`atlas_probe.cpp` for 1.91.5,
`atlas_probe_192.cpp` against a v1.92.8 worktree of the submodule) and `measure_donors.py` for §M5.

---

## Verdict first

**The fork in §9b.7 item 1 — (A) bake the repertoire on 1.91.5 vs (B) upgrade to 1.92 — is
decided by measurement, not by judgement: (B).**

| | arm A — 1.91.5, baked up front | arm B — 1.92.8, on demand |
|---|---|---|
| atlas for a realistic load (a few names' worth of hanzi) | **16.0 MB** (2048x2048, x1.0) | **0.25 MB** (512x128, x1.0) |
| same at x2.0 (4K) | **64.0 MB** (4096x4096) | 0.50 MB (512x256) |
| cost paid when the user changes scale or a role family (F1) | **139–416 ms full re-bake, every time** | 0.4–0.7 ms |
| cost paid when nobody types a hanzi | the same 16–64 MB and 139–416 ms | nothing |
| worst case measured (4 faces, CN+JP+emoji, x2.0) | 64 MB / 559 ms | 16 MB / 122 ms, and only if 3000 distinct hanzi are actually drawn |
| price to us | 0 (already vendored) | the API census in M2 — 11 call sites, of which **one is structural** (DX12 descriptors) |

Arm A does not *fail* — no `ImFontAtlas::Build()` returned false anywhere in the matrix, so the
design's feared "fontless UI" failure mode did not materialise. It is simply 64x the VRAM and
~300x the hitch for the same feature, paid by every user including the ones who never type a
non-Latin character.

---

## M3 — the `ImWchar` census

**The census is four `const ImWchar*` sites, all in one file, all type-transparent**
(`ui/fonts.cpp:120,134,144,188` — a parameter, a parameter, a parameter, and
`GetGlyphRangesCyrillic()`'s return). Nothing else in `src/` names the type.

**The design's claim that "both overlay halves" are `ImWchar` seams is FALSE.**
`overlay_backend_dx11.cpp`, `overlay_backend_dx12.cpp`, `overlay_backend_dx12_capture.cpp` and
`imgui_overlay.cpp` contain zero occurrences of `ImWchar`, `ImFontAtlas`, `ImFont` or
`GetTexDataAs*`. They are affected only through struct sizes, which is an ODR question, not a
porting question.

What the define actually does, measured in the vendored source:

| Effect | Where | Cost |
|---|---|---|
| `ImWchar` 16 -> 32 bit | `imgui.h:270-278` | ODR-critical: every TU including `imgui.h` must agree |
| `IM_UNICODE_CODEPOINT_MAX` 0xFFFF -> 0x10FFFF | `imgui.h:2512-2515` | lifts the astral drop at `imgui.cpp:1512` and the filter at `imgui_widgets.cpp:4264` |
| `ImFont::IndexAdvanceX[]` + `IndexLookup[]` sized to **max codepoint + 1** | `imgui_draw.cpp:3656-3669` (`GrowIndex(max_codepoint + 1)`) | this is the real memory cost — see the table below |
| `ImFont::Used4kPagesMap` 2 -> 34 bytes | `imgui.h:3446` | negligible |
| `ImFontGlyphRangesBuilder::UsedChars` 8 KB -> 136 KB | `imgui.h:3268` | transient, only if we use the builder |

Measured index-table cost across the resident set (from `atlas_probe`, default families):

| repertoire | max codepoint | 16-bit | 32-bit |
|---|---|---|---|
| today (Latin + Cyrillic) | U+0A69F | 0.26 MB | 0.35 MB |
| + CN common | U+0FFEE | 1.12 MB | 1.50 MB |
| + CN + emoji | U+1FAF6 | 1.12 MB (astral dropped) | **2.97 MB** |

**Mechanism (structural, and the design did not have it):** `third_party/imgui` is a **git
submodule** pinned at v1.91.5 (`.gitmodules`). `imconfig.h` therefore **cannot** be edited —
the edit would be a dirty submodule no fresh clone reproduces. The define must ride a compile
definition on the `imgui` target, exactly as `IMGUI_ENABLE_FREETYPE` already does
(`src/votv-coop/CMakeLists.txt:100`). ODR is safe because `votv-coop` is the **only** target
that links `imgui` (`:592`), and the definition is `PUBLIC`.

**The WM_CHAR path becomes correct, not broken.** `imgui_impl_win32.cpp:743-749` forwards UTF-16
code units to `ImGuiIO::AddInputCharacterUTF16`, which already pairs surrogates
(`imgui.cpp:1490-1512`) and discards the result *only* under `IM_UNICODE_CODEPOINT_MAX == 0xFFFF`.
With the define on, astral typing composes; with it off, it is dropped at that one line.

---

## M1 — the atlas drill

Run with `tools/probes/atlas_probe`, which reproduces `ui::fonts::Load()`: the same four
embedded families, the same five roles at 16/18/16/16/16 px, the same `(family, px*4, bold)`
dedup, and the same FreeType configuration (`FT_DISABLE_PNG/HARFBUZZ/BROTLI` ON). Matrix:
{default families, worst-case families} x {x1.0, x1.5, x2.0} x five repertoire tiers x
{16-bit, 32-bit `ImWchar`} = 60 bakes.

**The resident set is 3 faces at default settings, not 5** — Fixedsys 16 regular (Menu + Toast),
Fixedsys 18 (Chat), Roboto 16 (Net + Nameplate); `kNameplatePx == kUiPx == 16` so Net and
Nameplate dedup. Worst case a user can select is 4.

Selected rows (default families, 32-bit; full tables in the probe output):

| repertoire | x1.0 | x1.5 | x2.0 |
|---|---|---|---|
| today (Latin + Cyrillic) | 512x512, **1.0 MB**, 5.7 ms | 512x1024, 2.0 MB, 6.2 ms | 512x1024, 2.0 MB, 6.5 ms |
| + CN common | 1024x2048, 8.0 MB, 84.9 ms | 2048x2048, 16.0 MB, 140 ms | 2048x4096, 32.0 MB, 186 ms |
| + CN + JP | 2048x2048, 16.0 MB, 139 ms | 2048x4096, 32.0 MB, 236 ms | 2048x4096, 32.0 MB, 313 ms |
| + CN + emoji | 2048x2048, 16.0 MB, 139 ms | 2048x4096, 32.0 MB, 213 ms | **4096x4096, 64.0 MB, 287 ms** |
| + CN + JP + emoji | 2048x2048, 16.0 MB, 191 ms | 2048x4096, 32.0 MB, 304 ms | **4096x4096, 64.0 MB, 416 ms** |

- **`Build()` never returned false.** The design's named failure mode ("presents as a fontless
  UI") did not occur at any tier or scale. It is not the reason to reject arm A.
- **The 16-bit build silently drops astral emoji**: same tier, `+CN +emoji` at x1.0, 11,125
  glyphs at 16-bit vs 14,791 at 32-bit. The missing 3,666 are the plane-1 emoji.
- Every one of those milliseconds is paid **on the live F1 path**, because `fonts::Load()`
  clears and re-bakes the whole atlas on any scale or family change (`ui/fonts.cpp:176`).

**What was NOT measured, and why that is now fine.** The design asked for this drill *in-game*,
over the live F1 path, with the DX12 texture tear as an axis. The in-game half was not run:
its purpose was to price arm A's rebuild, and the offline numbers already reject arm A by a
64x margin on a metric the game cannot improve. If arm A is ever revived, the in-game half is
still owed.

**The design's "colour costs 4x VRAM for ALL faces" is FALSE.** `ImGuiFreeTypeBuilderFlags_LoadColor`
switches the *build buffer* to RGBA32 (`imgui_freetype.cpp:665-669`), but both our halves upload
via `GetTexDataAsRGBA32` (`imgui_impl_dx11.cpp:330`, `imgui_impl_dx12.cpp:310`), which allocates
and keeps an RGBA32 copy regardless (`imgui_draw.cpp:2501-2523`). The GPU texture is already
4 B/px today. Colour is VRAM-neutral and saves the conversion pass.

---

## M2 — the 1.92 classified diff

Priced against **our call sites**, not by line count — the error named in
`[[lesson-price-a-dependency-by-repair-history-not-by-line-count]]`. Diff is
`v1.91.5..v1.92.8` in the submodule; breaking list from `docs/CHANGELOG.txt` §1.92.0.

| # | Break | Our sites | Class |
|---|---|---|---|
| 1 | `PushFont(font)` gains a required size | `chat_input.cpp:81`, `net_stats_panel.cpp:178` | mechanical, 2 |
| 2 | `ImFont::FontSize` removed (`LegacySize` replaces it) | `fonts.cpp:234`, `hud.cpp:111`, `hud.cpp:327` | mechanical, 3 |
| 3 | `CalcWordWrapPositionA(scale,…)` -> `CalcWordWrapPosition(size,…)` | `hud.cpp:111`, `hud.cpp:327` | collapses into #2 — those sites pass `px / font->FontSize` today and would pass `px` |
| 4 | `SetWindowFontScale()` obsoleted | `loading_screen.cpp:74,78` | mechanical, 2 |
| 5 | `ImTextureID` -> `ImTextureRef` in `Image*`/`AddImage*` | `skins_panel.cpp:99-100` | 1; `ImTextureRef` has an implicit ctor from `ImTextureID` |
| 6 | `GetGlyphRangesXXX()` obsoleted (ranges unnecessary) | `fonts.cpp:188` | 1 — and this is the *win*, not a cost |
| 7 | `GetTexDataAsRGBA32` / `Build` / `SetTexID` / `IsBuilt` obsoleted | **zero** sites in our code | free — the vendored backends upgrade with the submodule |
| 8 | `CalcTextSizeA` no longer thread-safe (may load glyphs) | `hud.cpp:95,158,223,380,438`, `net_stats_panel.cpp:109` | 6 sites, all on the present thread — needs confirming, not changing |
| 9 | **DX12 descriptor allocation** | `overlay_backend_dx12.cpp:223`, `:307` | **structural, the only one** |

**#9 in full**, because it is the whole price. 1.92.8 still accepts the legacy
`ImGui_ImplDX12_Init(device, count, fmt, heap, cpu, gpu)` signature we call, but wraps it in an
allocator that asserts on the *second* texture:
`imgui_impl_dx12.cpp:894` — `"Only 1 simultaneous texture allowed with legacy ImGui_ImplDX12_Init() signature!"`.
A dynamic font atlas creates and destroys textures at runtime, so that assert is on the path.
The fix is bounded and half-built already: our DX12 half owns a heap of `kTextureSlots + 1`
descriptors with slot 0 reserved for the font (`overlay_backend_dx12.cpp:258`), i.e. we already
have the slot allocator — it has to be exposed as `SrvDescriptorAllocFn` / `SrvDescriptorFreeFn`
on `ImGui_ImplDX12_InitInfo`. DX11 has no equivalent constraint.

**`ImWchar` stays 16-bit by default in 1.92.8** (`imgui.h:274-278` at that tag). The upgrade does
**not** subsume M3 — `IMGUI_USE_WCHAR32` is still required for emoji, on either arm.

### The measurement behind the verdict

`tools/probes/atlas_probe/atlas_probe_192.cpp` runs the same three faces against a v1.92.8
worktree, declares `ImGuiBackendFlags_RendererHasTextures`, and draws progressively larger text:

| drawn so far | donors absent | donors merged (x1.0) | donors merged (x2.0) |
|---|---|---|---|
| latin + cyrillic | 512x128, 0.25 MB | 512x128, 0.25 MB, 0.6 ms | 512x128, 0.25 MB, 0.7 ms |
| + 20 hanzi | 512x128, 0.25 MB | **512x128, 0.25 MB, 0.4 ms** | 512x256, 0.50 MB, 0.7 ms |
| + 200 hanzi | 512x128, 0.25 MB | 512x256, 0.50 MB, 3.8 ms | 512x1024, 2.0 MB, 7.9 ms |
| + 3000 hanzi | 512x128, 0.25 MB | 1024x2048, 8.0 MB, 82 ms | 2048x2048, 16.0 MB, 122 ms |
| + 40 emoji | 512x128, 0.25 MB | +0.0 MB, 1.4 ms | +0.0 MB, 1.3 ms |

The "donors absent" column is the positive control: with no donor merged, the glyph count stays
flat at 144 across every load, i.e. absent codepoints cost nothing and the harness is measuring
what it claims to measure.

Two honest caveats. The 3000-hanzi rows are a stress load nobody produces by typing names — they
exist to show where arm B *would* converge on arm A. And the x2.0 3000-hanzi row reports fewer
glyphs (6,341) than x1.0 (9,141), which is 1.92's own bake eviction, not something this probe
established; it is not load-bearing for the verdict.

---

## M4 — the entry ladder

**Rung 1 (clipboard) is GREEN at the ImGui layer, and no rung 2 blocker was found there.**

- No clipboard override exists in our tree — `Platform_GetClipboardTextFn`/`Set` are never
  assigned outside imgui's own defaults (grep over `src/ui/`), so the path is
  `imgui.cpp:14784-14805`: `GetClipboardData(CF_UNICODETEXT)` -> `WideCharToMultiByte(CP_UTF8)`.
  Codepoint-transparent, astral included.
- The nick field is `server_browser.cpp:118` `InputText("##nick", g_nick, sizeof(g_nick))`, whose
  internal storage in 1.91.5 is UTF-8 (`ImGuiInputTextState::TextA`), so the field is
  byte-transparent even at 16-bit `ImWchar`; the drop is at render, not at storage.
- **Live confirmation is still owed** — that a Japanese player can type via IME into a window
  whose input we capture was not tested in-game. Rung 1 passing on the code path is what makes
  rung 2 not a blocker on paper, not a substitute for the drill.

**The ini read encoding: two measured defects.**

1. `config.cpp:165` opens with `_wfopen_s(path, L"r")` — **text mode**. UTF-8 bytes survive
   (they are read as bytes), but CRLF translation applies and a 0x1A byte truncates the file.
2. **No BOM handling exists anywhere** in `coop/config/` (grep for `BOM`/`0xEF` returns nothing).
   A Notepad-saved UTF-8 `multivoid.ini` therefore carries `EF BB BF` into the first line. That
   line is a comment in our generated file, so today the blast radius is one comment — but the
   moment a user's ini starts with a key, that key silently disappears.

**The widen census is bigger than the design's.** §9b.4 names two per-byte widens. Measured
today there are **seven**: six of the `std::wstring(s.begin(), s.end())` idiom
(`config.cpp:509`, `player_inventory_sync.cpp:100`, `client_model.cpp:32`, `local_body.cpp:83`,
`player_handshake_prefs.cpp:111`, `:132`) plus one written as an explicit loop that the idiom
grep does not match (`harness/session_runtime.cpp:380-384` — and note the design's path for that
file, `src/coop/harness/`, is stale; it lives at `src/harness/`). Only the first and the last are
nick-lane; the other five are skins, guid and prefs. They are named here so the codec commit
knows its true blast radius — see `[[lesson-census-the-operation-kind-not-only-the-sites]]`.

---

## M5 — the donor files

Measured with fontTools 4.63.0: each donor subset to the codepoint set named, saved, and the
resulting file measured. The candidate hanzi sets are **Dear ImGui's own vendored sets**, so the
answer to "which hanzi set counts as common" is a defined list rather than a preference.

| Set | Definition | Codepoints |
|---|---|---|
| CN common | `ImFontAtlas::GetGlyphRangesChineseSimplifiedCommon()` | 2,500 ideographs + 849 base = **3,349** |
| JP | `ImFontAtlas::GetGlyphRangesJapanese()` | 2,999 ideographs + 737 base = **3,736** |
| union | both | **4,992** |

| Donor | License | Subset | Kept | **Measured bytes** |
|---|---|---|---|---|
| Noto Sans SC (wght=400 instance) | OFL 1.1 | CN common | 3,218 | **1,037,496** |
| Noto Sans JP (wght=400 instance) | OFL 1.1 | JP | 3,683 | **1,321,980** |
| Noto Sans SC | OFL 1.1 | union(CN,JP) | 4,813 | **1,583,628** |
| Noto Sans JP | OFL 1.1 | kana + CJK punctuation (U+3000-U+30FF) | 253 | **193,716** |
| Twemoji Mozilla v0.7.0 | MIT (code) + CC-BY 4.0 (art) | single-codepoint emoji | 1,356 | **905,404** |

Embedded-tier options, as DLL bytes (the donors merge into every face at *atlas* level, so the
bytes are paid once, not per family):

- kana + emoji, hanzi deferred to the pack: **~1.09 MB**
- CN common + emoji: **~1.94 MB**
- union + emoji: **~2.49 MB** — which lands inside D-b's accepted "~+2.5-3 MB".

**The emoji donor is confirmed usable.** Twemoji Mozilla v0.7.0 (1,474,284 bytes as shipped) is
**COLR version 0 + CPAL, with no CBDT, no sbix, no SVG** — exactly the format FreeType's
`FT_LOAD_COLOR` rasterises, and the one `FT_DISABLE_PNG=ON` does *not* block. 1,418 cmap
codepoints, 1,232 of them astral (hence M3).

Also measured, and worth recording because it is tempting and wrong: Windows 10's own
`seguiemj.ttf` (2,072,388 bytes) is likewise **COLR v0 + CPAL**, 1,962 codepoints. It would work
technically and costs zero DLL bytes — but it is not redistributable, it is machine-local, and
§9b.5 requires the acceptance predicate to derive from the *embedded* donors. Using it would make
which names a machine accepts depend on which Windows it is.

---

## What this changes in the design

1. §9b.7 item 1's fork resolves to **arm B**. §9b.5's "ranges must be baked up front" premise
   (which rested on 1.91.5) goes with it.
2. §9b.3's "colour costs 4x VRAM for all faces" — **delete**, falsified above.
3. §9b.7 item 3's "both overlay halves are `ImWchar` seams" — **delete**, falsified above; the
   real constraint is the submodule, which no round of the pass surfaced.
4. §9b.4's two-widen census becomes seven sites, with the `src/coop/harness/` path corrected.
5. §9b.5's "which hanzi set is common" is answerable: ImGui's own CN-common (3,349) or the
   CN+JP union (4,992), at 1.94 MB or 2.49 MB embedded with emoji.
6. A new dependency question the design never had: the 1.92 upgrade is now on arc D's critical
   path, and its structural cost is one DX12 descriptor-allocator change.

Still open, and NOT closed by this session: the live IME/clipboard drill in-game, and — if arm B
is taken — whether the 1.92 upgrade lands as its own commit ahead of arcs B+D (it should; it is a
substrate change with its own smoke, and burying it inside a nickname feature would make both
un-bisectable).
