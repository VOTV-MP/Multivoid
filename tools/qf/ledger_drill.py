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


# ---- proof-of-read prior-art rows (added 2026-09-04) --------------------------------------------
# The same false-DEAD class this file's header describes, one instrument over. `verify_proof.
# ledger_rows` opened a row only when BOTH `**` markers sat on ONE line, so a lesson whose bold
# title WRAPS opened no row at all and its entire body vanished from the prior-art corpus -- 176 of
# 703 rows, a quarter of the ledger. A round-1 critic reply was discarded as fabricated for citing
# one of them; its fragment was verbatim at docs/LESSONS.md:6067. Live AND dead canaries, per
# [[lesson-a-self-test-must-assert-precision-not-only-recall]] -- recall alone would have passed the
# broken parser, because the 527 rows it did return were all real.
import verify_proof as V                                               # noqa: E402

print()
print("proof-of-read prior art -- a WRAPPED bold title is still a row")


def prior_arm(name, fragment, want):
    ok = V.check_prior(fragment, set()) == want
    print("  [%s] want=%-5s %s" % ("PASS" if ok else "FAIL", want, fragment[:58]))
    if not ok:
        FAIL.append(name)


def _bold_open(line):
    """(opens_bold, closes_on_this_line) -- the row-opener test, spelled without regex."""
    t = line[2:] if line.startswith("- ") else line
    return (t.startswith("**"), t.startswith("**") and "**" in t[2:])


def _rows_single_line_rule(path):
    """The PRE-FIX parser, kept ONLY as this drill's baseline -- never as a code path."""
    return sum(1 for line in path.read_text(encoding="utf-8", errors="replace").splitlines()
               if _bold_open(line)[1])


_now = sum(len(list(V.ledger_rows(p))) for p in V.LEDGERS if p.exists())
_old = sum(_rows_single_line_rule(p) for p in V.LEDGERS if p.exists())
print("  [%s] rows: single-line rule %d -> wrap-aware %d" %
      ("PASS" if _now > _old else "FAIL", _old, _now))
if _now <= _old:
    FAIL.append("wrap-recovers-rows")

# LIVE canary, derived at RUNTIME so editing any one lesson cannot rot it: the first body line of a
# row whose bold title wraps must resolve through check_prior.
_live = None
for _p in V.LEDGERS:
    if _live or not _p.exists():
        continue
    _lines = _p.read_text(encoding="utf-8", errors="replace").splitlines()
    for _i, _l in enumerate(_lines):
        _opens, _closes = _bold_open(_l)
        if not (_opens and not _closes):
            continue
        for _b in _lines[_i + 1:_i + 6]:
            _w = V.normalise(_b).split()
            if len(_w) >= 8 and "**" not in _b:
                _live = " ".join(_w[:8])
                break
        if _live:
            break
if _live:
    prior_arm("wrapped-title-live", _live, True)
else:
    print("  [FAIL] no wrapped-title row found to derive a live canary from")
    FAIL.append("wrapped-title-live-missing")

# DEAD canary: a fragment in no ledger row must still be refused.
prior_arm("fabricated-dead", "the pile arbiter reconciles quantum spatulas nightly", False)

print()
if FAIL:
    print("ledger_drill: %d ARM(S) FAILED: %s" % (len(FAIL), ", ".join(FAIL)))
    sys.exit(1)
print("ledger_drill: ALL PASS -- a unique basename resolves, an ambiguous one refuses rather than\n"
      "guessing, and a name that matches nothing still refuses.")
