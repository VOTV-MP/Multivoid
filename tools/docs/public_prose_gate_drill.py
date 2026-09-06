#!/usr/bin/env python3
"""public_prose_gate_drill -- show tools/docs/public_prose_gate.py RED before trusting it green.

A throwaway repository with one doc and one source file carrying every class of marker; the
gate must count each, FAIL against a baseline one lower, PASS against an exact one, refuse to
ratchet up, and ratchet down after the marker is removed. A link to a tracked file must not
count as dead; a link to an untracked one must.

    python tools/docs/public_prose_gate_drill.py
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
// PRECISION: an array index a[[i]] and a short [[ab]] are not slugs
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
    # Both notations of the memory pointer. Reading only the path form is what the gate did while
    # 80 wiki links sat in source and the counter read 0; matching any bracket pair instead flags
    # an array index and a short tag, which would push a sweep to damage correct prose.
    ("ptr_memory: path form only", 'r"(?<![\w.])memory/|\[\[[a-z0-9][a-z0-9-]{3,}\]\]"',
     'r"(?<![\w.])memory/"'),
    ("ptr_memory: any bracket pair", 'r"(?<![\w.])memory/|\[\[[a-z0-9][a-z0-9-]{3,}\]\]"',
     'r"(?<![\w.])memory/|\[\[.+?\]\]"'),
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
    with open(os.path.join(repo, "src", "votv-coop", "src", "x.cpp"), "w", encoding="utf-8") as f:
        f.write(SRC)
    with open(os.path.join(repo, "src", "votv-coop", "src", "clean.cpp"), "w", encoding="utf-8") as f:
        f.write(SRC_CLEAN)
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
    expect = {"md.files": 4, "md.over_600": 0, "md.cyrillic": 1, "md.user": 1, "md.verbatim": 1, "md.qf": 1, "md.agent": 1,
              "md.dated": 1, "md.ptr_memory": 2, "md.ptr_research": 1, "md.ptr_claude": 1,
              "md.ptr_security": 1, "md.dead_links": 2, "md.dead_paths": 1,
              "src.comment_pinned_offset": 1, "src.dead_declarations": 3,
              "src.comment_lines": 51, "src.files": 5, "src.files_not_swept": 3,
              "src.comment_blocks_over_15": 1, "src.comment_dated": 2, "src.comment_user": 1, "src.comment_verbatim": 1,
              "src.comment_qf": 1, "src.comment_agent": 1, "src.comment_ptr_memory": 1, "src.comment_ptr_research": 1,
              "src.comment_ptr_claude": 1, "src.comment_ptr_security": 0, "src.comment_lesson": 1,
              "src.comment_sha": 1, "src.files_half_comment": 0, "src.comment_dead_docpath": 1,
              "src.comment_review": 3, "src.comment_evidence": 4,
              "src.comment_label": 10}
    for k, v in expect.items():
        arm("counts {} = {}".format(k, v), counters.get(k) == v, "got {}".format(counters.get(k)))
    # --lines must name every hit it reports a count for. A counter added to `measure` and not to
    # `explain` would print its number with no lines under it, and a sweep driven by that would
    # edit what it could see and call the file done. The fixture trips every source counter, so
    # agreement here is agreement across the whole table.
    r = run(["--repo", repo, "--baseline", baseline, "--lines",
             "--file", "src/votv-coop/src/x.cpp", "--file", "docs/a.md"])
    owed, named, counter = {}, {}, None
    for line in r.stdout.splitlines():
        if line.startswith("    ") and not line.startswith("     "):
            counter, n = line.split()[0], int(line.split()[1])
            owed[counter] = n
            named.setdefault(counter, 0)
        elif line.startswith("      ") and counter:
            # Only a real `path:NN` counts. The "(not line-addressable)" note is what a DEAF
            # explainer prints, so counting it would let this arm pass on exactly the failure
            # it exists to catch.
            if not line.strip().startswith("("):
                named[counter] += 1
    arm("--lines reports the counters the file owes", bool(owed), r.stdout.strip()[:120])
    for k, n in sorted(owed.items()):
        arm("--lines names all {} hit(s) of {}".format(n, k), named.get(k) == n,
            "named {}".format(named.get(k)))
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
