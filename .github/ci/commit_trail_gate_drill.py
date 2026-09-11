#!/usr/bin/env python3
"""commit_trail_gate_drill -- show .github/ci/commit_trail_gate.py RED before trusting it green.

Every refusal the gate can make gets a throwaway repository that must turn it RED, and the
near-miss beside it one that must leave it GREEN. Then the mutation pass breaks the gate one
anchor at a time and requires this drill to go RED on each: an anchor that can be broken with
every arm still passing is logic no arm reads.

The windows here are small (`--window 20`, so the cap of 15% falls between 3 and 4 commits)
because a hundred-commit fixture would cost the same proof several seconds a repository. One arm
runs with no `--window` at all, so the default is exercised too.

    python .github/ci/commit_trail_gate_drill.py
"""
import io
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "commit_trail_gate.py")
CHECKER = os.path.join(HERE, "commit_msg_check.py")
# The path the message checker was ADDED at, which is where the gate looks for the boundary.
BOUNDARY_REL = ("tools", "git", "commit_msg_check.py")

# (name, old, new) -- each must still compile, and must turn this drill RED.
MUTANTS = [
    # The defect this gate was built out of: the window measured the FIRST commits after the
    # rule arrived, which is the stretch in which the documents were rewritten, so it reported
    # the rewrite instead of the trail a reader lands on.
    ("window: the oldest commits, not the newest", "window = trail[:a.window]",
     "window = trail[-a.window:]"),
    ("share: the cap is never crossed", "if share > DOCS_MAX_PERCENT:", "if share > 10 ** 9:"),
    ("share: the cap is crossed by reaching it", "if share > DOCS_MAX_PERCENT:",
     "if share >= DOCS_MAX_PERCENT:"),
    ("who counts: git-made commits are in the window",
     "if EXEMPT_SUBJECT_RE.match(subject) or subject.startswith(CLOSE_PREFIX):",
     "if subject is None:"),
    ("who counts: the script-made session close is in the window",
     "or subject.startswith(CLOSE_PREFIX):", "or False:"),
    ("labels: only the suffix says documentation",
     "return path.endswith(DOC_SUFFIX) or path.startswith(DOC_ROOT)",
     "return path.endswith(DOC_SUFFIX)"),
    ("labels: nothing is ever mislabelled", "elif paths and all(is_doc_path(p) for p in paths):",
     "elif paths and all(is_doc_path(p) for p in paths) and False:"),
    ("fail-closed: an empty window passes",
     '"measures nothing".format(rng))\n        return 1',
     '"measures nothing".format(rng))\n        return 0'),
    ("fail-closed: a shallow clone passes",
     'the window -- fetch the full history")\n        return 1',
     'the window -- fetch the full history")\n        return 0'),
]


def git(args, cwd, env, text=None):
    return subprocess.run(["git"] + args, cwd=cwd, check=True, capture_output=True, env=env,
                          input=text, text=text is not None)


def build(tmp, name, spec, env, boundary=True, pre=()):
    """A repository whose trail is `spec`: (subject, [paths]) oldest first, paths written as they
    are named. `boundary=False` leaves out the commit that adopts the message shape, which is the
    only thing that makes a history measurable at all; `pre` is committed BEFORE that one, so the
    boundary has a parent and a tip can name a commit that predates the rule."""
    repo = os.path.join(tmp, name)
    os.makedirs(repo)
    git(["init", "-q", "-b", "main"], repo, env)
    git(["config", "core.hooksPath", os.devnull], repo, env)
    for subject, paths in pre:
        for p in paths:
            full = os.path.join(repo, *p.split("/"))
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with io.open(full, "a", encoding="utf-8") as f:
                f.write(subject + "\n")
        git(["add", "."], repo, env)
        git(["commit", "-q", "-F", "-"], repo, env, subject)
    if boundary:
        os.makedirs(os.path.join(repo, *BOUNDARY_REL[:-1]))
        shutil.copy(CHECKER, os.path.join(repo, *BOUNDARY_REL))
        git(["add", "."], repo, env)
        git(["commit", "-q", "-F", "-"], repo, env, "[tools] the checker arrives")
    for subject, paths in spec:
        for p in paths:
            full = os.path.join(repo, *p.split("/"))
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with io.open(full, "a", encoding="utf-8") as f:
                f.write(subject + "\n")
        if paths:
            git(["add", "."], repo, env)
            git(["commit", "-q", "-F", "-"], repo, env, subject)
        else:
            git(["commit", "-q", "--allow-empty", "-F", "-"], repo, env, subject)
    return repo


