#!/usr/bin/env python3
"""build_repertoire.py -- ARC D2: mint the emoji donor AND the repertoire constant
from the SAME measurement, in one run.

WHY ONE SCRIPT AND NOT TWO. The design's load-bearing property (9d.2 item 5) is
that "the constant IS the eagerly-baked set" -- there must be no gap between what
`coop::text::InRepertoire` folds and what `ui::fonts::Load` can actually draw.
Two scripts, or a hand-written range table beside a generated font, is two owners
of one fact, and they drift the moment a donor or a family changes. So this file
derives BOTH artifacts from the fonts on disk:

  1. assets/fonts/TwemojiMozilla-Subset.ttf  -- the donor, subset to its cmap.
  2. include/coop/text/repertoire_ranges.inc -- the FOLD set: every codepoint the
     atlas can draw, minus the exclude set below.
  3. include/coop/text/exclude_ranges.inc    -- the EXCLUDE set, handed to ImGui
     as ImFontConfig::GlyphExcludeRanges on every font source.

THE REGIME CHANGED 2026-07-30 (the ImGui 1.92 flip), AND THE TWO EMISSIONS ABOVE
ARE WHY. Under 1.91.5 the atlas was eager: `repertoire_ranges.inc` was passed as
ImGui's `glyph_ranges` INCLUSION list, so what baked was exactly what folded, by
construction. 1.92's atlas is lazy -- it bakes a codepoint the first time
something draws it -- and `ImFontConfig::GlyphRanges` becomes DEAD INPUT
(measured; imgui_draw.cpp's on-demand path never consults it). The only surviving
lever is SUBTRACTIVE: `GlyphExcludeRanges`, checked by
ImFontAtlasBuildAcceptCodepointForSource (imgui_draw.cpp:4537-4544) on the
on-demand path at :4593.

So the invariant `fold set == bake set` is now maintained the other way round:

    render  = every codepoint any embedded face or the donor has in its cmap
    exclude = no-ink u Default_Ignorable u private-use u (combining marks n render)
    fold    = render - exclude          <- repertoire_ranges.inc
    bake    = render - exclude          <- because GlyphExcludeRanges says so

ONE SOURCE SET, TWO EMISSIONS, AND THEY DIFFER BY EXACTLY ONE CODEPOINT. The
exclude list ImGui receives is ZERO-TERMINATED, so it may not begin with U+0000 --
see the clamp in emit_exclude(). The fold table must NOT drop U+0000, because
dropping it from the shared set would ADMIT NUL to the repertoire (it is in the
FSEX300 and Roboto cmaps) and let a name fold on it. The generator asserts the
difference IS exactly {U+0000} rather than leaving a reader to notice.

THE DONOR SUBSET KEEPS THE WHOLE CMAP, deliberately. The obvious economy is to
keep "the common emoji" and drop the rest, and it was measured: 905,404 B for
1,356 codepoints against 1,172,504 B for all 1,418. The 267 KB is not worth it,
because "which emoji are common" is the same unanswerable product question the
15-round /qf pass dissolved for hanzi -- a hand-written range list silently
dropped U+2B50, U+231A and U+25B6. What the subset DOES drop is the multi-
codepoint sequences (ZWJ families, flags, skin-tone modifiers): those are GSUB
ligatures, and FT_DISABLE_HARFBUZZ is ON (CMakeLists.txt:81) with ImGui doing no
shaping at all, so they could never compose no matter what we embed.

UPSTREAM. Twemoji Mozilla (github.com/mozilla/twemoji-colr, release Twemoji.
Mozilla.ttf) -- COLR v0 + CPAL, which is the only colour format that survives our
build: FT_DISABLE_PNG is ON, so CBDT/sbix bitmap emoji fonts are ruled out.
Code MIT (Mozilla), artwork CC-BY 4.0 (Twitter). Pass the downloaded file with
--donor; the subset it produces is what gets committed.

Run:  python tools/text/build_repertoire.py --donor <path-to>/Twemoji.Mozilla.ttf
"""

import argparse
import io
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSETS = os.path.join(REPO, "src", "votv-coop", "assets", "fonts")
GEN = os.path.join(REPO, "src", "votv-coop", "include", "coop", "text")
SUBSET_NAME = "TwemojiMozilla-Subset.ttf"

# Unicode 15.1 DerivedCoreProperties.txt, Default_Ignorable_Code_Point=Yes.
# Characters defined to have NO visible rendering. Two consumers, one list:
#   - the nickname denylist, because a name made of them is a name with no
#     pixels and therefore not a unique nameplate;
#   - the repertoire subtraction, because baking them buys nothing and the
#     supplementary ones are ruinously expensive (see the note in main()).
DEFAULT_IGNORABLE = [
    (0x00AD, 0x00AD), (0x034F, 0x034F), (0x061C, 0x061C), (0x115F, 0x1160),
    (0x17B4, 0x17B5), (0x180B, 0x180F), (0x200B, 0x200F), (0x202A, 0x202E),
    (0x2060, 0x206F), (0x3164, 0x3164), (0xFE00, 0xFE0F), (0xFEFF, 0xFEFF),
    (0xFFA0, 0xFFA0), (0xFFF0, 0xFFF8), (0x1BCA0, 0x1BCA3), (0x1D173, 0x1D17A),
    (0xE0000, 0xE0FFF),
]

# Private Use. Subtracted for a different reason than the ignorables: a PUA
# codepoint HAS a picture, but only in the one font that assigned it, so it means
# nothing to anybody else and it is not a character anyone can type deliberately.
# Twemoji ships U+E50A (its own logo glyph), which arrived in the repertoire via
# the donor's cmap -- silently making a vendor logo a legal, non-sentinelled,
# uniqueness-bearing character in a player's name. Caught by audit, not by design.
PRIVATE_USE = [(0xE000, 0xF8FF), (0xF0000, 0xFFFFD), (0x100000, 0x10FFFD)]

