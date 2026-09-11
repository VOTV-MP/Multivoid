#!/usr/bin/env python3
"""commit_trail_gate -- what the recent commit trail READS as, measured on every push.

A reader who opens the commit list sees subjects, in order. The question this answers is whether
that list reads as software being built or as a project writing about itself, and the number for
it is the share of the recent trail whose scope is `[docs]`.

Two things are checked, because the first is only true while the second is:

  share    `[docs]` is at most 15% of the last 100 commits a person wrote
  labels   a commit whose diff is documentation and nothing else says `[docs]`, so the share
           above cannot be lowered by filing documentation under a code scope

The window is the LAST commits, not the first: the trail is judged where a reader enters it, and
a measurement taken once over a window that has scrolled away has no reader at all. Commits
before the message shape was adopted are outside it, for the reason `commit_msg_check` does not
walk past its own boundary -- a rule cannot judge a trail written before it existed.

Git-made commits (merges, reverts, `fixup!`, `squash!`) are outside the window in BOTH halves of
the fraction. Nobody wrote them as a contribution, and `commit_msg_check` already exempts their
subjects for that reason. Note which way this cuts: counting them would enlarge the denominator
and shrink the share, so leaving them out is the stricter reading.

The session-close commit is outside it for the same reason, and for a measurement that moved the
rule. `[docs] close: ...` is the subject `status_census.py close` writes, and
`commit_msg_check` REFUSES it from a hand -- so the repository already holds that a person did not
write it. Counting it made the ritual the project mandates spend the budget meant for discretionary
documentation: the close that exposed this landed in a window that also held three doc commits each
FORCED by another gate (a ledger row the contributor work owed, a wire-kind count `ledger_lint`
reads off the enum, and a public page the prose gate refused for citing an ignored path), and the
trail read 16 of 100 against a cap of 15. Excluding the two closes in that window reads 14 of 98.
The cap still binds every doc commit a person chose to write, which is what it was built to
measure.

Usage:
  commit_trail_gate.py                    the window ending at HEAD
  commit_trail_gate.py --tip REV          the same, ending at REV -- on a pull request pass the
                                          branch head, not the forge's ephemeral merge commit
  commit_trail_gate.py --window N         a window of N commits instead of 100
  commit_trail_gate.py --list             also print every commit in the window, newest first

Exit 0 when the trail passes, 1 when it does not, 2 when the arguments name no history.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
# The boundary walk, the subprocess wrapper and the two subject patterns are this checker's
# neighbours already: the trail's shape and a single message's shape are the same rule read at two
# scales, and a second copy of either pattern would let them drift apart silently.
from commit_msg_check import CLOSE_PREFIX, EXEMPT_SUBJECT_RE, SCOPE_RE, boundary, git  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
WINDOW = 100
DOCS_MAX_PERCENT = 15
DOCS_SCOPE = "docs"
# What a diff has to touch to be documentation and nothing else. `.md` anywhere and everything
# under docs/: the two together are what the public tree calls a document.
DOC_SUFFIX = ".md"
DOC_ROOT = "docs/"
# git's record and field separators inside one `git log` call, so the walk costs one process
# rather than one per commit.
REC = "\x02"
FLD = "\x01"


def is_doc_path(path):
    return path.endswith(DOC_SUFFIX) or path.startswith(DOC_ROOT)


def scope_of(subject):
    """-> the `[scope]` a subject declares, or None when it declares none."""
    if not SCOPE_RE.match(subject):
        return None
    return subject[1:subject.index("]")]


def walk(repo, rng):
    """-> [(sha, subject, [paths])] for `rng`, newest first, git-made and script-made dropped.

    `--name-only` prints nothing for a merge, which needs no special case here: a merge is
    dropped by its subject before its paths are read. The session-close subject is dropped beside
    it, and by the constant `commit_msg_check` refuses it with, so the two cannot drift."""
    raw = git(["-c", "core.quotePath=false", "log",
               "--format={}%H{}%s".format(REC, FLD), "--name-only", "--end-of-options", rng], repo)
    out = []
    for rec in raw.split(REC):
        if not rec.strip():
            continue
        head, _, rest = rec.partition("\n")
        sha, _, subject = head.partition(FLD)
        if EXEMPT_SUBJECT_RE.match(subject) or subject.startswith(CLOSE_PREFIX):
            continue
        out.append((sha, subject, [p for p in rest.split("\n") if p.strip()]))
    return out


def judge(commits):
    """-> (docs, total, mislabelled). `commits` is already the window."""
    docs = 0
    mislabelled = []
    for sha, subject, paths in commits:
        scope = scope_of(subject)
        if scope == DOCS_SCOPE:
            docs += 1
        # An empty diff cannot say what it is, so it is counted in the trail and not accused.
        elif paths and all(is_doc_path(p) for p in paths):
            mislabelled.append((sha, subject, scope))
    return docs, len(commits), mislabelled


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--tip", default="HEAD", help="the tip of the trail to measure (default HEAD)")
    ap.add_argument("--window", type=int, default=WINDOW, help="how many commits (default 100)")
    ap.add_argument("--list", action="store_true", help="print the window, newest first")
    ap.add_argument("--repo", default=REPO)
    a = ap.parse_args()

    if a.tip.startswith("-"):
        print("commit_trail_gate: a tip cannot start with '-' ({})".format(a.tip))
        return 2
    if a.window < 1:
        print("commit_trail_gate: a window of {} commits measures nothing".format(a.window))
        return 2
    if subprocess.run(["git", "rev-parse", "-q", "--verify", "--end-of-options",
                       a.tip + "^{commit}"], cwd=a.repo, capture_output=True).returncode != 0:
        print("commit_trail_gate: the tip names no commit in this clone ({})".format(a.tip))
        return 2
    # A shallow clone's oldest commits are absent, so a window reaching past the graft would be
    # measured over whatever happened to be fetched. Refuse rather than report a smaller trail.
    if git(["rev-parse", "--is-shallow-repository"], a.repo).strip() == "true":
        print("commit_trail_gate: a shallow clone cannot hold the window -- fetch the full history")
        return 1

    b = boundary(a.repo)
    if not b:
        print("commit_trail_gate: the message shape was never adopted in this history -- "
              "nothing to measure")
        return 0
    has_parent = subprocess.run(["git", "rev-parse", "-q", "--verify", b + "~1"], cwd=a.repo,
                                capture_output=True).returncode == 0
    rng = b + "~1.." + a.tip if has_parent else a.tip
    trail = walk(a.repo, rng)
    window = trail[:a.window]
    if not window:
        # Exiting 0 here would be the lane passing by measuring nothing, which is the one
        # outcome a ratchet must never have.
        print("commit_trail_gate: no commit since the boundary is in {} -- an EMPTY window "
              "measures nothing".format(rng))
        return 1

    docs, total, mislabelled = judge(window)
    share = 100.0 * docs / total
    if a.list:
        for sha, subject, _ in window:
            print("  {} {}".format(sha[:10], subject[:96]))
    short = "{}..{}".format(window[-1][0][:10], window[0][0][:10])
    head = ("commit_trail_gate: [docs] is {} of {} ({:.1f}%), cap {}%; {} mislabelled; "
            "window {}{}".format(docs, total, share, DOCS_MAX_PERCENT, len(mislabelled), short,
                                 "" if total == a.window else
                                 " (the whole trail since the boundary: {} < {})".format(
                                     total, a.window)))
    fails = []
    if share > DOCS_MAX_PERCENT:
        fails.append("[docs] is {} of {} ({:.1f}%), above the {}% the trail is held to".format(
            docs, total, share, DOCS_MAX_PERCENT))
    for sha, subject, scope in mislabelled:
        fails.append("{} touches documentation and nothing else but says [{}]: {}".format(
            sha[:10], scope if scope else "no scope", subject[:72]))
    print(head)
    if fails:
        print("commit_trail_gate: FAIL")
        for f in fails:
            print("  - " + f)
        return 1
    print("commit_trail_gate: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
