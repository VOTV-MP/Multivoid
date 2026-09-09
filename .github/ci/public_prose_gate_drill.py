#!/usr/bin/env python3
"""public_prose_gate_drill -- show .github/ci/public_prose_gate.py RED before trusting it green.

A throwaway repository with one doc and one source file carrying every class of marker; the
gate must count each, FAIL against a baseline one lower, PASS against an exact one, refuse to
ratchet up, and ratchet down after the marker is removed. A link to a tracked file must not
count as dead; a link to an untracked one must.

    python .github/ci/public_prose_gate_drill.py
"""
import glob
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GATE = os.path.join(HERE, "public_prose_gate.py")
from prose_drill_fixtures import (
    DOC, HDR, HDR_OWNER, HDR_TWIN, OTHER_IGNORE, OTHER_PY, OTHER_TOML, SRC, SRC_CLEAN)
from prose_drill_mutants import MUTANTS


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
    # A tracked doc one directory down, so the fixture can cite it by basename alone. Its capitals
    # also make it the tracked half of the naming rule: a public doc named UPPER_CASE.
    with open(os.path.join(repo, "docs", "TRACKED.md"), "w", encoding="utf-8") as f:
        f.write("# tracked\n")
    # The naming rule's precision canary: an ecosystem file keeps its capitals and must NOT count,
    # or the rule would make the repository stranger for the audience it serves.
    with open(os.path.join(repo, "docs", "CHANGELOG.md"), "w", encoding="utf-8") as f:
        f.write("# changelog\n")
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
    # A `.in` template, so the path axis has a name that only resolves when it is read in full.
    with open(os.path.join(repo, "src", "votv-coop", "include", "v.h.in"), "w", encoding="utf-8") as f:
        f.write('#define V "@VER@"\n')
    # The engine-mirror header: types named for the engine's own, which the member axis must not
    # adjudicate. Its path is what the guard matches, so it lives where the real one does.
    os.makedirs(os.path.join(repo, "src", "votv-coop", "include", "ue_wrap", "core"))
    with open(os.path.join(repo, "src", "votv-coop", "include", "ue_wrap", "core", "types.h"),
              "w", encoding="utf-8") as f:
        f.write("#pragma once\nstruct FMirror { float X; };\n")
    # A real nested repository, so `tracked` sees a gitlink and `vendored` has something to read.
    vend = os.path.join(repo, "third_party", "vend")
    os.makedirs(vend)
    git(["init", "-q", "-b", "main"], vend, env)
    git(["config", "core.hooksPath", os.devnull], vend, env)
    with open(os.path.join(vend, "vendored.cpp"), "w", encoding="utf-8") as f:
        f.write("int vendored() { return 0; }\n")
    git(["add", "."], vend, env)
    git(["commit", "-q", "-m", "vendored"], vend, env)
    git(["add", "."], repo, env)
    git(["commit", "-q", "-m", "[drill] seed"], repo, env)
    # The naming rule's UNTRACKED half, written AFTER the commit so git never learns them. A local
    # doc reads UPPER_CASE; the lowercase one is the fault, and the capitals one is the canary that
    # keeps the half from firing on everything it sees.
    with open(os.path.join(repo, "docs", "local_notes.md"), "w", encoding="utf-8") as f:
        f.write("# a local note named like a public doc\n")
    with open(os.path.join(repo, "docs", "LOCAL_ARC.md"), "w", encoding="utf-8") as f:
        f.write("# a local note named correctly\n")
    baseline = os.path.join(tmp, "baseline.json")
    results = []

    def arm(name, ok, detail=""):
        results.append(ok)
        print("  {} {}{}".format("PASS" if ok else "FAIL", name, (" -- " + detail) if detail else ""))

    r = run(["--repo", repo, "--baseline", baseline, "--init"])
    arm("init writes a baseline", r.returncode == 0 and os.path.isfile(baseline), r.stdout.strip())
    with open(baseline, encoding="utf-8") as f:
        counters = json.load(f)["counters"]
    expect = {"md.files": 6, "md.name_case": 3,
              "md.over_600": 0, "md.cyrillic": 1, "md.user": 1, "md.verbatim": 1, "md.qf": 1, "md.agent": 1,
              "md.dated": 1, "md.ptr_memory": 2, "md.ptr_research": 1, "md.ptr_claude": 1,
              "md.ptr_security": 1, "md.dead_links": 2, "md.dead_paths": 1,
              "other.files": 3, "other.cyrillic": 1, "other.user": 1, "other.verbatim": 1,
              "other.qf": 1, "other.agent": 1, "other.dated": 2, "other.ptr_memory": 2,
              "other.ptr_research": 1, "other.ptr_claude": 1, "other.ptr_security": 1,
              "other.dead_docpath": 4,
              "src.comment_pinned_offset": 1, "src.dead_declarations": 3,
              "src.comment_lines": 128, "src.files": 6, "src.files_not_swept": 3,
              "src.comment_doc_row": 6,
              "src.comment_blocks_over_15": 4, "src.comment_dated": 4, "src.comment_user": 16, "src.comment_verbatim": 1,
              "src.comment_qf": 1, "src.comment_agent": 1, "src.comment_ptr_memory": 2, "src.comment_ptr_research": 1,
              "src.comment_ptr_claude": 1, "src.comment_ptr_security": 0, "src.comment_lesson": 1,
              "src.comment_sha": 1, "src.files_half_comment": 0, "src.comment_dead_docpath": 5,
              "src.comment_review": 3, "src.comment_evidence": 4,
              "src.comment_label": 16,
              "src.string_marker": 11,
              "src.comment_dead_path": 1, "src.comment_dead_log_marker": 1,
              "src.comment_dead_env": 1, "src.comment_dead_ini_key": 2,
              "src.comment_dead_member": 2}
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
        ("src/votv-coop/src/x.cpp", "src.comment_blocks_over_15"): ["src/votv-coop/src/x.cpp:3", "src/votv-coop/src/x.cpp:20",
                                                                     "src/votv-coop/src/x.cpp:52",
                                                                     "src/votv-coop/src/x.cpp:77"],
        (".gitignore", "other.dated"): [".gitignore:2"],
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
    # THE CI SHAPE: a checkout with no submodule working copy. Every plain `actions/checkout` is
    # this, and a carve-out that reads a vendored tree has no input here -- it must decline to
    # accuse rather than call every vendored citation dead.
    r_with = run(["--repo", repo, "--baseline", baseline])
    shutil.move(os.path.join(repo, "third_party", "vend", ".git"),
                os.path.join(tmp, "vend_dotgit"))
    r_without = run(["--repo", repo, "--baseline", baseline])
    shutil.move(os.path.join(tmp, "vend_dotgit"),
                os.path.join(repo, "third_party", "vend", ".git"))
    arm("an unchecked-out submodule gives the same verdict as a checked-out one",
        r_with.returncode == r_without.returncode
        and "doc_row" not in r_without.stdout.split("FAIL")[-1],
        (r_without.stdout.strip().splitlines() or [""])[-1])
    # a 601-line doc crosses the cap; a 600-line one does not (counted above)
    with open(os.path.join(repo, "docs", "six.md"), "a", encoding="utf-8") as f:
        f.write("one more\n")
    git(["commit", "-q", "-am", "[drill] 601"], repo, env)
    r = run(["--repo", repo, "--baseline", baseline])
    arm("a 601-line doc crosses the 600-line cap", r.returncode == 1 and "md.over_600 0->1" in r.stdout)
    # --file is the sweep's own instrument, and it used to answer three different states with one
    # sentence: a path the tree does not have, a file this gate is FORBIDDEN to read, and a file it
    # read and found nothing in. Calling the second one swept tells a sweep it is finished with a
    # file nobody looked at, so each state gets an arm of its own.
    with open(os.path.join(repo, "LICENSE"), "w", encoding="utf-8") as f:
        f.write("a licence text, reproduced as-is\n")
    git(["add", "-A"], repo, env)
    git(["commit", "-q", "-m", "add an unmeasured tracked file"], repo, env)
    r = run(["--repo", repo, "--baseline", baseline, "--file", "LICENSE"])
    arm("--file: a tracked file the gate does not read says NOT MEASURED",
        r.returncode == 0 and "NOT MEASURED" in r.stdout and "SWEPT" not in r.stdout, r.stdout.strip())
    r = run(["--repo", repo, "--baseline", baseline, "--file", "notes.md"])
    arm("--file: a basename is anchored on a path segment, not a suffix",
        r.returncode == 0 and "no tracked file matches" in r.stdout
        and "local_notes.md" not in r.stdout, r.stdout.strip())
    r = run(["--repo", repo, "--baseline", baseline, "--file", ".nosuch/x.md"])
    arm("--file: an absent path is echoed with its leading dot intact",
        r.returncode == 0 and ".nosuch/x.md: no tracked file matches" in r.stdout, r.stdout.strip())
    r = run(["--repo", repo, "--baseline", baseline, "--file", "docs/a.md", "--lines"])
    arm("--file: a measured file still reports the counters it owes",
        r.returncode == 0 and "md.user" in r.stdout and "comment lines" not in r.stdout,
        (r.stdout.strip().splitlines() or [""])[0])
    # The mutation arms run the drill again against a broken copy of the gate, so the nested run
    # must not mutate in turn: one level is the proof, two is a fork bomb.
    if os.environ.get("PPG_DRILL_NESTED"):
        bad = results.count(False)
        print("public_prose_gate_drill: {} arms, {} failed".format(len(results), bad))
        return 1 if bad else 0
    mut = os.path.join(tmp, "mutant")
    os.makedirs(mut, exist_ok=True)
    # Two sets, and they are not the same one. COPIED is everything the nested run
    # needs: the gate family and this drill's own modules. SEARCHED is the gate
    # family alone -- prose_drill_mutants quotes every anchor, so searching it
    # would find a second copy of each one that carries no backslash to escape,
    # and this file is not part of the gate either.
    #
    # An anchor must be unique across the family's UNION, which is stricter than
    # uniqueness inside any one file and need not be told which sibling holds it.
    self_name = os.path.basename(__file__)
    sources = {}
    for pattern, searched in (("public_prose_*.py", True), ("prose_drill_*.py", False)):
        for path in sorted(glob.glob(os.path.join(HERE, pattern))):
            base = os.path.basename(path)
            shutil.copy(path, os.path.join(mut, base))
            if searched and base != self_name:
                sources[base] = io.open(path, encoding="utf-8").read()
    for name, a, b in MUTANTS:
        found = sum(text.count(a) for text in sources.values())
        if found != 1:
            arm("mutant anchor is unique: " + name, False,
                "found {} across {} files".format(found, len(sources)))
            continue
        holder = [f for f, text in sources.items() if a in text][0]
        # Each round rewrites every sibling from its pristine text, so one mutant
        # never stacks on the one before it.
        for base, text in sources.items():
            with io.open(os.path.join(mut, base), "w", encoding="utf-8", newline="") as f:
                f.write(text.replace(a, b) if base == holder else text)
        chk = subprocess.run(
            [sys.executable, "-c",
             # The mutant's own directory leads sys.path, so importing a sibling
             # reaches the mutated copy and never the pristine one beside it.
             "import importlib.util,os,sys\n"
             "p=sys.argv[1]; sys.path.insert(0, os.path.dirname(p))\n"
             "s=importlib.util.spec_from_file_location('g',p)\n"
             "m=importlib.util.module_from_spec(s); s.loader.exec_module(m)",
             os.path.join(mut, "public_prose_gate.py")],
            capture_output=True, text=True, encoding="utf-8", errors="replace")
        arm("mutant still compiles: " + name, chk.returncode == 0,
            (chk.stderr.strip().splitlines() or ["no output"])[-1])
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
