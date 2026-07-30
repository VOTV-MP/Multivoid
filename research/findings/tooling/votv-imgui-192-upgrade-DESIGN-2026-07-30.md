# ImGui 1.91.5 -> 1.92.9: the measured migration, and the plan the `/qf` left standing

**Status: THE UPGRADE IS BUILT AND SMOKE-MEASURED ON BOTH RHIs. NOT hands-on.**
Submodule pinned **`v1.92.9`** (`01380c579`) since `b33aae30`. Shipped from this plan:

| step | commit | state |
|---|---|---|
| arm-L instrument (§3.2) | `fcae169e` | run; log `build/imgui192_armL.log` |
| **P0** `g_pending` unbounded | `af234c08` | links clean; DX12-only path, unexercised by a DX11 smoke |
| **C1** pin + port + selftest rebuild | `b33aae30` | baseline-vs-C1 i18n smoke, DX11 **and** DX12 |
| i18n smoke self-verifies each send | `c142d077` | fixed a coin-flip verdict |
| **C2a** DX12 `InitInfo` + unified pool | `780a93af` | DX12 smoke before/after |
| both stale imgui citations re-pointed | `683f8214` | comment-only; build clean |
| **the FLIP** (was C2b) | — | **NOT BUILT — design of record is §7** (10 `/qf` rounds, still no "that holds") |
| **C3** | — | **NOT BUILT.** §4; cannot precede the flip (§7.1) |

**The capability flag is deliberately CLEARED ON BOTH RHIs**
(`overlay_backend_dx11.cpp:98`, `overlay_backend_dx12.cpp:273`), so this build has ONE drawable
repertoire and **delivers no new glyphs yet**. That is the point of the sequencing, not an oversight.

Logs: `build/imgui1929_pricing{,2}.log` (the reverted spike), `build/imgui192_armL.log` (§3.2),
`build/baseline_i18n_1915.log` (the 1.91.5 control), `build/c1_i18n_1929{,_run2}.log`,
`build/c2a_dx12_smoke{,2}.log`. Snapshot: `build/imgui1929/`.

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

## 1. The originating ask, and what the 8-round design `/qf` settled

> Two `/qf` passes are recorded in this document and they are not the same pass. This section's
> **8-round DESIGN pass** produced the P0/C1/C2a/C2b/C3 sequencing of §4. A separate **10-round
> IMPLEMENTATION pass** (§7) then rewrote C2b and reversed its ordering; §7 wins wherever they
> disagree.

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

#### 2.7a The widened table, computed for real (2026-07-30) — and a hazard §2.7 would have shipped

§2.7's `+4,772` was an estimate off a "no-pixel categories" subtraction that was never spelled out.
Computed against the actual cmaps:

| quantity | measured |
|---|---|
| both-weight intersection | **8,148** (`regular ^ bold` = 0, as §2.6 said) |
| candidate = both-weight ∪ donor | **9,478** |
| no-ink, `Cc/Cf/Cs/Zl/Zp/Zs` minus `U+0020` | **105** |
| private-use in the candidate set | **1,778** |
| **new repertoire** | **7,595 cp, max U+1FBF9** → **+5,078**, not +4,772 |

The no-ink set derived from Unicode *general category* contains **all 33 codepoints of §3.3**
(`U+0080..U+009F` plus `U+00A0`) without naming any of them — an invariant, not a block list, which is
the form this repo already learned to prefer. `U+0020` is the sole `Zs` kept: every other space
character is a confusable **of** it, and space itself legitimately changes layout.

**THE HAZARD.** The obvious wider filter also subtracts `Cn` (unassigned) — and **412 codepoints in
the candidate set are `Cn` according to the generator machine's Python** (3.11 ships Unicode 14.0.0;
`U+1CC21`, the `U+1CD00` block and others were assigned in Unicode 16). Those are real glyphs our
fonts really draw. Subtracting them would (a) fold a visibly-distinct name to the sentinel, and worse
(b) make **the fold table a function of the generator's Python version** — so two builds cut from the
same commit on different machines would disagree about which names collide. That is precisely the
machine-dependence arc D2 exists to prevent, arriving through the toolchain instead of through the
font. **`Cn` is NOT subtracted**, and the regeneration gate must assert the resulting count and max
codepoint so a Unicode-table change fails the build instead of silently re-cutting the table.

