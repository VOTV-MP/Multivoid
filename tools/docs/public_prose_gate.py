#!/usr/bin/env python3
"""public_prose_gate -- the public tree measured against CONTRIBUTING.md, as a ratchet.

What it measures, over every tracked `*.md` and over the comments of the mod's own C++
(`src/votv-coop/{src,include}`), is in COUNTERS below: working-notes vocabulary, non-English
text, dated diary lines, pointers to files that are not in the repository, oversized docs,
comment-heavy sources. Lower is better for every counter. `src.files_not_swept` is the burn-down:
how many of `src.files` still carry any of them, and `--report` lists its worst offenders in size
order, which is the sweep's work queue.

The baseline (`public_prose_baseline.json`) holds the last accepted value of each counter.
The gate FAILS when a RULE counter is above its baseline and names the files that carry the
excess; the three VOLUME counters (`md.lines`, `src.comment_lines`, `src.comment_permille`) are
measured and reported but not compared, so an added sentence or a permitted comment block does
not fail a push. `--update` writes the current values after a PASS, so the baseline only moves
down by itself; a counter that has no baseline fails until it is added by hand in the commit
that needs it, where a reviewer sees it; raising a value is the same hand edit.

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
from urllib.parse import unquote

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
BASELINE = os.path.join(HERE, "public_prose_baseline.json")

MD_EXEMPT = ("THIRD-PARTY-NOTICES.md",)          # third-party license texts, reproduced as-is
SRC_ROOTS = ("src/votv-coop/src/", "src/votv-coop/include/")
SRC_EXT = (".cpp", ".h", ".inc")
MD_HARD_CAP = 600
# The tracked text files that are neither markdown nor our own C++: build, CI, ignore rules,
# scripts. Extension-gated so a binary is never read as prose; vendored trees are skipped whole,
# since a third-party file is not ours to rewrite.
OTHER_EXT = (".py", ".ps1", ".yml", ".yaml", ".json", ".toml", ".rs", ".cs", ".txt", ".tsv",
             ".rc", ".in", ".cmake", "CMakeLists.txt", ".gitignore", ".gitattributes")
OTHER_SKIP = ("src/votv-coop/third_party/", "reference/", "tools/client_model/mesh_extract/")
# The files whose JOB is these markers: a gate that refuses a word must name it, and a drill
# that proves the refusal must carry a fixture containing it. Counting those would push a sweep
# to break the very checks it is measured by -- the same trap the offset detector hit.
OTHER_MARKER_OWNERS = ("tools/docs/public_prose_gate", "tools/docs/public_leak_gate",
                       "tools/docs/lessons_gate", "tools/git/commit_msg_check")
# Third-party licence texts are reproduced as-is, and the baseline is generated from the counters,
# so it names them by construction.
OTHER_EXEMPT = ("LICENSE", "THIRD-PARTY", "public_prose_baseline.json")
# In an ignore file the RULE is data and only the comment is prose: a rule cannot ignore a path
# without naming it, so `docs/AGENT_SPAWNING.md` must appear for the rule to work at all.
OTHER_COMMENTS_ONLY = (".gitignore", ".gitattributes")
HALF_COMMENT_MIN_LINES = 300
INFORMATIONAL = ("md.lines", "src.comment_lines", "src.comment_permille", "src.files",
                 "other.files")  # reported, never compared

CYRILLIC = re.compile("[" + chr(0x0400) + "-" + chr(0x04FF) + "]")
DATE = re.compile(r"\b20\d\d-\d\d-\d\d\b")
LINK = re.compile(r"\]\(([^)\s#]+)(?:#[^)]*)?\)")
BACKTICK_PATH = re.compile(r"`((?:docs|tools|src)/[A-Za-z0-9_./-]+\.md)`")
DOC_PATH = re.compile(r"\bdocs/[A-Za-z0-9_./-]+?\.md\b")   # a doc named in a source comment
LONG_COMMENT_BLOCK = 15
# A struct offset pinned in prose. The offset CONTEXT is required, not merely a hex literal: the
# tree is full of correct hex that is not an offset -- bytecode opcodes (0x45), sentinels (0xFF),
# struct sizes, colour components -- and flagging those would push a sweep to make good comments
# worse. Matching any hex over-reported by 118 lines when this was measured.
RAW_OFFSET = re.compile(r"(?:@\s*\+?|\+|\bat\s+\+?|\boffset\s+)(0x[0-9A-Fa-f]{2,4})\b")
HEX_LITERAL = re.compile(r"0x[0-9A-Fa-f]{2,4}\b")
# The files whose JOB is offsets. Everywhere else a pinned number duplicates them and rots when the
# game is recooked, silently, because nothing ever compiles against a comment.
OFFSET_OWNERS = ("sdk_profile", "reflected_offset", "gvas_meta")
# A free-function declaration in one of our headers. An identifier appearing at most twice in the
# whole tree is its own declaration plus its definition: nothing calls it.
DECL = re.compile(r"^[A-Za-z_][\w:<>,*&\s]*?[\s*&]([A-Za-z_]\w+)\s*\([^;{]*\)\s*(?:const\s*)?;", re.M)
IDENT = re.compile(r"[A-Za-z_]\w*")
INCLUDE = re.compile(r'#\s*include\s+"([^"]+)"')
DECL_SKIP = {"if", "for", "while", "return", "switch", "sizeof", "static_cast", "reinterpret_cast",
             "const_cast", "dynamic_cast", "assert", "catch"}

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
    """-> (files, submodule paths). A link into a submodule is live: its content is public too.
    NUL-separated so a non-ASCII path arrives unquoted."""
    out = git(["ls-files", "-s", "-z"], repo)
    files, subs = [], []
    for line in out.split("\0"):
        if not line:
            continue
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


def code_only(text):
    """-> the text with comments removed, for counting how often an identifier is really USED.
    Counting raw occurrences called a dead function alive whenever some comment happened to name
    it: `DumpAnimNodeRegions` has a declaration, a definition and one mention in an sdk_profile.h
    comment, and that third occurrence hid it. Strings are left in place; a name in a log message
    is at least evidence of a caller nearby, while a name in prose is evidence of nothing."""
    out, in_block = [], False
    for line in text.split("\n"):
        if in_block:
            k = line.find("*/")
            if k < 0:
                continue
            line, in_block = line[k + 2:], False
        while True:
            b = line.find("/*")
            s = line.find("//")
            if s >= 0 and (b < 0 or s < b):
                line = line[:s]
                break
            if b < 0:
                break
            e = line.find("*/", b + 2)
            if e < 0:
                line, in_block = line[:b], True
                break
            line = line[:b] + " " + line[e + 2:]
        out.append(line)
    return "\n".join(out)


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
            elif c == "'" and i > 0 and s[i - 1].isdigit() and i + 1 < n and s[i + 1].isdigit():
                pass                                    # a digit separator, not a quote
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
        lines = text.splitlines()
        c["md.lines"] += len(lines)
        who["md.lines"][p] = len(lines)
        if len(lines) > MD_HARD_CAP:
            c["md.over_%d" % MD_HARD_CAP] += 1
            who["md.over_%d" % MD_HARD_CAP][p] = len(lines)
        base = os.path.dirname(p)
        fenced = False
        for line in lines:
            if line.lstrip().startswith("```"):
                fenced = not fenced
                continue
            if fenced:
                continue
            for k, (rx, _) in LINE_MARKERS.items():
                if rx.search(line):
                    c["md." + k] += 1
                    who["md." + k][p] += 1
            for m in LINK.finditer(line):
                target = unquote(m.group(1))
                if re.match(r"^[a-z]+:", target, re.I) or target.startswith("/"):
                    continue
                if "\\" in target:                       # a backslash path is dead on Linux
                    c["md.dead_links"] += 1
                    who["md.dead_links"][p] += 1
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
    # Every OTHER tracked text file: the build, the CI workflows, the ignore rules, the scripts a
    # contributor runs. They are as public as the docs and were the last class no counter read --
    # the ignore file alone carried 31 dated lines, a user quote and a paragraph that counted the
    # open rows in an unpublished security register. Whole lines are measured, not just comments:
    # in a config file the distinction does not hold, and a marker anywhere in one is the problem.
    other = [p for p in files
             if not p.endswith(".md") and p.endswith(OTHER_EXT)
             and not p.startswith(SRC_ROOTS) and not p.startswith(OTHER_SKIP)
             and not p.startswith(OTHER_MARKER_OWNERS)
             and not any(x in os.path.basename(p) for x in OTHER_EXEMPT)]
    c["other.files"] = len(other)
    for k in LINE_MARKERS:
        c["other." + k] = 0
    for p in other:
        text = read(repo, p)
        if text is None:
            continue
        who["other.files"][p] = len(text.splitlines())
        lines = text.splitlines()
        if p.endswith(OTHER_COMMENTS_ONLY):
            lines = [l for l in lines if l.lstrip().startswith("#")]
        for line in lines:
            for k, (rx, _) in LINE_MARKERS.items():
                if rx.search(line):
                    c["other." + k] += 1
                    who["other." + k][p] += 1
    src = [p for p in files if p.startswith(SRC_ROOTS) and p.endswith(SRC_EXT)]
    c["src.comment_lines"] = 0
    code_total = 0
    for k in list(LINE_MARKERS) + list(SRC_EXTRA):
        c["src.comment_" + k] = 0
    c["src.files_half_comment"] = 0
    c["src.comment_blocks_over_%d" % LONG_COMMENT_BLOCK] = 0
    c["src.comment_dead_docpath"] = 0
    c["src.comment_pinned_offset"] = 0
    bare_by_file = {}                      # path -> its code with comments and strings gone
    declared = collections.defaultdict(list)   # name -> every header that declares it
    for p in src:
        text = read(repo, p)
        if text is None:
            continue
        bare = code_only(text)
        bare_by_file[p] = bare
        # The offsets this file's own code reads. A comment naming one of them sits beside the read
        # it explains and the number IS the fact; a comment naming any other pins a number whose
        # owner is elsewhere -- another file's named constant, or a property the code resolves by
        # name -- where a recook moves the field and nothing ever compiles against the prose.
        read_offsets = {int(h, 16) for h in HEX_LITERAL.findall(bare)}
        if p.endswith(".h") and "/include/" in p:
            for name in set(DECL.findall(bare)):
                if name not in DECL_SKIP and len(name) > 3:
                    declared[name].append(p)
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
            if any(m not in tracked_set for m in DOC_PATH.findall(line)):
                c["src.comment_dead_docpath"] += 1
                who["src.comment_dead_docpath"][p] += 1
            if not any(o in os.path.basename(p) for o in OFFSET_OWNERS):
                pinned = {int(h, 16) for h in RAW_OFFSET.findall(line)}
                if pinned - read_offsets:
                    c["src.comment_pinned_offset"] += 1
                    who["src.comment_pinned_offset"][p] += 1
    # A capability nothing calls is not shipped, and its comment describes code no one runs.
    #
    # Uses are attributed to the header that DECLARES the name, counted only where that header is
    # actually reachable. Counting the bare name tree-wide could not do this: `SetActiveFn` is
    # declared by both ue_wrap/desk/desk_audio.h and ue_wrap/devices/lightswitch.h, and the live
    # call to the first marked the second alive, hiding a dead capability behind a live sibling.
    # The reach is TRANSITIVE and includes the header itself, because the two cheaper answers are
    # both wrong: direct includers alone miss a caller that picks the declaration up through
    # another header, and excluding the declaring header misses the inline wrapper that calls the
    # out-of-line function right beneath it.
    direct = collections.defaultdict(set)          # header path -> files that #include it directly
    for path, bare in bare_by_file.items():
        for inc in INCLUDE.findall(bare):
            for root in SRC_ROOTS:
                if root + inc in bare_by_file:
                    direct[root + inc].add(path)
                    break
    reach_cache = {}

    def reach(header):
        """Every file that can see `header`'s declarations, itself included."""
        if header not in reach_cache:
            seen, stack = {header}, [header]
            while stack:
                for f in direct.get(stack.pop(), ()):
                    if f not in seen:
                        seen.add(f)
                        stack.append(f)
            reach_cache[header] = seen
        return reach_cache[header]

    c["src.dead_declarations"] = 0
    for name, headers in declared.items():
        word = re.compile(r"\b" + re.escape(name) + r"\b")
        for header in headers:
            uses = sum(len(word.findall(bare_by_file[f])) for f in reach(header))
            if uses <= 2:                  # its own declaration and its definition, and nothing else
                c["src.dead_declarations"] += 1
                who["src.dead_declarations"][header] += 1
    c["src.comment_permille"] = int(round(1000.0 * c["src.comment_lines"] / max(1, code_total + c["src.comment_lines"])))
    c["src.files"] = len(src)
    # The burn-down. A source file is SWEPT when it contributes zero to every rule counter above;
    # this counts the ones that do not, and `who` orders them by comment size, so --report's top
    # list is the work queue itself.
    rule_src = [k for k in c if k.startswith("src.") and k not in INFORMATIONAL
                and k not in ("src.files", "src.files_not_swept")]
    not_swept = {path for k in rule_src for path in who[k]}
    c["src.files_not_swept"] = len(not_swept)
    for path in not_swept:
        who["src.files_not_swept"][path] = who["src.comment_lines"][path]
    return c, who


