#!/usr/bin/env python3
"""public_prose_gate_drill -- show tools/docs/public_prose_gate.py RED before trusting it green.

A throwaway repository with one doc and one source file carrying every class of marker; the
gate must count each, FAIL against a baseline one lower, PASS against an exact one, refuse to
ratchet up, and ratchet down after the marker is removed. A link to a tracked file must not
count as dead; a link to an untracked one must.

    python tools/docs/public_prose_gate_drill.py
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GATE = os.path.join(HERE, "public_prose_gate.py")

DOC = """# A doc

USER said, verbatim: «делай». A /qf round and two audit agents agreed on 2026-09-05.
See memory/feedback_x.md, research/runs/x, CLAUDE.md, .claude/skills and docs/security/TRACKER.md.
A [tracked link](../README.md), an [external](https://example.com), and a [dead one](../nowhere.md).
A backticked `docs/nowhere.md` names no file; a backticked `docs/a.md` does.
An [uppercase scheme](HTTPS://example.com/x), an [encoded name](My%20File.md), a [backslash](sub\\c.md).

```
a [link in a code block](nothing.md) is not a link
```
"""
SRC = """// 2026-09-05: the USER asked for this, verbatim; a /qf round and an agent agreed.
// see research/findings/x.md and CLAUDE.md, lesson 12, commit deadbeef12
int f() { return 0; } // trailing comments are code lines
const char* s = "// not a comment";
/* a block
   comment */
// line 0 of a long block
// line 1 of a long block
// line 2 of a long block
// line 3 of a long block
// line 4 of a long block
// line 5 of a long block
// line 6 of a long block
// line 7 of a long block
// line 8 of a long block
// line 9 of a long block
// line 10 of a long block
// line 11 of a long block
// line 12 of a long block
// line 13 of a long block
// line 14 of a long block
// line 15 of a long block
int g() { return 1; }
"""


def git(args, cwd, env):
    subprocess.run(["git"] + args, cwd=cwd, env=env, check=True, capture_output=True)


def run(args):
    return subprocess.run([sys.executable, GATE] + args, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def main():
    tmp = tempfile.mkdtemp(prefix="ppg_drill_")
    repo = os.path.join(tmp, "repo")
    os.makedirs(os.path.join(repo, "docs"))
    os.makedirs(os.path.join(repo, "src", "votv-coop", "src"))
    env = dict(os.environ, GIT_AUTHOR_NAME="drill", GIT_AUTHOR_EMAIL="d@x", GIT_COMMITTER_NAME="drill",
               GIT_COMMITTER_EMAIL="d@x", GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull)
    git(["init", "-q", "-b", "main"], repo, env)
    git(["config", "core.hooksPath", os.devnull], repo, env)
    with open(os.path.join(repo, "README.md"), "w", encoding="utf-8") as f:
        f.write("# root\n")
    with open(os.path.join(repo, "docs", "a.md"), "w", encoding="utf-8") as f:
        f.write(DOC)
    with open(os.path.join(repo, "docs", "My File.md"), "w", encoding="utf-8") as f:
        f.write("# spaced\n")
    with open(os.path.join(repo, "docs", "six.md"), "w", encoding="utf-8") as f:
        f.write("# exactly six hundred lines\n" + "x\n" * 599)
    with open(os.path.join(repo, "src", "votv-coop", "src", "x.cpp"), "w", encoding="utf-8") as f:
        f.write(SRC)
    git(["add", "."], repo, env)
    git(["commit", "-q", "-m", "[drill] seed"], repo, env)
    baseline = os.path.join(tmp, "baseline.json")
    results = []

    def arm(name, ok, detail=""):
        results.append(ok)
        print("  {} {}{}".format("PASS" if ok else "FAIL", name, (" -- " + detail) if detail else ""))

    r = run(["--repo", repo, "--baseline", baseline, "--init"])
    arm("init writes a baseline", r.returncode == 0 and os.path.isfile(baseline), r.stdout.strip())
    with open(baseline, encoding="utf-8") as f:
        counters = json.load(f)["counters"]
    expect = {"md.files": 4, "md.over_600": 0, "md.cyrillic": 1, "md.user": 1, "md.verbatim": 1, "md.qf": 1, "md.agent": 1,
              "md.dated": 1, "md.ptr_memory": 1, "md.ptr_research": 1, "md.ptr_claude": 1,
              "md.ptr_security": 1, "md.dead_links": 2, "md.dead_paths": 1,
              "src.comment_lines": 20, "src.comment_blocks_over_15": 1, "src.comment_dated": 1, "src.comment_user": 1, "src.comment_verbatim": 1,
              "src.comment_qf": 1, "src.comment_agent": 1, "src.comment_ptr_research": 1,
              "src.comment_ptr_claude": 1, "src.comment_ptr_security": 0, "src.comment_lesson": 1,
              "src.comment_sha": 1, "src.files_half_comment": 0}
    for k, v in expect.items():
        arm("counts {} = {}".format(k, v), counters.get(k) == v, "got {}".format(counters.get(k)))
    r = run(["--repo", repo, "--baseline", baseline])
    arm("exact baseline passes", r.returncode == 0, r.stdout.strip().splitlines()[-1])
    lowered = dict(counters, **{"md.cyrillic": 0})
    with open(baseline, "w", encoding="utf-8") as f:
        json.dump({"as_of": "drill", "counters": lowered}, f)
    r = run(["--repo", repo, "--baseline", baseline])
    arm("one above the baseline fails and names the file",
        r.returncode == 1 and "md.cyrillic 0->1" in r.stdout and "docs/a.md" in r.stdout,
        r.stdout.strip().splitlines()[-1])
    r = run(["--repo", repo, "--baseline", baseline, "--update"])
    arm("update refuses to ratchet UP", r.returncode == 1, r.stdout.strip().splitlines()[-1])
    # remove the Cyrillic line, commit, update: the baseline must drop to 0 by itself
    with open(os.path.join(repo, "docs", "a.md"), "w", encoding="utf-8") as f:
        f.write(DOC.replace("«делай»", "do it"))
    git(["commit", "-q", "-am", "[drill] fix"], repo, env)
    with open(baseline, "w", encoding="utf-8") as f:
        json.dump({"as_of": "drill", "counters": counters}, f)      # back to 1
    r = run(["--repo", repo, "--baseline", baseline, "--update"])
    with open(baseline, encoding="utf-8") as f:
        after = json.load(f)["counters"]
    arm("update ratchets DOWN after the fix", r.returncode == 0 and after["md.cyrillic"] == 0,
        "md.cyrillic {} -> {}".format(counters["md.cyrillic"], after["md.cyrillic"]))
    r = run(["--repo", repo, "--baseline", os.path.join(tmp, "absent.json")])
    arm("no baseline is a failure, not a pass", r.returncode == 1, r.stdout.strip())
    # a counter missing from the baseline fails, and --update does not mint it
    with open(baseline, encoding="utf-8") as f:
        b = json.load(f)
    del b["counters"]["md.user"]
    with open(baseline, "w", encoding="utf-8") as f:
        json.dump(b, f)
    r = run(["--repo", repo, "--baseline", baseline, "--update"])
    with open(baseline, encoding="utf-8") as f:
        b2 = json.load(f)
    arm("a counter with no baseline fails and is not minted", r.returncode == 1 and "no baseline" in r.stdout
        and "md.user" not in b2["counters"], r.stdout.strip().splitlines()[-1])
    # volume counters are informational: a longer doc does not fail
    with open(baseline, "w", encoding="utf-8") as f:
        json.dump({"as_of": "drill", "counters": dict(after, **{"md.lines": 1})}, f)
    r = run(["--repo", repo, "--baseline", baseline])
    arm("md.lines above the baseline is informational, not a failure", r.returncode == 0, r.stdout.strip().splitlines()[-1])
    # --init over an existing baseline is refused without --force
    r = run(["--repo", repo, "--baseline", baseline, "--init"])
    arm("--init over an existing baseline is refused", r.returncode == 1 and "--force" in r.stdout)
    # a 601-line doc crosses the cap; a 600-line one does not (counted above)
    with open(os.path.join(repo, "docs", "six.md"), "a", encoding="utf-8") as f:
        f.write("one more\n")
    git(["commit", "-q", "-am", "[drill] 601"], repo, env)
    r = run(["--repo", repo, "--baseline", baseline])
    arm("a 601-line doc crosses the 600-line cap", r.returncode == 1 and "md.over_600 0->1" in r.stdout)
    bad = results.count(False)
    print("public_prose_gate_drill: {} arms, {} failed".format(len(results), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
