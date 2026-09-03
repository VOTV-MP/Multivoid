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
  identity  rows = still-open + actually-done + stale-done + partial + still-true + not-a-label
            (the sixth is the hand REJECTING a row the grammar mis-flagged: it measures precision).
  tiling    `base=` equals the previous close commit's sha in the range (the first close after the
            boundary: any ancestor), so consecutive censuses tile the history with no gap.
  novelty   `census=` (the private history's commit) differs from every earlier close's -- a
            trailer pasted from the previous close is caught.
  ratchet   ro-bytes / ro-longest / mem-over200 never grow against the previous close.

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
RATCHET_COLS = ("ro-bytes", "ro-longest", "mem-over200", "wikilinks-dead", "pairing-unref", "pairing-dead")
VERDICT_COLS = ("still-open", "actually-done", "stale-done", "partial", "still-true", "not-a-label")


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
        try:
            rows = int(trailer.get("rows", "x"))
            parts = [int(trailer.get(c, "x")) for c in VERDICT_COLS]
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
            for c in RATCHET_COLS:
                # A column the PREVIOUS trailer does not carry is a column that did not exist yet
                # (the script gained one): there is nothing to compare, so it is not a failure --
                # otherwise the very push that adds a column is red by construction, the shape
                # docs/LESSONS.md names ("a gate left red on purpose carries no signal"). A column
                # that DISAPPEARS from a later trailer is a regression and does fail.
                if c not in prev_trailer:
                    continue
                if c not in trailer:
                    fails.append("{} ratchet column {} vanished (the previous close carried it)".format(short, c))
                    continue
                try:
                    a, b = int(prev_trailer[c]), int(trailer[c])
                except ValueError:
                    fails.append("{} ratchet column {} unreadable ({!r} -> {!r})".format(
                        short, c, prev_trailer[c], trailer[c]))
                    continue
                if b > a:
                    fails.append("{} ratchet: {} grew {} -> {}".format(short, c, a, b))
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