## 2.7 The free widening this exposes (measured, unbuilt)

Asking for what both weights can actually draw, minus the no-pixel categories, adds **+5,078
codepoints** (fold set `2,517 -> 7,595`) with **zero donor bytes**; max codepoint moves
`U+1FAF6 -> U+1FBF9` (+259 index entries, ~2 KB/face).

> **CORRECTED 2026-07-30.** The headline said **+4,772**, which reproduces under no construction the
> generator's own tables can build — it was an unrecorded estimate, left standing above §2.7a's own
> correction of it. **§7.3 is the authoritative table**: it adds the range counts that decide which
> subtraction is even expressible as `GlyphExcludeRanges`, and records that the alternative frozen
> table gives +5,164. **Zero CJK Unified and zero kana** — our faces have neither, so this cannot accidentally
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

## 4. The plan (P0 + C1 + C2a AS-BUILT; C2b + C3 still DESIGN)

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
  - **C2b — SUPERSEDED BY §7.** This bullet's design was rewritten by a 10-round implementation `/qf`
    (2026-07-30). Three of its own load-bearing claims were measured false: that the servicing must
    precede the flip, that the extraction was needed at all, and that "fold == render at every commit
    boundary" holds post-flip. Read **§7**, not this bullet. Retained here only so the superseded
    reasoning is not re-derived: it merged the servicing with the flip, required a pool extraction
    first because it assumed ~150 LOC landing *inside* `overlay_backend_dx12.cpp`, and treated the
    no-ink table as a fold-side subtraction rather than as one table with two consumers.
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

---

## 7. THE FLIP — design of record (implementation `/qf`, 9 rounds, 2026-07-30)

**Status: DESIGN. Nothing here is built.** Supersedes §4's C2b bullet. The pass ran **10 rounds / 40
questions** and **twenty of the primary's claims were measured false**, including one where a
*correct* recorded number was wrongly "corrected". **Still no "that holds" verdict** — round 10 (run
2026-07-30 after the critic became reachable again) returned four material questions and **all four
landed**, one of them on this section's own framing. Rounds 5, 8 and 10 each reversed something
structural, so the tail of a pass is not decoration here.

Only one thing from this pass is committed: `683f8214`, the two stale imgui citations in
`fonts.cpp` (comment-only, measured, independent of the flip).

### 7.0 What a player sees the day this ships (measured, round 10)

The framing this section shipped with — *"the flip delivers zero new visible glyphs; CJK needs C3"* —
was **FALSE**, and it was false in the direction that undersells the user's own ask. It was true of
**s15's build**, which deliberately clears the flag, and it rode unexamined into the section that
describes deleting that clear.

Measured at HEAD, two ways:

```
repertoire_ranges.inc          161 ranges / 2,517 cp   <- what is baked, and folded, TODAY
  U+2014 em dash    in-fold=False        U+2026 ellipsis   in-fold=False
  U+2019 / U+201C   in-fold=False        U+20BD ruble sign in-fold=False
face cmaps (fontTools, assets/fonts/*)   UNION 9,478      [FSEX300 alone 5,992]
  em dash / curly quotes / ellipsis / ruble ....... present
  hebrew U+05D0 | thai U+0E01 | arabic U+0627 ..... present
  CJK U+4E00 | hangul U+AC00 ...................... ABSENT from every face
```

So the flip converts **+5,078 codepoints from fallback box into glyph, out of fonts already inside
the DLL, for zero new bytes** — the em dash, both curly-quote pairs, the ellipsis and the ruble sign
this user types daily, plus entire scripts (Hebrew, Thai, Arabic) the shipped faces already carry.
**CJK and Hangul remain boxes**, and only because no face contains them: that is the honest scope of
what C3 adds, and it is a *source* problem, not a flip problem. This commit is the first in the whole
saga that deletes boxes the user can point at.

### 7.1 The order, and why it inverted

