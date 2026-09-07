#!/usr/bin/env python3
"""public_prose_gate_drill -- show .github/ci/public_prose_gate.py RED before trusting it green.

A throwaway repository with one doc and one source file carrying every class of marker; the
gate must count each, FAIL against a baseline one lower, PASS against an exact one, refuse to
ratchet up, and ratchet down after the marker is removed. A link to a tracked file must not
count as dead; a link to an untracked one must.

    python .github/ci/public_prose_gate_drill.py
"""
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GATE = os.path.join(HERE, "public_prose_gate.py")

DOC = """# A doc

USER said, verbatim: «делай». A /qf round and two audit agents agreed on 2026-09-05.
See memory/feedback_x.md, research/runs/x, CLAUDE.md, .claude/skills and docs/security/TRACKER.md.
A wiki link [[lesson-some-slug]] is the same pointer in the notation memory uses.
A [tracked link](../README.md), an [external](https://example.com), and a [dead one](../nowhere.md).
A backticked `docs/nowhere.md` names no file; a backticked `docs/a.md` does.
An [uppercase scheme](HTTPS://example.com/x), an [encoded name](My%20File.md), a [backslash](sub\\c.md).

```
a [link in a code block](nothing.md) is not a link
```
"""
SRC = """#include "d.h"
// 2026-09-05: the USER asked for this, verbatim; a /qf round and an agent agreed.
// see research/findings/x.md and CLAUDE.md, lesson 12, commit deadbeef12
// a wiki link like [[feedback-some-rule]] points at memory the same as memory/x.md does
// PRECISION: a[[i]], [[ab]] and the C++ attribute [[no_unique_address]] are not slugs
// the files are NAMED with underscores: [[feedback_probe_dont_guess_rule]] is the same pointer
// docs/nowhere.md names no tracked doc; docs/a.md does
// the field sits at 0x04F8, a number pinned in prose that this file's code never reads
// PRECISION: opcode 0x45, sentinel 0xFF and colour 0x40 are hex, not offsets, and must NOT count
// OWNERSHIP: the translation at +0x10 is read three lines down, so the number IS the fact here
// RECALL: naming hiddenByAComment() here must NOT rescue it from the dead list
// the retry came from a review nobody outside can read
// two audits agreed; a third audited it; a fourth is auditing it now
// AUDIT F-3 in capitals is the same citation
// `[V]` an evidence tag from our working docs, which source has no legend for
// a bare [?] on its own line
// and a [RD] on its own
int calledOnce() { return 0; }
// one label family per line, so dropping any one of them turns this drill red
// CRIT-1 names a review finding
// security A34 names a row of a register that is not published
// Inc-2 names a build increment
// take-9 names an attempt
// WP-2 names a work package
// the s28 cut named a session
// finding 3 named itself
// K-5: a bare label counts when it opens the comment
// v133 names the build a change landed in, which a reader cannot look up
// PRECISION: IPv4, a path like tools/v12/x, a decimal 1.v20 and a bare v9 are not build tags
// a colon does too, as in R-2: the shared-scan consumer
// PRECISION: an auditorium, a bracketed [Value], an [ok] flag, leaving N-1 rows and
// a balance of X -> X-93 are none of them
// `[A]` is an evidence tag of the same family as the three above
int neverCalledAnywhere() { return calledOnce(); }
int hiddenByAComment() { return 0; }
int sameNameAsALiveOne() { return 0; }
int useTheLiveTwin() { return sameNameAsALiveOne(); }
int f() { return 0; } // trailing comments are code lines for the VOLUME counters
// POSITION: a citation is a citation wherever it sits, so the MARKER counters read the tail of a
// code line too. One marker per line, so dropping the tails cannot stay green on a survivor.
int t1() { return 0; } // WP-4 names a work package from a trailing comment
int t2() { return 0; } // this one was settled by an audit nobody outside can read
int t3() { return 0; } // added 2026-09-06, which is a diary entry wherever it sits
int t4() { return 0; } /* a trailing BLOCK comment carries a citation too: take-9 */
const char* d = /* closes here */ "docs/nowhere.md";  // the string is CODE, not comment
float translation(const char* base) { return *(const float*)(base + 0x10); }
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
// a survey row cited with no slug at all, as `row :79` is
int r1() { return 0; } // and the same citation carrying its slug, as census row a_file:138 does
int r2() { return 0; } // plural too, as census rows a_file:192/:196
// PRECISION: a row that is a MIRROR -> a 1:1, rows 1 and 2, and a ratio of 3:4 are all prose
// a line of a file version that no longer exists, as x.cpp's old :436-783 block is
// PRECISION: a deliberate :7777 and the default :80/:443 are ports, not a line of anything
// a citation of a line a file no longer has, as x.cpp:9000 is in a file this short
// a citation of a file the tree does not carry at all, as gone.cpp:12 is
// PRECISION: x.cpp:1 resolves -- the file is here and it is that long, so it is not debt
// one document spelling per line, so dropping any one of them turns this drill red
// COOP_EVENT_JOIN.md names a document with no directory in front of it and none in the tree
// DEATH_ARC section 3.1 is the same name with the extension dropped and a locator in its place
// the design doc settled it, which is a document with no name a reader can look up at all
// PRECISION: docs/TRACKED.md named bare, as TRACKED.md, resolves by BASENAME and is not debt
// PRECISION: MY_PORT (10001) is configuration, and the tail of votv-x-design-SOMEWHERE.md
// belongs to the pointer that names the finding, not to a document of its own
"""

