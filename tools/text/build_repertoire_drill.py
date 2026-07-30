#!/usr/bin/env python3
"""build_repertoire_drill.py -- the must-FAIL control for build_repertoire.py's gates.

WHY THIS FILE EXISTS. build_repertoire.py grew four hard `sys.exit` gates in the
2026-07-30 flip commit, and this project has been burned repeatedly by assertions
that were only ever observed PASSING
([[lesson-an-instrument-never-shown-failing-passes-by-construction]]). One of the
four -- the leading-zero check -- guards a defect that NOTHING downstream can
see: an exclude list beginning with U+0000 is read by ImGui as an empty list, so
every codepoint bakes, `fold != bake` silently, and both of ImGui's own asserts
pass (size 0 is even and <= 64) before NDEBUG strips them anyway. A gate for an
invisible defect that has never been shown to fire is decoration.

HOW. Each drill copies the generator's SOURCE to a temp file, applies one textual
mutation that re-introduces the defect the gate exists to catch, and runs the
copy. A drill PASSES when the mutated generator EXITS NON-ZERO with the expected
message. There is no switch, flag, or env var in the production generator -- a
"drill mode" knob would be a live-looking path RULE 2 forbids, and it would let
the drill and the real run diverge.

Run:  python tools/text/build_repertoire_drill.py
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
GEN = os.path.join(HERE, "build_repertoire.py")
DONOR = os.path.join(REPO, "src", "votv-coop", "assets", "fonts",
                     "TwemojiMozilla-Subset.ttf")

# (name, mutation, the phrase the gate must print)
#
# Each mutation re-creates a REAL failure mode, not a synthetic one:
#   clamp      -- the U+0001 clamp removed. This is the exact code that was
#                 specified for three /qf rounds before round 12 measured it,
#                 and it would have shipped.
#   difference -- the fold set and the emission diverging by more than {U+0000}
#                 (here: the emission also dropping U+0001), i.e. fold != bake.
#   zero-adv   -- a font swap introducing a new zero-advance inked codepoint
#                 (here: pretending U+055B was never adjudicated).
#   weights    -- the two weights covering different sets, which one fold table
#                 cannot describe.
DRILLS = [
    ("clamp",
     [("exclude_emit = exclude - {0x0000}", "exclude_emit = set(exclude)")],
     "still begins at U+0000"),
    ("difference",
     [("exclude_emit = exclude - {0x0000}", "exclude_emit = exclude - {0x0000, 0x0001}")],
     "differ by exactly"),
    ("zero-adv",
     [("EXPECTED_ZERO_ADVANCE = {0x055B, 0x055C, 0x055E}",
       "EXPECTED_ZERO_ADVANCE = {0x055C, 0x055E}")],
     "zero-advance residue moved"),
    ("weights",
     [('faces_union = per_weight["regular"]', 'faces_union = per_weight["regular"]'),
      ('only_reg = per_weight["regular"] - per_weight["bold"]',
       'only_reg = (per_weight["regular"] - per_weight["bold"]) | {0x4E00}')],
     "no longer cover the same codepoints"),
    # COMMIT 2 (2026-07-30). Admitting the combining marks needed the zero-advance
    # gate to stop seeing them -- otherwise its residue goes 3 -> 322 and the only
    # way to quiet it is to widen the constant its own message forbids widening.
    # Subtracting a whole class from a gate is exactly how a gate stops firing
    # without anyone noticing, so the class gets its OWN gate and its own drill.
    ("ink",
     # U+034F COMBINING GRAPHEME JOINER is the arc-D2 defect itself: zero advance,
     # NO CONTOURS, invisible mid-name. It stays out because it is
     # Default_Ignorable, not because it is a mark -- so dropping it from that
     # table admits it, and the ink gate is the only thing left that can object.
     [("(0x00AD, 0x00AD), (0x034F, 0x034F), (0x061C, 0x061C), (0x115F, 0x1160),",
       "(0x00AD, 0x00AD), (0x061C, 0x061C), (0x115F, 0x1160),")],
     "have no outline in a face that claims them"),
    ("zero-adv-still-live",
     # The residue gate now subtracts the marks. Prove it did NOT go blind to
     # everything else: U+055B is a non-mark, and un-adjudicating it must still
     # fire. Same anchor as "zero-adv" above, kept as a separate row because the
     # two now assert different things -- that one proved the gate existed, this
     # one proves the commit-2 subtraction left it able to fire.
     [("EXPECTED_ZERO_ADVANCE = {0x055B, 0x055C, 0x055E}",
       "EXPECTED_ZERO_ADVANCE = set()")],
     "zero-advance residue moved"),
]


def main():
    src = open(GEN, encoding="utf-8").read()
    # The copy runs from a temp dir, so its self-relative REPO would point at
    # nothing. Pin it -- and note this is why the drill checks the expected
    # MESSAGE and not merely a non-zero exit: the first run of this file exited 1
    # on a missing-font error and would have read as four gates firing.
    anchor = "REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))"
    if anchor not in src:
        sys.exit(f"drill: FAIL -- cannot pin REPO; the generator's path preamble moved")
    src = src.replace(anchor, f"REPO = {REPO!r}", 1)

    fails = 0
    for name, muts, expect in DRILLS:
        text = src
        for old, new in muts:
            if old not in text:
                print(f"  {name:<11} SKIP -- anchor not found: {old!r}")
                fails += 1
                break
            text = text.replace(old, new, 1)
        else:
            with tempfile.TemporaryDirectory() as td:
                path = os.path.join(td, "mutant.py")
                with open(path, "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
                r = subprocess.run([sys.executable, path, "--donor", DONOR, "--dry-run"],
                                   capture_output=True, text=True)
            out = (r.stdout or "") + (r.stderr or "")
            red = r.returncode != 0 and expect in out
            print(f"  {name:<11} {'RED (gate fired)' if red else 'GREEN -- GATE DID NOT FIRE'}"
                  f"  rc={r.returncode}")
            if not red:
                fails += 1
                print("    expected phrase:", expect)
                print("    tail:", out.strip().splitlines()[-1] if out.strip() else "(no output)")

    # The unmutated generator must still be GREEN, or a "RED" above could be any
    # error at all rather than the gate under test.
    r = subprocess.run([sys.executable, GEN, "--donor", DONOR, "--dry-run"],
                       capture_output=True, text=True)
    print(f"  {'baseline':<11} {'GREEN' if r.returncode == 0 else 'RED -- THE REAL GENERATOR FAILS'}"
          f"  rc={r.returncode}")
    if r.returncode != 0:
        fails += 1
        print(r.stdout, r.stderr)

    print(f"\ndrill: {'PASS' if fails == 0 else 'FAIL'} ({len(DRILLS)} gates + baseline, "
          f"{fails} problem(s))")
    sys.exit(1 if fails else 0)


main()