The pass began with "hand-roll our own DX12 texture servicing, then flip". **Round 5 reversed it.**
Both backends service `ImDrawData::Textures` **ungated by the capability flag**
(`imgui_impl_dx12.cpp:236`, `imgui_impl_dx11.cpp:181`), and `imgui.cpp:5973` assigns
`draw_data->Textures = &g.PlatformIO.Textures` unconditionally — so upstream's DX12 path, INFINITE
wait included, **already runs today** at boot and twice per rescale, and had never been measured.
Every argument for the 200-LOC manager was a claim about the *post-flip* regime, so it was justified
only by what the flip enables. Worse, the leak it had to fix (§7.6) was one **it introduced itself**
by taking ownership of a resource ImGui currently frees correctly.

So: **flip first, with its instrument in the same commit.** Round 8 dissolved the separate
"measure later" step — the probe is ~15 lines, and shipping a regime before the instrument that can
see it is how the first data point comes from a build you cannot read.

`C3 cannot precede the flip` — not "riskier", *impossible*: `ImFontAtlasBakedGetOrAdd`
(`imgui_draw.cpp:5470-5479`) with `atlas->Locked` falls back to a closest-size match and otherwise
hits `IM_ASSERT(!atlas->Locked)` — **stripped under NDEBUG** — and returns NULL, so
`PushFont(font, px)` at an unbaked size gets a null baked.

### 7.2 The flip commit

1. **Delete both clears** (`overlay_backend_dx11.cpp:98`, `overlay_backend_dx12.cpp:273`). ONE AXIS:
   a per-RHI split is a two-regime binary whose drawable behaviour depends on the player's GPU API —
   the defect C2a's accidental DX12 flip already demonstrated. DX12's extra risk rides §7.7's gate,
   not a split flip.
2. **Reorder `ui::fonts::Load()` to AFTER `overlay_backend::InitRenderer`** — an **invariant, not a
   necessity**, and round 10 demoted it. The R4 finding is real: the flag is set at
   `imgui_impl_dx12.cpp:931` *inside* `InitRenderer` (`imgui_overlay.cpp:303`), **eight lines after**
   `Load()` at `:295`, and `ImFontAtlasBuildMain` samples it off the context **at that instant**
   (`imgui_draw.cpp:3497-3499`); upstream names the bug at `imgui_draw.cpp:2818`. **But item 5 kills
   the same bug by a different route, and both are in this commit**, so the old claim here — *"the
   single find that saves the flip"* — is FALSE as written and is retracted. Measured: exactly two
   `Fonts->Build()` sites exist in the whole tree, both in `fonts.cpp`; `AddFontDefaultBitmap` tail-
   calls `AddFontFromMemoryCompressedTTF` and does not build; nothing else in `Load` touches the atlas
   beyond `Clear` and the adds. With items 5 + 6 landed, **`Load` reaches `ImFontAtlasBuildMain` not
   at all** and the first build is `ImFontAtlasUpdateNewFrame` inside the first `NewFrame()`, which is
   unconditionally after `InitRenderer`. The reorder is kept anyway because it is the *invariant* that
   makes the R4 bug unreachable rather than a site-list that a future glyph-touching line in `Load`
   silently re-opens. Blast radius measured: `OnContextDestroyed` (`fonts.cpp:541-543`) only nulls
   `g_roleFont[]`, so both bring-up early returns (`imgui_overlay.cpp:299`, `:305`) are safe with or
   without `Load` having run, and `MaybeRescale` (`:330`) already exercises Load-after-InitRenderer on
   every scale change.
3. **ONE frozen table, `no-ink ∪ PUA` = 28 ranges / 56 values, driving BOTH the fold table and
   `ImFontConfig::GlyphExcludeRanges`.** Post-flip `GlyphRanges` is dead input, so "fold == render"
   stops being true by construction; `GlyphExcludeRanges` restores it, measured to gate the
   **on-demand** path at `imgui_draw.cpp:4593` via `ImFontAtlasBuildAcceptCodepointForSource`
   (`:4537-4544`). Same list on every source, so no codepoint can be excluded from one face and baked
   by another.
