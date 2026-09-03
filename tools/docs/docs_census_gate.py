#!/usr/bin/env python3
"""docs_census_gate -- CI reads the Docs-Census trailers back, from a boundary it computes itself.

WHY THIS EXISTS (docs/DOCUMENTIZE_ARC.md WP-1(c), /qf rounds 2-6)
------------------------------------------------------------------
tools/docs/status_census.py writes ONE `Docs-Census:` trailer into every close commit. A number
nobody reads back is prose with digits in it, so this gate reads the stream of close commits on
every push and fails on what it can check WITHOUT the trees CI never sees (research/, the memory
directory, the ignored docs):

  shape     a `[docs] close:` subject without the trailer; a trailer without the prefix; a subject
            that still starts with the RETIRED close form `[docs] documentize` (one close path);
            a close commit without `Co-Authored-By:` (the trailer STAYS -- CLAUDE.md).
  identity  rows = the sum of every VERDICT column in trailer_schema -- the five status verdicts
            plus the FOUR rejection buckets, because the hand's one "not a claim" token is
            attributed to the rung whose row it rejects (label / cite / drift / loose). A column a
            close PREDATES counts as zero, or the push that adds a token is red by construction.
  tiling    `base=` equals the previous close commit's sha in the range (the first close after the
            boundary: any ancestor), so consecutive censuses tile the history with no gap.
  novelty   `census=` (the private history's commit) differs from every earlier close's -- a
            trailer pasted from the previous close is caught.
  ratchet   no RATCHETED column grows against the previous close (the list lives in
            trailer_schema, not here -- naming three of eight in prose is how it went stale).
  monotone  resolved / flips never shrink -- they are cumulative totals of the resolved ledger,
            which lives in the private history CI cannot read, so append-only is all it can check.

THE BOUNDARY IS COMPUTED, NEVER WRITTEN: the commit that ADDED this gate's own workflow file
(`git log --diff-filter=A -- .github/workflows/docs-census.yml`). A commit cannot carry its own
hash, and a sha typed into the workflow would be minted where it is not checked (the ledger's
file-hash-gate row). History before the boundary is not judged (249 old-form closes live there).

USAGE
    python tools/docs/docs_census_gate.py [--repo DIR] [--workflow PATH] [--report]
"""
import argparse
import os
import subprocess
import sys

CLOSE_PREFIX = "[docs] close:"
RETIRED_PREFIX = "[docs] documentize"
TRAILER_KEY = "Docs-Census"
WORKFLOW = ".github/workflows/docs-census.yml"
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trailer_schema as TS  # noqa: E402  -- the ONE column vocabulary, shared with status_census.py

RATCHET_COLS = TS.RATCHETED
MONOTONE_COLS = TS.MONOTONE
VERDICT_COLS = TS.VERDICT


