# ImGui 1.91.5 -> 1.92.9: the measured migration, and the plan the `/qf` left standing

**Status: MEASURED (compile + link, by a reverted spike) + §3.2 RUN. The UPGRADE ITSELF IS STILL
UNBUILT** — submodule remains pinned `v1.91.5` (`f401021d5`). Shipped from this plan so far: **P0
only** (`af234c08`), plus the arm-L instrument (`fcae169e`).
Logs: `build/imgui1929_pricing.log` (pre-port errors), `build/imgui1929_pricing2.log` (clean link),
`build/imgui192_armL.log` (the §3.2 run). Snapshot: `build/imgui1929/` (a 1.92.9 checkout).

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

**And on-demand baking is NOT gated on the flag** — `ImFontBaked_BuildLoadGlyph`
(`imgui_draw.cpp:4562-4571`) checks only `atlas->Locked` and `ImFontFlags_NoLoadGlyphs`; the
per-source accept test uses `GlyphExcludeRanges`, never `GlyphRanges`.

**CORRECTED 2026-07-30 by the §3.2 run: right about the flag, wrong about the consequence.**
`UpdateFontsNewFrame` (`imgui.cpp:9089-9094`) turns the missing flag INTO `atlas->Locked = true`,
and `UpdateFontsEndFrame` (`imgui.cpp:6173-6175`) clears it. So `Locked` *is* the flag, one
indirection away, and the legacy regime does not demand-bake while drawing. What it does NOT cover
is the gap between `EndFrame` and the next `NewFrame` — see §3.2.

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

### 3.2 RUN AND ANSWERED (2026-07-30) — drawing is identical; our own selftest is the break

The open question was whether a straight port draws, stalls or breaks when a codepoint outside the
preload arrives. **Measured** by `tools/probes/atlas_probe/atlas_probe_192.cpp` arm L (commit
`fcae169e`, log `build/imgui192_armL.log`), on the REAL atlas — same three deduped faces, same
per-role px, same generated repertoire, same shipped donor as `ui::fonts::Load`:

| | measured |
|---|---|
| `atlas->Locked` inside every legacy frame | **1** |
| 7 out-of-repertoire codepoints drawn (em dash, ellipsis, both curly quotes, ruble, numero, arrow) | **not baked** — glyph total stays 7,557 |
| `TexIsBuilt` after drawing them | **1** (unchanged) |
| texels diverging from the single upload | **0** |
| witness UVs moved (`A`, U+0410, U+FFFD, U+1F600) | **0** |
| ImGui errors raised | **none** |

**So a straight port IS behaviour-identical for drawing: the character renders `FallbackGlyph`,
exactly as 1.91.5 did.** That also makes C1's DX11 flag-clear measured-safe rather than merely
plausible.

**The one real break is ours, and it is a C1 blocker.** `Locked` holds only `NewFrame..EndFrame`.
Outside a frame it is 0, and there `FindGlyphNoFallback` **bakes**: measured `glyphs 7557 -> 7558`
and **`TexIsBuilt` 1 -> 0**, after which every subsequent legacy frame raises the
`imgui_draw.cpp:2815` user error — permanently, because nothing re-uploads.
`ui/fonts.cpp:265,285,287` is exactly that call, issued from `Load()`, which `imgui_overlay.cpp:295`
runs before `ImGui_ImplWin32_Init` with no frame in scope at all — and its RED case is deliberately
an ABSENT codepoint. Under 1.91.5 the same query was a pure read. **The plan's "relabel the
selftest" bullet therefore moves from cosmetic to blocking**, and the fix is not a relabel: the
selftest must stop asking the atlas a question that mutates it.

The first version of this arm inspected after `Render()`, in the unlocked gap, and reported "the
glyph bakes on demand" — it had measured its own instrument.
`[[lesson-querying-a-lazy-cache-populates-it]]`, now with a victim.

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
  Not an upgrade precondition. **DONE 2026-07-30, `af234c08`** — `std::vector` of live entries,
  compacted per frame; the overflow branch + `g_pendingCount` retired (RULE 2). Links clean, NOT
  verified at runtime (DX12-only path; a default LAN smoke launches DX11).