4. **`TexMaxWidth/Height = 2048`**, explicitly — the 8192 default must not be inherited. Not 4096:
   `pTexUploadBufferSize` only ever **grows** (`imgui_impl_dx12.cpp:466`) and upstream uploads the
   `UpdateRect` **bounding box** (`:455-458`), so 4096 raises the worst upload to 67 MB behind an
   unmeasured INFINITE wait. 2048 is today's measured geometry, so the commit cannot regress the
   per-upload **ceiling** — it does regress **frequency**, and conflating the two was an error.

   **The pack-capacity half of the argument, measured in round 10** (it was missing, and the number
   survives it). `ImFontAtlasPackAddRect` (`imgui_draw.cpp:4457`) retries **four times**, calling
   `ImFontAtlasTextureMakeSpace` between attempts; MakeSpace (`:4239`) runs
   `ImFontAtlasBuildDiscardBakes(atlas, 2)` — **every baked unused for 2 frames is discarded** — then
   repacks in place if discarded ≥ 20 % of packed, else grows (`:4225`, doubling height-then-width,
   clamped to TexMax). **The atlas is therefore not cumulative**: the resident set is what has been
   *drawn in the last two frames*, so the 7,595 reachable codepoints can never all be live, and
   post-flip resident pressure is **strictly below today's**, where all 2,517 are baked for every live
   size unconditionally. That is why 2048 need not hold the whole repertoire.

   Two consequences that ARE new. **(a) Pack failure is not transient**: at `:4818` `return false`
   writes NOT_FOUND into that baked's `IndexLookup`, and a baked drawn every frame is never discarded,
   so a *continuously displayed* glyph that lost the pack race stays a box for that baked's lifetime —
   this saga's own symptom, arriving through the clamp this commit chooses. §7.4's detector is the
   guard, and it must log `RectsPackedSurface`/`RectsDiscardedSurface` **at the transition**, not the
   state alone. **(b) `TexList` holds old AND new across a repack** (`:4097-4107`), so peak is two
   textures: 2048² RGBA32 = 16.8 MB each → **33.6 MB peak**, where 4096 would be **134 MB**.

   A FreeType metrics pass pricing the full post-flip surface **cannot run read-only today** (the bake
   is FreeType-through-ImGui and needs the probe harness). It is **owed at C3**, where a CJK source
   actually widens the demand set — not here, where nothing can demand more than today's.
5. **Retire both `ImFontAtlas::Build()` calls** — `RunFontRepertoireSelftest`'s `IsBuilt()` guard and
   the timed block near the end of `Load()` (`fonts.cpp:299`, `:440` at HEAD; this section shipped
   citing `:283`/`:410`, which `683f8214` invalidated **the same day it added the lesson that a
   citation rots silently** — cite the symbol, keep the line as a hint) — an obsolete shim
   (`imgui_draw.cpp:3060-3065`, inside `#ifndef IMGUI_DISABLE_OBSOLETE_FUNCTIONS`, which we do not
   define) that is just `ImFontAtlasBuildMain(this); return true;`. Post-retire the atlas builds
   lazily at `imgui_draw.cpp:2807-2811` with the flag already set, `PreloadedAllGlyphsRanges` stays
   false so `:2818` can never fire, and the numbers come from where they now exist: geometry from
   `atlas->TexData->Width/Height` logged on change, cost from the per-frame `Glyphs.Size` delta + ms.
   There is no single bake to time post-flip, so a boot number would be a fiction.
6. **Selftest becomes PER-BUILD, in-frame.** `Load()` sets a pending flag; the frame loop consumes it
   after `NewFrame()`. **Not** a first-frame one-shot — that re-introduces the exact `static bool done`
   latch s15 removed, observing boot and never the F1 font switch. Running in-frame **dissolves** C1's
   "refuse to ask" guard rather than inverting it, and removes the last out-of-frame atlas query in
   the codebase — the whole §3.2 defect.
