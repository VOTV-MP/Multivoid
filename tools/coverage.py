#!/usr/bin/env python3
"""Coop-readiness register generator.

Every column comes from ONE machine source with a NAMED limit. Nothing here
judges whether a class "should" count -- the 2026-07-22 pass measured that a
syntactic filter cannot express that semantic property (36% false negatives on
a known-positive set, and a false-positive side that is uncalibratable in
principle). So this tool ENUMERATES and ATTRIBUTES; it never classifies.

Columns and their sources:

  denominator  the dump. 2291 BP `_C` classes by the `class A<name>_C` form.
  floor        classes with ZERO own functions -- they cannot diverge between
               peers by construction. SUBTRACTED FROM THE DENOMINATOR, never
               added to the numerator: 838/2291 rendered as a percentage would
               read as "36% done" where it means "this work does not exist".
  coverage     literal | base-walk | none.  `literal` = our source names the
               class in a string literal (parameter-free: a literal either
               equals the class name or it does not). `base-walk` = the class
               is not named but a non-engine ANCESTOR is -- our lanes match by
               WalksToBase, so these ARE covered and a name-only rule reports
               them as holes. Measured 2026-07-22: 3 of the 44 doc-named
               classes (7%) are base-walk-only (kerfusFace_C, prop_deadRoach_C,
               wisp_C). This column exists so that 7% is not silently read as
               absence -- it is the characterized false-negative of the literal
               source, and a characterized false negative hidden in prose reads
               as a boundary.
  rung         sub-partition INSIDE the covered set, parsed from the free-text
               tail our docs already write (smoke / awaiting hands-on / ...).
               NEVER the boundary: measured 2026-07-22, tail-parse and the
               literal source diverge in BOTH directions (17 vs 27, 14 common).
  verified     NOT derived. Read from tools/verified_takes.tsv, where every row
               must cite a real artifact. Tail-parse scored 6 against a
               measured 3 on the same set -- it is not admissible on this rung.

Ancestor walking STOPS at the engine roots. Walking to AActor once returned
"100% covered", because every class reaches it and our source mentions it
somewhere; that wrong number was caught only by absurdity.
"""

import argparse
import collections
import csv
import os
import re
import sys

DUMP = ("Game_0.9.0n_HOST/WindowsNoEditor/VotV/Binaries/Win64/"
        "CXXHeaderDump")
SRC = "src/votv-coop/src"
TAKES = "tools/verified_takes.tsv"

# The walk stops here. Every BP class reaches these, so continuing past them
# makes "is an ancestor covered?" trivially true for everything.
ENGINE_ROOTS = {
    "AActor", "APawn", "ACharacter", "AController", "APlayerController",
    "UObject", "UActorComponent", "USceneComponent", "UUserWidget",
    "USaveGame", "AInfo", "AHUD", "AGameModeBase", "AGameMode",
    # Slipped past the first version and "covered" 57 classes on its own.
    "UAnimInstance", "UBlueprintFunctionLibrary", "UInterface",
}

# A class HEADER, not a member declaration. `class Aprop_fireExt_C* fireExt;`
# is a member whose TYPE is a class, and it matches a bare `class X_C` pattern
# exactly as a header does. Splitting on the bare form cut class bodies at
# their own member declarations and re-attributed the functions that followed
# to the member's type: measured 2026-07-22, AfireExtHolder_C reported 0 own
# functions while its four (ReceiveBeginPlay, two BndEvt__, ExecuteUbergraph_)
# were credited to Aprop_fireExt_C. That single bug is what put the floor at
# 1170. Requiring the inheritance colon or the opening brace separates them.
CLASS_RE = re.compile(r"\bclass\s+([AU][A-Za-z0-9_]+_C)\s*(?=:|\{|\n\{)")
# `class Aprop_drive_C : public Aprop_C` -- the dump's single-inheritance form.
INHERIT_RE = re.compile(
    r"\bclass\s+([AU][A-Za-z0-9_]+_C)\s*:\s*public\s+([AU][A-Za-z0-9_]+)")
# Own functions = declarations inside the class body. The dump lists them one
# per line with a return type; counting them is what makes the floor a fact
# rather than a guess.
FUNC_RE = re.compile(r"^\s+(?:static\s+)?[A-Za-z_][\w:<>*&,\s]*\s+"
                     r"([A-Za-z_]\w*)\s*\(", re.M)
LITERAL_RE = re.compile(r'"([A-Za-z0-9_]{3,})"')
WLITERAL_RE = re.compile(r'L"([A-Za-z0-9_]{3,})"')


def read_dump(root):
    """Return (own_fn_count_by_class, parent_by_class)."""
    own = {}
    parent = {}
    for dirpath, _, names in os.walk(root):
        for name in names:
            if not name.endswith(".hpp"):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
            for m in INHERIT_RE.finditer(text):
                parent[m.group(1)] = m.group(2)
            # Split on class headers so each body is attributed to its class.
            parts = CLASS_RE.split(text)
            for i in range(1, len(parts), 2):
                cls, body = parts[i], parts[i + 1]
                own[cls] = own.get(cls, 0) + len(FUNC_RE.findall(body))
    return own, parent