# --- THE FROZEN CATEGORY TABLES -------------------------------------------
#
# LITERAL, not derived at generate time, and that is deliberate. A generated
# constant must not depend on the Unicode data version of whatever CPython
# happens to run this script: `unicodedata` gains members between releases, so
# calling it here would silently move the fold table -- and the fold table is a
# CROSS-PEER agreement about which names collide. Both tables below were minted
# from Unicode 14.0.0 (CPython's unicodedata at authoring time, 2026-07-30) and
# are frozen with that version recorded.
#
# What catches a codepoint a NEWER Unicode would have added to one of these
# categories: the zero-advance census at the end of main(), which measures the
# fonts on disk rather than trusting any table. That is the only set that
# matters -- a category member no embedded face carries can never be baked,
# folded or typed.

# General_Category in {Cc, Cf, Cs, Zl, Zp, Zs}, MINUS U+0020.
# "Renders no ink" by category. The space is carved out by hand because it is
# Zs and it is a legal, load-bearing character; that carve-out is the reason the
# arithmetic below lands on 7,258 and not 7,257.
NO_INK = [
    (0x0000, 0x001F), (0x007F, 0x00A0), (0x00AD, 0x00AD),
    (0x0600, 0x0605), (0x061C, 0x061C), (0x06DD, 0x06DD),
    (0x070F, 0x070F), (0x0890, 0x0891), (0x08E2, 0x08E2),
    (0x1680, 0x1680), (0x180E, 0x180E), (0x2000, 0x200F),
    (0x2028, 0x202F), (0x205F, 0x2064), (0x2066, 0x206F),
    (0x3000, 0x3000), (0xD800, 0xDFFF), (0xFEFF, 0xFEFF),
    (0xFFF9, 0xFFFB), (0x110BD, 0x110BD), (0x110CD, 0x110CD),
    (0x13430, 0x13438), (0x1BCA0, 0x1BCA3), (0x1D173, 0x1D17A),
    (0xE0001, 0xE0001), (0xE0020, 0xE007F),
]


