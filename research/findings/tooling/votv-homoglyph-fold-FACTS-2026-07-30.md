# Homoglyph collisions in the shipped fold set — measured fact base, 2026-07-30

**Status: MEASURED. THE ARC WAS BUILT AND THEN DECLINED — see the banner below.**

> **DECLINED BY THE USER, 2026-07-30** — verbatim: *"I don't care if Alex and Аlex play together and their nicknames look identical"*, *"Alex (latin) and Аlex (cyrillic А + latin lex) is normal"*. Built across a 13-round `/qf`, then reverted whole. It is a CLOSED product decision, not a backlog item — do not re-open it as a defect.
>
> The MEASUREMENTS in this file remain true and are kept for that reason; two of them were
> corrected during the build and are marked inline. What is dead is the conclusion that this
> is a defect worth fixing. `Alex`/`Аlex` sharing a nameplate is WANTED behaviour.
>
> Corrections made while building: (a) "the count is size-dependent" is FALSE — Roboto measures
> 221 pairs at 6px, 211 at 16, 199 at 20, 205 at 32 and **314 at 64**; fonts duplicate outlines
> across scripts and small-size HINTING splits them. (b) `3`≡`З` is not FSEX300-specific in the
> way the note implies — FSEX300 draws both with the SAME OUTLINE.
>
> Why the arc happened at all: `[[feedback-a-declined-product-question-does-not-go-away]]`.

## The measurement

Rasterise every codepoint of the fold set in every shipped face (PIL/FreeType,
`layout_engine=BASIC`, which is what ImGui does — `FT_DISABLE_HARFBUZZ` is on,
`CMakeLists.txt:81`), hash each bitmap, group by hash. Any group with more than one
codepoint is a set of characters that draw as the same pixels.

| | 13 px | 20 px |
|---|---|---|
| pixel-identical codepoint pairs | **476** | 390 |
| explained by canonical singleton equivalence | 23 | 23 |
| **not explained by Unicode equivalence at all** | **453** | 367 |

Sample of the unexplained majority:

```
U+0041 LATIN CAPITAL A    == U+0391 GREEK CAPITAL ALPHA  == U+0410 CYRILLIC CAPITAL A
U+0043 LATIN CAPITAL C    == U+0421 CYRILLIC CAPITAL ES
U+0033 DIGIT THREE        == U+0417 CYRILLIC CAPITAL ZE      (FSEX300 only)
U+0021 EXCLAMATION MARK   == U+01C3 LATIN LETTER RETROFLEX CLICK
U+002D HYPHEN-MINUS       == U+2010 HYPHEN == U+2011 NON-BREAKING HYPHEN
                          == U+2012 FIGURE DASH == U+2212 MINUS SIGN
U+003A COLON              == U+0589 ARMENIAN FULL STOP == U+2236 RATIO
U+002C COMMA              == U+0375 GREEK LOWER NUMERAL SIGN == U+201A SINGLE LOW-9 QUOTE
```

**Instrument checked before the numbers were believed.** These are distinct glyph IDs with
real outlines, not `.notdef` boxes — spot-verified per pair. Codepoints with no ink are
excluded from grouping, so "all the blanks collide" cannot manufacture a class. Two
independent methods agree at the boundary: a `getBestCmap()` inverse map finds only **2**
codepoint-pairs sharing a glyph ID anywhere (Roboto: one canonical singleton, plus
**U+0394 GREEK CAPITAL DELTA == U+2206 INCREMENT**), which is the strict subset of the
pixel result that you would predict.

## Why it matters

`nickname_arbiter::FoldKey` exists so that two players cannot hold visually identical
nameplates (`nickname_arbiter.cpp:73-96`: *"the key is not a stand-in for the pixels; it IS
the pixels"*). It folds case and maps undrawable codepoints to one sentinel — but two
DIFFERENT drawable codepoints that render identically get different keys, so the arbiter
sees no clash and assigns no suffix.

Concretely: `Alex` (Latin) and `Аlex` (Cyrillic А, U+0410) are two different strings, two
different fold keys, one set of pixels. Both can be in the lobby at once.

## Why it is NOT in commit 2

Commit 2 admits the combining marks so five scripts stop drawing as half-boxes. This set is
orthogonal to that: it is pre-existing, roughly 20x larger, and closing it is a product
decision rather than a correctness one.

## The open questions a design pass has to answer

1. **Is folding it desirable?** Folding `3`≡`З` means a legitimately Cyrillic name can take
   a numeric suffix it did not earn. The trade is a harmless false collision against two
   indistinguishable nameplates — defensible, but a user-facing choice.
2. **Which size?** The count is size-dependent (476 at 13 px, 390 at 20 px) because glyphs
   that differ by a pixel or two converge as they shrink. The mod draws names at several
   sizes. Union of the role sizes is the conservative answer; it is not the only one.
3. **What is the table's shape?** A canonical representative per collision class, folded
   like `FoldCase` — or a class ID. Either is generated, never hand-written: the whole
   finding is that Unicode equivalence is neither necessary nor sufficient here.
4. **Does it belong to the FONT or to the NAME?** The table is measured from the shipped
   faces, so it changes when a face changes — like the repertoire table, and unlike a
   Unicode property. That is consistent with arc D2's doctrine, but it means a font swap
   silently re-partitions the name space, and a name that was unique can stop being so.
5. **Chat is NOT affected.** Chat is not uniqueness-bearing; this is a nameplate concern
   only. Do not widen it into a text-wide normalisation.

## Reproducing

The census is ~40 lines over `fontTools` + `PIL`; both are already used by
`tools/text/build_repertoire.py` and its drill. The generator already opens every face, so
the natural home for the emission is a fifth `.inc` beside `mark_ranges.inc` — if the design
pass concludes it should be emitted at all.

Related: `research/findings/tooling/votv-imgui-192-upgrade-DESIGN-2026-07-30.md` §7 (the
as-built for commits 1 and 2), `nickname_arbiter.cpp:72` (`FoldKey`),
`research/findings/join-identity/votv-arc-d-gate-measurements-2026-07-28.md` (arc D2, where
the pixel doctrine was set).
