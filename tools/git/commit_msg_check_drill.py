#!/usr/bin/env python3
"""commit_msg_check_drill -- show tools/git/commit_msg_check.py RED before trusting it green.

Every rule in the checker gets one message that must be REFUSED and the good shapes get
messages that must PASS; then the two real entry points are exercised: the hook (a message
file) and the CI range mode (a throwaway repository with one good and one bad commit).

    python tools/git/commit_msg_check_drill.py
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import commit_msg_check as C  # noqa: E402

TRAILERS = "\n\nCo-Authored-By: Someone <someone@example.com>\nClaude-Session: https://example.com/s"

GOOD = "[coop] doors address by portable identity\n\nThe game mints a random key per process.\nMeasured: two-peer smoke PASS, keysHash equal on 4 channels." + TRAILERS

ARMS = [
    # (name, message, must_be_refused, expected fragment of the refusal)
    ("good message with trailers", GOOD, False, None),
    ("merge subject is exempt", "Merge branch 'x' into main", False, None),
    ("revert subject is exempt", "Revert \"[ui] a thing\"\n\nThis reverts commit abc.", False, None),
    ("comment lines are ignored", "[docs] a thing\n# Please enter the commit message\n#\n# On branch main", False, None),
    ("scissors cut the diff off", "[docs] a thing\n\nbody\n" + C.SCISSORS + "\ndiff --git a/USER b/USER\n" + "x\n" * 30, False, None),
    ("73-character subject", "[coop] " + "x" * 66, True, "subject is 73 characters"),
    ("no scope prefix", "doors address by portable identity", True, "[scope]"),
    ("uppercase scope", "[Coop] doors", True, "[scope]"),
    ("13-line body", "[coop] a thing\n\n" + "\n".join("line {}".format(i) for i in range(13)) + TRAILERS, True, "body is 13"),
    ("12-line body passes", "[coop] a thing\n\n" + "\n".join("line {}".format(i) for i in range(12)) + TRAILERS, False, None),
    ("Cyrillic in the body", "[coop] a thing\n\nисправлено", True, "Cyrillic"),
    ("Cyrillic in a trailer", "[coop] a thing\n\nbody\n\nCo-Authored-By: Кто-то <x@y>", True, "Cyrillic in a trailer"),
    ("/qf", "[coop] a thing\n\nthree things a /qf round forced", True, "/qf"),
    ("documentize", "[docs] close: the census\n\nwritten by documentize", True, "documentize"),
    ("agents", "[net] a thing\n\ntwo audit agents found it", True, "agent"),
    ("memory/", "[docs] a thing\n\nsee memory/feedback_x.md", True, "memory/"),
    ("research/", "[docs] a thing\n\nlogs in research/runs/2026", True, "research/"),
    ("CLAUDE.md", "[docs] a thing\n\nper CLAUDE.md rule 1", True, "CLAUDE.md"),
    (".claude/", "[tools] a thing\n\nthe skill in .claude/skills/x", True, ".claude/"),
    ("docs/security/", "[net] a thing\n\nsee docs/security/TRACKER.md", True, "docs/security/"),
    ("verbatim", "[docs] a thing\n\nthe user said, verbatim, do it", True, "verbatim"),
    ("USER", "[docs] a thing\n\nUSER DECISION: keep it", True, "USER"),
    ("user (lowercase) is fine", "[ui] a thing\n\nthe user presses E", False, None),
    ("Docs-Census trailer", "[docs] close: x\n\nbody\n\nDocs-Census: base=abc rows=1\nCo-Authored-By: A <a@b>", True, "Docs-Census"),
    ("empty message", "\n\n", True, "empty"),
]


def git(args, cwd, env=None):
    subprocess.run(["git"] + args, cwd=cwd, check=True, capture_output=True, env=env)


def run_entry_points():
    """The hook (file mode) and the CI range mode, on a throwaway repository."""
    bad = 0
    tmp = tempfile.mkdtemp(prefix="cmc_drill_")
    good_file = os.path.join(tmp, "good.txt")
    bad_file = os.path.join(tmp, "bad.txt")
    with open(good_file, "w", encoding="utf-8") as f:
        f.write(GOOD)
    with open(bad_file, "w", encoding="utf-8") as f:
        f.write("[coop] a thing\n\nисправлено by two agents")
    script = os.path.join(HERE, "commit_msg_check.py")
    r = subprocess.run([sys.executable, script, good_file], capture_output=True, text=True)
    ok = r.returncode == 0
    print("  {} hook mode: good file -> exit {}".format("PASS" if ok else "FAIL", r.returncode))
    bad += 0 if ok else 1
    r = subprocess.run([sys.executable, script, bad_file], capture_output=True, text=True)
    ok = r.returncode == 1 and "Cyrillic" in r.stdout and "agent" in r.stdout
    print("  {} hook mode: bad file -> exit {} ({} refusal line(s))".format(
        "PASS" if ok else "FAIL", r.returncode, r.stdout.count("\n  - ")))
    bad += 0 if ok else 1
    # range mode
    repo = os.path.join(tmp, "repo")
    os.makedirs(repo)
    env = dict(os.environ, GIT_AUTHOR_NAME="drill", GIT_AUTHOR_EMAIL="d@x", GIT_COMMITTER_NAME="drill",
               GIT_COMMITTER_EMAIL="d@x", GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull)
    git(["init", "-q", "-b", "main"], repo, env)
    git(["config", "core.hooksPath", os.devnull], repo, env)
    for i, msg in enumerate([GOOD, "[coop] fine\n\nsecond commit", "a bad one with no scope"]):
        with open(os.path.join(repo, "f{}.txt".format(i)), "w") as f:
            f.write(str(i))
        git(["add", "."], repo, env)
        subprocess.run(["git", "commit", "-q", "-F", "-"], cwd=repo, env=env, input=msg,
                       text=True, check=True, capture_output=True)
    first = subprocess.run(["git", "rev-list", "--max-parents=0", "HEAD"], cwd=repo, env=env,
                           capture_output=True, text=True, check=True).stdout.strip()
    r = subprocess.run([sys.executable, script, "--repo", repo, "--range", first + "..HEAD"],
                       capture_output=True, text=True)
    ok = r.returncode == 1 and "1 refused" in r.stdout and "a bad one" in r.stdout
    print("  {} range mode: 2 commits after the root, 1 bad -> exit {}: {}".format(
        "PASS" if ok else "FAIL", r.returncode, r.stdout.strip().splitlines()[-1] if r.stdout else ""))
    bad += 0 if ok else 1
    r = subprocess.run([sys.executable, script, "--repo", repo, "--range", first + ".." + first],
                       capture_output=True, text=True)
    ok = r.returncode == 0
    print("  {} range mode: empty range -> exit {}".format("PASS" if ok else "FAIL", r.returncode))
    bad += 0 if ok else 1
    return bad


def main():
    bad = 0
    for name, msg, must_refuse, frag in ARMS:
        refusals = C.check_message(msg)
        refused = bool(refusals)
        ok = refused == must_refuse and (not frag or any(frag in r for r in refusals))
        bad += 0 if ok else 1
        print("  {} {:<32} -> {}".format("PASS" if ok else "FAIL", name,
                                          "; ".join(refusals) if refusals else "accepted"))
    bad += run_entry_points()
    print("commit_msg_check_drill: {} arms, {} failed".format(len(ARMS) + 4, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
