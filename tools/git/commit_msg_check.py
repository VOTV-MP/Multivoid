#!/usr/bin/env python3
"""commit_msg_check -- the shape of a commit message, checked where the message is made.

The rules are the "Commits" section of CONTRIBUTING.md:

  subject   `[scope] imperative summary` on ONE line, at most 72 characters; a git-made
            `Merge branch ...` / `Revert "..."` / `fixup!` / `squash!` subject is exempt from the
            scope prefix, and `[docs] close:` belongs to the session-close script
  body      at most 12 non-empty lines; the attribution trailer block at the end is not counted
  language  English only -- no Cyrillic; the file must be valid UTF-8 without a byte-order mark
  content   none of the working-notes vocabulary (VOCABULARY below names each word and why)

Usage:
  commit_msg_check.py <message-file>        the commit-msg hook (tools/git/hooks/commit-msg):
                                            git's template comment lines and the scissors block
                                            are ignored, as git itself drops them
  commit_msg_check.py --range A..B          every commit in the range, judged as recorded (CI)
  commit_msg_check.py --from-boundary       CI default: every commit since this checker was added

Exit 0 when every message passes, 1 otherwise; each refusal names the rule it breaks.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SELF_REL = "tools/git/commit_msg_check.py"

SUBJECT_MAX = 72
BODY_MAX_LINES = 12
CLOSE_PREFIX = "[docs] close:"

SCOPE_RE = re.compile(r"^\[[a-z0-9][a-z0-9_.-]*\] \S")
EXEMPT_SUBJECT_RE = re.compile(r'^(Merge (branch|remote-tracking branch|pull request|tag|commit)\b|Revert "|fixup! |squash! )')
TRAILER_LINE_RE = re.compile(r"^([A-Za-z][A-Za-z0-9-]*): \S")
KNOWN_TRAILER_KEYS = {"co-authored-by", "claude-session", "signed-off-by", "reviewed-by", "acked-by",
                      "tested-by", "fixes", "closes", "see-also", "docs-census"}
SCISSORS = "# ------------------------ >8 ------------------------"
# git's own template lines, dropped in hook mode only (git drops them before recording a message
# written in the editor; a `#` line in a message passed with -F or -m is recorded and is judged)
TEMPLATE_PREFIXES = ("# Please enter", "# On branch", "# Your branch", "# Changes to be committed",
                     "# Changes not staged", "# Untracked files", "#\t", "# (use ", "# Lines starting",
                     "# with '#' will be", "# HEAD detached", "# Not currently on any branch",
                     "# Author:", "# Date:", "# Committer:", "# It looks like you may be committing",
                     "# If this is not correct", "# Do not modify or remove the line above",
                     "# Everything below it will be ignored")

CYRILLIC = re.compile("[" + chr(0x0400) + "-" + chr(0x04FF) + "]")   # the Cyrillic block

# (pattern, what it is, why it does not belong in a public commit message)
VOCABULARY = [
    (re.compile(r"(?<![\w/])/qf\b|\bqf\b(?!\.)"), "/qf",
     "the review ritual is not part of the change"),
    (re.compile(r"\bdocumentize\b", re.I), "documentize",
     "the session-close ritual is not part of the change"),
    (re.compile(r"(?<![Uu]ser-)\b(?:sub)?agents?\b", re.I), "agent(s)",
     "who reviewed the change is not part of the change"),
    (re.compile(r"(?<![\w./])memory/"), "memory/",
     "the maintainer's memory directory is not in the repository"),
    (re.compile(r"(?<![\w./])research/"), "research/",
     "the research corpus is not in the repository"),
    (re.compile(r"\bCLAUDE\.md\b"), "CLAUDE.md",
     "not in the repository"),
    (re.compile(r"\.claude/"), ".claude/",
     "not in the repository"),
    (re.compile(r"\bdocs/security/"), "docs/security/",
     "not in the repository"),
    (re.compile(r"\bverbatim\b", re.I), "verbatim",
     "quoted conversations stay in the maintainer's notes"),
    (re.compile(r"(?<![$\w])USER\b"), "USER",
     "quoted decisions stay in the maintainer's notes; state the decision itself"),
    (re.compile(r"\bDocs-Census\b"), "Docs-Census",
     "machine trailers from the close ritual are not part of the change"),
]


def split_message(text, hook_mode=False):
    """-> (subject, body_lines, trailer_lines, subject_continues).

    In hook mode git's template comment lines and everything below the scissors line are dropped,
    as git drops them; in range mode the message is judged exactly as recorded. The trailer block is
    the LAST paragraph when every line of it is `Key: value` (or a continuation) and at least one
    key is a known attribution key; a paragraph that merely looks like trailers is body."""
    lines = text.replace("\r\n", "\n").split("\n")
    kept = []
    for line in lines:
        if hook_mode:
            if line.startswith(SCISSORS):
                break
            if line == "#" or line.startswith(TEMPLATE_PREFIXES):
                continue
        kept.append(line.rstrip())
    while kept and not kept[0].strip():
        kept.pop(0)
    while kept and not kept[-1].strip():
        kept.pop()
    if not kept:
        return "", [], [], False
    subject = kept[0]
    rest = kept[1:]
    subject_continues = bool(rest and rest[0].strip())
    trailers = []
    para_start = len(rest)
    for i in range(len(rest) - 1, -1, -1):
        if not rest[i].strip():
            para_start = i + 1
            break
        para_start = i
    last_para = rest[para_start:]
    if last_para and all(TRAILER_LINE_RE.match(l) or l.startswith((" ", "\t")) for l in last_para):
        keys = {TRAILER_LINE_RE.match(l).group(1).lower() for l in last_para if TRAILER_LINE_RE.match(l)}
        if keys & KNOWN_TRAILER_KEYS:
            trailers = last_para
            rest = rest[:para_start]
    body = [l for l in rest if l.strip()]
    return subject, body, trailers, subject_continues


def check_message(text, allow_close=False, hook_mode=False):
    """-> list of refusal strings; empty when the message passes.

    `allow_close`: the `[docs] close:` subject belongs to the session-close script, which finds the
    previous close by that subject; a hand-made one would move its base while recording nothing. The
    script sets MULTIVOID_CLOSE=1 for its own commits and the hook passes that through."""
    if text.startswith("\ufeff"):
        return ["the message starts with a UTF-8 byte-order mark; save the file without a BOM"]
    subject, body, trailers, subject_continues = split_message(text, hook_mode)
    out = []
    if not subject:
        return ["empty message"]
    if subject.startswith(CLOSE_PREFIX) and not allow_close:
        out.append("`{}` is the session-close script's subject; a hand-made close records nothing "
                   "(run the close script)".format(CLOSE_PREFIX))
    if len(subject) > SUBJECT_MAX:
        out.append("subject is {} characters; the limit is {}".format(len(subject), SUBJECT_MAX))
    if subject_continues:
        out.append("the subject is one line, followed by a blank line")
    if not SCOPE_RE.match(subject) and not EXEMPT_SUBJECT_RE.match(subject):
        out.append("subject must start with a lowercase `[scope] ` prefix and a summary")
    if len(body) > BODY_MAX_LINES:
        out.append("body is {} non-empty lines; the limit is {} (the attribution trailers excluded)".format(
            len(body), BODY_MAX_LINES))
    everything = "\n".join([subject] + body + trailers)
    if CYRILLIC.search(everything):
        out.append("Cyrillic in the message; English only")
    for pat, name, why in VOCABULARY:
        if pat.search(everything):
            out.append("`{}` in the message: {}".format(name, why))
    for line in trailers:
        if line.startswith("Docs-Census:"):
            out.append("`Docs-Census` trailer: machine trailers from the close ritual are not "
                       "part of the change")
    return out


def read_message_file(path):
    """-> (text, refusal). A message that is not valid UTF-8 is refused: a legacy encoding would
    carry Cyrillic that no Unicode check can see, and git records the bytes as they are."""
    with open(path, "rb") as f:
        raw = f.read()
    try:
        return raw.decode("utf-8"), None
    except UnicodeDecodeError:
        return None, "the message is not valid UTF-8 (a cp1251 or other legacy encoding); save it as UTF-8"


def git(args, cwd, check=True):
    return subprocess.run(["git"] + args, cwd=cwd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", check=check).stdout


def boundary(repo):
    out = git(["log", "--diff-filter=A", "--format=%H", "--", SELF_REL], repo).split()
    return out[-1] if out else None


def check_range(repo, rng):
    if rng.startswith("-"):
        print("commit_msg_check: a range cannot start with '-' ({})".format(rng))
        return 2
    raw = git(["log", "-z", "--format=%H%n%B", "--end-of-options", rng], repo)
    bad = total = 0
    for rec in raw.split("\0"):
        if not rec.strip():
            continue
        sha, _, body = rec.lstrip("\n").partition("\n")
        subject = body.split("\n", 1)[0]
        total += 1
        refusals = check_message(body, allow_close=True)   # a recorded close was made by the script
        if refusals:
            bad += 1
            print("{} {}".format(sha[:10], subject[:80]))
            for r in refusals:
                print("    - " + r)
    print("commit_msg_check: {} commit(s) in {}, {} refused{}".format(
        total, rng, bad, "" if total else " -- an EMPTY range judges nothing"))
    return 1 if bad else 0


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("message_file", nargs="?", help="the message file (hook mode)")
    ap.add_argument("--range", help="git revision range, e.g. origin/main..HEAD")
    ap.add_argument("--from-boundary", action="store_true",
                    help="every commit since the one that added this checker")
    ap.add_argument("--repo", default=REPO)
    a = ap.parse_args()
    if a.message_file:
        text, refusal = read_message_file(a.message_file)
        refusals = [refusal] if refusal else check_message(
            text, allow_close=os.environ.get("MULTIVOID_CLOSE") == "1", hook_mode=True)
        if refusals:
            print("commit_msg_check: REFUSED (CONTRIBUTING.md, Commits):")
            for r in refusals:
                print("  - " + r)
            return 1
        return 0
    if a.range:
        return check_range(a.repo, a.range)
    if a.from_boundary:
        if git(["rev-parse", "--is-shallow-repository"], a.repo).strip() == "true":
            print("commit_msg_check: a shallow clone cannot find the boundary -- fetch the full history")
            return 1
        b = boundary(a.repo)
        if not b:
            print("commit_msg_check: this checker was never added to this history -- nothing to judge")
            return 0
        has_parent = subprocess.run(["git", "rev-parse", "-q", "--verify", b + "~1"], cwd=a.repo,
                                    capture_output=True).returncode == 0
        return check_range(a.repo, b + "~1..HEAD" if has_parent else "HEAD")
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