def read_source_literals(root):
    """Distinct string literals our own source carries. No free parameter."""
    lits = set()
    for dirpath, _, names in os.walk(root):
        for name in names:
            if not name.endswith((".cpp", ".h")):
                continue
            with open(os.path.join(dirpath, name),
                      encoding="utf-8", errors="replace") as fh:
                text = fh.read()
            lits.update(LITERAL_RE.findall(text))
            lits.update(WLITERAL_RE.findall(text))
    return lits


def names_class(lits, cls):
    """A literal names the class, or it does not. Nothing in between."""
    stem = cls[1:]
    core = stem[:-2] if stem.endswith("_C") else stem
    return any(lit in (cls, stem, core, core + "_C") for lit in lits)


def ancestors(cls, parent):
    """Ancestors up to (not through) the engine roots."""
    out = []
    seen = {cls}
    cur = parent.get(cls)
    while cur and cur not in ENGINE_ROOTS and cur not in seen:
        out.append(cur)
        seen.add(cur)
        cur = parent.get(cur)
    return out


def coverage_of(cls, lits, parent):
    """Returns (kind, covering_ancestor_or_None).

    The ancestor is returned, not discarded, because the AGGREGATE of
    base-walk coverage is misleading and must never be printed as one number.
    Measured 2026-07-22: `Aprop_C` alone accounts for 346 of them and
    `Aactor_save_C` for 108 -- these are second-tier ROOTS, so "681 covered"
    reads as reach the generic lane does not have. "346 classes descend from
    the prop_C lane" is checkable; "681 covered" is the same shape of false
    number as the ancestor walk that once returned "100% covered".
    """
    if names_class(lits, cls):
        return "literal", None
    for anc in ancestors(cls, parent):
        if names_class(lits, anc):
            return "base-walk", anc
    return "none", None


def read_verified(path):
    """Hand-curated, but every row must CITE an artifact. A row without a
    citation is refused, not silently counted -- the whole point of this rung
    is that it cannot be derived from a syntactic proxy."""
    rows = {}
    if not os.path.exists(path):
        return rows, ["%s absent -- VERIFIED rung reports 0" % path]
    problems = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split("\t") if p.strip()]
            if len(parts) < 4:
                problems.append("row lacks class/date/artifact/verdict: %r"
                                % line)
                continue
            cls, date, artifact, verdict = parts[:4]
            if not os.path.exists(artifact):
                problems.append("%s cites a missing artifact: %s"
                                % (cls, artifact))
                continue
            rows[cls] = (date, artifact, verdict)
    return rows, problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", default=DUMP)
    ap.add_argument("--src", default=SRC)
    ap.add_argument("--takes", default=TAKES)
    ap.add_argument("--csv", help="write the per-class register here")
    args = ap.parse_args()

    if not os.path.isdir(args.dump):
        sys.exit("dump not found: %s" % args.dump)

    own, parent = read_dump(args.dump)
    lits = read_source_literals(args.src)
    verified, problems = read_verified(args.takes)

    total = len(own)
    floor = {c for c, n in own.items() if n == 0}
    live = [c for c in own if c not in floor]

    cov = {}
    via = {}
    for c in live:
        cov[c], via[c] = coverage_of(c, lits, parent)
    tally = collections.Counter(cov.values())
    by_anc = collections.Counter(a for a in via.values() if a)

    print("REGISTER  (every column one machine source, each with a named limit)")
    print()
    print("  dump BP classes                 : %d" % total)
    print("  floor, zero own functions       : %d   "
          "(cannot diverge by construction)" % len(floor))
    print("  denominator = dump - floor      : %d   "
          "<- the floor REDUCES this, it is not progress" % len(live))
    print()
    print("  named by a literal              : %d" % tally["literal"])
    print("  not named, no covered ancestor  : %d" % tally["none"])
    print()
    print("  reached only through an ancestor's lane -- NOT summed, because")
    print("  the aggregate reads as coverage the generic lane does not have:")
    for anc, n in by_anc.most_common(10):
        print("    via %-26s %4d" % (anc, n))
    if len(by_anc) > 10:
        print("    (+%d more ancestors)" % (len(by_anc) - 10))
    print()
    print("  VERIFIED hands-on (cited takes) : %d   "
          "<- re-counted from %s, never parsed from a doc tail"
          % (len(verified), args.takes))
    print()

    if problems:
        print("PROBLEMS (a register that hides these is the defect it replaces)")
        for p in problems:
            print("  ! %s" % p)
        print()

    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            w.writerow(["class", "own_fns", "coverage", "via_ancestor", "verified_date",
                        "verified_artifact"])
            for c in sorted(live):
                v = verified.get(c, ("", "", ""))
                w.writerow([c, own[c], cov[c], via[c] or "", v[0], v[1]])
        print("per-class register -> %s" % args.csv)


if __name__ == "__main__":
    main()
