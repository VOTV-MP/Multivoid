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
