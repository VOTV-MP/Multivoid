#!/usr/bin/env python3
"""commit_msg_check_drill -- show tools/git/commit_msg_check.py RED before trusting it green.

Every rule in the checker gets one message that must be REFUSED and the good shapes get
messages that must PASS; then the real entry points are exercised: the hook (a message file,
including a legacy-encoded one) and the CI range mode on throwaway repositories (a bad commit,
a merge with a bad body, a boundary that is the root commit).

    python tools/git/commit_msg_check_drill.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import commit_msg_check as C  # noqa: E402

TRAILERS = "\n\nCo-Authored-By: Someone <someone@example.com>\nClaude-Session: https://example.com/s"

GOOD = "[coop] doors address by portable identity\n\nThe game mints a random key per process.\nMeasured: two-peer smoke PASS, keysHash equal on 4 channels." + TRAILERS

# (name, message, must_be_refused, expected fragment of the refusal, hook_mode)
ARMS = [
    ("good message with trailers", GOOD, False, None, False),
    ("git's merge subject is exempt", "Merge branch 'x' into main", False, None, False),
    ("git's revert subject is exempt", "Revert \"[ui] a thing\"\n\nThis reverts commit abc.", False, None, False),
    ("an ordinary 'Merge ...' sentence is not", "Merge the two lanes into one", True, "[scope]", False),
    ("template lines are ignored in the hook", "[docs] a thing\n# Please enter the commit message\n#\n# On branch main", False, None, True),
    ("scissors cut the diff off in the hook", "[docs] a thing\n\nbody\n" + C.SCISSORS + "\ndiff --git a/USER b/USER\n" + "x\n" * 30, False, None, True),
    ("a '#' line is content as recorded", "[coop] a thing\n\n# USER said so", True, "USER", False),
    ("73-character subject", "[coop] " + "x" * 66, True, "subject is 73 characters", False),
    ("a two-line subject paragraph", "[coop] short\nand this continuation is the rest of it\n\nbody", True, "one line", False),
    ("no scope prefix", "doors address by portable identity", True, "[scope]", False),
    ("uppercase scope", "[Coop] doors", True, "[scope]", False),
    ("13-line body", "[coop] a thing\n\n" + "\n".join("line {}".format(i) for i in range(13)) + TRAILERS, True, "body is 13", False),
    ("12-line body passes", "[coop] a thing\n\n" + "\n".join("line {}".format(i) for i in range(12)) + TRAILERS, False, None, False),
    ("a trailer-shaped body is body", "[coop] a thing\n\n" + "\n".join("Line-{}: value".format(i) for i in range(20)), True, "body is 20", False),
    ("vocabulary in the trailer block", "[coop] a thing\n\nbody\n\nNote: the USER said so\nCo-Authored-By: A <a@b>", True, "USER", False),
    ("Cyrillic in the body", "[coop] a thing\n\nисправлено", True, "Cyrillic", False),
    ("Cyrillic in a trailer", "[coop] a thing\n\nbody\n\nCo-Authored-By: Кто-то <x@y>", True, "Cyrillic", False),
    ("a byte-order mark", "\ufeff[coop] a thing", True, "byte-order mark", False),
    ("/qf", "[coop] a thing\n\nthree things a /qf round forced", True, "/qf", False),
    ("documentize", "[docs] close: the census\n\nwritten by documentize", True, "documentize", False),
    ("agents", "[net] a thing\n\ntwo audit agents found it", True, "agent", False),
    ("User-Agent is not an agent", "[tools] master: send a User-Agent header", False, None, False),
    ("memory/", "[docs] a thing\n\nsee memory/feedback_x.md", True, "memory/", False),
    ("memory/ inside a URL is fine", "[docs] a thing\n\nsee https://example.com/memory/x", False, None, False),
    ("research/", "[docs] a thing\n\nlogs in research/runs/2026", True, "research/", False),
    ("CLAUDE.md", "[docs] a thing\n\nper CLAUDE.md rule 1", True, "CLAUDE.md", False),
    (".claude/", "[tools] a thing\n\nthe skill in .claude/skills/x", True, ".claude/", False),
    ("docs/security/", "[net] a thing\n\nsee docs/security/TRACKER.md", True, "docs/security/", False),
    ("verbatim", "[docs] a thing\n\nthe user said, verbatim, do it", True, "verbatim", False),
    ("USER", "[docs] a thing\n\nUSER DECISION: keep it", True, "USER", False),
    ("$USER is a shell variable", "[tools] read $USER from the environment", False, None, False),
    ("user (lowercase) is fine", "[ui] a thing\n\nthe user presses E", False, None, False),
    ("Docs-Census trailer", "[docs] close: x\n\nbody\n\nDocs-Census: base=abc rows=1\nCo-Authored-By: A <a@b>", True, "Docs-Census", False),
    ("empty message", "\n\n", True, "empty", False),
    ("a hand-made close subject", "[docs] close: by hand\n\nbody", True, "session-close script", False),
]


def git(args, cwd, env=None, input_text=None):
    return subprocess.run(["git"] + args, cwd=cwd, check=True, capture_output=True, env=env,
                          input=input_text, text=input_text is not None)


def new_repo(tmp, name, env):
    repo = os.path.join(tmp, name)
    os.makedirs(repo)
    git(["init", "-q", "-b", "main"], repo, env)
    git(["config", "core.hooksPath", os.devnull], repo, env)
    return repo


def commit(repo, env, fname, msg):
    with open(os.path.join(repo, fname), "w") as f:
        f.write(fname)
    git(["add", "."], repo, env)
    git(["commit", "-q", "-F", "-"], repo, env, msg)


def run_entry_points(results):
    """The hook (file mode) and the CI range mode, on throwaway repositories."""
    def arm(name, ok, detail=""):
        results.append(ok)
        print("  {} {}{}".format("PASS" if ok else "FAIL", name, (" -- " + detail) if detail else ""))

    tmp = tempfile.mkdtemp(prefix="cmc_drill_")
    script = os.path.join(HERE, "commit_msg_check.py")
    files = {"good.txt": GOOD.encode("utf-8"),
             "bad.txt": "[coop] a thing\n\nисправлено by two agents".encode("utf-8"),
             "close.txt": b"[docs] close: the record\n\nCo-Authored-By: A <a@b>",
             "cp1251.txt": "[coop] a thing\n\nисправлено\n".encode("cp1251")}
    for name, data in files.items():
        with open(os.path.join(tmp, name), "wb") as f:
            f.write(data)

    def hook(name, **kw):
        return subprocess.run([sys.executable, script, os.path.join(tmp, name)], capture_output=True,
                              text=True, encoding="utf-8", **kw)

    r = hook("good.txt")
    arm("hook mode: good file -> exit 0", r.returncode == 0)
    r = hook("close.txt", env=dict(os.environ, MULTIVOID_CLOSE="1"))
    arm("hook mode: the close script's subject with MULTIVOID_CLOSE=1 -> exit 0", r.returncode == 0)
    r = hook("close.txt")
    arm("hook mode: the same subject by hand -> refused", r.returncode == 1 and "session-close" in r.stdout)
    r = hook("bad.txt")
    arm("hook mode: bad file -> exit 1 with both reasons", r.returncode == 1 and "Cyrillic" in r.stdout and "agent" in r.stdout)
    r = hook("cp1251.txt")
    arm("hook mode: a cp1251 file is refused as not UTF-8", r.returncode == 1 and "not valid UTF-8" in r.stdout, r.stdout.strip().splitlines()[-1] if r.stdout else "")

    env = dict(os.environ, GIT_AUTHOR_NAME="drill", GIT_AUTHOR_EMAIL="d@x", GIT_COMMITTER_NAME="drill",
               GIT_COMMITTER_EMAIL="d@x", GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull)

    def rng(repo, *args):
        return subprocess.run([sys.executable, script, "--repo", repo] + list(args), capture_output=True,
                              text=True, encoding="utf-8")

    # range mode: one bad commit among three
    repo = new_repo(tmp, "range", env)
    for i, msg in enumerate([GOOD, "[coop] fine\n\nsecond commit", "a bad one with no scope"]):
        commit(repo, env, "f{}.txt".format(i), msg)
    first = git(["rev-list", "--max-parents=0", "HEAD"], repo, env).stdout.decode().strip()
    r = rng(repo, "--range", first + "..HEAD")
    arm("range mode: 2 commits after the root, 1 bad -> exit 1", r.returncode == 1 and "1 refused" in r.stdout and "a bad one" in r.stdout)
    r = rng(repo, "--range", first + ".." + first)
    arm("range mode: an empty range exits 0 and says so", r.returncode == 0 and "EMPTY" in r.stdout)
    r = rng(repo, "--range=--output=" + os.path.join(tmp, "pwned.txt"))
    arm("range mode: an option-shaped range is refused", r.returncode == 2 and not os.path.exists(os.path.join(tmp, "pwned.txt")))
    # a '#' line recorded with -F is judged in range mode
    commit(repo, env, "f3.txt", "[coop] a thing\n\n# USER said so, verbatim")
    r = rng(repo, "--range", "HEAD~1..HEAD")
    arm("range mode: a recorded '#' line is judged", r.returncode == 1 and "USER" in r.stdout)
    # a merge commit's body is judged too
    git(["checkout", "-q", "-b", "side"], repo, env)
    commit(repo, env, "side.txt", "[coop] side work")
    git(["checkout", "-q", "main"], repo, env)
    git(["merge", "-q", "--no-ff", "side", "-m", "Merge branch 'side'\n\nthe USER said verbatim: делай"], repo, env)
    r = rng(repo, "--range", "HEAD~1..HEAD")
    arm("range mode: a merge commit's body is judged", r.returncode == 1 and "Cyrillic" in r.stdout)
    # --from-boundary when the checker was added in the ROOT commit
    repo2 = new_repo(tmp, "root", env)
    os.makedirs(os.path.join(repo2, "tools", "git"))
    shutil.copy(script, os.path.join(repo2, "tools", "git", "commit_msg_check.py"))
    git(["add", "."], repo2, env)
    git(["commit", "-q", "-F", "-"], repo2, env, "[tools] the checker arrives")
    commit(repo2, env, "later.txt", "[coop] later, fine")
    r = rng(repo2, "--from-boundary")
    arm("from-boundary: a root-commit boundary judges the whole history", r.returncode == 0 and "2 commit(s)" in r.stdout, r.stdout.strip().splitlines()[-1] if r.stdout else r.stderr[-200:])
    # a shallow clone is refused, not judged
    shallow = os.path.join(tmp, "shallow")
    subprocess.run(["git", "clone", "-q", "--depth", "1", "file:///" + repo2.replace(os.sep, "/"), shallow],
                   check=True, capture_output=True, env=env)
    r = rng(shallow, "--from-boundary")
    arm("from-boundary: a shallow clone is refused", r.returncode == 1 and "shallow" in r.stdout)


def main():
    results = []
    for name, msg, must_refuse, frag, hook_mode in ARMS:
        refusals = C.check_message(msg, hook_mode=hook_mode)
        refused = bool(refusals)
        ok = refused == must_refuse and (not frag or any(frag in r for r in refusals))
        results.append(ok)
        print("  {} {:<44} -> {}".format("PASS" if ok else "FAIL", name,
                                          "; ".join(refusals) if refusals else "accepted"))
    ok = not C.check_message("[docs] close: by the script\n\nbody", allow_close=True)
    results.append(ok)
    print("  {} {:<44} -> {}".format("PASS" if ok else "FAIL", "the script's own close subject", "accepted" if ok else "refused"))
    run_entry_points(results)
    bad = results.count(False)
    print("commit_msg_check_drill: {} arms, {} failed".format(len(results), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