7. **The OS-fallback WARN** (`fonts.cpp:449`) gains the sentence that uniqueness is not guaranteed
   there. Its comment reasons about the atlas coming out **short**; post-flip the failure **inverts**
   to a machine-dependent **superset** (Segoe UI carries Hebrew/Thai/Arabic), so two legible
   non-Latin names could both fold to U+FFFD. That path runs only when every embedded RCDATA family
   failed.
8. **The ~15-line upload probe, in this commit**: walk `GetPlatformIO().Textures`, call
   `ImGui_ImplDX12_UpdateTexture(tex)` **timed** per texture, then null `draw_data->Textures` so the
   backend does not repeat the work. Yields per-upload ms / box dims / bytes / geometry, and it is
   exactly the seam our own servicing would later replace.
9. **RULE 2: delete the dead inclusion path in the same commit** (round 10). The ranges reach ImGui as
   the `glyph_ranges` **parameter** of `AddFontFromMemoryTTF` (`fonts.cpp:133`), which ImGui stores
   into `ImFontConfig::GlyphRanges` — so it is passed on every add, through `AddFromResource` and
   `MergeBackstops`. Once it is measured-dead input, leaving it is precisely the live-looking knob
   RULE 2 forbids: the next editor widens it and believes they changed what renders. So the `ranges`
   parameter goes from both functions, and `Repertoire()`'s ImWchar-array conversion goes with it
   (verify no other caller at build time). **The repertoire TABLE stays** — it is the fold authority
   and now also the source of the exclude set; only its *inclusion* form dies.
10. Proto unchanged. `overlay_backend_dx12.cpp` 735 of 800; this commit adds ~20 lines there.
   `fonts.cpp` 545 of 800, and item 9 removes lines from it.

### 7.3 The table, measured (`scratchpad/fold_vs_render.py`, the generator's own tables)

```
face union regular 8,148 | bold 8,148 | union - intersection = 0 cp
donor cmap 1,418 | donor n faces 88 | donor-only 1,330 | faces-only 8,060
TODAY fold set (shipped)      2,517 cp  max U+1FAF6  161 ranges
POST-FLIP render, no exclude  9,478 cp -> MISMATCH 1,797 cp render-but-fold-to-U+FFFD
POST-FLIP render, w/ exclude  7,595 cp -> MISMATCH 0 cp, BOTH directions
```

Exclude-set options, all measured against ImGui's **64-VALUE** cap (`imgui_draw.cpp:3115`):

| set | ranges / values | fold set |
|---|---|---|
| `DEFAULT_IGNORABLE ∪ PUA` | 20 / 40 | 7,681 (+5,164) |
| **`no-ink ∪ PUA`** | **28 / 56** | **7,595 (+5,078)** |
| `no-ink ∪ DEFAULT_IGNORABLE ∪ PUA` | **32 / 64 — exactly the cap** | 7,593 |

Chosen: **`no-ink ∪ PUA`**, four ranges of margin. The union sits precisely on a limit whose
`IM_ASSERT` our Release build strips — the wrong place to sit. The 19 MB index hazard stays covered
because the U+E0000 TAG characters are `Cf`, hence inside no-ink.

**`+5,078` is the correct number and `docs/LESSONS.md` already records it.** Round 2 of this pass
called it wrong; round 6 measured that the objection was to its *derivation* (calling `unicodedata`
at generate time) and not to the *set*. The fix is **freezing no-ink as literal ranges** with its
Unicode version documented — `Cf` and `Zs` gain members between versions exactly as `Cn` does, which
is `[[lesson-a-generated-constant-must-not-depend-on-the-toolchains-data-version]]` a second time.
The generator gets a `sys.exit` if the exclude table exceeds 32 ranges, because :3115's assert cannot
fire in our build.

The field is set on **two** `ImFontConfig` objects, covering all eight of our adds: `cfg` (`:370`) →
`AddFromResource` (`:224`) + the OS fallbacks (`:438`, `:446`); `merge` (`:178`) → the three
backstops (`:182`); `donor` (`:187`) **inherits by copy**; `AddFromResource` does `cfg = baseCfg`
(`:130`). Not ours: `AddFontDefaultBitmap()` (`:465`), whose cmap is the 7×13 ASCII bitmap.

### 7.4 The instruments

