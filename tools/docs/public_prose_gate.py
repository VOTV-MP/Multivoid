#!/usr/bin/env python3
"""public_prose_gate -- the public tree measured against CONTRIBUTING.md, as a ratchet.

What it measures, over every tracked `*.md` and over the comments of the mod's own C++
(`src/votv-coop/{src,include}`), is in COUNTERS below: working-notes vocabulary, non-English
text, dated diary lines, pointers to files that are not in the repository, oversized docs,
comment-heavy sources. Lower is better for every counter.

The baseline (`public_prose_baseline.json`) holds the last accepted value of each counter.
The gate FAILS when any counter is above its baseline and names the files that carry the
excess. `--update` writes the current values after a PASS, so the baseline only ever moves
down by itself; raising a value is a hand edit of the JSON, made in the commit that needs it,
where a reviewer sees it.

    python tools/docs/public_prose_gate.py              # PASS/FAIL against the baseline
    python tools/docs/public_prose_gate.py --report     # every counter, baseline, and top files
    python tools/docs/public_prose_gate.py --update     # after a PASS: ratchet the baseline down
    python tools/docs/public_prose_gate.py --init       # first run: write the baseline as-is
"""
import argparse
import collections
import io
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
BASELINE = os.path.join(HERE, "public_prose_baseline.json")

MD_EXEMPT = ("THIRD-PARTY-NOTICES.md",)          # third-party license texts, reproduced as-is
SRC_ROOTS = ("src/votv-coop/src/", "src/votv-coop/include/")
SRC_EXT = (".cpp", ".h", ".inc")
MD_HARD_CAP = 600
HALF_COMMENT_MIN_LINES = 300

CYRILLIC = re.compile("[" + chr(0x0400) + "-" + chr(0x04FF) + "]")
DATE = re.compile(r"\b20\d\d-\d\d-\d\d\b")
LINK = re.compile(r"\]\(([^)\s#]+)(?:#[^)]*)?\)")
BACKTICK_PATH = re.compile(r"`((?:docs|tools|src)/[A-Za-z0-9_./-]+\.md)`")
LONG_COMMENT_BLOCK = 15

# name -> (regex, what it counts). Each is applied per LINE of markdown / per comment line.
LINE_MARKERS = collections.OrderedDict([
    ("cyrillic",     (CYRILLIC, "lines with Cyrillic")),
    ("user",         (re.compile(r"\bUSER\b"), "lines quoting a decision as USER")),
    ("verbatim",     (re.compile(r"\bverbatim\b", re.I), "lines saying verbatim")),
    ("qf",           (re.compile(r"(?<![\w/])/qf\b|\bqf\b(?!\.)"), "lines naming the /qf ritual")),
    ("agent",        (re.compile(r"\b(?:sub)?agents?\b", re.I), "lines naming an agent")),
    ("dated",        (DATE, "lines carrying a date")),
    ("ptr_memory",   (re.compile(r"(?<![\w.])memory/"), "pointers into the memory directory")),
    ("ptr_research", (re.compile(r"(?<![\w.])research/"), "pointers into research/")),
    ("ptr_claude",   (re.compile(r"\bCLAUDE\.md\b|\.claude/"), "pointers to CLAUDE.md or .claude/")),
    ("ptr_security", (re.compile(r"\bdocs/security/"), "pointers into docs/security/")),
])
SRC_EXTRA = collections.OrderedDict([
    ("lesson", (re.compile(r"\bLESSONS\b|\blessons?\b", re.I), "comment lines citing a lesson")),
    ("sha",    (re.compile(r"`[0-9a-f]{8,10}`|\bcommit [0-9a-f]{7,10}\b"), "comment lines citing a commit")),
])


