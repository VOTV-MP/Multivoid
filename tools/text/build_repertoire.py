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
  2. include/coop/text/repertoire_ranges.inc -- the codepoint ranges that are
     (a) present in at least one embedded face, or (b) present in the donor.

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
# mod has baked since b100. Kept as the BASE ask so the repertoire can only grow.
BASE_RANGES = [
    (0x0020, 0x00FF), (0x0400, 0x052F), (0x2DE0, 0x2DFF), (0xA640, 0xA69F),
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
EXPECTED_BASE_GAP = {0xA69E}


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

    union = set()
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
        union = w_union if not union else (union & w_union)
    gap = base_ask - union
    print(f"base   : ask {len(base_ask)}, covered by BOTH weights {len(base_ask - gap)}, "
          f"gap {sorted(hex(c) for c in gap)}")

    # --- 3. the repertoire = what a peer can actually SEE --------------------
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
    ignorable = set()
    for a, b in DEFAULT_IGNORABLE:
        ignorable |= set(range(a, b + 1))
    private = set()
    for a, b in PRIVATE_USE:
        private |= set(range(a, b + 1))
    drawable = (base_ask - gap) | kept_cmap
    rep = drawable - ignorable - private
    ranges = to_ranges(rep)
    dropped = sorted(drawable & ignorable)
    dropped_pua = sorted(drawable & private)
    print(f"repert.: {len(rep):,} codepoints in {len(ranges)} ranges "
          f"(max U+{max(rep):04X}); dropped {len(dropped)} ignorable "
          f"({', '.join('U+%04X' % c for c in dropped[:4])}...) + "
          f"{len(dropped_pua)} private-use "
          f"({', '.join('U+%04X' % c for c in dropped_pua)})")

    head = [
        "// coop/text/{name} -- GENERATED by tools/text/build_repertoire.py.",
        "// Do not edit by hand.",
    ]
    rep_lines = [h.format(name="repertoire_ranges.inc") for h in head] + [
        "//",
        "// Every codepoint at least one embedded face or the emoji donor can DRAW,",
        "// as inclusive (begin, end) runs. It is consumed twice and that is the",
        "// point: coop::text::InRepertoire folds against it and ui::fonts::Load",
        "// bakes it, so what a name folds by is exactly what a name renders as.",
        "//",
        f"// {len(rep)} codepoints, {len(ranges)} ranges, max U+{max(rep):04X}.",
        f"// Base ask = ImGui's Cyrillic ranges minus "
        f"{', '.join('U+%04X' % c for c in sorted(gap))} (no embedded face has it).",
        f"// Donor = {SUBSET_NAME}, {len(kept_cmap)} codepoints.",
        f"// Default_Ignorable subtracted: {len(dropped)}; private-use: {len(dropped_pua)}.",
        "",
    ] + [f"    {{ 0x{a:05X}, 0x{b:05X} }}," for a, b in ranges]

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

    if args.dry_run:
        print("\n(dry run -- nothing written)")
        return

    with open(os.path.join(ASSETS, SUBSET_NAME), "wb") as f:
        f.write(donor_bytes)
    for name, lines in (("repertoire_ranges.inc", rep_lines),
                        ("ignorable_ranges.inc", ign_lines)):
        with open(os.path.join(GEN, name), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        print(f"wrote {os.path.join(GEN, name)}")
    print(f"wrote {os.path.join(ASSETS, SUBSET_NAME)}")


main()