- **C1 — pin -> `v1.92.9` + the 3-file port + explicitly clear `RendererHasTextures` on DX11**
  (named transitional, retired in C2), so one build has ONE repertoire regardless of RHI. **The
  behaviour-identity claim is now MEASURED** (§3.2): the legacy regime locks the atlas, so drawing is
  identical and the DX11 clear is safe. **But C1 no longer ships without the selftest fix** — the
  boot selftest queries the atlas with no frame in scope, which BAKES and flips `TexIsBuilt`, turning
  every later frame into an ImGui user error. Also verify the pre-`NewFrame` ordering — `BringUp`
  calls `fonts::Load()` before `ImGui_ImplWin32_Init` (`imgui_overlay.cpp:295`); that ordering is
  precisely why the selftest sits in the unlocked gap. Fold in the two one-liners from §5b
  (`CmdListsCount` -> `CmdLists.Size`; `AddFontDefaultBitmap()` + `IMGUI_DISABLE_DEFAULT_FONT_VECTOR`,
  worth 14,562 B).
- **C2 — SPLIT IN TWO once the DX12 half was measured.** The design's reason for merging was that
  the *fold table* must land with the *flag flip*, so fold == render at every commit boundary. That
  constraint binds C2b only; the descriptor plumbing changes no repertoire and is separately
  verifiable, so keeping them together would have made one unbisectable commit for no benefit.
  - **C2a — DX12 `InitInfo` + our alloc/free callbacks + ONE unified free list.** DONE 2026-07-30.
    The hard-coded "slot 0 = ImGui font" reservation retires: it could only ever describe ONE ImGui
    texture and 1.92 keeps two alive across a repack. `TexSlot` gained an explicit
    `Owner {Free, Ours, Imgui, Pending}` because the old (res, gpuPtr) encoding had no room for
    "occupied by a resource we must never release". ImGui's `Free` is fence-deferred through
    `QueuePendingRelease` like ours. Index 0 stays non-allocatable: it is both the existing "no slot"
    sentinel and the descriptor `SrvAlloc` hands out if the pool is exhausted, since ImGui's callback
    has no failure channel and will write an SRV to whatever it is given. Flag still OFF.
  - **C2b — flag ON for both RHIs** (the DX11 clear retires), **our own bounded texture servicing**
    via `ImDrawData::Textures = nullptr` (see §6 — ImGui's own path has an INFINITE wait), **TOGETHER
    WITH** the regenerated fold table (+4,772; `BASE_RANGES` retires per RULE 2; the no-ink
    subtraction closing §3.3; the `Default_Ignorable` justification rewritten) **and** the provenance
    assertion of §5. Merged deliberately: in the flag-off regime the atlas preloads the fold table,
    so **fold == render at every commit boundary and the eager +4,772 bake cost never exists in any
    commit.**
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

## 5b. What the v1.92.9 release page adds (checked against the 1.92.9 source, 2026-07-30)

Four entries land on our code; none was visible in the port diff.

- **`ImFontAtlas::Clear()`/`ClearFonts()` documented as "unlikely to be useful nowadays"**, plus
  improved recovery for `ClearFonts()` called *during rendering* (`imgui.h:3745`, `:3586`).
  `fonts.cpp:301` opens `Load()` with `io.Fonts->Clear()`, and `Load()` is re-entrant on every
  scale/family change — i.e. exactly the mid-render case upstream patched *recovery* for. This is
  upstream corroboration for **C3**, which retires that idiom.
- **`ImGuiItemFlags_LiveEditOnInputScalar` now defaults OFF** (`imgui.h:1259-1262`, changelog
  `imgui.cpp:398-402`). We have three `InputInt`s — `dev_menu.cpp:100/103/106`, the day/hour/minute
  clock setters. Before: typing "12" in the hour field wrote 1 then 12. After: 12 on validation.
  For a world-clock setter that is a fix, but **it is a silent UI behaviour change a compile cannot
  see**. `InputText` (the nickname + host fields) is unaffected.
- **`IMGUI_DISABLE_DEFAULT_FONT_VECTOR` is a free DLL-size lever**, worth **14,562 compressed bytes**
  (`imgui_draw.cpp:6560` — a fifth of the port's +71 KB). 1.92 embeds a second, VECTOR default font
  beside ProggyClean (9,583 B, `:6371`). `AddFontDefault` is now a size heuristic
  (`imgui_draw.cpp:3180-3186`: vector when expected size >= 15, else bitmap), and our last-resort
  `fonts.cpp:394` sits at 13 -> bitmap, so today the vector font is dead weight. **Conditional:** if
  C3 sets `style.FontSizeBase` >= 15 the heuristic flips. Make it unconditional by calling
  `AddFontDefaultBitmap()` explicitly, which is upstream's own advice at `:3178`.
- **`ImDrawData::CmdListsCount` obsoleted** — used in OUR code at `overlay_backend_dx12.cpp:413` (a
  log line). Compiles, so it is §2.3-class RULE-2 baggage; one-line fix, `-> CmdLists.Size`.

Two lower-risk watch items: `CalcTextSize()` width rounding was refined to avoid 1-px differences,
and we word-wrap chat through `CalcWordWrapPositionA` — so pre-upgrade chat-layout screenshots are
not pixel-comparable. And destroying a context now asserts the atlas has no remaining references
(`imgui.cpp:4539`), a new assert on the device-reset path we exercise.

## 6. Still unmeasured, knowingly

All remaining runtime behaviour. §3.2 is now RUN (see above), and two more items closed with it:

- **`LoadColor` DOES still paint under per-size baking** — measured `UseColors=1`, `Colored=3`,
  **2,600 non-greyscale texels**; without the flag `emoji-visible=0`, confirming `fonts.cpp:186`'s
  comment still holds in 1.92.
- **Which format 1.92 picks: RGBA32**, driven by `UseColors` when a colour source is present. And
  **`TexDesiredFormat = ImTextureFormat_Alpha8` is a TRAP**: measured `Colored=3` but
  **`non-greyscale=0`** at 0.06 MB vs 0.25 MB. It buys 4x memory by silently greying every emoji.
  **Do not set it.**

**The `UpdateTexture` question is answered, and the answer is not contention.**
`ImGui_ImplDX12_UpdateTexture` ends in `WaitForSingleObject(bd->FenceEvent, INFINITE)`
(`imgui_impl_dx12.cpp` ~:565) — an **unbounded** GPU wait on the calling thread, which is our render
thread. This codebase bounds every wait it owns at `kFenceWaitMs = 2000` and takes the
device-removed path on timeout, precisely so a TDR disables the overlay instead of hanging VOTV. And
with the dynamic atlas ON, `UpdateTexture` runs whenever a glyph is demand-baked — i.e. eventually
**on arbitrary remote text**. That is the on-demand thread's count-3 "remote amplifier" reappearing
one layer down: not atlas cost, but an unbounded wait a peer's chat message can trigger.

The answer is not to accept it: `imgui_impl_dx12.h:75` documents `ImDrawData::Textures = nullptr`
for exactly this ("if you need to precisely control the timing of texture updates"), and we already
own a bounded, fence-gated uploader. **One upload mechanism with one bound, not two with different
failure modes.** This is now part of C2b, not an open question.

A related measured fact that shaped C2a: the legacy `ImGui_ImplDX12_Init` **created its own command
queue** (`imgui_impl_dx12.cpp:973`, `commandQueueOwned = true`). Passing `InitInfo.CommandQueue =
g_queue` would therefore have been a behaviour change — moving ImGui's uploads *and* that infinite
wait onto the game's presenting queue. C2a keeps a dedicated queue for parity.

Still open: the per-size bake cost when five roles at different px
each demand their own `ImFontBaked`; and `g_pending`'s steady state once ImGui is a second producer
(the cap is gone as of `af234c08` — §3.4 — so the question is now the rate, not the ceiling).
**The ported selftest was compiled and linked, never run** — which texture a glyph's UV addresses
after a repack, with two textures live, is exactly the sort of thing not to infer.

Also unmeasured and now REQUIRED by §3.2: the replacement selftest. Its RED case must stay
discriminating (a codepoint no face carries) without asking the live atlas — `build_repertoire.py`
already knows every face's cmap at generate time, so the honest question is a cmap fact, not an
atlas one.

**What this upgrade delivers: no CJK and no OS fonts.** It is the precondition. The prior session's
own dynamic-atlas probe (`votv-arc-d-gate-measurements-2026-07-28.md` M2) priced 3,000 drawn hanzi at
**8 MB / 82 ms at x1.0** against the eager path's 64-256 MB — that is the size of the prize, and it is
one arc away, not in this one.

Related: `[[project-imgui-192-upgrade-measured-2026-07-30]]`,
`votv-bake-everything-atlas-cost-2026-07-29.md` (the eager price list + the MTA font architecture),
`votv-arc-d-gate-measurements-2026-07-28.md` (RF3, superseded on the "structural" framing),
`votv-nickname-arbitration-roster-id-DESIGN-2026-07-27.md` (arc D2 + the fold),
`docs/VERSION_MIGRATION.md` §11 (the UE4SS decision + tripwire ledger).