# General_Category in {Mn, Me, Mc} -- the combining marks.
#
# EXCLUDED IN COMMIT 1, ADMITTED IN COMMIT 2, and the two are one decision made
# twice. A combining mark draws real ink at zero advance, so post-flip
# "A" + U+0301 is pixel-identical to U+00C1 while folding to a different key --
# arc D2's defect (two names, same pixels, no collision detected) arriving
# through the widening. Today the marks are safe only by accident: BASE_RANGES
# jumps 0x024F -> 0x0370 so they were never baked at all.
#
# Commit 1 therefore keeps them out, which preserves EXACTLY today's behaviour
# while widening everything else. Commit 2 adds canonical composition (NFC) to
# FoldKey and admits them in the same change -- the fix is normalization, not
# prohibition, because a deny table over this set would make Thai, Tamil,
# Thaana, Arabic and Hebrew structurally unwritable as names (measured: of the
# 337 marks our faces carry, 296 never compose with anything).
#
# Intersected with the render set before emission, so the exclude list carries
# only marks some face can actually draw.
COMBINING_MARKS = [
    (0x0300, 0x036F), (0x0483, 0x0489), (0x0591, 0x05BD),
    (0x05BF, 0x05BF), (0x05C1, 0x05C2), (0x05C4, 0x05C5),
    (0x05C7, 0x05C7), (0x0610, 0x061A), (0x064B, 0x065F),
    (0x0670, 0x0670), (0x06D6, 0x06DC), (0x06DF, 0x06E4),
    (0x06E7, 0x06E8), (0x06EA, 0x06ED), (0x0711, 0x0711),
    (0x0730, 0x074A), (0x07A6, 0x07B0), (0x07EB, 0x07F3),
    (0x07FD, 0x07FD), (0x0816, 0x0819), (0x081B, 0x0823),
    (0x0825, 0x0827), (0x0829, 0x082D), (0x0859, 0x085B),
    (0x0898, 0x089F), (0x08CA, 0x08E1), (0x08E3, 0x0903),
    (0x093A, 0x093C), (0x093E, 0x094F), (0x0951, 0x0957),
    (0x0962, 0x0963), (0x0981, 0x0983), (0x09BC, 0x09BC),
    (0x09BE, 0x09C4), (0x09C7, 0x09C8), (0x09CB, 0x09CD),
    (0x09D7, 0x09D7), (0x09E2, 0x09E3), (0x09FE, 0x09FE),
    (0x0A01, 0x0A03), (0x0A3C, 0x0A3C), (0x0A3E, 0x0A42),
    (0x0A47, 0x0A48), (0x0A4B, 0x0A4D), (0x0A51, 0x0A51),
    (0x0A70, 0x0A71), (0x0A75, 0x0A75), (0x0A81, 0x0A83),
    (0x0ABC, 0x0ABC), (0x0ABE, 0x0AC5), (0x0AC7, 0x0AC9),
    (0x0ACB, 0x0ACD), (0x0AE2, 0x0AE3), (0x0AFA, 0x0AFF),
    (0x0B01, 0x0B03), (0x0B3C, 0x0B3C), (0x0B3E, 0x0B44),
    (0x0B47, 0x0B48), (0x0B4B, 0x0B4D), (0x0B55, 0x0B57),
    (0x0B62, 0x0B63), (0x0B82, 0x0B82), (0x0BBE, 0x0BC2),
    (0x0BC6, 0x0BC8), (0x0BCA, 0x0BCD), (0x0BD7, 0x0BD7),
    (0x0C00, 0x0C04), (0x0C3C, 0x0C3C), (0x0C3E, 0x0C44),
    (0x0C46, 0x0C48), (0x0C4A, 0x0C4D), (0x0C55, 0x0C56),
    (0x0C62, 0x0C63), (0x0C81, 0x0C83), (0x0CBC, 0x0CBC),
    (0x0CBE, 0x0CC4), (0x0CC6, 0x0CC8), (0x0CCA, 0x0CCD),
    (0x0CD5, 0x0CD6), (0x0CE2, 0x0CE3), (0x0D00, 0x0D03),
    (0x0D3B, 0x0D3C), (0x0D3E, 0x0D44), (0x0D46, 0x0D48),
    (0x0D4A, 0x0D4D), (0x0D57, 0x0D57), (0x0D62, 0x0D63),
    (0x0D81, 0x0D83), (0x0DCA, 0x0DCA), (0x0DCF, 0x0DD4),
    (0x0DD6, 0x0DD6), (0x0DD8, 0x0DDF), (0x0DF2, 0x0DF3),
    (0x0E31, 0x0E31), (0x0E34, 0x0E3A), (0x0E47, 0x0E4E),
    (0x0EB1, 0x0EB1), (0x0EB4, 0x0EBC), (0x0EC8, 0x0ECD),
    (0x0F18, 0x0F19), (0x0F35, 0x0F35), (0x0F37, 0x0F37),
    (0x0F39, 0x0F39), (0x0F3E, 0x0F3F), (0x0F71, 0x0F84),
    (0x0F86, 0x0F87), (0x0F8D, 0x0F97), (0x0F99, 0x0FBC),
    (0x0FC6, 0x0FC6), (0x102B, 0x103E), (0x1056, 0x1059),
    (0x105E, 0x1060), (0x1062, 0x1064), (0x1067, 0x106D),
    (0x1071, 0x1074), (0x1082, 0x108D), (0x108F, 0x108F),
    (0x109A, 0x109D), (0x135D, 0x135F), (0x1712, 0x1715),
    (0x1732, 0x1734), (0x1752, 0x1753), (0x1772, 0x1773),
    (0x17B4, 0x17D3), (0x17DD, 0x17DD), (0x180B, 0x180D),
    (0x180F, 0x180F), (0x1885, 0x1886), (0x18A9, 0x18A9),
    (0x1920, 0x192B), (0x1930, 0x193B), (0x1A17, 0x1A1B),
    (0x1A55, 0x1A5E), (0x1A60, 0x1A7C), (0x1A7F, 0x1A7F),
    (0x1AB0, 0x1ACE), (0x1B00, 0x1B04), (0x1B34, 0x1B44),
    (0x1B6B, 0x1B73), (0x1B80, 0x1B82), (0x1BA1, 0x1BAD),
    (0x1BE6, 0x1BF3), (0x1C24, 0x1C37), (0x1CD0, 0x1CD2),
    (0x1CD4, 0x1CE8), (0x1CED, 0x1CED), (0x1CF4, 0x1CF4),
    (0x1CF7, 0x1CF9), (0x1DC0, 0x1DFF), (0x20D0, 0x20F0),
    (0x2CEF, 0x2CF1), (0x2D7F, 0x2D7F), (0x2DE0, 0x2DFF),
    (0x302A, 0x302F), (0x3099, 0x309A), (0xA66F, 0xA672),
    (0xA674, 0xA67D), (0xA69E, 0xA69F), (0xA6F0, 0xA6F1),
    (0xA802, 0xA802), (0xA806, 0xA806), (0xA80B, 0xA80B),
    (0xA823, 0xA827), (0xA82C, 0xA82C), (0xA880, 0xA881),
    (0xA8B4, 0xA8C5), (0xA8E0, 0xA8F1), (0xA8FF, 0xA8FF),
    (0xA926, 0xA92D), (0xA947, 0xA953), (0xA980, 0xA983),
    (0xA9B3, 0xA9C0), (0xA9E5, 0xA9E5), (0xAA29, 0xAA36),
    (0xAA43, 0xAA43), (0xAA4C, 0xAA4D), (0xAA7B, 0xAA7D),
    (0xAAB0, 0xAAB0), (0xAAB2, 0xAAB4), (0xAAB7, 0xAAB8),
    (0xAABE, 0xAABF), (0xAAC1, 0xAAC1), (0xAAEB, 0xAAEF),
    (0xAAF5, 0xAAF6), (0xABE3, 0xABEA), (0xABEC, 0xABED),
    (0xFB1E, 0xFB1E), (0xFE00, 0xFE0F), (0xFE20, 0xFE2F),
    (0x101FD, 0x101FD), (0x102E0, 0x102E0), (0x10376, 0x1037A),
    (0x10A01, 0x10A03), (0x10A05, 0x10A06), (0x10A0C, 0x10A0F),
    (0x10A38, 0x10A3A), (0x10A3F, 0x10A3F), (0x10AE5, 0x10AE6),
    (0x10D24, 0x10D27), (0x10EAB, 0x10EAC), (0x10F46, 0x10F50),
    (0x10F82, 0x10F85), (0x11000, 0x11002), (0x11038, 0x11046),
    (0x11070, 0x11070), (0x11073, 0x11074), (0x1107F, 0x11082),
    (0x110B0, 0x110BA), (0x110C2, 0x110C2), (0x11100, 0x11102),
    (0x11127, 0x11134), (0x11145, 0x11146), (0x11173, 0x11173),
    (0x11180, 0x11182), (0x111B3, 0x111C0), (0x111C9, 0x111CC),
    (0x111CE, 0x111CF), (0x1122C, 0x11237), (0x1123E, 0x1123E),
    (0x112DF, 0x112EA), (0x11300, 0x11303), (0x1133B, 0x1133C),
    (0x1133E, 0x11344), (0x11347, 0x11348), (0x1134B, 0x1134D),
    (0x11357, 0x11357), (0x11362, 0x11363), (0x11366, 0x1136C),
    (0x11370, 0x11374), (0x11435, 0x11446), (0x1145E, 0x1145E),
    (0x114B0, 0x114C3), (0x115AF, 0x115B5), (0x115B8, 0x115C0),
    (0x115DC, 0x115DD), (0x11630, 0x11640), (0x116AB, 0x116B7),
    (0x1171D, 0x1172B), (0x1182C, 0x1183A), (0x11930, 0x11935),
    (0x11937, 0x11938), (0x1193B, 0x1193E), (0x11940, 0x11940),
    (0x11942, 0x11943), (0x119D1, 0x119D7), (0x119DA, 0x119E0),
    (0x119E4, 0x119E4), (0x11A01, 0x11A0A), (0x11A33, 0x11A39),
    (0x11A3B, 0x11A3E), (0x11A47, 0x11A47), (0x11A51, 0x11A5B),
    (0x11A8A, 0x11A99), (0x11C2F, 0x11C36), (0x11C38, 0x11C3F),
    (0x11C92, 0x11CA7), (0x11CA9, 0x11CB6), (0x11D31, 0x11D36),
    (0x11D3A, 0x11D3A), (0x11D3C, 0x11D3D), (0x11D3F, 0x11D45),
    (0x11D47, 0x11D47), (0x11D8A, 0x11D8E), (0x11D90, 0x11D91),
    (0x11D93, 0x11D97), (0x11EF3, 0x11EF6), (0x16AF0, 0x16AF4),
    (0x16B30, 0x16B36), (0x16F4F, 0x16F4F), (0x16F51, 0x16F87),
    (0x16F8F, 0x16F92), (0x16FE4, 0x16FE4), (0x16FF0, 0x16FF1),
    (0x1BC9D, 0x1BC9E), (0x1CF00, 0x1CF2D), (0x1CF30, 0x1CF46),
    (0x1D165, 0x1D169), (0x1D16D, 0x1D172), (0x1D17B, 0x1D182),
    (0x1D185, 0x1D18B), (0x1D1AA, 0x1D1AD), (0x1D242, 0x1D244),
    (0x1DA00, 0x1DA36), (0x1DA3B, 0x1DA6C), (0x1DA75, 0x1DA75),
    (0x1DA84, 0x1DA84), (0x1DA9B, 0x1DA9F), (0x1DAA1, 0x1DAAF),
    (0x1E000, 0x1E006), (0x1E008, 0x1E018), (0x1E01B, 0x1E021),
    (0x1E023, 0x1E024), (0x1E026, 0x1E02A), (0x1E130, 0x1E136),
    (0x1E2AE, 0x1E2AE), (0x1E2EC, 0x1E2EF), (0x1E8D0, 0x1E8D6),
    (0x1E944, 0x1E94A), (0xE0100, 0xE01EF),
]