def git(args, cwd):
    r = subprocess.run(["git"] + list(args), cwd=cwd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("git {}: {}".format(" ".join(args), (r.stderr or r.stdout).strip()[:300]))
    return r.stdout


def parse_trailer(body):
    for line in body.splitlines():
        if line.startswith(TRAILER_KEY + ":"):
            kv = {}
            for tok in line.split(":", 1)[1].split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    kv[k] = v
            return kv
    return None


def boundary(repo, workflow):
    out = git(["log", "--diff-filter=A", "--format=%H", "--", workflow], repo).split()
    return out[-1] if out else None


def commits_after(repo, bound):
    out = git(["log", "--reverse", "--format=%H%x00%s%x00%B%x01", bound + "..HEAD"], repo)
    for rec in out.split("\x01"):
        if "\x00" not in rec:
            continue
        sha, subject, body = rec.strip("\n").split("\x00", 2)
        yield sha.strip(), subject.strip(), body


def is_ancestor(repo, a, b):
    return subprocess.run(["git", "merge-base", "--is-ancestor", a, b], cwd=repo,
                          capture_output=True).returncode == 0


def judge(repo, workflow, report=False):
    bound = boundary(repo, workflow)
    if not bound:
        print("docs_census_gate: {} was never added to this history -- nothing to judge".format(workflow))
        return []
    print("boundary: {} (the commit that added {})".format(bound[:12], workflow))
    fails = []
    prev_close, prev_trailer = None, None
    seen_census = {}
    n_close = 0
    for sha, subject, body in commits_after(repo, bound):
        short = sha[:10]
        trailer = parse_trailer(body)
        is_close = subject.startswith(CLOSE_PREFIX)
        if subject.startswith(RETIRED_PREFIX):
            fails.append("{} retired close form in the subject: {!r}".format(short, subject[:60]))
        if is_close and trailer is None:
            fails.append("{} '{}' subject without a {} trailer".format(short, CLOSE_PREFIX, TRAILER_KEY))
        if trailer is not None and not is_close:
            fails.append("{} {} trailer on a subject without the '{}' prefix: {!r}".format(
                short, TRAILER_KEY, CLOSE_PREFIX, subject[:60]))
        if not is_close or trailer is None:
            continue
        n_close += 1
        if "Co-Authored-By:" not in body:
            fails.append("{} close commit without Co-Authored-By".format(short))
        # EVERY column declares its KIND (trailer_schema). Without this an unread column ships
        # silently -- the trailer wrote 23 and this gate read 15 (round 20, Q4).
        undeclared = sorted(c for c in trailer if c not in TS.KIND)
        if undeclared:
            fails.append("{} trailer column(s) with no declared kind in trailer_schema: {}".format(
                short, ", ".join(undeclared)))
        try:
            rows = int(trailer.get("rows", "x"))
            # A verdict column ABSENT from a trailer counts as ZERO, for the same reason an absent
            # ratchet column is not a failure below: the close predates the column. A seventh verdict
            # token was added on 2026-09-03 and the two closes already on record cannot carry it --
            # failing them would make the push that adds a token red by construction, which is the
            # shape docs/LESSONS.md names ("a gate left red on purpose carries no signal"). The
            # identity still binds: those closes distributed every row among the tokens they had.
            parts = [int(trailer[c]) for c in VERDICT_COLS if c in trailer]
            if not any(c in trailer for c in VERDICT_COLS):
                raise ValueError("no verdict column at all")
        except ValueError:
            fails.append("{} trailer lacks rows or a verdict column: {}".format(short, sorted(trailer)))
            rows, parts = -1, []
        if parts and sum(parts) != rows:
            fails.append("{} verdict sum {} != rows {}".format(short, sum(parts), rows))
        base = trailer.get("base", "")
        if prev_close:
            # `not base` FIRST: every string starts with "", so an EMPTY base= would tile onto any
            # predecessor and the check would pass vacuously (measured 2026-09-03 by a post-ship audit).
            if not base or not prev_close.startswith(base):
                fails.append("{} base={} does not tile onto the previous close {}".format(short, base, prev_close[:12]))
        else:
            if not base or not is_ancestor(repo, base, sha):
                fails.append("{} base={} is not an ancestor of the close".format(short, base))
        census = trailer.get("census", "")
        if census in seen_census:
            fails.append("{} census={} repeats {}'s -- a pasted trailer".format(short, census, seen_census[census][:10]))
        seen_census[census] = sha
        if prev_trailer:
            # Two kinds, opposite directions, one comparison. RATCHETED counts must not GROW (the
            # reading order, the over-long memory lines); MONOTONE counts are cumulative ledger
            # totals and must not SHRINK -- CI cannot read the private history the resolved ledger
            # lives in, so "a close may not un-record what an earlier close recorded" is the only
            # property available to it, and it is the one that matters.
            for kind, cols in (("ratchet", RATCHET_COLS), ("monotone", MONOTONE_COLS)):
                for c in cols:
                    # A column the PREVIOUS trailer does not carry is a column that did not exist yet
                    # (the script gained one): there is nothing to compare, so it is not a failure --
                    # otherwise the very push that adds a column is red by construction, the shape
                    # docs/LESSONS.md names ("a gate left red on purpose carries no signal"). A column
                    # that DISAPPEARS from a later trailer is a regression and does fail.
                    if c not in prev_trailer:
                        continue
                    if c not in trailer:
                        fails.append("{} {} column {} vanished (the previous close carried it)".format(short, kind, c))
                        continue
                    try:
                        a, b = int(prev_trailer[c]), int(trailer[c])
                    except ValueError:
                        fails.append("{} {} column {} unreadable ({!r} -> {!r})".format(
                            short, kind, c, prev_trailer[c], trailer[c]))
                        continue
                    if kind == "ratchet" and b > a:
                        fails.append("{} ratchet: {} grew {} -> {}".format(short, c, a, b))
                    elif kind == "monotone" and b < a:
                        fails.append("{} monotone: {} shrank {} -> {} (the resolved ledger is "
                                     "append-only)".format(short, c, a, b))
        if report:
            print("close {} {}".format(short, " ".join("{}={}".format(k, v) for k, v in sorted(trailer.items()))))
        prev_close, prev_trailer = sha, trailer
    print("closes judged: {}".format(n_close))
    return fails


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    ap.add_argument("--workflow", default=WORKFLOW)
    ap.add_argument("--report", action="store_true")
    a = ap.parse_args(argv)
    fails = judge(a.repo, a.workflow, a.report)
    for f in fails:
        print("FAIL " + f)
    print("docs_census_gate: " + ("FAIL ({} finding(s))".format(len(fails)) if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
