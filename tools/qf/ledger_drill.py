#!/usr/bin/env python
"""ledger_drill.py -- show the /qf ledger's ANCHOR VERIFIER red on every class it claims to catch.

Written 2026-09-03 after round 7 of the documentize design pass was DISCARDED for an anchor that was
true: the critic wrote `status_census.py:602`, a file tracked at exactly one path, and the verifier
resolved only against the repo root and reported "does not exist". That is the same false-DEAD class
as `lessons_gate.CITE_ROOTS` naming a directory this repo never had -- an instrument refusing a real
citation -- and it was invisible because nothing ever asserted the resolver's behaviour.

Run:  python tools/qf/ledger_drill.py        (exit 0 = every arm behaved as claimed)
"""
import sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ledger as L                                                     # noqa: E402

FAIL = []


def arm(name, raw, want_resolved, want_detail_contains=""):
    path, why = L._resolve_loc(raw)
    got = "resolved" if path else "refused"
    ok = (path is not None) == want_resolved and (want_detail_contains in why)
    print("  [%s] %-30s %-9s %s" % ("PASS" if ok else "FAIL", raw, got, why or path))
    if not ok:
        FAIL.append(name)


print("anchor path resolution -- unique-or-refuse")
# A bare basename that names exactly one tracked file IS a legitimate anchor: the rest of this
# project's instruments resolve one (lessons_gate.resolve_cite), so refusing it manufactures rot.
arm("unique-basename", "status_census.py", True)
arm("full-path", "tools/docs/status_census.py", True)
# An AMBIGUOUS basename must NOT silently resolve to the first match -- the wrong file's line count
# would verify a claim about a file the critic never opened. 22 tracked README.md today.
arm("ambiguous-basename", "README.md", False, "ambiguous")
arm("missing-basename", "zz_no_such_file_at_all.py", False, "matches no tracked file")
arm("missing-full-path", "tools/docs/zz_nope.py", False, "does not exist")

# PREMISE (the lesson from CITE_ROOTS: a tolerance branch keyed on a path that never existed is
# always taken). The index must be non-empty, or every basename arm above would refuse for the
# wrong reason and this drill would pass while proving nothing.
idx = L._basename_index()
print("premise: basename index holds %d distinct names" % len(idx))
if len(idx) < 100:
    FAIL.append("premise: the basename index is empty or tiny -- `git ls-files` did not run")
    print("  [FAIL] the index is too small to have been built from this tree")
else:
    print("  [PASS] index built")

print("\nnumeric command anchors -- the claim is the first FIELD, and only a digit field")


class _Fake:
    anchor_kind = staticmethod(L.Ledger.anchor_kind)
    verify_anchors = L.Ledger.verify_anchors


def cmd_arm(name, anchor, want):
    r = _Fake().verify_anchors({"unresolved": [{"id": "Q", "anchor": anchor}]})[0]
    ok = (r["verified"] is True) == want
    print("  [%s] %-32s want=%-5s %s" % ("PASS" if ok else "FAIL", anchor, want, r["detail"][:46]))
    if not ok:
        FAIL.append(name)


# A counting pipeline rarely ends in a bare number: `uniq -c | head -1` prints "14 2026-08-30".
# Rejecting that rejects a TRUE anchor (measured 2026-09-03, round 15 of the documentize pass).
cmd_arm("bare-number", "echo 14 = 14", True)
cmd_arm("count-plus-value", "echo '14 2026-08-30' = 14", True)
# ...but only a DIGIT field may satisfy a claim, or a date would answer a year-shaped one.
cmd_arm("date-not-year", "echo '2026-08-30' = 2026", False)
cmd_arm("second-field", "echo '14 2026-08-30' = 2026", False)
cmd_arm("wrong-number", "echo 15 = 14", False)

print()
if FAIL:
    print("ledger_drill: %d ARM(S) FAILED: %s" % (len(FAIL), ", ".join(FAIL)))
    sys.exit(1)
print("ledger_drill: ALL PASS -- a unique basename resolves, an ambiguous one refuses rather than\n"
      "guessing, and a name that matches nothing still refuses.")