# The other.* class: a script a contributor runs, an ignore file, and a manifest. Whole lines
# are measured here, except in an ignore file where only the # comments are. One marker per line.
OTHER_PY = """# a script
# see NOWHERE.md, a document this tree does not carry
# the design doc says so, which names one that cannot even be looked up
# PRECISION: docs/a.md and a bare TRACKED.md both resolve and are not debt
# dated 2026-09-05
# the USER asked for it
# said verbatim
# a /qf round
# an agent ran it
# see research/runs/x
# see memory/feedback_y.md
# see CLAUDE.md
# see docs/security/TRACKER.md
# a wiki link [[lesson-some-other-slug]]
print("делай")
"""
# PRECISION for the comments-only carve-out: the RULES name research/ and .claude/ and must NOT
# count, because a rule cannot ignore a path without naming it. Only the comment counts.
OTHER_IGNORE = """# the notes are not published, decided 2026-09-05
research/
.claude/
"""
# PRECISION for the wikilink slug shape: a TOML array-of-tables header is not a pointer.
OTHER_TOML = """# a manifest
[[bin]]
[[test]]
name = "x"
"""

# a source that carries comment but no counter at all: it must count as SWEPT
SRC_CLEAN = """// What this does, in one line of present-tense fact.
// A second line, well under the block cap.
int h() { return 2; }
"""
# a header declaring one function the tree calls and one nothing calls
HDR = """#pragma once
int calledOnce();
int neverCalledAnywhere();
int hiddenByAComment();
int sameNameAsALiveOne();
"""
# The precision canary for attribution: this header declares a name that ALSO exists, live, in
# d.h. Counting the bare name tree-wide called both alive and hid a real dead capability.
HDR_TWIN = """#pragma once
int sameNameAsALiveOne();
"""
# the offsets belong here, so a pinned number in THIS file is not counted
HDR_OWNER = """#pragma once
// the profile owns offsets: 0x1234 here is the point of the file
inline constexpr int kThing = 0x1234;
"""


