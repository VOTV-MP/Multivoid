#!/usr/bin/env python3
"""docs_census_gate_drill -- show tools/docs/docs_census_gate.py RED before trusting it green.

Builds synthetic histories in temporary repositories: an old-form close BEFORE the boundary (must
be ignored), the commit that adds the workflow file (the boundary the gate computes), then one
defect per arm after it. Each arm must FAIL for its own reason; the well-formed history must PASS.
(`memory/lesson_an_instrument_never_shown_failing_passes_by_construction.md`.)

    python tools/docs/docs_census_gate_drill.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GATE = os.path.join(HERE, "docs_census_gate.py")
WF = ".github/workflows/docs-census.yml"
COAUTH = "Co-Authored-By: Drill <drill@example>"


def git(args, cwd, input_text=None):
    r = subprocess.run(["git"] + list(args), cwd=cwd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", input=input_text)
    if r.returncode != 0:
        raise RuntimeError("git {}: {}".format(" ".join(args), (r.stderr or r.stdout)[:300]))
    return r.stdout


def commit(repo, subject, body_lines=(), touch="f.txt"):
    with open(os.path.join(repo, touch), "a", encoding="utf-8") as f:
        f.write(subject + "\n")
    git(["add", "--", touch], repo)
    msg = subject + "\n\n" + "\n".join(body_lines) + "\n"
    git(["commit", "-q", "-F", "-"], repo, input_text=msg)
    return git(["rev-parse", "HEAD"], repo).strip()


def trailer(base, census, rows=2, so=0, ad=0, sd=0, pa=0, st=2, ro=100, rl=10, mo=0, res=5, fl=3):
    return ("Docs-Census: base={} rows={} labels={} still-open={} actually-done={} stale-done={} partial={} "
            "still-true={} not-a-label=0 not-a-cite=0 drift-ok=0 not-loose=0 cited-dead=0 "
            "accretion=0 resolved={} flips={} "
            "ro-bytes={} ro-longest={} mem-over200={} "
            "wikilinks-dead=0 pairing-unref=40 pairing-dead=0 "
            "sweep-cursor=1 sweep-cycle=1 census={} research-base=- new=0 foreign=0").format(
        base[:12], rows, rows, so, ad, sd, pa, st, res, fl, ro, rl, mo, census)


def make_repo():
    d = tempfile.mkdtemp(prefix="dcg_")
    git(["init", "-q", "-b", "main", "."], d)
    git(["config", "user.name", "drill"], d)
    git(["config", "user.email", "drill@example"], d)
    commit(d, "[docs] documentize: an OLD-form close before the boundary (must be ignored)")
    os.makedirs(os.path.join(d, ".github", "workflows"))
    with open(os.path.join(d, WF), "w") as f:
        f.write("name: docs-census\n")
    git(["add", "--", WF], d)
    git(["commit", "-q", "-m", "[tools] land the census gate"], d)
    bound = git(["rev-parse", "HEAD"], d).strip()
    return d, bound


def run_gate(repo):
    r = subprocess.run([sys.executable, GATE, "--repo", repo, "--workflow", WF, "--report"],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    return r.returncode, r.stdout + r.stderr


def arm(name, build, expect_fail_substr):
    repo, bound = make_repo()
    try:
        build(repo, bound)
        code, out = run_gate(repo)
    finally:
        shutil.rmtree(repo, ignore_errors=True)
    if expect_fail_substr is None:
        ok = code == 0
        detail = "PASS expected" if ok else "expected PASS, got:\n" + out
    else:
        ok = code != 0 and expect_fail_substr in out
        detail = "RED for the right reason" if ok else "expected FAIL containing {!r}, got exit {}:\n{}".format(
            expect_fail_substr, code, out)
    print("{:<8} {:<42} {}".format("ok" if ok else "FAILED", name, detail if not ok or True else ""))
    return ok


def green(repo, bound):
    c3 = commit(repo, "[tools] unrelated work after the boundary")
    c4 = commit(repo, "[docs] close: first census", [trailer(bound, "aaa111"), COAUTH])
    commit(repo, "[docs] close: second census", [trailer(c4, "bbb222", ro=90), COAUTH])


def main():
    results = [
        arm("well-formed history (old form before boundary)", green, None),
        arm("close prefix without trailer",
            lambda r, b: commit(r, "[docs] close: no trailer", [COAUTH]), "without a Docs-Census trailer"),
        arm("trailer without the prefix",
            lambda r, b: commit(r, "[docs] some edit", [trailer(b, "aaa111"), COAUTH]), "without the '[docs] close:' prefix"),
        arm("retired close form after the boundary",
            lambda r, b: commit(r, "[docs] documentize: the old form", [COAUTH]), "retired close form"),
        arm("close without Co-Authored-By",
            lambda r, b: commit(r, "[docs] close: no attribution", [trailer(b, "aaa111")]), "without Co-Authored-By"),
        arm("verdict sum != rows",
            lambda r, b: commit(r, "[docs] close: bad sum", [trailer(b, "aaa111", rows=3, st=2), COAUTH]), "verdict sum"),
        arm("base does not tile onto the previous close",
            lambda r, b: (commit(r, "[docs] close: first", [trailer(b, "aaa111"), COAUTH]),
                          commit(r, "[docs] close: second", [trailer(b, "bbb222"), COAUTH])), "does not tile"),
        arm("first close base not an ancestor",
            lambda r, b: commit(r, "[docs] close: bad base", [trailer("0123456789ab", "aaa111"), COAUTH]), "not an ancestor"),
        arm("repeated census= (a pasted trailer)",
            lambda r, b: (lambda c4: commit(r, "[docs] close: second", [trailer(c4, "aaa111"), COAUTH]))(
                commit(r, "[docs] close: first", [trailer(b, "aaa111"), COAUTH])), "a pasted trailer"),
        arm("ratchet column grew",
            lambda r, b: (lambda c4: commit(r, "[docs] close: second", [trailer(c4, "bbb222", ro=101), COAUTH]))(
                commit(r, "[docs] close: first", [trailer(b, "aaa111"), COAUTH])), "ratchet: ro-bytes grew"),
        # A column the script GAINS must not make the adding push red (the ledger's "a gate left red
        # on purpose carries no signal"); a column that VANISHES is a regression and must fail.
        arm("a NEW ratchet column appears (must stay green)",
            lambda r, b: (lambda c4: commit(r, "[docs] close: second", [trailer(c4, "bbb222"), COAUTH]))(
                commit(r, "[docs] close: first",
                       [trailer(b, "aaa111").replace(" wikilinks-dead=0", ""), COAUTH])), None),
        # An EMPTY base= tiles onto any predecessor, because every string starts with "" (post-ship
        # audit, 2026-09-03: the shipped drill only ever tried a wrong-but-nonempty base).
        arm("empty base= on a later close",
            lambda r, b: (lambda c4: commit(r, "[docs] close: second",
                                            [trailer(c4, "bbb222").replace("base=" + c4[:12], "base="), COAUTH]))(
                commit(r, "[docs] close: first", [trailer(b, "aaa111"), COAUTH])), "does not tile"),
        arm("a ratchet column vanishes",
            lambda r, b: (lambda c4: commit(r, "[docs] close: second",
                                            [trailer(c4, "bbb222").replace(" wikilinks-dead=0", ""), COAUTH]))(
                commit(r, "[docs] close: first", [trailer(b, "aaa111"), COAUTH])), "vanished"),
        # The MONOTONE kind runs the OTHER way: `resolved`/`flips` are cumulative totals of a ledger
        # in the private history CI cannot read, so growth is the normal case and a DROP means the
        # history was replaced or rolled back. The three arms mirror the ratchet's exactly, inverted.
        arm("monotone column shrank",
            lambda r, b: (lambda c4: commit(r, "[docs] close: second", [trailer(c4, "bbb222", res=4), COAUTH]))(
                commit(r, "[docs] close: first", [trailer(b, "aaa111"), COAUTH])), "monotone: resolved shrank"),
        arm("monotone column GROWS (the normal case, must stay green)",
            lambda r, b: (lambda c4: commit(r, "[docs] close: second", [trailer(c4, "bbb222", res=9, fl=6), COAUTH]))(
                commit(r, "[docs] close: first", [trailer(b, "aaa111"), COAUTH])), None),
        # A VERDICT column the close predates is absent, and the identity must still hold: the two
        # closes on record were written before `not-a-cite` / `drift-ok` / `not-loose` existed, and
        # `int(trailer.get(c, "x"))` used to make every one of them a hard failure -- so the push that
        # adds a verdict token would have been red by construction.
        arm("a verdict column the close predates is absent (must stay green)",
            lambda r, b: commit(r, "[docs] close: pre-token",
                                [trailer(b, "aaa111").replace(" not-a-cite=0 drift-ok=0 not-loose=0", ""),
                                 COAUTH]), None),
        arm("EVERY verdict column absent",
            lambda r, b: commit(r, "[docs] close: no verdicts",
                                [" ".join(w for w in trailer(b, "aaa111").split()
                                          if not any(w.startswith(c + "=") for c in
                                                     ("still-open", "actually-done", "stale-done",
                                                      "partial", "still-true", "not-a-label",
                                                      "not-a-cite", "drift-ok", "not-loose"))),
                                 COAUTH]), "lacks rows or a verdict column"),
        # The gate's READ side of the undeclared-column rule. status_census's WRITE side is drilled
        # in status_census_drill arm I; a column can be undeclared in two places and only one was held.
        arm("a trailer column with no declared kind",
            lambda r, b: commit(r, "[docs] close: undeclared",
                                [trailer(b, "aaa111") + " invented-column=7", COAUTH]),
            "no declared kind in trailer_schema"),
        arm("a ratchet column carrying a non-integer",
            lambda r, b: (lambda c4: commit(r, "[docs] close: second",
                                            [trailer(c4, "bbb222").replace("ro-bytes=100", "ro-bytes=lots"),
                                             COAUTH]))(
                commit(r, "[docs] close: first", [trailer(b, "aaa111"), COAUTH])), "unreadable"),
        arm("a monotone column vanishes",
            lambda r, b: (lambda c4: commit(r, "[docs] close: second",
                                            [trailer(c4, "bbb222").replace(" flips=3", ""), COAUTH]))(
                commit(r, "[docs] close: first", [trailer(b, "aaa111"), COAUTH])), "monotone column flips vanished"),
    ]
    bad = results.count(False)
    print("docs_census_gate_drill: {} arms, {} failed".format(len(results), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
