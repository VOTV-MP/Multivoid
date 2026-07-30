"""The CASE-FOLD table -- simple lowercase, restricted to what this build draws.

Called by build_repertoire.py, which owns the face list and the repertoire.

WHY IT IS GENERATED. It replaces a HAND-WRITTEN table in nickname_arbiter.cpp
that covered ASCII, Latin-1 and Cyrillic, and whose comment claimed those were
"exactly the cased scripts the repertoire draws".

That was TRUE ON THE DAY IT WAS WRITTEN. Then two unrelated changes widened the
repertoire -- s13 added Latin Extended and Greek, and the lazy-atlas flip added
+4,741 codepoints -- and neither touched the function. Nothing compiled
differently, no test failed, no log line changed. Measured 2026-07-30 against the
current repertoire:

    cased codepoints whose lowercase is ALSO drawable : 890
    FoldCase returned IDENTITY (missed the fold)      : 649  (73%)
       LATIN 374 | GREEK 146 | ARMENIAN 38 | GEORGIAN 38
       CIRCLED 26 | ROMAN 17 | COPTIC 7 | OHM/KELVIN/ANGSTROM 3
    FoldCase mapped to the WRONG codepoint            : 0

Incomplete, never incorrect -- which is exactly why nobody saw it. A wrong fold
makes a visibly wrong name; a MISSING fold just means two names were not
deduplicated, and a lobby with no collision looks identical to a lobby whose
collisions were all caught. `Ωμέγα` and `ωμέγα` do not collide in b133.

THE FREEZE AND THE CENSUS ARE TWO DIFFERENT MECHANISMS, and both are needed:
  - the EMISSION comes from a frozen Unicode 14.0.0 table (unicode_case_14_0_0.py)
    so the constant does not move with the generator host's CPython -- the fold is
    a cross-peer agreement about which names collide;
  - the CLAIM is guarded by a LIVE unicodedata census that fails the build if any
    cased-and-drawable codepoint is left unfolded. Freezing alone would have
    reproduced the original defect with better provenance.
"""

import os

from unicode_case_14_0_0 import SIMPLE_LOWERCASE, UNICODE_VERSION


def runs(fold):
    """Frozen simple-lowercase, intersected with what this build can draw.

    Both halves must be drawable: folding `Ω` to a `ω` the atlas cannot bake
    would map a visible character onto one that renders as the sentinel.
    """
    out = []
    for begin, end, delta in SIMPLE_LOWERCASE:
        run = None
        for cp in range(begin, end + 1):
            if cp in fold and (cp + delta) in fold:
                if run and run[1] == cp - 1:
                    run[1] = cp
                else:
                    run = [cp, cp, delta]
                    out.append(run)
            else:
                run = None
    return [tuple(r) for r in out]


def census(fold, emitted, live_unicodedata, fail):
    """The check the hand table never had. Two gates, two failure modes."""
    if live_unicodedata.unidata_version != UNICODE_VERSION:
        fail(f"unicode_case_14_0_0.py was minted from Unicode {UNICODE_VERSION}; "
             f"this Python ships {live_unicodedata.unidata_version}. A Unicode "
             f"bump is a deliberate re-freeze -- the case fold is a CROSS-PEER "
             f"agreement about which names collide, not a local convenience.")
    covered = set()
    for begin, end, _ in emitted:
        covered |= set(range(begin, end + 1))
    missing = []
    for cp in sorted(fold):
        lo = chr(cp).lower()
        if len(lo) == 1 and ord(lo) != cp and ord(lo) in fold and cp not in covered:
            missing.append(cp)
    if missing:
        fail(f"{len(missing)} cased-and-drawable codepoints are NOT folded by the "
             f"emitted table, first "
             f"{', '.join('U+%04X' % c for c in missing[:8])}. This is the exact "
             f"gap the hand-written FoldCase carried for two sessions.")
    return len(covered)


def emit(emitted, gen_dir, header):
    lines = header("case_fold.inc") + [
        "//",
        "// SIMPLE LOWERCASE, restricted to pairs this build can DRAW.",
        "// { begin, end, delta }: lower(cp) == cp + delta for begin <= cp <= end.",
        "//",
        "// IT REPLACES A HAND TABLE THAT WENT 73% STALE IN SILENCE. That table",
        "// covered ASCII, Latin-1 and Cyrillic and its comment called those",
        "// \"exactly the cased scripts the repertoire draws\" -- true when written,",
        "// falsified by the Latin-Ext/Greek widening and by the lazy-atlas flip's",
        "// +4,741 codepoints, with no edit to the function and nothing to notice.",
        "// Measured before this file existed: 649 of 890 cased-and-drawable",
        "// codepoints folded to THEMSELVES (LATIN 374, GREEK 146, ARMENIAN 38,",
        "// GEORGIAN 38, CIRCLED 26, ROMAN 17, COPTIC 7); none folded WRONG.",
        "//",
        f"// Minted from Unicode {UNICODE_VERSION}, FROZEN in",
        "// tools/text/unicode_case_14_0_0.py. A live unicodedata census in",
        "// tools/text/case_fold.py fails the build on any cased-and-drawable",
        "// codepoint this table leaves unfolded -- the freeze governs the",
        "// emission, the census guards the claim.",
        "//",
        f"// {sum(e - b + 1 for b, e, _ in emitted)} codepoints in {len(emitted)} runs.",
        "",
    ] + [f"    {{ 0x{b:05X}, 0x{e:05X}, {d} }}," for b, e, d in emitted]
    path = os.path.join(gen_dir, "case_fold.inc")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {path}")