def load_baseline(path):
    if not os.path.isfile(path):
        return None
    with io.open(path, encoding="utf-8") as f:
        return json.load(f)


def save_baseline(path, counters, repo):
    sha = git(["rev-parse", "--short", "HEAD"], repo).strip()
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"as_of": sha, "note": "measured on the working tree; as_of is HEAD at that moment",
                   "counters": counters}, f, indent=1)
        f.write("\n")


FIXED_DESCRIPTIONS = {
    "md.files": "tracked markdown files",
    "md.lines": "markdown lines",
    "md.over_%d" % MD_HARD_CAP: "docs over the %d-line hard cap" % MD_HARD_CAP,
    "md.dead_links": "markdown links to a path not in the repository",
    "md.dead_paths": "backticked docs/tools/src paths that name no tracked file",
    "src.comment_blocks_over_%d" % LONG_COMMENT_BLOCK: "comment blocks longer than %d lines" % LONG_COMMENT_BLOCK,
    "src.comment_lines": "comment lines in the mod's own C++",
    "src.comment_dead_docpath": "comment lines naming a docs/*.md that is not in the repository",
    "src.comment_permille": "comment lines per 1000 lines of code+comment",
    "src.files_half_comment": "sources over %d lines that are more than half comment" % HALF_COMMENT_MIN_LINES,
    "src.files": "tracked sources in the mod's own C++",
    "src.files_not_swept": "sources still carrying at least one counter above",
    "src.comment_pinned_offset": "comment lines pinning an offset this file's own code never reads",
    "src.dead_declarations": "declared functions nothing in the tree calls",
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
    ap.add_argument("--force", action="store_true", help="with --init: overwrite an existing baseline")
    ap.add_argument("--top", type=int, default=5)
    ap.add_argument("--file", action="append", metavar="PATH",
                    help="report what these files owe, counter by counter, and exit; repeatable")
    a = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    counters, who = measure(a.repo)
    if a.file:
        for want in a.file:
            want = want.replace("\\", "/").lstrip("./")
            hits = sorted({p for k in who for p in who[k] if p.endswith(want)})
            if not hits:
                print("{}: no tracked file matches".format(want))
                continue
            for p in hits:
                owed = [(k, who[k][p]) for k in counters
                        if k not in INFORMATIONAL and k != "src.files_not_swept" and p in who[k]]
                print("{}  ({} comment lines)".format(p, who["src.comment_lines"][p]))
                for k, n in owed:
                    print("    {:<30} {:>4}  {}".format(k, n, describe(k)))
                if not owed:
                    print("    SWEPT -- contributes to no counter")
        return 0
    if a.init:
        if os.path.isfile(a.baseline) and not a.force:
            print("public_prose_gate: FAIL -- a baseline exists at {}; --init --force overwrites it "
                  "(that can raise values silently, so say why in the commit)".format(a.baseline))
            return 1
        save_baseline(a.baseline, counters, a.repo)
        print("public_prose_gate: baseline written ({} counters)".format(len(counters)))
        return 0
    base = load_baseline(a.baseline)
    if base is None:
        print("public_prose_gate: FAIL -- no baseline at {} (run --init once)".format(a.baseline))
        return 1
    bc = base.get("counters", {})
    fails = []
    added = [k for k in counters if k not in bc and k not in INFORMATIONAL]
    for k, v in counters.items():
        if k in INFORMATIONAL:
            continue
        b = bc.get(k)
        if b is not None and v > b:
            fails.append((k, b, v))
    if a.report or fails:
        print("{:<28} {:>8} {:>8}  {}".format("counter", "baseline", "now", "what"))
        for k, v in counters.items():
            b = bc.get(k, "-")
            flag = "  <-- GREW" if (isinstance(b, int) and v > b and k not in INFORMATIONAL) else (
                "  (informational)" if k in INFORMATIONAL else ("  <-- NO BASELINE" if k in added else ""))
            print("{:<28} {:>8} {:>8}  {}{}".format(k, b, v, describe(k), flag))
            if a.report or flag:
                for p, n in who[k].most_common(a.top):
                    print("{:<28} {:>8} {:>8}    {} ({})".format("", "", "", p, n))
    if added:
        print("public_prose_gate: FAIL -- {} counter(s) with no baseline: {} -- add them to {} by hand "
              "in the commit that introduces them".format(len(added), ", ".join(added), os.path.basename(a.baseline)))
        return 1
    if fails:
        print("public_prose_gate: FAIL -- {} counter(s) above the baseline: {}".format(
            len(fails), ", ".join("{} {}->{}".format(k, b, v) for k, b, v in fails)))
        return 1
    if a.update:
        lowered = [k for k, v in counters.items() if k in bc and v < bc[k] and k not in INFORMATIONAL]
        save_baseline(a.baseline, counters, a.repo)
        print("public_prose_gate: PASS -- baseline updated ({} lowered: {})".format(
            len(lowered), ", ".join(lowered) or "none"))
        return 0
    print("public_prose_gate: PASS ({} counters at or below the baseline)".format(len(counters)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