**Primary — the superset invariant.** `ImFontBaked::Glyphs` is **public** (`imgui.h:3891`) beside
`IndexLookup` (`:3890`), so: **on any `Glyphs.Size` change, walk the newly baked glyphs and assert
every codepoint is in the repertoire.** One statement catches a missed `GlyphExcludeRanges` on any
config, the donor path, and the OS-fallback superset. O(new glyphs), only when they change. This
replaced a three-probe design that could not see a superset at all.

**Negative controls**, census-derived so a missed field on *any* config is caught (round 8 measured
that a single probe cannot do it, and corrected a claim that Cascadia carries U+E0B0 — JetBrains
Mono does):

| probe | carried by | catches |
|---|---|---|
| U+00AD | **all seven faces** | a missed field on any config at all |
| U+E0B0 | JetBrains Mono only | the backstop-merge path |
| U+E0067 | **donor only** | the donor config — and it is the TAG class whose baking blows index tables 1.04 → 7.34 MB per face |

**Regime assertion first**: `Fonts->RendererHasTextures == true`. Under an eager boot every one of the
above is green-by-construction — `[[lesson-an-instrument-never-shown-failing-passes-by-construction]]`
one level up, at the *precondition* rather than the assertion.

**The pack-failure detector.** Excluded / genuinely-absent / pack-failed all write the same
`IM_FONTGLYPH_INDEX_NOT_FOUND` (`imgui_draw.cpp:4626-4629`), so the predicate must **subtract the
exclude set**: `IsGlyphInFont(cp) && !InExcludeSet(cp) && IndexLookup[cp] == NOT_FOUND`. Three states
are measured distinct: `>= Size` or `0xFFFF` UNUSED = never requested (`:4552` resize default);
`0xFFFE` = requested and every source failed; `< 0xFFFE` = baked. Needed because the real drop is
silent — `ImFontAtlasPackAddRect` returns Invalid, the `IM_ASSERT` at `:4818` is stripped, and
`return false` sends the glyph to the fallback box, i.e. the exact symptom this whole saga exists to
delete. Geometry-at-clamp is neither necessary nor sufficient, so it is a **state-transition** log
(a repack can relieve and re-reach it), with `RectsPackedSurface`/`RectsDiscardedSurface`
(`imgui_internal.h:3996-3998`) as the repack-under-pressure signal.

**`IndexLookup` sharpens `[[lesson-querying-a-lazy-cache-populates-it]]`** rather than contradicting
it: the *documented* accessor (`FindGlyphNoFallback`) mutates, but the underlying index is public and
pure, so absence *is* assertable — just not through the API the lesson was written against.

### 7.5 Two security rows, not one

Both belong in `docs/security/TRACKER.md`, with **different severity and different mitigations**:

- **Remote rasterisation amplification** — chat is the one attacker-controlled string (W11) and the
  whole repertoire is bakeable from it, so a few hundred bytes of diverse UTF-8 forces thousands of
  FreeType rasterisations plus repacks **on every receiving peer**. **RHI-independent.** Existing caps
  (`kMaxSnapshotLines` 206 × `text[256]`) bound one frame, not a stream of fresh messages. Nothing in
  ImGui bounds per-frame rasterisation (measured by absence). Mitigation is now implementable at our
  layer via §7.4's pure predicate — count first-sight codepoints per frame, defer the rest of a row
  past a threshold — but is **not built** in the flip commit. The number rides the `Glyphs.Size` delta
  (O(1)); a text scan would be O(text) on the hot path, the pattern the audit rule exists to catch.
- **The unbounded upload wait** — `WaitForSingleObject(…, INFINITE)` per upload on the render thread.
  **DX12 only**; DX11's `UpdateSubresource` has no fence and no wait.

Calling the first an engineering deferral was wrong: it is a tracked row with a known fix.

### 7.6 LATER — our own DX12 servicing, only if the numbers demand it

