# "Bake all, including hieroglyphs" — what the whole repertoire actually costs

**USER REQUEST, verbatim (2026-07-29):** *"Lets bake all, including hieroglyphs to see how much it
will actually cost"* — after asking, of the arc-D2 outcome, *"so we're not supporting any glyphs at
all?"*

**Status: MEASURED, nothing built.** Instrument: `tools/probes/atlas_probe` (dev-only, RULE-2 exempt),
extended this session with four new tiers and rebuilt with `-DWCHAR32=ON` so `ImWchar` is 32-bit
exactly as the shipping DLL compiles it. Raw output: `scratchpad/atlas_all.txt`.

This supersedes the `16-64 MB` figure quoted throughout §9d — that number was measured against ImGui's
**"ChineseSimplifiedCommon"** subset, not the whole block. The real ceiling is far higher.

---

## 1. What the probe actually bakes

Same families, same per-role px, same `(family, px*4, bold)` dedup as `ui::fonts::BakeEmbeddedRoles`.
Two face configurations: **default** = the shipped role→family defaults (3 distinct faces), **worst** =
the worst resident set a user can select through the F1 menu (**5** faces — the ceiling corrected on
2026-07-28; the earlier probe row produced 4 and understated every worst-case cell by 2x).

New tiers, all cross-merging the other three embedded families behind the chosen one exactly as
`MergeBackstops` does:

| tier | what it adds |
|---|---|
| `SHIPPING b132` | the control — Latin-1 + Cyrillic + the emoji donor, i.e. what players run today |
| `+LatExt+Greek` | Latin Extended-A (`0100..017F`), Extended-B (`0180..024F`), Greek (`0370..03FF`) |
| `+FULL CJK` | the **whole** CJK Unified block `4E00..9FFF` — 20,992 codepoints, not a "common" subset |
| `EVERYTHING` | + kana + Hangul Jamo + Hangul Syllables + Thai |

## 2. The atlas — measured

**default config (3 faces) — what most players get**

| tier | @x1.0 | @x1.5 | @x2.0 | glyphs | bake @x1.0 |
|---|---|---|---|---|---|
| SHIPPING b132 | 8 MB | 16 MB | 32 MB | 5,710 | 55 ms |
| **+LatExt+Greek** | **8 MB** | **16 MB** | **32 MB** | 7,497 | **62 ms** |
| +FULL CJK | 64 MB | 128 MB | 256 MB | 70,425 | **810 ms** |
| EVERYTHING | 64 MB | 256 MB | 256 MB | 105,324 | **947 ms** |

**worst config (5 faces) — the ceiling a user can select**

| tier | @x1.0 | @x1.5 | @x2.0 | glyphs | bake @x2.0 |
|---|---|---|---|---|---|
| SHIPPING b132 | 16 MB | 32 MB | 32 MB | 8,911 | 173 ms |
| **+LatExt+Greek** | **16 MB** | **32 MB** | 64 MB | 12,495 | **200 ms** |
| +FULL CJK | 128 MB | 256 MB | 256 MB | 117,375 | **3,192 ms** |
| EVERYTHING | 128 MB | 256 MB | **512 MB** | 175,540 | **3,492 ms** |

## 3. Three findings

### 3a. Latin Extended + Greek is FREE

**Identical texture bytes at every scale in the default config, and at x1.0/x1.5 in the worst.** It adds
1,787 glyphs (default) / 3,584 (worst) and **+7 ms / +15 ms** of bake. It costs **zero donor bytes**,
because our four embedded families already carry those glyphs — the gate doc's own cmap census:
*"union of all 7 faces 8,148 cp (Cyrillic 256/256, **Latin Ext-A/B complete, Greek 135/144**)"*. We ship
the outlines today and never ask the atlas for them.

The one cell that moves is worst-config @x2.0 (32 → 64 MB), i.e. a user who has deliberately selected
four different families **and** 2x UI scale.

Concretely, this is the difference between `Micha▯ / ▯imon / Güne▯ / ▯tefan / ▯▯▯▯▯▯▯` and
`Michał / Šimon / Güneş / Ștefan / Γιώργος`.