# The seven faces fonts.rc embeds -- but the coverage question is asked PER
# WEIGHT, because that is what an atlas actually holds. ui/fonts.cpp merges, for
# each baked face, the four families AT ONE WEIGHT (chosen + 3 backstops); a
# codepoint present only in the Bold faces is in no Regular atlas and vice versa.
# Unioning all seven would assert the constant against a set the atlas never has.
FACE_SETS = {
    "regular": ["Roboto-Regular.ttf", "JetBrainsMono-Regular.ttf",
                "CascadiaCode-Regular.ttf", "FSEX300.ttf"],
    "bold":    ["Roboto-Bold.ttf", "JetBrainsMono-Bold.ttf",
                "CascadiaCode-Bold.ttf", "FSEX300.ttf"],   # Fixedsys has one weight
}

# ImFontAtlas::GetGlyphRangesCyrillic (imgui_draw.cpp:3525-3536) -- the set the
# mod baked from b100 until the 1.92 flip.
#
# DEMOTED 2026-07-30 FROM "THE ASK" TO "THE COVERAGE TRIPWIRE". Post-flip the
# repertoire is the face cmaps minus the exclude set, so this list no longer
# decides what bakes or what folds -- every codepoint in it is already inside the
# render set. It stays because it is a useful assertion about the FONTS: these
# are the ranges players are actually named in, and EXPECTED_BASE_GAP below
# fails the build if a family or donor swap ever stops covering one of them.
# RULE 2 check, deliberate: this is not a dead knob kept "for now" -- widening it
# changes nothing and it is documented here as changing nothing. The knob that
# DID decide what renders (the `ranges` parameter threaded through ui/fonts.cpp)
# is deleted in the same commit.
BASE_RANGES = [
    (0x0020, 0x00FF), (0x0400, 0x052F), (0x2DE0, 0x2DFF), (0xA640, 0xA69F),
    # Latin Extended-A/B and Greek. MEASURED FREE 2026-07-29 (s7, the
    # bake-everything probe): our own embedded faces already CARRY these glyphs --
    # Latin Ext-A/B complete, Greek 135/144 -- so asking for them costs a
    # byte-identical texture at every scale in the default config, +1,787 glyphs and
    # about +7 ms of bake. Nothing is downloaded and nothing is shipped; the atlas
    # simply stops refusing to draw what the fonts already contain.
    #
    # This is `Michal / Simon / Gunes / Stefan / Giorgos` spelled properly instead of
    # as replacement boxes. It is deliberately SEPARATE from the CJK question, which
    # is not free (64-256 MB of atlas) and is answered by on-demand OS fonts.
    #
    # Widening the repertoire moves the FOLD TABLE and the ATLAS together -- one
    # generator, by construction -- so a name that used to collapse into U+FFFD now
    # folds as itself. That is the intended direction: fewer names collide because
    # more names are legible.
    (0x0100, 0x017F), (0x0180, 0x024F), (0x0370, 0x03FF),
    # U+FFFD REPLACEMENT CHARACTER, and it has to be asked for EXPLICITLY.
    # ImFont::BuildLookupTable (imgui_draw.cpp:3700) picks the fallback glyph
    # from { U+FFFD, '?', ' ' } -- in that order, from what is BAKED. All seven
    # embedded faces have U+FFFD in their cmap, so "cross-merge gives every
    # family U+FFFD" sounded true and was not: no glyph range ever ASKED for it,
    # so nothing baked it and the fallback fell through to '?'. Every absent
    # codepoint then drew as a question mark, indistinguishable from a name that
    # really contains one. Caught by the boot font selftest on its first run,
    # against a design that asserted the opposite mechanism.
    (0xFFFD, 0xFFFD),
]