Record into the **frame** list `g_list` (already `Reset` at `overlay_backend_dx12.cpp:534`, its
`fc.fenceValue` already waited at `:531`), so the only wait involved is the frame context's own;
`g_uploadList` is untouched. Per-rect `tex->Updates[]` uploads, not the bounding box. **Never write
`BackendUserData`** — `ImGui_ImplDX12_DestroyTexture` (`:373`) casts it to its own type at every
`InvalidateDeviceObjects`. Key on `TexID` (the SRV slot's GPU descriptor ptr, our preview-texture
convention). **Release-then-create for `WantCreate`-with-a-live-`TexID`** is load-bearing, not
defensive: `ReinitBackendIfDescChanged` (`:384`) → `ImGui_ImplDX12_Shutdown` →
`InvalidateDeviceObjects` (`:997`) → the destroy loop (`:874-876`) status-flips every atlas texture
with our live TexID still attached.

**And `imgui_overlay.cpp:331`'s `InvalidateDeviceObjects` must die in that same commit.** Traced:
`Load()` → `Clear()` → `ClearTexData` nulls `Pixels`; the rebuild sets `WantDestroyNextFrame`; the
destroy loop with our null `BackendUserData` does **not** clear `TexID`, and `SetStatus`'s
Destroyed→WantCreate rewrite is skipped because `WantDestroyNextFrame` is true; next frame
`ImTextureDataUpdateNewFrame` hits `if (Status == Destroyed)` (`imgui_draw.cpp:2873`), the guarding
`IM_ASSERT` is **stripped**, `remove_from_list = true`, `IM_DELETE` + erase — while our resource and
our slot stay allocated and **unreachable**. One resource + one of 256 slots per scale or F1 font
change. It does not exist today because ImGui owns that resource.

**The clamp does NOT belong to this section** (round 10 corrected the coupling). An earlier draft said
"the clamp rises to 4096 here" — inside the *servicing* section, which ties the atlas ceiling to who
performs the upload. Wrong axis: the clamp is a function of the **repertoire's width**, so it rises
(if at all) at **C3**, when a source that widens the demand set lands, gated on §7.4's pack-failure
detector and the metrics pass owed there. Whether we or upstream drives the copy is unrelated.

RULE 2 also retires `g_imguiTexQueue`, `Owner::Imgui` and the exhaustion reserve branch:
`bd->pCommandQueue` has exactly **one** use site (`imgui_impl_dx12.cpp:555`, inside `UpdateTexture`),
the path being disabled. `SrvAlloc`/`SrvFree` survive as **fail-loud tripwires** because `Init` deep-
copies the `InitInfo` and a future path reaching `UpdateTexture` would call through null.

### 7.7 The gate

`tripwires.ps1` is the **wrong home** — its own header says *"ADVISORY, always exit 0"*. The gate is a
source-level script in the family `build-core.yml` already runs from a clean clone
(`registry_gate.ps1:91`, `peerconn_gate.ps1:101`): **if neither RHI clears `RendererHasTextures`, then
either the servicing TU exists or this document carries a dated `MEASURED-UPLOAD-VERDICT:` line.** It
**censuses positively** — both backend TUs must exist and each must present exactly one
`RendererHasTextures` site classified by operation kind — and fails on a zero or unexpected census,
because a grep-for-absence goes green the moment a file is renamed or the clear moves into a helper.
It carries a drill switch and **must be shown RED by injection before it counts as a guard**. The same
predicate becomes a labelled line in `judge.ps1` (the refuse-to-publish predicate). It must cover any
**published** artifact, not just a release — b125/b127 already put a dev build in a tester's hands.
`tools/mp.py:1539`'s `selftest: FAIL` stays the smoke layer; it needs installs and is not a
clean-clone gate.

Noted, not folded in: **`nick_gate.ps1` is still not in CI.**

---

Related: `[[project-imgui-192-upgrade-measured-2026-07-30]]`,
`votv-bake-everything-atlas-cost-2026-07-29.md` (the eager price list + the MTA font architecture),
`votv-arc-d-gate-measurements-2026-07-28.md` (RF3, superseded on the "structural" framing),
`votv-nickname-arbitration-roster-id-DESIGN-2026-07-27.md` (arc D2 + the fold),
`docs/VERSION_MIGRATION.md` §11 (the UE4SS decision + tripwire ledger).