def git(args, cwd):
    return subprocess.run(["git"] + args, cwd=cwd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", check=True).stdout


def tracked(repo):
    """-> (files, submodule paths). A link into a submodule is live: its content is public too."""
    out = git(["ls-files", "-s"], repo)
    files, subs = [], []
    for line in out.splitlines():
        mode, rest = line.split(" ", 1)
        path = rest.split("\t", 1)[1]
        (subs if mode == "160000" else files).append(path)
    return files, subs


def read(repo, path):
    try:
        with io.open(os.path.join(repo, path), encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def comment_lines(text):
    """-> (comment_line_texts, code_line_count, long_blocks). A line counts as COMMENT when it holds
    nothing but comment (and whitespace); quotes are respected so a `//` inside a string is code.
    `long_blocks` counts runs of more than LONG_COMMENT_BLOCK consecutive comment lines (blank lines
    do not break a run; a code line does)."""
    lines = text.split("\n")
    comments, code, long_blocks, run = [], 0, 0, 0
    in_block = False

    def comment(line):
        nonlocal run, long_blocks
        comments.append(line)
        run += 1
        if run == LONG_COMMENT_BLOCK + 1:
            long_blocks += 1

    for line in lines:
        s = line.strip()
        if not s:
            continue
        if in_block:
            comment(line)
            if "*/" in s:
                in_block = False
            continue
        if s.startswith("//"):
            comment(line)
            continue
        if s.startswith("/*"):
            comment(line)
            if "*/" not in s[2:]:
                in_block = True
            continue
        run = 0
        # code line; a trailing block opener leaves the state in a block
        i, n, q = 0, len(s), None
        while i < n:
            c = s[i]
            if q:
                if c == "\\":
                    i += 2
                    continue
                if c == q:
                    q = None
            elif c in "\"'":
                q = c
            elif s.startswith("//", i):
                break
            elif s.startswith("/*", i):
                if "*/" not in s[i + 2:]:
                    in_block = True
                break
            i += 1
        code += 1
    return comments, code, long_blocks


def measure(repo):
    """-> (counters dict, contributors dict: counter -> Counter(path -> hits))."""
    files, subs = tracked(repo)
    tracked_set = set(files)
    c = collections.OrderedDict()
    who = collections.defaultdict(collections.Counter)
    md = [p for p in files if p.endswith(".md") and os.path.basename(p) not in MD_EXEMPT]
    c["md.files"] = len(md)
    c["md.lines"] = 0
    c["md.over_%d" % MD_HARD_CAP] = 0
    for k in LINE_MARKERS:
        c["md." + k] = 0
    c["md.dead_links"] = 0
    c["md.dead_paths"] = 0
    for p in md:
        text = read(repo, p)
        if text is None:
            continue
        lines = text.split("\n")
        c["md.lines"] += len(lines)
        who["md.lines"][p] = len(lines)
        if len(lines) > MD_HARD_CAP:
            c["md.over_%d" % MD_HARD_CAP] += 1
            who["md.over_%d" % MD_HARD_CAP][p] = len(lines)
        base = os.path.dirname(p)
        for line in lines:
            for k, (rx, _) in LINE_MARKERS.items():
                if rx.search(line):
                    c["md." + k] += 1
                    who["md." + k][p] += 1
            for m in LINK.finditer(line):
                target = m.group(1)
                if re.match(r"^[a-z]+:", target) or target.startswith("/"):
                    continue
                rel = os.path.normpath(os.path.join(base, target)).replace("\\", "/")
                if rel in tracked_set or any(t.startswith(rel + "/") for t in tracked_set) \
                        or any(rel == s or rel.startswith(s + "/") or s.startswith(rel + "/") for s in subs):
                    continue
                c["md.dead_links"] += 1
                who["md.dead_links"][p] += 1
            for m in BACKTICK_PATH.finditer(line):
                rel = m.group(1)
                if rel in tracked_set or any(t.startswith(rel + "/") for t in tracked_set):
                    continue
                c["md.dead_paths"] += 1
                who["md.dead_paths"][p] += 1
    src = [p for p in files if p.startswith(SRC_ROOTS) and p.endswith(SRC_EXT)]
    c["src.comment_lines"] = 0
    code_total = 0
    for k in list(LINE_MARKERS) + list(SRC_EXTRA):
        if k.startswith("ptr_") and k != "ptr_memory" and k != "ptr_research" and k != "ptr_claude":
            continue
        c["src.comment_" + k] = 0
    c["src.files_half_comment"] = 0
    c["src.comment_blocks_over_%d" % LONG_COMMENT_BLOCK] = 0
    for p in src:
        text = read(repo, p)
        if text is None:
            continue
        comments, code, long_blocks = comment_lines(text)
        c["src.comment_lines"] += len(comments)
        code_total += code
        who["src.comment_lines"][p] = len(comments)
        if long_blocks:
            c["src.comment_blocks_over_%d" % LONG_COMMENT_BLOCK] += long_blocks
            who["src.comment_blocks_over_%d" % LONG_COMMENT_BLOCK][p] = long_blocks
        if code + len(comments) > HALF_COMMENT_MIN_LINES and len(comments) > code:
            c["src.files_half_comment"] += 1
            who["src.files_half_comment"][p] = len(comments)
        for line in comments:
            for k, (rx, _) in list(LINE_MARKERS.items()) + list(SRC_EXTRA.items()):
                key = "src.comment_" + k
                if key in c and rx.search(line):
                    c[key] += 1
                    who[key][p] += 1
    c["src.comment_permille"] = int(round(1000.0 * c["src.comment_lines"] / max(1, code_total + c["src.comment_lines"])))
    return c, who


def load_baseline(path):
    if not os.path.isfile(path):
        return None
    with io.open(path, encoding="utf-8") as f:
        return json.load(f)


def save_baseline(path, counters, repo):
    sha = git(["rev-parse", "--short", "HEAD"], repo).strip()
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"as_of": sha, "counters": counters}, f, indent=1)
        f.write("\n")


