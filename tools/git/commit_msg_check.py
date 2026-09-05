#!/usr/bin/env python3
"""commit_msg_check -- the shape of a commit message, checked where the message is made.

The rules are the "Commits" section of CONTRIBUTING.md:

  subject   `[scope] imperative summary`, at most 72 characters
            (a Merge / Revert / fixup! / squash! subject is exempt from the scope prefix)
  body      at most 12 non-empty lines, trailers excluded
  language  English only -- no Cyrillic
  content   no working-notes vocabulary: the review ritual, the maintainer's private notes and
            tooling, quoted conversations (see VOCABULARY below for the list and the reason each)

Usage:
  commit_msg_check.py <message-file>        the commit-msg hook (tools/git/hooks/commit-msg)
  commit_msg_check.py --range A..B          every non-merge commit in the range (CI)
  commit_msg_check.py --from-boundary       CI default: every commit since this checker was added

Exit 0 when every message passes, 1 otherwise; each refusal names the rule it breaks.
"""
import argparse
import io
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SELF_REL = "tools/git/commit_msg_check.py"

SUBJECT_MAX = 72
BODY_MAX_LINES = 12

SCOPE_RE = re.compile(r"^\[[a-z0-9][a-z0-9_.-]*\] \S")
EXEMPT_SUBJECT_RE = re.compile(r"^(Merge |Revert |fixup! |squash! )")
TRAILER_LINE_RE = re.compile(r"^[A-Za-z][A-Za-z0-9-]*: \S")
SCISSORS = "# ------------------------ >8 ------------------------"

CYRILLIC = re.compile("[" + chr(0x0400) + "-" + chr(0x04FF) + "]")   # the Cyrillic block

# (pattern, what it is, why it does not belong in a public commit message)
VOCABULARY = [
    (re.compile(r"(?<![\w/])/qf\b|\bqf\b(?!\.)"), "/qf",
     "the review ritual is not part of the change"),
    (re.compile(r"\bdocumentize\b", re.I), "documentize",
     "the session-close ritual is not part of the change"),
    (re.compile(r"\b(?:sub)?agents?\b", re.I), "agent(s)",
     "who reviewed the change is not part of the change"),
    (re.compile(r"(?<![\w.])memory/"), "memory/",
     "the maintainer's memory directory is not in the repository"),
    (re.compile(r"(?<![\w.])research/"), "research/",
     "the research corpus is not in the repository"),
    (re.compile(r"\bCLAUDE\.md\b"), "CLAUDE.md",
     "not in the repository"),
    (re.compile(r"\.claude/"), ".claude/",
     "not in the repository"),
    (re.compile(r"\bdocs/security/"), "docs/security/",
     "not in the repository"),
    (re.compile(r"\bverbatim\b", re.I), "verbatim",
     "quoted conversations stay in the maintainer's notes"),
    (re.compile(r"\bUSER\b"), "USER",
     "quoted decisions stay in the maintainer's notes; state the decision itself"),
    (re.compile(r"\bDocs-Census\b"), "Docs-Census",
     "machine trailers from the close ritual are not part of the change"),
]


def split_message(text):
    """-> (subject, body_lines, trailer_lines). Comment lines and everything below git's
    scissors line are dropped, as git itself drops them before recording the message."""
    lines = text.replace("\r\n", "\n").split("\n")
    kept = []
    for line in lines:
        if line.startswith(SCISSORS):
            break
        if line.startswith("#"):
            continue
        kept.append(line.rstrip())
    while kept and not kept[0].strip():
        kept.pop(0)
    while kept and not kept[-1].strip():
        kept.pop()
    if not kept:
        return "", [], []
    subject = kept[0]
    rest = kept[1:]
    # the trailer block is the LAST paragraph when every line of it is `Key: value`
    # (or a continuation line starting with whitespace)
    trailers = []
    para_start = len(rest)
    for i in range(len(rest) - 1, -1, -1):
        if not rest[i].strip():
            para_start = i + 1
            break
        para_start = i
    last_para = rest[para_start:]
    if last_para and all(TRAILER_LINE_RE.match(l) or l.startswith((" ", "\t")) for l in last_para):
        trailers = last_para
        rest = rest[:para_start]
    body = [l for l in rest if l.strip()]
    return subject, body, trailers


def check_message(text):
    """-> list of refusal strings; empty when the message passes."""
    subject, body, trailers = split_message(text)
    out = []
    if not subject:
        return ["empty message"]
    if len(subject) > SUBJECT_MAX:
        out.append("subject is {} characters; the limit is {}".format(len(subject), SUBJECT_MAX))
    if not SCOPE_RE.match(subject) and not EXEMPT_SUBJECT_RE.match(subject):
        out.append("subject must start with a lowercase `[scope] ` prefix and a summary")
    if len(body) > BODY_MAX_LINES:
        out.append("body is {} non-empty lines; the limit is {} (trailers excluded)".format(
            len(body), BODY_MAX_LINES))
    prose = "\n".join([subject] + body)
    if CYRILLIC.search(prose):
        out.append("Cyrillic in the message; English only")
    for pat, name, why in VOCABULARY:
        if pat.search(prose):
            out.append("`{}` in the message: {}".format(name, why))
    for line in trailers:
        if CYRILLIC.search(line):
            out.append("Cyrillic in a trailer; English only")
        if line.startswith("Docs-Census:"):
            out.append("`Docs-Census` trailer: machine trailers from the close ritual are not "
                       "part of the change")
    return out


def git(args, cwd):
    return subprocess.run(["git"] + args, cwd=cwd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", check=True).stdout


def boundary(repo):
    out = git(["log", "--diff-filter=A", "--format=%H", "--", SELF_REL], repo).split()
    return out[-1] if out else None


def check_range(repo, rng):
    raw = git(["log", "--no-merges", "--format=%H%x00%s%x00%B%x01", rng], repo)
    bad = 0
    total = 0
    for rec in raw.split("\x01"):
        if "\x00" not in rec:
            continue
        sha, subject, body = rec.lstrip("\n").split("\x00", 2)
        total += 1
        refusals = check_message(body)
        if refusals:
            bad += 1
            print("{} {}".format(sha[:10], subject[:80]))
            for r in refusals:
                print("    - " + r)
    print("commit_msg_check: {} commit(s) in {}, {} refused".format(total, rng, bad))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("message_file", nargs="?", help="the message file (hook mode)")
    ap.add_argument("--range", help="git revision range, e.g. origin/main..HEAD")
    ap.add_argument("--from-boundary", action="store_true",
                    help="every commit since the one that added this checker")
    ap.add_argument("--repo", default=REPO)
    a = ap.parse_args()
    if a.message_file:
        with io.open(a.message_file, encoding="utf-8", errors="replace") as f:
            refusals = check_message(f.read())
        if refusals:
            print("commit_msg_check: REFUSED (CONTRIBUTING.md, Commits):")
            for r in refusals:
                print("  - " + r)
            return 1
        return 0
    if a.range:
        return check_range(a.repo, a.range)
    if a.from_boundary:
        b = boundary(a.repo)
        if not b:
            print("commit_msg_check: this checker was never added to this history -- nothing to judge")
            return 0
        return check_range(a.repo, b + "~1..HEAD")
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