### 3b. Full CJK is 8x the texture, 15x the bake, and 7 MB of DLL

Donor bytes, measured by real subsetting (fontTools, layout tables dropped — the same method that took
Twemoji from 1.47 MB to the 0.66 MB we ship):

| donor | codepoints | subset bytes |
|---|---|---|
| **FULL CJK Unified** (Noto Sans SC, OFL) | 20,976 | **6.96 MB** |
| Kana (Noto Sans JP, OFL) | 205 | 0.04 MB |
| Hangul (malgun — **NOT redistributable**, size proxy only; shipping needs Noto Sans KR) | 11,428 | ~4.70 MB |
| Thai | 0 in the JP donor | — |
| *(shipped today: TwemojiMozilla-Subset)* | 1,418 | 0.66 MB |

So "everything" is roughly **+11.7 MB of DLL** on top of today's 0.66 MB — against a user constraint
that was voiced as «не хотелось бы добавлять кучу мб внутрь dll».

### 3c. The ceiling row is not merely expensive — it is ILLEGAL

`EVERYTHING` at worst-config x2.0 produces a **4096 x 32768** texture. `ImFontAtlas::Build()` returns
true (it is a CPU-side pack), but **D3D11/D3D12 cap a 2D texture at 16,384 in any dimension**
(`D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION`), so `CreateTexture2D` would fail and the overlay would have
**no font texture at all**. The failure would not be "slow" — it would be a blank UI, on the machine of
whichever user picked four families and 2x scale.

`+FULL CJK` @ worst x1.5/x2.0 and `EVERYTHING` @ default x1.5/x2.0 all land on 4096x16384 = exactly at
the limit, with zero headroom.

## 4. The bake time is the part that actually hurts

The atlas is rebuilt **on the render thread inside `Present`** — measured 2026-07-28, it freezes the
game, not just the overlay. Today's worst rebake is 173 ms and already produced a visible hitch worth a
12-frame debounce. `+FULL CJK` costs **810-3,192 ms**. A user dragging a window edge, or moving the F1
scale slider, would hard-freeze the game for up to **3.5 seconds** per settle.

Index tables are NOT the problem and never were: `idx` stays at 2.97 MB (default) / 4.95 MB (worst)
across every tier, because `GrowIndex(max_codepoint + 1)` is priced by the **maximum** baked codepoint
and the emoji donor at `U+1FAF6` already set it. Adding all of CJK moves the max codepoint *down*.

## 5. What this changes

- **`+LatExt+Greek` should ship.** It is free in bytes, free in texture at the sizes that matter, ~7 ms
  of bake, and it converts five European scripts from boxes to letters. It sits inside the constraint
  the user already stated, because it adds no megabytes to the DLL.
- **CJK/Hangul stays out, and now for a measured reason rather than an estimated one:** +11.7 MB DLL,
  8-16x the texture, a multi-second freeze per rebake, and a configuration that exceeds the D3D texture
  limit outright. §9d.4's *"if CJK is ever embedded, this section is the starting point"* still holds —
  and this section is now the price list.
- The `16-64 MB` figure in §9d should be read as **the cost of ImGui's "common" subset**, not of CJK.
  The whole-block cost is 64-256 MB.

---

## 6. "Does MTA also freeze and have a 256 MB atlas?" — measured: NO, and the reason IS option B

**USER QUESTION 2026-07-29.** Answered from the vendored source
(`reference/mtasa-blue/vendor/cegui-0.4.0-custom/src/CEGUIFont.cpp`), per the standing
follow-MTA-architecture rule.

**MTA never downloads a font, and never bakes a whole block.** The
`CResourceFileDownloadManager` path is for maps/models/scripts
(`RESOURCE_FILE_TYPE_MAP/SCRIPT/CLIENT_FILE`) — glyphs do not travel that way at all. What MTA
does instead, in its custom CEGUI 0.4.0 fork, is **on-demand page-granular glyph caching**:

| mechanism | file:line | what it does |
|---|---|---|
| on-demand insert | `CEGUIFont.cpp:753, :827` | `refreshStringForGlyphs(text)` runs on **every** `getTextExtent` and `drawText` — the cache grows from the text actually drawn |
| ASCII is free | `:1511` | `if (ulGlyph < 128) return NULL;` — the Latin core is always resident, never re-examined |
| **page** granularity | `:1489` `addGlyphPageInfo` | glyphs group into pages via `GlyphToGlyphPageId`; pages are added and freed independently, never the whole block |
| **fallback chain** | `:1519-1521` | if the face lacks the glyph, `FontManager::getSubstituteFont()->insertGlyphToCache(...)` — a substitute font supplies it |
| **LRU eviction** | `:1505`, `:1527` | every touch stamps `pInfo->uiLastUsedTime = d_uiLastPulseTime` |
| pages really are freed | `:1595-1605` | `bWaitingToBeDeleted` -> `freeGlyphPage(...)` + `d_GlyphPageInfoMap.erase(...)` |
| rebuild at a quiet point | `onClearRenderList()` | *"Called when the renderer has no cached images from this font, so will speed up rebuilding"* — not inside a draw |

So MTA pays neither cost: no 11.7 MB of donor bytes (it uses fonts already present), and no 64-256 MB
atlas (it only ever rasterises what was drawn, and gives the pages back).

### 6a. This retires the objection that killed demand baking in arc D2

§9d.4 rejected demand baking partly because *"a set that never shrinks converges on the eager cost
anyway, making the saving a deferral bought with a lifecycle."* **MTA's set shrinks** — that is
exactly what `uiLastUsedTime` + `bWaitingToBeDeleted` + `freeGlyphPage` are for, and it has held at
multi-thousand-peer scale for 15+ years. The objection was correct about a cache with no eviction; it
was not an argument against caching.

### 6b. Why we do NOT need CEGUI's page machinery

MTA needs pages because its text is arbitrary — chat, Lua-drawn UI, any string a server sends. **Ours
is names.** The demand set is the roster: <=4 peers x 20 codepoints = **<=80 codepoints**, changing only
on join / leave / rename, sourced from the arc-A ledger that already owns per-slot identity.

At that size a **whole-atlas rebuild is cheap enough to need no page granularity at all** — measured in
this same probe run (`OS +Nhz`, real system faces merged, only the demanded glyphs rasterised):

```
OS +4hz  +0em  x1.00   1.00 MB   1,609 glyphs    5.7 ms
OS +20hz +0em  x1.00   1.00 MB   1,657 glyphs    5.9 ms
OS +60hz +8em  x1.00   1.00 MB   1,801 glyphs    7.8 ms
OS +200hz+40em x2.00   8.00 MB   2,317 glyphs   16.2 ms
```

So the design is MTA's **shape** (on-demand, OS-supplied, evicting) with MTA's **granularity dropped**
as unnecessary — which is the documented way to diverge: cite the file, state the reason. ImGui 1.91.5
has no dynamic atlas (that is what 1.92 added, and RF3 keeps us pinned), so a page cache is not
available to us anyway; a bounded full rebuild is, and at 6-16 ms it is under today's 55-105 ms boot
bake.

### 6c. What the shape implies for the build

- The **fallback chain** is MTA's `getSubstituteFont()`. Ours is `IDWriteFontFallback::MapCharacters` —
  an invariant ("which font draws this codepoint?") rather than a hardcoded filename list, which the
  gate doc already named as the correct form. **DirectWrite is not linked today**; the precondition is
  obtaining a file path or stream to hand FreeType.
- The **trigger** is a roster change, not a keystroke — so the per-keystroke site-list objection in
  §9d.4 does not apply either.
- **Eviction** falls out for free: rebuild the demand set FROM the live roster each time, so a departed
  peer's script leaves with them. No timestamps, no LRU, no lifecycle to get wrong.
- `FoldKey` must keep folding on the **compile-time** repertoire, NOT on what happens to be baked —
  otherwise uniqueness becomes machine-dependent and two peers disagree about whether two names
  collide. This is the arc-D2 guarantee and it is the one thing this change must not touch.