FIXED_DESCRIPTIONS = {
    "md.files": "tracked markdown files",
    "md.lines": "markdown lines",
    "md.over_%d" % MD_HARD_CAP: "docs over the %d-line hard cap" % MD_HARD_CAP,
    "md.dead_links": "markdown links to a path not in the repository",
    "md.dead_paths": "backticked docs/tools/src paths that name no tracked file",
    "src.comment_blocks_over_%d" % LONG_COMMENT_BLOCK: "comment blocks longer than %d lines" % LONG_COMMENT_BLOCK,
    "src.comment_lines": "comment lines in the mod's own C++",
    "src.comment_permille": "comment lines per 1000 lines of code+comment",
    "src.files_half_comment": "sources over %d lines that are more than half comment" % HALF_COMMENT_MIN_LINES,
}


def describe(k):
    if k in FIXED_DESCRIPTIONS:
        return FIXED_DESCRIPTIONS[k]
    base = k.split(".", 1)[1].replace("comment_", "")
    if base in LINE_MARKERS:
        return ("comment " if k.startswith("src.") else "") + LINE_MARKERS[base][1]
    if base in SRC_EXTRA:
        return SRC_EXTRA[base][1]
    return k


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--repo", default=REPO)
    ap.add_argument("--baseline", default=BASELINE)
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--update", action="store_true", help="after a PASS, ratchet the baseline down")
    ap.add_argument("--init", action="store_true", help="write the baseline from the current tree")
    ap.add_argument("--top", type=int, default=5)
    a = ap.parse_args()
    counters, who = measure(a.repo)
    if a.init:
        save_baseline(a.baseline, counters, a.repo)
        print("public_prose_gate: baseline written ({} counters)".format(len(counters)))
        return 0
    base = load_baseline(a.baseline)
    if base is None:
        print("public_prose_gate: FAIL -- no baseline at {} (run --init once)".format(a.baseline))
        return 1
    bc = base.get("counters", {})
    fails = []
    for k, v in counters.items():
        b = bc.get(k)
        if b is not None and v > b:
            fails.append((k, b, v))
    if a.report or fails:
        print("{:<28} {:>8} {:>8}  {}".format("counter", "baseline", "now", "what"))
        for k, v in counters.items():
            b = bc.get(k, "-")
            flag = "  <-- GREW" if (isinstance(b, int) and v > b) else ""
            print("{:<28} {:>8} {:>8}  {}{}".format(k, b, v, describe(k), flag))
            if a.report or flag:
                for p, n in who[k].most_common(a.top):
                    print("{:<28} {:>8} {:>8}    {} ({})".format("", "", "", p, n))
    if fails:
        print("public_prose_gate: FAIL -- {} counter(s) above the baseline: {}".format(
            len(fails), ", ".join("{} {}->{}".format(k, b, v) for k, b, v in fails)))
        return 1
    if a.update:
        lowered = [k for k, v in counters.items() if k in bc and v < bc[k]]
        save_baseline(a.baseline, counters, a.repo)
        print("public_prose_gate: PASS -- baseline updated ({} lowered: {})".format(
            len(lowered), ", ".join(lowered) or "none"))
        return 0
    print("public_prose_gate: PASS ({} counters at or below the baseline)".format(len(counters)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