# A positive control on the "cross-merge closes the base ask" claim (9d.2 item 6).
# Measured 2026-07-28: the union of the seven faces covers every codepoint of
# BASE_RANGES except exactly one, U+A69E (CYRILLIC SMALL LETTER IOTIFIED A). If a
# future donor/family swap changes that, this script FAILS rather than silently
# shrinking what players can be uniquely named in.
EXPECTED_BASE_GAP = {
    0xA69E,  # CYRILLIC SMALL LETTER IOTIFIED A -- a real hole in our faces.
    # The nine below are NOT holes in our faces. Every one is a PERMANENTLY
    # UNASSIGNED Unicode codepoint inside the Greek and Coptic block, so no font can
    # cover them and nothing can ever be named with them. MEASURED 2026-07-29 by the
    # gate below on the regular weight; s7's "Greek 135/144" figure was this and only
    # this -- 144 codepoints in the block minus these 9 reserved slots = 135 assigned,
    # ALL of which our embedded faces carry. Recorded explicitly so a later reader
    # does not go looking for a font that "fixes" them.
    0x0378, 0x0379, 0x0380, 0x0381, 0x0382, 0x0383, 0x038B, 0x038D, 0x03A2,
}

# THE ADJUDICATED ZERO-ADVANCE RESIDUE (2026-07-30). Measured from hmtx: 523
# codepoints in the render set have zero advance width, and the exclude set
# already covers 520 of them (they are combining marks, ignorables or PUA). These
# three are not marks -- they are General_Category Po, Armenian punctuation, and
# only FSEX300 gives them zero advance (Roboto and the rest advance normally).
#
# ADMITTED, deliberately, and here is the reasoning a future reader should not
# have to redo. The uniqueness invariant is violated when two DIFFERENT fold keys
# produce the SAME pixels. These three draw real ink, so `A` + U+055B is visibly
# different from `A`, and no other string in the repertoire renders like it --
# there is nothing for them to collide WITH. They are also legitimate Armenian
# punctuation: excluding them would delete real characters from a real script to
# protect an invariant they do not threaten.
#
# The gate is on the SET, not on the count: a NEW zero-advance codepoint appearing
# after a font swap gets adjudicated by a human, because whether it can collide is
# a question about what else the repertoire draws.
EXPECTED_ZERO_ADVANCE = {0x055B, 0x055C, 0x055E}