# Every counter above is proved by a COUNT, and a count only proves the detector is not silent.
# These prove it is not deaf either: each row breaks one detector on a COPY of the gate, and the
# drill must go RED. A row that stays green names a regression that would ship -- which is how
# `[A]` and the whole named-label family were found to have no canary at all.
MUTANTS = [
    ("review: no inflections", 'r"\\baudit(?:s|ed|ing)?\\b", re.I', 'r"\\baudit\\b", re.I'),
    ("review: case-sensitive", 'r"\\baudit(?:s|ed|ing)?\\b", re.I', 'r"\\baudit(?:s|ed|ing)?\\b"'),
    ("review: matches nothing", 'r"\\baudit(?:s|ed|ing)?\\b", re.I', 'r"\\bZZZZ\\b", re.I'),
    ("review: no trailing boundary", 'r"\\baudit(?:s|ed|ing)?\\b", re.I', 'r"\\baudit", re.I'),
    ("evidence: drops [RD]", 'r"\\[(?:V|\\?|RD|A)\\]"', 'r"\\[(?:V|\\?|A)\\]"'),
    ("evidence: drops [?]", 'r"\\[(?:V|\\?|RD|A)\\]"', 'r"\\[(?:V|RD|A)\\]"'),
    ("evidence: drops [V]", 'r"\\[(?:V|\\?|RD|A)\\]"', 'r"\\[(?:\\?|RD|A)\\]"'),
    ("evidence: drops [A]", 'r"\\[(?:V|\\?|RD|A)\\]"', 'r"\\[(?:V|\\?|RD)\\]"'),
    ("evidence: any bracket token", 'r"\\[(?:V|\\?|RD|A)\\]"', 'r"\\[[A-Za-z?]{1,3}\\]"'),
    ("label: drops the named families",
     'r"\\b(?:CRIT|MAJOR|MINOR|HIGH|MED|LOW|IMP)-\\d+\\b"', 'r"\\bZZZZ\\b"'),
    ("label: drops the one-letter form",
     'r"|//[\\s*-]*[A-Z]-\\d{1,2}\\b|\\b[A-Z]-\\d{1,2}:"', 'r""'),
    ("label: one-letter form unanchored",
     'r"|//[\\s*-]*[A-Z]-\\d{1,2}\\b|\\b[A-Z]-\\d{1,2}:"', 'r"|\\b[A-Z]-\\d{1,2}\\b"'),
    # A DEAF explainer: it still prints each counter and its number, and names no line under it.
    # That is the failure the agreement arms exist for, so breaking either half must turn them red.
    ("--lines: names no source line",
     "        for no, line in comments + tails:\n", "        for no, line in []:\n"),
    ("--lines: names no doc link", "            for k in md_link_faults(line, base, tracked_set, subs):\n"
                                   "                out.append((no, k, line))\n",
     "            pass\n"),
    # The whole other.* class: 102 tracked files whose only proof was that nothing read them.
    ("other: reads no file at all", "    for p in other:\n", "    for p in []:\n"),
    # An ignore file's RULES are data. Dropping the carve-out makes --lines name `research/` and
    # `.claude/` as debt, and a sweep obeying it would un-ignore the private trees.
    ("other: ignore rules count as prose",
     '        if p.endswith(OTHER_COMMENTS_ONLY):\n', '        if False:\n'),
    # explain()'s third branch: blind it and --lines goes quiet for every non-md non-src file.
    ("--lines: names nothing outside md and src",
     '    prefix, fenced = ("md." if path.endswith(".md") else "other."), False\n',
     '    if not path.endswith(".md"):\n        return out\n'
     '    prefix, fenced = ("md." if path.endswith(".md") else "other."), False\n'),
    # A trailing /* ... */ is a comment too, and the tail must stop at its close.
    ("markers: blind to a trailing block comment",
     "                tails.append((no, s[i:e + 2]))\n", "                pass\n"),
    # The build tag. Reading only the named families is what the gate did while 452 lines across
    # 187 files cited a build; matching one digit as well would flag `v9` and a decimal.
    # The row citation. Dropping it is what the gate did while 18 lines named a survey nobody can
    # open; letting the number drift away from the word instead flags a ratio and an enumeration,
    # and a sweep obeying that would rewrite correct prose.
    ("doc_row: matches nothing", r'r"\brows?\s+[\w./-]*:\d+"', r'r"\bZZZZ\b"'),
    ("doc_row: singular only", r'r"\brows?\s+[\w./-]*:\d+"', r'r"\brow\s+[\w./-]*:\d+"'),
    ("doc_row: number may drift from the word", r'r"\brows?\s+[\w./-]*:\d+"', r'r"\brows?\b.*:\d+"'),
    ("doc_row: slug not optional", r'r"\brows?\s+[\w./-]*:\d+"', r'r"\brows?\s+[\w./-]+:\d+"'),
    # The version-marker half of the row citation, and its precision. Reading only the `row`
    # spelling is what the gate did while `net_pump's old :436-783` named a file version that has
    # not existed for months; requiring no marker at all instead flags a port, and a sweep obeying
    # that would delete the reason a migration exists.
    ("doc_row: version marker unread",
     'r"|\\b(?:old|former|previous|pre-cut|pre-extraction|pre-split)"', 'r"|\\bZZZZ"'),
    ("doc_row: marker not required",
     'r"|\\b(?:old|former|previous|pre-cut|pre-extraction|pre-split)"\n'
     '                            r"\\s+(?:[\\w\'-]+\\s+)?:\\d{2,5}"',
     'r"|:\\d{2,5}"'),
    # The three spellings of a document citation, and the two carve-outs that keep them off correct
    # prose. Each alternative is the only thing its fixture line trips, so a mutant that blinds one
    # lowers the count; each precision mutant raises it, which fails the same arm from the other
    # side.
    ("dead_docpath: bare filename unread",
     'r"(?<![\\w/.-])([A-Z][A-Z0-9]*(?:[_-][A-Z0-9]+)*\\.md)\\b"', 'r"ZZZZNOMATCH"'),
    ("dead_docpath: reads a research finding's tail as a document",
     'r"(?<![\\w/.-])([A-Z]', 'r"(?<![\\w/.])([A-Z]'),
    ("dead_docpath: sectioned name unread",
     'r"\\b([A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+)"', 'r"\\bZZZZNOMATCH([A-Z0-9]*)"'),
    ("dead_docpath: locator not required",
     'r"\\s+(?:section\\b|par\\.|paragraph\\b|Tier-|S\\d)"', 'r"\\b"'),
    ("dead_docpath: unnamed document unread", 'r"\\bdesign doc\\b", re.I', 'r"\\bZZZZ\\b", re.I'),
    # A citation resolves by BASENAME: `COOP_EVENT_JOIN.md` and `docs/COOP_EVENT_JOIN.md` name the
    # same file. Resolving by path alone flags every doc a comment names without its directory.
    ("dead_docpath: resolves by path only",
     "        self.names = set(tracked) | {t.rsplit(\"/\", 1)[-1] for t in tracked}",
     "        self.names = set(tracked)"),
    # The rotted line citation. It is judged by RESOLUTION, not by shape, so both halves of the
    # judgement need an arm: a file the tree does not carry, and a line past the end of one it
    # does. Accepting either would call a pointer live that leads nowhere.
    ("doc_row: an absent file resolves",
     "        if name not in self.lengths:\n            return False",
     "        if name not in self.lengths:\n            return True"),
    ("doc_row: any line number resolves",
     "        return first <= self.lengths[name]", "        return True"),
    ("doc_row: line citations unread",
     'SRC_LINE_CITE = re.compile(r"\\b([A-Za-z0-9_]+\\.(?:cpp|h|inc))\\s*:(\\d{1,5})")',
     'SRC_LINE_CITE = re.compile(r"ZZZZNOMATCH()()")'),
    # The dead-document predicate in the other.* family: it ran only over our C++ once, while 33
    # lines in ten tracked scripts and workflows named an absent doc with nothing reading them.
    ("other: dead documents unread",
     "            if doc_faults(line, docs):\n                c[\"other.dead_docpath\"] += 1",
     "            if False:\n                c[\"other.dead_docpath\"] += 1"),
    # The unstaged guard. It had no arm at all, and diffed two of the three measured families, so
    # a baseline could be written from an unstaged build file -- the very incident R-P12 names.
    ("guard: reads only part of what is measured",
     '    return [p for p in out.split("\\n") if p.strip() and measured(p)]',
     '    return [p for p in out.split("\\n") if p.strip() and measured_src(p)]'),
    ("label: drops the build tag", 'r"|(?<![\\w/.])v\\d{2,3}\\b"', 'r""'),
    ("label: build tag from one digit", 'r"|(?<![\\w/.])v\\d{2,3}\\b"', 'r"|v\\d+"'),
    # Both notations of the memory pointer. Reading only the path form is what the gate did while
    # 80 wiki links sat in source and the counter read 0; matching any bracket pair instead flags
    # an array index and a short tag, which would push a sweep to damage correct prose.
    # Recall: the bracket notation is half the habit, and dropping it took the counter to 0 while
    # 84 links sat in the tree.
    ("ptr_memory: path form only", 'r"|\[\[(?!(?:no_unique_address', 'r"|ZZZZNOMATCH(?!(?:no_unique_address'),
    # Precision, both ways. A slug is three or more words joined by - or _; matching any bracket
    # content instead flags a TOML array-of-tables header, and dropping the one attribute that has
    # that shape flags C++ code. A sweep obeying either would delete something real.
    ("ptr_memory: any bracket content",
     r"[A-Za-z0-9]+(?:[-_][A-Za-z0-9]+){2,}(?:[|#][^\]]*)?\]\]", r"[^\]|#]+\]\]"),
    ("ptr_memory: no attribute guard", "no_unique_address|carries_dependency",
     "ZZZZNOMATCH|carries_dependency"),
    # The markers must read the comment TRAILING a code line. Blinding them to it is what the
    # gate did until the tails were threaded through, and it hid 54 citations plus 98 pinned
    # offsets while calling their files swept.
    ("markers: blind to trailing comments",
     "        for _no, line in comments + tails:\n", "        for _no, line in comments:\n"),
]


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
    # A tracked doc one directory down, so the fixture can cite it by basename alone.
    with open(os.path.join(repo, "docs", "TRACKED.md"), "w", encoding="utf-8") as f:
        f.write("# tracked\n")
    with open(os.path.join(repo, "src", "votv-coop", "src", "x.cpp"), "w", encoding="utf-8") as f:
        f.write(SRC)
    with open(os.path.join(repo, "src", "votv-coop", "src", "clean.cpp"), "w", encoding="utf-8") as f:
        f.write(SRC_CLEAN)
    os.makedirs(os.path.join(repo, "tools"))
    with open(os.path.join(repo, "tools", "x.py"), "w", encoding="utf-8") as f:
        f.write(OTHER_PY)
    with open(os.path.join(repo, ".gitignore"), "w", encoding="utf-8") as f:
        f.write(OTHER_IGNORE)
    with open(os.path.join(repo, "Cargo.toml"), "w", encoding="utf-8") as f:
        f.write(OTHER_TOML)
    os.makedirs(os.path.join(repo, "src", "votv-coop", "include"))
    with open(os.path.join(repo, "src", "votv-coop", "include", "d.h"), "w", encoding="utf-8") as f:
        f.write(HDR)
    with open(os.path.join(repo, "src", "votv-coop", "include", "sdk_profile.h"), "w", encoding="utf-8") as f:
        f.write(HDR_OWNER)
    with open(os.path.join(repo, "src", "votv-coop", "include", "twin.h"), "w", encoding="utf-8") as f:
        f.write(HDR_TWIN)
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
    expect = {"md.files": 5, "md.over_600": 0, "md.cyrillic": 1, "md.user": 1, "md.verbatim": 1, "md.qf": 1, "md.agent": 1,
              "md.dated": 1, "md.ptr_memory": 2, "md.ptr_research": 1, "md.ptr_claude": 1,
              "md.ptr_security": 1, "md.dead_links": 2, "md.dead_paths": 1,
              "other.files": 3, "other.cyrillic": 1, "other.user": 1, "other.verbatim": 1,
              "other.qf": 1, "other.agent": 1, "other.dated": 2, "other.ptr_memory": 2,
              "other.ptr_research": 1, "other.ptr_claude": 1, "other.ptr_security": 1,
              "other.dead_docpath": 4,
              "src.comment_pinned_offset": 1, "src.dead_declarations": 3,
              "src.comment_lines": 68, "src.files": 5, "src.files_not_swept": 3,
              "src.comment_doc_row": 6,
              "src.comment_blocks_over_15": 2, "src.comment_dated": 2, "src.comment_user": 1, "src.comment_verbatim": 1,
              "src.comment_qf": 1, "src.comment_agent": 1, "src.comment_ptr_memory": 2, "src.comment_ptr_research": 1,
              "src.comment_ptr_claude": 1, "src.comment_ptr_security": 0, "src.comment_lesson": 1,
              "src.comment_sha": 1, "src.files_half_comment": 0, "src.comment_dead_docpath": 5,
              "src.comment_review": 3, "src.comment_evidence": 4,
              "src.comment_label": 12}
    for k, v in expect.items():
        arm("counts {} = {}".format(k, v), counters.get(k) == v, "got {}".format(counters.get(k)))
    # --lines must name every hit it reports a count for. A counter added to `measure` and not to
    # `explain` would print its number with no lines under it, and a sweep driven by that would
    # edit what it could see and call the file done. The fixture trips every source counter, so
    # agreement here is agreement across the whole table.
    r = run(["--repo", repo, "--baseline", baseline, "--lines",
             "--file", "src/votv-coop/src/x.cpp", "--file", "docs/a.md",
             "--file", "tools/x.py", "--file", ".gitignore"])
    # Keyed by (file, counter): `owed` was keyed by counter alone, so two fixture files owing
    # the same one overwrote the count while the named lines accumulated across both.
    owed, named, where, counter, path = {}, {}, {}, None, None
    for line in r.stdout.splitlines():
        if line and not line.startswith(" "):
            path, counter = line.split()[0], None
        elif line.startswith("    ") and not line.startswith("     "):
            head = line.split()
            if head[0] == "SWEPT":          # a fixture that owes nothing carries no count
                counter = None
                continue
            counter = (path, head[0])
            owed[counter] = int(head[1])
            named.setdefault(counter, 0)
            where.setdefault(counter, [])
        elif line.startswith("      ") and counter:
            # Only a real `path:NN` counts. The "(not line-addressable)" note is what a DEAF
            # explainer prints, so counting it would let this arm pass on exactly the failure
            # it exists to catch.
            if not line.strip().startswith("("):
                named[counter] += 1
                where[counter].append(line.strip().split()[0])
    arm("--lines reports the counters the file owes", bool(owed), r.stdout.strip()[:120])
    for k, n in sorted(owed.items()):
        arm("--lines names all {} hit(s) of {} in {}".format(n, k[1], k[0]), named.get(k) == n,
            "named {}".format(named.get(k)))
    # A count says the explainer is not silent; only the NUMBER says it points at the right line.
    # Turning the block-start list into a block-END list keeps every count and every arm above.
    pinned = {
        ("src/votv-coop/src/x.cpp", "src.comment_blocks_over_15"): ["src/votv-coop/src/x.cpp:2", "src/votv-coop/src/x.cpp:48"],
        (".gitignore", "other.dated"): [".gitignore:1"],
    }
    for k, want in pinned.items():
        arm("--lines points at {} for {}".format(want[0], k[1]), where.get(k) == want,
            "got {}".format(where.get(k)))
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
    # --relevel is the ONE operation allowed to raise a row, so it must re-copy every row from a
    # single measurement. Hand-editing the two rows expected to move is what left a third stale.
    stale = dict(after)
    stale["md.cyrillic"] = 9          # a row that must come DOWN to the tree
    stale["md.dated"] = 0             # a row that must go UP to the tree
    with open(baseline, "w", encoding="utf-8") as f:
        json.dump({"as_of": "drill", "counters": stale}, f)
    r = run(["--repo", repo, "--baseline", baseline, "--relevel"])
    with open(baseline, encoding="utf-8") as f:
        levelled = json.load(f)["counters"]
    arm("relevel re-copies every row, up and down",
        r.returncode == 0 and levelled["md.cyrillic"] == 0 and levelled["md.dated"] == after["md.dated"],
        "cyrillic {} dated {}".format(levelled["md.cyrillic"], levelled["md.dated"]))
    arm("relevel names the row that RISES", "RISES" in r.stdout and "md.dated" in r.stdout,
        r.stdout.strip().splitlines()[0] if r.stdout.strip() else "no output")
    arm("relevel leaves no row differing from the tree",
        all(levelled[k] == after[k] for k in after), "")
    with open(baseline, "w", encoding="utf-8") as f:
        json.dump({"as_of": "drill", "counters": after}, f)
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
    # A baseline describes a COMMIT, so a write refuses while ANY measured file is unstaged --
    # one arm per family, because the guard once read two of the three and a build file could be
    # modified-but-unstaged under a baseline written from its contents.
    for rel, family in (("src/votv-coop/src/x.cpp", "src"),
                        ("docs/a.md", "md"),
                        ("tools/x.py", "other")):
        with open(os.path.join(repo, *rel.split("/")), "a", encoding="utf-8") as f:
            f.write("\n" if family == "md" else "\n// probe\n" if family == "src" else "\n# probe\n")
        r = run(["--repo", repo, "--baseline", baseline, "--update"])
        arm("an unstaged {} file refuses a baseline write".format(family),
            r.returncode == 1 and "modified but not staged" in r.stdout and rel in r.stdout,
            r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "")
        git(["checkout", "--", rel], repo, env)
    # a 601-line doc crosses the cap; a 600-line one does not (counted above)
    with open(os.path.join(repo, "docs", "six.md"), "a", encoding="utf-8") as f:
        f.write("one more\n")
    git(["commit", "-q", "-am", "[drill] 601"], repo, env)
    r = run(["--repo", repo, "--baseline", baseline])
    arm("a 601-line doc crosses the 600-line cap", r.returncode == 1 and "md.over_600 0->1" in r.stdout)
    # The mutation arms run the drill again against a broken copy of the gate, so the nested run
    # must not mutate in turn: one level is the proof, two is a fork bomb.
    if os.environ.get("PPG_DRILL_NESTED"):
        bad = results.count(False)
        print("public_prose_gate_drill: {} arms, {} failed".format(len(results), bad))
        return 1 if bad else 0
    mut = os.path.join(tmp, "mutant")
    os.makedirs(mut, exist_ok=True)
    for f in ("public_prose_gate.py", "public_prose_gate_drill.py"):
        shutil.copy(os.path.join(HERE, f), os.path.join(mut, f))
    gate_src = io.open(os.path.join(mut, "public_prose_gate.py"), encoding="utf-8").read()
    for name, a, b in MUTANTS:
        if gate_src.count(a) != 1:
            arm("mutant anchor is unique: " + name, False, "found {}".format(gate_src.count(a)))
            continue
        with io.open(os.path.join(mut, "public_prose_gate.py"), "w", encoding="utf-8",
                     newline="") as f:
            f.write(gate_src.replace(a, b))
        r = subprocess.run([sys.executable, os.path.join(mut, "public_prose_gate_drill.py")],
                           capture_output=True, text=True, encoding="utf-8", errors="replace",
                           env=dict(os.environ, PPG_DRILL_NESTED="1"))
        arm("breaking " + name + " turns the drill RED", r.returncode != 0,
            (r.stdout.strip().splitlines() or ["no output"])[-1])
    bad = results.count(False)
    print("public_prose_gate_drill: {} arms, {} failed".format(len(results), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