def trail(n_docs, n_code, doc_path="docs/a.md", code_path="src/a.cpp"):
    """`n_docs` documentation commits then `n_code` code ones, all correctly labelled."""
    return ([("[docs] a document, {}".format(i), [doc_path]) for i in range(n_docs)]
            + [("[ci] some code, {}".format(i), [code_path]) for i in range(n_code)])


def main():
    results = []

    def arm(name, ok, detail=""):
        results.append(ok)
        print("  {} {}{}".format("PASS" if ok else "FAIL", name, (" -- " + detail) if detail else ""))

    tmp = tempfile.mkdtemp(prefix="ctg_drill_")
    env = dict(os.environ, GIT_AUTHOR_NAME="drill", GIT_AUTHOR_EMAIL="d@x",
               GIT_COMMITTER_NAME="drill", GIT_COMMITTER_EMAIL="d@x",
               GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull)

    def run(repo, *args):
        return subprocess.run([sys.executable, SCRIPT, "--repo", repo] + list(args),
                              capture_output=True, text=True, encoding="utf-8", errors="replace")

    def last(r):
        return (r.stdout.strip().splitlines() or [r.stderr.strip()[-200:] or "no output"])[-1]

    # ---- the share, and where its cap falls -------------------------------------------------
    # 20 person commits: the boundary plus 19, so a window of 20 is exactly full.
    repo = build(tmp, "clean", trail(0, 19), env)
    r = run(repo, "--window", "20")
    arm("a trail with no documentation commits passes", r.returncode == 0 and "0 of 20" in r.stdout,
        last(r))
    repo = build(tmp, "at_cap", trail(3, 16), env)
    r = run(repo, "--window", "20")
    arm("3 of 20 is 15.0% and the cap is not crossed by reaching it",
        r.returncode == 0 and "3 of 20 (15.0%)" in r.stdout, last(r))
    repo = build(tmp, "over_cap", trail(4, 15), env)
    r = run(repo, "--window", "20")
    arm("4 of 20 is 20.0% and is refused",
        r.returncode == 1 and "4 of 20 (20.0%)" in r.stdout and "above the 15%" in r.stdout, last(r))

    # ---- the ritual's own commit is not a person writing about the project -------------------
    # Two closes and two chosen documents in a window of twenty: counted, that is 4 of 20 and
    # refused; read as the gate now reads it, 2 of 18. The cap still binds the two a hand wrote.
    repo = build(tmp, "close_commits", trail(2, 16)
                 + [("[docs] close: a session", ["docs/c.md"]),
                    ("[docs] close: another session", ["docs/d.md"])], env)
    r = run(repo, "--window", "20")
    arm("the session-close commit is outside both halves",
        r.returncode == 0 and " of 20 (" not in r.stdout, last(r))

    # ---- the window is the LAST commits, not the first ---------------------------------------
    # One repository read twice, which is the shape the real trail has: a stretch of document
    # commits that a window anchored at the start would report forever, and a window anchored at
    # the tip reports only while it is recent.
    repo = build(tmp, "slides", trail(19, 0) + trail(0, 20)[19:] + trail(0, 20), env)
    docs_tip = subprocess.run(["git", "rev-parse", "HEAD~20"], cwd=repo, capture_output=True,
                              text=True, env=env).stdout.strip()
    r = run(repo, "--window", "20")
    arm("the window at the tip sees the recent commits and passes",
        r.returncode == 0 and "0 of 20" in r.stdout, last(r))
    r = run(repo, "--window", "20", "--tip", docs_tip)
    arm("the same trail read at the older tip is refused",
        r.returncode == 1 and "above the 15%" in r.stdout, last(r))

    # ---- the label has to be honest, or the share is not a measurement -----------------------
    repo = build(tmp, "mislabel", trail(0, 18) + [("[ci] a document filed as code", ["docs/b.md"])],
                 env)
    r = run(repo, "--window", "20")
    arm("a documentation-only commit under a code scope is named",
        r.returncode == 1 and "touches documentation and nothing else" in r.stdout
        and "[ci]" in r.stdout, last(r))
    repo = build(tmp, "mixed_docs", trail(0, 18)
                 + [("[docs] a document and the code beside it", ["docs/b.md", "src/b.cpp"])], env)
    r = run(repo, "--window", "20")
    arm("a [docs] commit that also touches code is not accused",
        r.returncode == 0 and "0 mislabelled" in r.stdout, last(r))
    repo = build(tmp, "mixed_code", trail(0, 18)
                 + [("[ci] the code and a document beside it", ["docs/b.md", "src/b.cpp"])], env)
    r = run(repo, "--window", "20")
    arm("a code commit that also touches a document is not accused",
        r.returncode == 0 and "0 mislabelled" in r.stdout, last(r))
    repo = build(tmp, "empty_diff", trail(0, 18) + [("[ci] a commit with no diff at all", [])], env)
    r = run(repo, "--window", "20")
    arm("a commit with an empty diff is counted and not accused",
        r.returncode == 0 and "0 of 20" in r.stdout and "0 mislabelled" in r.stdout, last(r))

    # ---- what a document is ------------------------------------------------------------------
    repo = build(tmp, "root_md", trail(0, 18) + [("[ci] the front page", ["README.md"])], env)
    r = run(repo, "--window", "20")
    arm("a .md outside docs/ is a document", r.returncode == 1 and "mislabelled" in r.stdout, last(r))
    repo = build(tmp, "docs_txt", trail(0, 18) + [("[ci] a note under docs", ["docs/notes.txt"])], env)
    r = run(repo, "--window", "20")
    arm("a non-.md file under docs/ is a document",
        r.returncode == 1 and "touches documentation" in r.stdout, last(r))
    repo = build(tmp, "src_only", trail(0, 18) + [("[ci] a source file", ["src/deep/c.cpp"])], env)
    r = run(repo, "--window", "20")
    arm("a source-only commit is not a document", r.returncode == 0, last(r))

    # ---- who counts: nobody wrote the git-made commits ----------------------------------------
    repo = build(tmp, "gitmade", [("[docs] a document", ["docs/a.md"]),
                                  ("[ci] some code", ["src/a.cpp"])], env)
    git(["checkout", "-q", "-b", "side"], repo, env)
    git(["commit", "-q", "--allow-empty", "-F", "-"], repo, env, "[ci] work on the side")
    git(["checkout", "-q", "main"], repo, env)
    git(["merge", "-q", "--no-ff", "side", "-m", "Merge branch 'side'"], repo, env)
    for subject in ('Revert "[ci] some code"', "fixup! [ci] some code", "squash! [ci] some code"):
        git(["commit", "-q", "--allow-empty", "-F", "-"], repo, env, subject)
    r = run(repo, "--window", "20")
    arm("merges, reverts, fixups and squashes are outside both halves of the fraction",
        r.returncode == 1 and "1 of 4" in r.stdout, last(r))

    # ---- fail closed -------------------------------------------------------------------------
    repo = build(tmp, "shallow_src", trail(0, 19), env)
    shallow = os.path.join(tmp, "shallow")
    subprocess.run(["git", "clone", "-q", "--depth", "1",
                    "file:///" + repo.replace(os.sep, "/"), shallow],
                   check=True, capture_output=True, env=env)
    r = run(shallow, "--window", "20")
    arm("a shallow clone is refused, not measured over what was fetched",
        r.returncode == 1 and "shallow" in r.stdout, last(r))
    r = run(repo, "--tip", "0" * 40)
    arm("a tip that names no commit is refused", r.returncode == 2 and "names no commit" in r.stdout)
    tipped = os.path.join(tmp, "tipped.txt")
    r = run(repo, "--tip=--output=" + tipped)
    arm("an option-shaped tip is refused before git sees it",
        r.returncode == 2 and "cannot start with" in r.stdout and not os.path.exists(tipped))
    r = run(repo, "--window", "0")
    arm("a window of zero commits is refused", r.returncode == 2 and "measures nothing" in r.stdout)

    # A tip that predates the boundary leaves the range empty. Exiting 0 there would be the lane
    # passing by measuring nothing at all, which is the one verdict a ratchet must never reach.
    repo = build(tmp, "old_tip", trail(0, 2), env,
                 pre=[("[ci] a commit from before the shape existed", ["src/old.cpp"])])
    root = subprocess.run(["git", "rev-list", "--max-parents=0", "HEAD"], cwd=repo,
                          capture_output=True, text=True, env=env).stdout.strip()
    r = run(repo, "--window", "20", "--tip", root)
    arm("an empty window is refused, not passed",
        r.returncode == 1 and "EMPTY window" in r.stdout, last(r))
    repo = build(tmp, "no_boundary", trail(0, 3), env, boundary=False)
    r = run(repo, "--window", "20")
    arm("a history that never adopted the shape is not judged by it",
        r.returncode == 0 and "nothing to measure" in r.stdout, last(r))

    # ---- what it reports ---------------------------------------------------------------------
    repo = build(tmp, "reporting", trail(1, 5), env)
    r = run(repo)
    arm("the default window is 100 and a shorter trail says so",
        r.returncode == 0 and "1 of 7" in r.stdout and "7 < 100" in r.stdout, last(r))
    arm("the number is printed on a PASS, not only on a FAIL",
        r.returncode == 0 and "cap 15%" in r.stdout and "PASS" in r.stdout, last(r))
    r = run(repo, "--list")
    arm("--list prints the window, newest first",
        r.returncode == 0 and "a document, 0" in r.stdout
        and r.stdout.index("some code, 4") < r.stdout.index("a document, 0"), last(r))

    # ---- the mutation pass -------------------------------------------------------------------
    # The nested run must not mutate in turn: one level is the proof, two is a fork bomb.
    if os.environ.get("CTG_DRILL_NESTED"):
        bad = results.count(False)
        print("commit_trail_gate_drill: {} arms, {} failed".format(len(results), bad))
        return 1 if bad else 0
    mut = os.path.join(tmp, "mutant")
    os.makedirs(mut, exist_ok=True)
    for path in (SCRIPT, CHECKER, os.path.abspath(__file__)):
        shutil.copy(path, os.path.join(mut, os.path.basename(path)))
    pristine = io.open(SCRIPT, encoding="utf-8").read()
    for name, old, new in MUTANTS:
        if pristine.count(old) != 1:
            arm("mutant anchor is unique: " + name,
                False, "found {} times".format(pristine.count(old)))
            continue
        with io.open(os.path.join(mut, os.path.basename(SCRIPT)), "w", encoding="utf-8",
                     newline="") as f:
            f.write(pristine.replace(old, new))
        chk = subprocess.run(
            [sys.executable, "-c",
             # The mutant's own directory leads sys.path, so the sibling it imports is the copy
             # beside it and never the pristine one in the tree.
             "import importlib.util,os,sys\n"
             "p=sys.argv[1]; sys.path.insert(0, os.path.dirname(p))\n"
             "s=importlib.util.spec_from_file_location('g',p)\n"
             "m=importlib.util.module_from_spec(s); s.loader.exec_module(m)",
             os.path.join(mut, os.path.basename(SCRIPT))],
            capture_output=True, text=True, encoding="utf-8", errors="replace")
        arm("mutant still compiles: " + name, chk.returncode == 0,
            (chk.stderr.strip().splitlines() or ["no output"])[-1])
        r = subprocess.run([sys.executable, os.path.join(mut, os.path.basename(__file__))],
                           capture_output=True, text=True, encoding="utf-8", errors="replace",
                           env=dict(env, CTG_DRILL_NESTED="1"))
        arm("breaking " + name + " turns the drill RED", r.returncode != 0,
            (r.stdout.strip().splitlines() or ["no output"])[-1])

    bad = results.count(False)
    print("commit_trail_gate_drill: {} arms, {} failed".format(len(results), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