def to_ranges(cps):
    """Sorted codepoint set -> minimal list of inclusive (begin, end) runs."""
    out = []
    for cp in sorted(cps):
        if out and cp == out[-1][1] + 1:
            out[-1][1] = cp
        else:
            out.append([cp, cp])
    return [(a, b) for a, b in out]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--donor", required=True, help="path to Twemoji.Mozilla.ttf")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    from fontTools.ttLib import TTFont
    from fontTools import subset

    # --- 1. the donor subset ------------------------------------------------
    src = TTFont(args.donor, lazy=False)
    donor_cmap = set(src.getBestCmap().keys())
    opts = subset.Options()
    opts.layout_features = []        # no shaping happens; GSUB/GPOS are dead weight
    opts.name_IDs = ["*"]            # keep the name table: it carries the attribution
    opts.notdef_outline = True
    opts.drop_tables = ["DSIG"]
    opts.color_layers = True         # COLR/CPAL must survive -- they ARE the emoji
    s = subset.Subsetter(options=opts)
    s.populate(unicodes=sorted(donor_cmap))
    s.subset(src)
    buf = io.BytesIO()
    src.save(buf)
    donor_bytes = buf.getvalue()

    kept = TTFont(io.BytesIO(donor_bytes), lazy=True)
    kept_cmap = set(kept.getBestCmap().keys())
    astral = len([c for c in kept_cmap if c > 0xFFFF])
    print(f"donor  : {os.path.basename(args.donor)} -> {SUBSET_NAME}")
    print(f"         {len(kept_cmap)} codepoints ({astral} astral), "
          f"{len(donor_bytes):,} bytes (source {os.path.getsize(args.donor):,})")
    for t in ("COLR", "CPAL"):
        if t not in kept:
            sys.exit(f"FAIL: the subset dropped {t} -- the emoji would bake as outlines")

    # --- 2. the embedded faces, PER WEIGHT ----------------------------------
    base_ask = set()
    for a, b in BASE_RANGES:
        base_ask |= set(range(a, b + 1))

    per_weight = {}
    for weight, faces in FACE_SETS.items():
        w_union = set()
        for fn in faces:
            fp = os.path.join(ASSETS, fn)
            if not os.path.isfile(fp):
                sys.exit(f"FAIL: missing embedded face {fp}")
            w_union |= set(TTFont(fp, lazy=True).getBestCmap().keys())
        gap = base_ask - w_union
        print(f"faces  : {weight:<8} {len(faces)} faces, union {len(w_union):,} cp, "
              f"base gap {sorted(hex(c) for c in gap)}")
        if gap != EXPECTED_BASE_GAP:
            sys.exit(f"FAIL: the {weight} base coverage gap moved -- expected "
                     f"{sorted(hex(c) for c in EXPECTED_BASE_GAP)}, got "
                     f"{sorted(hex(c) for c in gap)}. Re-derive before shipping: this "
                     f"set decides which names render, and therefore which names fold. "
                     f"Both weights must satisfy it -- an atlas only ever merges one.")
        per_weight[weight] = w_union

    # ONE FOLD TABLE, TWO WEIGHTS -- so the two weights must cover the SAME set.
    # An atlas merges one weight's four faces plus the donor, and the fold table
    # is a single build constant shared by both. A codepoint present only in Bold
    # would therefore be foldable-as-itself in a Regular atlas that cannot draw
    # it: fold != bake, which is the exact invariant this generator exists to
    # hold. Measured 2026-07-30: the two unions are IDENTICAL (8,148 each,
    # symmetric difference 0), so this is an assertion about a property that
    # already holds, not a constraint being imposed.
    only_reg = per_weight["regular"] - per_weight["bold"]
    only_bold = per_weight["bold"] - per_weight["regular"]
    if only_reg or only_bold:
        sys.exit(f"FAIL: the two weights no longer cover the same codepoints "
                 f"(regular-only {len(only_reg)}, bold-only {len(only_bold)}). "
                 f"One fold table cannot describe two different atlases -- either "
                 f"re-subset the faces to agree, or the fold table must become "
                 f"per-weight and the arbiter must learn which weight a peer runs.")
    faces_union = per_weight["regular"]
    gap = base_ask - faces_union
    print(f"base   : ask {len(base_ask)}, covered by BOTH weights {len(base_ask - gap)}, "
          f"gap {sorted(hex(c) for c in gap)}")

    # --- 3. render, exclude, fold -------------------------------------------
    #
    # RENDER is now the whole cmap, not the base ask. Under the lazy atlas
    # anything a source carries can be baked on demand, so the honest render set
    # is every codepoint the faces or the donor have -- and the fold table has to
    # match THAT, not what some inclusion list used to request.
    #
    # Default_Ignorable is SUBTRACTED, and the reason is measured rather than
    # tidy. ImFont::GrowIndex(max_codepoint + 1) (imgui_draw.cpp:3669) sizes
    # IndexLookup + IndexAdvanceX to the LARGEST codepoint baked, at 8 bytes an
    # entry with ImWchar32. Twemoji's cmap carries ten TAG characters up at
    # U+E0062..U+E007F -- the letters of the subdivision-flag sequences, which
    # cannot compose here at all (FT_DISABLE_HARFBUZZ, and ImGui does no
    # shaping). Baking them moves max from U+1FAF6 to U+E007F and the index
    # tables from 1.04 MB to 7.34 MB PER FACE: 3.1 -> 22.0 MB on the default
    # config, 5.2 -> 36.7 MB on the worst. Nineteen megabytes for ten
    # codepoints that can only ever draw as the fallback box.
    def expand(rs):
        out = set()
        for a, b in rs:
            out |= set(range(a, b + 1))
        return out

    ignorable = expand(DEFAULT_IGNORABLE)
    private = expand(PRIVATE_USE)
    no_ink = expand(NO_INK)
    marks = expand(COMBINING_MARKS)

    render = faces_union | kept_cmap
    marks_in_render = marks & render
    # COMMIT 2 (2026-07-30): the marks are NO LONGER excluded.
    #
    # Commit 1 excluded them to preserve exactly the pre-flip behaviour, on the
    # design's claim that admitting them would let "A" + U+0301 draw
    # pixel-identically to U+00C1 while folding to a different key. That claim was
    # MEASURED FALSE before this change landed: rendered through FreeType with no
    # shaping (which is what ImGui does -- FT_DISABLE_HARFBUZZ, CMakeLists.txt:81),
    # a base+mark pair is pixel-identical to its precomposed form in exactly
    # 1 of ~3,560 face-pair combinations. There is no GPOS anchor, so the mark
    # draws at its own left-side-bearing and the two are visibly different.
    #
    # So the NFC machinery the design specified (814 composition pairs + canonical
    # combining classes) would have collided things that do not look alike, to
    # prevent a collision that does not occur. It was dropped, not deferred.
    #
    # WHAT EXCLUDING THEM COST, and it is the reason this is a user-visible
    # commit rather than an invariant-preserving one: the marks are how five
    # scripts are actually written. Measured in the shipped faces --
    #   THAANA  11 of 11 -- Dhivehi is written ENTIRELY in Mn, so it was unwritable
    #   TAMIL   12 of 12 vowel signs
    #   THAI    16 of 26 tone marks
    #   ARABIC  17 of 33 harakat
    #   HEBREW   8 of 10 points
    # were excluded, in the build whose release note advertises Hebrew, Thai and
    # Arabic. The base letters drew; the marks were fallback boxes.
    #
    # TWO MARKS STAY OUT, and not by a mark rule: U+034F (COMBINING GRAPHEME
    # JOINER) and U+FE0F (VARIATION SELECTOR-16) are Default_Ignorable, so
    # `ignorable` still holds them. That is exactly right -- U+034F is the arc-D2
    # defect (advance 0, NO CONTOURS, invisible mid-name) and the ink gate below
    # exists to prove nothing else in the class is like it. 337 - 2 = 335 admitted,
    # which is precisely the repertoire's growth.
    exclude = no_ink | ignorable | private
    rep = render - exclude
    ranges = to_ranges(rep)
    admitted_marks = marks_in_render - exclude

    exc_ranges = to_ranges(exclude)
    print(f"render : {len(render):,} codepoints (faces {len(faces_union):,} + donor "
          f"{len(kept_cmap):,}), max U+{max(render):04X}")
    print(f"exclude: {len(exc_ranges)} ranges / {len(exc_ranges) * 2} values "
          f"(no-ink {len(no_ink):,} + ignorable {len(ignorable):,} + private "
          f"{len(private):,}; combining marks are ADMITTED as of commit 2)")
    print(f"repert.: {len(rep):,} codepoints in {len(ranges)} ranges "
          f"(max U+{max(rep):04X}); removed from render: "
          f"{len(render & ignorable)} ignorable, {len(render & private)} private-use, "
          f"{len(render & no_ink)} no-ink")
    print(f"marks  : {len(marks_in_render)} in render, {len(admitted_marks)} ADMITTED "
          f"(held back by Default_Ignorable: "
          f"{', '.join('U+%04X' % c for c in sorted(marks_in_render & exclude)) or 'none'})")

    # --- 3a. THE ZERO-ADVANCE CENSUS -- the tripwire the categories cannot be --
    #
    # Arc D2's invariant is about PIXELS; the tables above are about Unicode
    # CATEGORIES, and the two are not the same set. A codepoint that draws ink at
    # zero advance composes onto its neighbour, so a name containing it can be
    # pixel-identical to a name without it while folding to a different key.
    # Categories catch the marks; they do not catch a font that assigns zero
    # advance to something else. So measure the fonts, and require a human to
    # adjudicate anything the exclude set does not already cover.
    zero_advance = set()
    for fn in sorted(set(sum(FACE_SETS.values(), []))) + [SUBSET_NAME]:
        fp = os.path.join(ASSETS, fn)
        f = TTFont(fp, lazy=True) if fn != SUBSET_NAME else TTFont(io.BytesIO(donor_bytes))
        metrics = f["hmtx"].metrics
        for cp, gname in f.getBestCmap().items():
            if gname in metrics and metrics[gname][0] == 0:
                zero_advance.add(cp)
    # THE MARKS ARE SUBTRACTED HERE, AND THAT WOULD GUT THE GATE ON ITS OWN.
    #
    # Before commit 2 the marks were inside `exclude`, so this residue was 3. They
    # are admitted now, and leaving them in would make the residue 322 -- the gate
    # would fire on every run and the only way to quiet it is to widen the constant,
    # which its own message forbids. Subtracting the class keeps the gate's original
    # power exactly: it still catches the next NON-mark codepoint a font update
    # gives zero advance, which is the case a human has to adjudicate because
    # whether it can collide depends on what else the repertoire draws.
    #
    # But subtraction alone would blind it to a zero-advance mark that is also
    # BLANK, and that is the dangerous shape -- an invisible character inside a
    # nickname, which is U+034F one level down, inside the very class being
    # admitted. This gate could never have caught it anyway: it tests hmtx ADVANCE
    # and never ink. So the ink gate below covers the class this one now skips.
    residue = zero_advance - exclude - marks_in_render
    print(f"zero-adv: {len(zero_advance)} in render, {len(residue)} NOT excluded "
          f"({', '.join('U+%04X' % c for c in sorted(residue))}) "
          f"[{len(admitted_marks)} admitted marks are covered by the ink gate instead]")
    if residue != EXPECTED_ZERO_ADVANCE:
        sys.exit(f"FAIL: the zero-advance residue moved -- expected "
                 f"{sorted('U+%04X' % c for c in EXPECTED_ZERO_ADVANCE)}, got "
                 f"{sorted('U+%04X' % c for c in residue)}. Every codepoint here "
                 f"draws ink WITHOUT advancing the pen, so it stacks onto its "
                 f"neighbour: two names that differ only by one of these can look "
                 f"identical while folding differently. Adjudicate each new one by "
                 f"hand -- admit it (and record why nothing else draws like it) or "
                 f"add it to the exclude set. Do not widen this constant to make "
                 f"the build pass.")

    # --- 3b. THE INK GATE -- what admitting the marks actually needs proved ---
    #
    # The threat a combining mark carries is NOT that it draws at zero advance --
    # that is what a mark IS, and a name with one is visibly different from a name
    # without it (measured: base+mark matches its precomposed form in 1 of ~3,560
    # face-pair combinations, so they do not collide). The threat is a mark that
    # draws NOTHING: an invisible, zero-width character that can sit inside a
    # nickname, survive to the wire, and give two identical-looking names two
    # different fold keys. That is exactly the U+034F defect arc D2 closed, and
    # admitting 335 more marks is precisely the move that could re-open it.
    #
    # So: every ADMITTED mark must have outline contours in every face that claims
    # it. numberOfContours == 0 is a glyph with no outline at all -- measured to
    # agree exactly with rasterising the mark and testing the bitmap for ink (both
    # find U+034F in FSEX300, Roboto-Regular and Roboto-Bold, and nothing else), so
    # this needs no rasteriser and no new dependency.
    #
    # It is a hard fail rather than a warning because the failure is SILENT
    # downstream: nothing at runtime can distinguish "this mark drew nothing"
    # from "this mark drew correctly over its base".
    blank_marks = {}
    for fn in sorted(set(sum(FACE_SETS.values(), []))):
        fp = os.path.join(ASSETS, fn)
        f = TTFont(fp, lazy=True)
        if "glyf" not in f:
            continue          # CFF outlines: no contour count to read
        glyf, bc = f["glyf"], f.getBestCmap()
        for cp in sorted(admitted_marks & set(bc)):
            if glyf[bc[cp]].numberOfContours == 0:
                blank_marks.setdefault(cp, []).append(fn)
    print(f"ink gate: {len(admitted_marks)} admitted marks checked for outlines, "
          f"{len(blank_marks)} blank")
    if blank_marks:
        detail = "; ".join(f"U+{cp:04X} in {', '.join(fs)}"
                           for cp, fs in sorted(blank_marks.items()))
        sys.exit(f"FAIL: {len(blank_marks)} ADMITTED combining mark(s) have no "
                 f"outline in a face that claims them -- {detail}. A mark that "
                 f"draws nothing at zero advance is an INVISIBLE character inside "
                 f"a nickname: two names that look identical get different fold "
                 f"keys and the arbiter never disambiguates them. That is the "
                 f"U+034F defect (closed by arc D2) arriving one level down, "
                 f"inside the class this commit admits. Add it to the exclude set "
                 f"-- do NOT admit an invisible codepoint.")

    # --- 4. the two emissions ------------------------------------------------
    #
    # THE CLAMP. ImGui walks GlyphExcludeRanges as a ZERO-TERMINATED array --
    # ImFontAtlasBuildAcceptCodepointForSource does `for (; exclude_list[0] != 0;
    # exclude_list += 2)` (imgui_draw.cpp:4539-4542) and the sizing loop at
    # :3111-3113 counts the same way. no-ink begins at Cc = U+0000, so an
    # unclamped emission starts with 0, the walk terminates at index 0, and
    # NOTHING IS EVER EXCLUDED. Both IM_ASSERTs at :3114-3115 would pass (size 0
    # is even and <= 64) and both are stripped under NDEBUG anyway, so no build
    # of ours can see it. The fold table must NOT be clamped the same way: U+0000
    # is in the FSEX300 and Roboto cmaps, so removing it from the shared set
    # would ADMIT NUL to the repertoire and let a name fold on it.
    exclude_emit = exclude - {0x0000}
    exc_emit_ranges = to_ranges(exclude_emit)
    if exc_emit_ranges[0][0] == 0:
        sys.exit("FAIL: the exclude emission still begins at U+0000. ImGui reads this "
                 "list as zero-terminated, so it would silently exclude NOTHING and "
                 "every codepoint any face carries would bake. Nothing downstream can "
                 "detect this -- not the asserts (stripped under NDEBUG), not the "
                 "selftest, not a smoke.")
    if exclude - exclude_emit != {0x0000}:
        sys.exit(f"FAIL: the fold set and the exclude emission must differ by exactly "
                 f"{{U+0000}}, but they differ by "
                 f"{sorted('U+%04X' % c for c in (exclude - exclude_emit))}. One source "
                 f"set, two emissions -- any other difference means fold != bake.")
    print(f"exc.emit: {len(exc_emit_ranges)} ranges / {len(exc_emit_ranges) * 2} values, "
          f"first U+{exc_emit_ranges[0][0]:04X} (ImGui's advisory cap is 64 values; "
          f"exceeding it is a longer linear scan, not a break -- imgui_draw.cpp:4539)")

    head = [
        "// coop/text/{name} -- GENERATED by tools/text/build_repertoire.py.",
        "// Do not edit by hand.",
    ]
    rep_lines = [h.format(name="repertoire_ranges.inc") for h in head] + [
        "//",
        "// THE FOLD SET: every codepoint this build can DRAW, as inclusive",
        "// (begin, end) runs. Derived as `render - exclude` where render is the",
        "// union of the embedded faces' and the donor's cmaps -- so it is exactly",
        "// what the lazy atlas will bake, because exclude_ranges.inc (the same",
        "// source set, one codepoint apart) is what forbids the rest.",
        "//",
        "// coop::text::InRepertoire folds names against this; ui::fonts hands the",
        "// COMPLEMENT to ImGui. What a name folds by is what a name renders as.",
        "//",
        f"// {len(rep)} codepoints, {len(ranges)} ranges, max U+{max(rep):04X}.",
        f"// Render set {len(render)} = faces {len(faces_union)} + donor "
        f"{len(kept_cmap)} ({SUBSET_NAME}).",
        f"// Excluded from it: {len(render & no_ink)} no-ink, "
        f"{len(render & ignorable)} Default_Ignorable, {len(render & private)} "
        f"private-use.",
        f"// Combining marks are IN: {len(admitted_marks)} of {len(marks_in_render)} "
        f"admitted (commit 2), which is how Thaana, Tamil, Thai, Arabic and Hebrew",
        "// are actually written. The rest are held out by Default_Ignorable.",
        f"// Base-ask coverage gap (no embedded face has it): "
        f"{', '.join('U+%04X' % c for c in sorted(gap))}.",
        "",
    ] + [f"    {{ 0x{a:05X}, 0x{b:05X} }}," for a, b in ranges]

    exc_lines = [h.format(name="exclude_ranges.inc") for h in head] + [
        "//",
        "// THE EXCLUDE SET, handed to ImGui as ImFontConfig::GlyphExcludeRanges on",
        "// every font source. It is the complement of repertoire_ranges.inc within",
        "// the render set, and the two are minted from ONE source set in one run --",
        "// that is what keeps `fold == bake` true under a lazy atlas.",
        "//",
        "// U+0000 IS DELIBERATELY ABSENT and must stay absent: ImGui walks this",
        "// list as a zero-terminated array, so a leading zero would silently make",
        "// the whole list a no-op. U+0000 is still OUT of the fold set -- the two",
        "// emissions differ by exactly that one codepoint, asserted by the",
        "// generator. ImGui text is NUL-terminated, so nothing can request it.",
        "//",
        f"// {len(exclude_emit)} codepoints, {len(exc_emit_ranges)} ranges "
        f"({len(exc_emit_ranges) * 2} values). ImGui's IM_ASSERT at",
        "// imgui_draw.cpp:3114 advises <= 64 values; it is a linear-scan hint, not",
        "// a correctness bound, and it is stripped under NDEBUG either way.",
        "",
    ] + [f"    {{ 0x{a:05X}, 0x{b:05X} }}," for a, b in exc_emit_ranges]

    ign_ranges = to_ranges(ignorable)
    ign_lines = [h.format(name="ignorable_ranges.inc") for h in head] + [
        "//",
        "// Unicode 15.1 Default_Ignorable_Code_Point=Yes. Characters DEFINED to",
        "// have no visible rendering, which is why a nickname may not contain one:",
        "// a name whose codepoints differ only here is a distinct fold key with",
        "// identical pixels -- the exact failure arc D2 exists to close.",
        "//",
        f"// {len(ignorable)} codepoints, {len(ign_ranges)} ranges.",
        "",
    ] + [f"    {{ 0x{a:05X}, 0x{b:05X} }}," for a, b in ign_ranges]

    mark_ranges = to_ranges(admitted_marks)
    mark_lines = [h.format(name="mark_ranges.inc") for h in head] + [
        "//",
        "// The combining marks this build can DRAW (General_Category Mn/Me/Mc,",
        "// intersected with the render set, minus anything the exclude set still",
        "// holds). Its ONE consumer is the leading-mark rule in SanitizeNickname:",
        "// a mark at position 0 has no base to sit on, so it stacks onto whatever",
        "// the UI drew before the name.",
        "//",
        "// IT EXISTS BECAUSE THE RULE USED TO CARRY ITS OWN LITERAL. That literal",
        "// was `c >= 0x0300 && c <= 0x036F` -- one block, written when no other",
        "// mark could bake. Commit 2 admits Thaana, Tamil, Thai, Arabic and Hebrew",
        "// marks, every one of which draws, so the hand-written range would have",
        "// policed Latin diacritics and silently let the other five scripts stack",
        "// onto the UI. One concept, one place (RULE 2).",
        "//",
        "// Only ADMITTED marks are listed: an excluded one folds to the sentinel",
        "// and cannot draw, so it has nothing to stack with.",
        "//",
        f"// {len(admitted_marks)} codepoints, {len(mark_ranges)} ranges.",
        "",
    ] + [f"    {{ 0x{a:05X}, 0x{b:05X} }}," for a, b in mark_ranges]

    if args.dry_run:
        print("\n(dry run -- nothing written)")
        return

    with open(os.path.join(ASSETS, SUBSET_NAME), "wb") as f:
        f.write(donor_bytes)
    for name, lines in (("repertoire_ranges.inc", rep_lines),
                        ("exclude_ranges.inc", exc_lines),
                        ("ignorable_ranges.inc", ign_lines),
                        ("mark_ranges.inc", mark_lines)):
        with open(os.path.join(GEN, name), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        print(f"wrote {os.path.join(GEN, name)}")
    print(f"wrote {os.path.join(ASSETS, SUBSET_NAME)}")


main()
