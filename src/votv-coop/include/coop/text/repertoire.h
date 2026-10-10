// coop/text/repertoire.h -- the set of codepoints this build can DRAW.
//
// A naming module owns a font fact because Dear ImGui substitutes ONE fallback glyph for
// every codepoint absent from the atlas, so two names with no codepoint in common draw as
// the same nameplate. No glyph budget fixes that. The arbiter's FoldKey maps every codepoint
// outside this repertoire to one sentinel instead, so names that render alike COLLIDE and
// one takes the numeric suffix: uniqueness is font-independent and this table is a
// legibility knob rather than a correctness input.
//
// The table is a BUILD CONSTANT, not the live atlas, or one player's font install would be
// the authority over everyone else's name. It is generated from the fonts on disk, and the
// ranges ui::fonts::Load bakes are emitted in the same run. The two fallback font paths bake
// no embedded family and their atlas is short of this table, which the boot font selftest
// logs; a language pack whose script our faces lack has a Windows face bake that script beyond
// it (ui/fonts.cpp). The FOLD moves in neither case, so peers still agree.
#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::text {

// An inclusive codepoint run. Generated tables are sorted and non-overlapping,
// which RunRepertoireSelftest asserts rather than assumes.
struct CodepointRange {
    uint32_t begin;
    uint32_t end;
};

// Can this build draw `cp`? The predicate FoldKey folds against and the atlas
// bakes. Binary search over 453 ranges.
bool InRepertoire(uint32_t cp);

// The codepoints we REFUSE to bake: a coarse cover holding everything the render set carries
// that InRepertoire omits, merged across unassigned space to stay under ImGui's list cap. The atlas
// is lazy and ignores ImFontConfig::GlyphRanges, so the only lever is subtractive
// (GlyphExcludeRanges). One generator run emits both tables, so fold and bake stay one fact for
// every embedded face; the Windows face a pack's script needs takes this table united with all
// outside that script (ui/fonts.cpp), so it bakes the script while the fold reads it as the sentinel.
//
// InExcludeSet is NOT `!InRepertoire`: the repertoire is what we can draw, the exclude set what we
// refuse to draw, and a codepoint no font carries is outside BOTH. "The font has it, we did not
// forbid it, and it still failed to bake" is a defect; "the font has it and we forbade it" is this
// table working. The pack-failure detector (ui/atlas_watch.cpp) asks that of each source's own list.
bool InExcludeSet(uint32_t cp);

// The exclude set as ranges, for the atlas builder. Sorted, non-overlapping, and
// GUARANTEED NOT TO BEGIN AT U+0000 -- ImGui reads the list it is converted into
// as zero-terminated, so a leading zero would silently make it a no-op and every
// codepoint would bake. The generator hard-fails rather than emit one.
const CodepointRange* ExcludeRanges(size_t* outCount);

// Unicode 15.1 Default_Ignorable_Code_Point. Characters defined to have no visible rendering:
// soft hyphen, the variation selectors, the bidi controls, the Hangul fillers, the tag block.
// A name may not contain one, because a name that differs from another only in ignorables is a
// distinct fold key with identical pixels -- the same defect as an absent glyph, arriving from
// the other direction. U+034F, for one, has advance 0 in both default families.
bool IsDefaultIgnorable(uint32_t cp);

// Is `cp` a combining mark this build can DRAW (Mn/Me/Mc, in the render set, and not held out
// by the exclude set)?
//
// One consumer, `coop::text::FilterNameChars`, which drops a mark at position 0 of every displayed
// name, because a mark with no base sits on whatever the UI drew before the name; the table is
// generated rather than written down so that rule covers every script the atlas draws.
//
// It is deliberately not `!InRepertoire`-shaped and not a denylist: a mark in the MIDDLE of a
// name is legitimate text in five scripts and passes untouched. Only position 0 is a
// rendering problem.
bool IsCombiningMark(uint32_t cp);

// Whether any codepoint of the generated repertoire satisfies `pred`: how a neighbour asserts a
// property against the table without the table leaving this file.
bool AnyRepertoireCodepoint(bool (*pred)(uint32_t));

// Asserted at boot beside the codec and arbiter selftests. Covers the table's own invariants
// (sorted, disjoint, non-empty) and the four membership facts the fold depends on.
bool RunRepertoireSelftest();

}  // namespace coop::text
