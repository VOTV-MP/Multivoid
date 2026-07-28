// coop/text/repertoire.h -- ARC D2: the set of codepoints this build can DRAW.
//
// WHY A NAMING MODULE OWNS A FONT FACT. Arc B guarantees that no two players
// carry the same display name, and it enforces that by comparing strings. Dear
// ImGui substitutes ONE FallbackGlyph for EVERY codepoint absent from the atlas
// (imgui_draw.cpp:3830-3838), so 张伟 and 李明 -- two names with no codepoint in
// common, no collision, and therefore no numeric suffix -- draw as the SAME
// nameplate. The guarantee was true in the domain the code compares in and false
// in the domain the human looks at, which is the only domain the user asked
// about ("просто чтобы у всех был уникальный Nameplate").
//
// Buying more glyphs cannot fix that. Any donor budget leaves some script out,
// so a coverage-based guarantee is budget-shaped and known-incomplete whatever
// the budget. Moving the guarantee into the ARBITER makes it total: FoldKey maps
// every codepoint outside this repertoire to ONE sentinel, so names that render
// alike now COLLIDE and one of them takes the suffix arc B already ships.
// Uniqueness becomes FONT-INDEPENDENT, and the repertoire demotes from a
// correctness input to a legibility knob.
//
// THE TABLE IS A BUILD CONSTANT, NOT THE LIVE ATLAS. Folding against the atlas
// the local machine happens to have built would make one player's font install
// the authority over everyone else's name, and peers would disagree about who
// collided. It is generated from the fonts on disk at build time
// (tools/text/build_repertoire.py), which is also what keeps it honest: the same
// script emits the ranges ui::fonts::Load bakes, so what folds and what renders
// are one definition rather than two that drift.
//
// SCOPE OF THE GUARANTEE. It holds on the PRIMARY font path -- our RCDATA
// families plus the emoji donor. ui/fonts.cpp's two fallbacks (a Windows system
// face, then ProggyClean) bake no embedded family, so the atlas there is short
// of this table; the boot font selftest is what turns that from a silent
// difference into a logged one. The FOLD does not move on those paths, so peers
// still agree about names even where one of them cannot draw them.
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
// bakes. Binary search over ~156 ranges.
bool InRepertoire(uint32_t cp);

// Unicode 15.1 Default_Ignorable_Code_Point. Characters DEFINED to have no
// visible rendering: soft hyphen, the variation selectors, the bidi controls,
// the Hangul fillers, the tag block. A name may not contain one, because a name
// that differs from another only in ignorables is a distinct fold key with
// identical pixels -- the same defect as an absent glyph, arriving from the
// other direction. Measured hole this closes: U+034F has advance 0 in Fixedsys
// AND Roboto (the two default families) and was accepted mid-name, because
// SanitizeNickname's combining-mark check only fires while the output is empty.
bool IsDefaultIgnorable(uint32_t cp);

// The repertoire as ranges, for the atlas builder. Sorted, non-overlapping.
const CodepointRange* RepertoireRanges(size_t* outCount);

// Machine-asserted at boot beside the codec and arbiter selftests. Covers the
// table's own invariants (sorted, disjoint, non-empty) and the four membership
// facts the fold depends on, none of which any LAN drill can reach.
bool RunRepertoireSelftest();

}  // namespace coop::text
