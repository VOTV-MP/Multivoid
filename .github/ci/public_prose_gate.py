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

    python .github/ci/public_prose_gate.py              # PASS/FAIL against the baseline
    python .github/ci/public_prose_gate.py --report     # every counter, baseline, and top files
    python .github/ci/public_prose_gate.py --update     # after a PASS: ratchet the baseline down
    python .github/ci/public_prose_gate.py --init       # first run: write the baseline as-is
"""
import argparse
import collections
import io
import json
import os
import re
import sys
from public_prose_faults import (
    DocIndex, comment_lines, doc_faults, md_link_faults, other_prose, src_comment_faults,
    string_marker_lines)
from public_prose_markers import (
    DECL, DECL_SKIP, HEX_LITERAL, INCLUDE, LINE_MARKERS, LONG_COMMENT_BLOCK, OFFSET_OWNERS,
    SRC_EXTRA)
from public_prose_scope import (
    BASELINE, DOC_ROOT, HALF_COMMENT_MIN_LINES, INFORMATIONAL, MD_HARD_CAP, OTHER_COMMENTS_ONLY,
    OTHER_HARD_CAP, OTHER_MARKER_OWNERS, REPO, SRC_EXT, SRC_ROOTS, code_only, doc_name_fault, git,
    measured, measured_md, measured_other, measured_src, names_path, other_file, read, tracked)


def measure(repo):
    """-> (counters dict, contributors dict: counter -> Counter(path -> hits), detail dict:
    counter -> path -> [note]). `detail` carries the counters whose unit is not a line, so
    `--lines` can name them without a second implementation of the analysis that found them."""
    files, subs = tracked(repo)
    tracked_set = set(files)
    docs = DocIndex(repo, files, subs)
    c = collections.OrderedDict()
    who = collections.defaultdict(collections.Counter)
    detail = collections.defaultdict(lambda: collections.defaultdict(list))
    md = [p for p in files if measured_md(p)]
    c["md.files"] = len(md)
    c["md.lines"] = 0
    c["md.over_%d" % MD_HARD_CAP] = 0
    for k in LINE_MARKERS:
        c["md." + k] = 0
    c["md.dead_links"] = 0
    c["md.dead_paths"] = 0
    # The naming convention, over BOTH halves of docs/: every tracked doc, and every .md sitting
    # in that tree untracked. Listing the directory is what lets the untracked half be seen at all,
    # since `tracked()` cannot report a file git does not know.
    c["md.name_case"] = 0
    seen_docs = [(p, True) for p in files if p.startswith(DOC_ROOT) and p.endswith(".md")]
    doc_dir = os.path.join(repo, DOC_ROOT.rstrip("/"))
    if os.path.isdir(doc_dir):
        for name in sorted(os.listdir(doc_dir)):
            rel = DOC_ROOT + name
            if name.endswith(".md") and rel not in tracked_set:
                seen_docs.append((rel, False))
    for p, is_tracked in seen_docs:
        why = doc_name_fault(p, is_tracked)
        if why:
            c["md.name_case"] += 1
            who["md.name_case"][p] += 1
            detail["md.name_case"][p].append(why)
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
            for k in md_link_faults(line, base, tracked_set, subs):
                c[k] += 1
                who[k][p] += 1
    # Every OTHER tracked text file: the build, the CI workflows, the ignore rules, the scripts a
    # contributor runs. They are as public as the docs and were the last class no counter read --
    # the ignore file alone carried 31 dated lines, a user quote and a paragraph that counted the
    # open rows in an unpublished security register. Whole lines are measured, not just comments:
    # in a config file the distinction does not hold, and a marker anywhere in one is the problem.
    # Length is measured over every other file, the marker owners included: a gate
    # that must name a refused word has no excuse for being 1,300 lines, and this
    # one was, unnoticed, for exactly as long as nothing counted it.
    c["other.over_%d" % OTHER_HARD_CAP] = 0
    for p in [q for q in files if other_file(q)]:
        text = read(repo, p)
        if text is None:
            continue
        n = len(text.splitlines())
        if n > OTHER_HARD_CAP:
            c["other.over_%d" % OTHER_HARD_CAP] += 1
            who["other.over_%d" % OTHER_HARD_CAP][p] = n

    other = [p for p in files if measured_other(p)]
    c["other.files"] = len(other)
    for k in LINE_MARKERS:
        c["other." + k] = 0
    c["other.dead_docpath"] = 0
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
            prose = other_prose(p, line)
            if prose and doc_faults(prose, docs):
                c["other.dead_docpath"] += 1
                who["other.dead_docpath"][p] += 1
    src = [p for p in files if measured_src(p)]
    c["src.comment_lines"] = 0
    code_total = 0
    for k in list(LINE_MARKERS) + list(SRC_EXTRA):
        c["src.comment_" + k] = 0
    c["src.files_half_comment"] = 0
    c["src.comment_blocks_over_%d" % LONG_COMMENT_BLOCK] = 0
    c["src.comment_dead_docpath"] = 0
    c["src.comment_pinned_offset"] = 0
    c["src.string_marker"] = 0
    for k in ("dead_path", "dead_log_marker", "dead_env", "dead_ini_key", "dead_member"):
        c["src.comment_" + k] = 0
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
        comments, code, long_blocks, tails = comment_lines(text)
        c["src.comment_lines"] += len(comments)
        code_total += code
        who["src.comment_lines"][p] = len(comments)
        if long_blocks:
            c["src.comment_blocks_over_%d" % LONG_COMMENT_BLOCK] += len(long_blocks)
            who["src.comment_blocks_over_%d" % LONG_COMMENT_BLOCK][p] = len(long_blocks)
        if code + len(comments) > HALF_COMMENT_MIN_LINES and len(comments) > code:
            c["src.files_half_comment"] += 1
            who["src.files_half_comment"][p] = len(comments)
        smark = string_marker_lines(text)
        if smark:
            c["src.string_marker"] += len(smark)
            who["src.string_marker"][p] = len(smark)
        owns_offsets = any(o in os.path.basename(p) for o in OFFSET_OWNERS)
        for _no, line in comments + tails:
            for k in src_comment_faults(line, docs, read_offsets, owns_offsets, p):
                if k in c:
                    c[k] += 1
                    who[k][p] += 1
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
                detail["src.dead_declarations"][header].append(name)
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
    return c, who, detail


def load_baseline(path):
    if not os.path.isfile(path):
        return None
    with io.open(path, encoding="utf-8") as f:
        return json.load(f)


def save_baseline(path, counters, repo):
    sha = git(["rev-parse", "--short", "HEAD"], repo).strip()
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"as_of": sha, "note": "measured on the working tree; as_of is HEAD at that moment",
                   "counters": counters}, f, indent=2)   # the shape the file is read in
        f.write("\n")


FIXED_DESCRIPTIONS = {
    "md.files": "tracked markdown files",
    "md.lines": "markdown lines",
    "md.over_%d" % MD_HARD_CAP: "docs over the %d-line hard cap" % MD_HARD_CAP,
    "other.over_%d" % OTHER_HARD_CAP:
        "scripts and CI files over the %d-line cap (the gates included)" % OTHER_HARD_CAP,
    "md.dead_links": "markdown links to a path not in the repository",
    "md.dead_paths": "backticked docs/tools/src paths that name no tracked file",
    "md.name_case": "docs named for the wrong half (public lowercase, local UPPER_CASE)",
    "src.comment_blocks_over_%d" % LONG_COMMENT_BLOCK: "comment blocks longer than %d lines" % LONG_COMMENT_BLOCK,
    "src.comment_lines": "comment lines in the mod's own C++",
    "src.comment_dead_docpath": "comment lines naming a document that is not in the repository",
    "src.comment_dead_path": "comment lines naming a repository path that resolves to nothing",
    "src.comment_dead_log_marker": "comment lines quoting a log line no format in the tree sends",
    "src.comment_dead_env": "comment lines naming an environment variable no code reads",
    "src.comment_dead_ini_key": "comment lines naming an ini row the registry does not carry",
    "src.comment_dead_member": "comment lines naming a member our own type does not have",
    "other.dead_docpath": "lines naming a document that is not in the repository",
    "src.comment_permille": "comment lines per 1000 lines of code+comment",
    "src.files_half_comment": "sources over %d lines that are more than half comment" % HALF_COMMENT_MIN_LINES,
    "src.files": "tracked sources in the mod's own C++",
    "src.files_not_swept": "sources still carrying at least one counter above",
    "src.comment_pinned_offset": "comment lines pinning an offset this file's own code never reads",
    "src.dead_declarations": "declared functions nothing in the tree calls",
    "src.string_marker": "string literals carrying a marker a comment may not carry",
}


def explain(repo, path, tracked_set, subs=()):
    """-> [(line number, counter, line text)] for the counters whose unit is a line.

    Every predicate here is the SAME object `measure` counts with -- the marker tables, the comment
    lexer, the doc-path and offset patterns -- so this names lines without becoming a second
    implementation that can drift from the gate. The counters whose unit is not a line
    (`dead_declarations`, `files_half_comment`, the block and burn-down counters) are not produced
    here; the caller reports them from `detail` and `who` instead of passing over them silently."""
    text = read(repo, path)
    if text is None:
        return []
    out = []
    # One index per CALL, never per line: it reads every tracked source to learn their lengths.
    docs = DocIndex(repo, sorted(tracked_set), subs)
    if path.startswith(SRC_ROOTS) and path.endswith(SRC_EXT):
        bare = code_only(text)
        read_offsets = {int(h, 16) for h in HEX_LITERAL.findall(bare)}
        owns_offsets = any(o in os.path.basename(path) for o in OFFSET_OWNERS)
        comments, _code, long_blocks, tails = comment_lines(text)
        for start in long_blocks:
            out.append((start, "src.comment_blocks_over_%d" % LONG_COMMENT_BLOCK,
                        "-- block starts here --"))
        for no, line in comments + tails:
            for k in src_comment_faults(line, docs, read_offsets, owns_offsets, path):
                out.append((no, k, line))
        for no, body in string_marker_lines(text):
            out.append((no, "src.string_marker", '"' + body + '"'))
        return sorted(out)
    prefix, fenced = ("md." if path.endswith(".md") else "other."), False
    comments_only = path.endswith(OTHER_COMMENTS_ONLY)
    base = os.path.dirname(path)
    for no, line in enumerate(text.splitlines(), 1):
        if prefix == "md.":
            if line.lstrip().startswith("```"):
                fenced = not fenced
                continue
            if fenced:
                continue
        elif comments_only and not line.lstrip().startswith("#"):
            continue
        for k, (rx, _) in LINE_MARKERS.items():
            if rx.search(line):
                out.append((no, prefix + k, line))
        if prefix == "other.":
            prose = other_prose(path, line)
            if prose and doc_faults(prose, docs):
                out.append((no, "other.dead_docpath", line))
        if prefix == "md.":
            for k in md_link_faults(line, base, tracked_set, subs):
                out.append((no, k, line))
    return sorted(out)


def describe(k):
    if k in FIXED_DESCRIPTIONS:
        return FIXED_DESCRIPTIONS[k]
    base = k.split(".", 1)[1].replace("comment_", "")
    if base in LINE_MARKERS:
        return ("comment " if k.startswith("src.") else "") + LINE_MARKERS[base][1]
    if base in SRC_EXTRA:
        return SRC_EXTRA[base][1]
    return k


def unstaged_measured(repo):
    """Measured files whose working-tree content differs from the index.

    A baseline describes a COMMIT: the gate re-runs at that commit and compares. Measuring the
    working tree and then committing only some of it writes numbers no commit ever has -- the
    baseline lands one commit before the change that made it true, and every commit in between
    fails a gate that has not regressed. So a write refuses while a measured file is unstaged.
    """
    out = git(["diff", "--name-only"], repo)
    return [p for p in out.split("\n") if p.strip() and measured(p)]


def unstaged_message(dirty):
    return ("public_prose_gate: FAIL -- {} measured file(s) are modified but not staged, so "
            "these numbers describe no commit: {}{} -- stage them first "
            "(--force writes anyway)".format(
                len(dirty), ", ".join(dirty[:6]), " ..." if len(dirty) > 6 else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--repo", default=REPO)
    ap.add_argument("--baseline", default=BASELINE)
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--update", action="store_true", help="after a PASS, ratchet the baseline down")
    ap.add_argument("--relevel", action="store_true",
                    help="re-level EVERY row to the tree after a detector widens; prints each "
                         "rise and fall. Use this instead of hand-editing the rows you expect to "
                         "move -- editing two of them left the rest describing an older tree.")
    ap.add_argument("--init", action="store_true", help="write the baseline from the current tree")
    ap.add_argument("--force", action="store_true", help="with --init: overwrite an existing baseline")
    ap.add_argument("--top", type=int, default=5)
    ap.add_argument("--file", action="append", metavar="PATH",
                    help="report what these files owe, counter by counter, and exit; repeatable")
    ap.add_argument("--lines", action="store_true",
                    help="with --file: name the offending line, so a sweep edits what the gate reads")
    a = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    counters, who, detail = measure(a.repo)
    if a.file:
        tracked_files, tracked_subs = tracked(a.repo)
        tracked_set = set(tracked_files)
        for want in a.file:
            # lstrip takes a CHARACTER SET, so lstrip("./") ate the leading dot of every .github
            # path and echoed it back that way. It cost no lookup -- endswith matched regardless
            # -- but the name printed back was not the name it was handed.
            want = want.replace("\\", "/")
            while want.startswith(("./", "../")):
                want = want.split("/", 1)[1]
            hits = sorted({p for k in who for p in who[k] if names_path(p, want)})
            if not hits:
                # THREE states shared one sentence here, and two of them were false. A file this
                # gate is not allowed to READ is not a file that is finished: OTHER_MARKER_OWNERS
                # holds the gates whose job is to name these markers, and calling one of them
                # swept tells a sweep it is done with a file nobody looked at.
                known = sorted(p for p in tracked_set if names_path(p, want))
                if not known:
                    print("{}: no tracked file matches".format(want))
                for p in known:
                    if not measured(p):
                        print("{}  NOT MEASURED -- this gate does not read this file".format(p))
                    elif p.startswith(OTHER_MARKER_OWNERS):
                        # A FOURTH state. Calling this file swept would say its markers had been
                        # looked at, and they never are; calling it unread would hide that its
                        # length is counted like everyone else's.
                        text = read(a.repo, p)
                        n = len(text.splitlines()) if text is not None else 0
                        print("{}  SIZE ONLY ({} lines, cap {}) -- naming these markers is this "
                              "file's job, so they are not counted; its length is"
                              .format(p, n, OTHER_HARD_CAP))
                    else:
                        print("{}  SWEPT -- read by this gate, contributes to no counter".format(p))
                continue
            for p in hits:
                owed = [(k, who[k][p]) for k in counters
                        if k not in INFORMATIONAL and k != "src.files_not_swept" and p in who[k]]
                # src.comment_lines is filled for the mod's own C++ only, and `who` is a
                # defaultdict -- so printing it unconditionally told every workflow and every
                # document it had 0 comment lines (build.yml has 20), and inserted the file into
                # the counter on the way past. Say it only where it was measured.
                if p in who["src.comment_lines"]:
                    print("{}  ({} comment lines)".format(p, who["src.comment_lines"][p]))
                else:
                    print(p)
                # Each counter's lines print UNDER its own heading -- a flat list of every
                # offending line in the file would not say which rule each one answers to, and
                # one line often answers to two. Anything owed that the explainer did not account
                # for says so: a sweep that silently saw fewer lines than the gate counts would
                # edit what it could see and call the file done.
                by_counter = collections.defaultdict(list)
                if a.lines:
                    for no, k, line in explain(a.repo, p, tracked_set, tracked_subs):
                        by_counter[k].append((no, line))
                for k, n in owed:
                    print("    {:<30} {:>4}  {}".format(k, n, describe(k)))
                    if not a.lines:
                        continue
                    for no, line in by_counter.get(k, []):
                        print("      {}:{}  {}".format(p, no, line.strip()))
                    for note in detail.get(k, {}).get(p, []):
                        print("      {}  <- declared here, called nowhere".format(note))
                    if not by_counter.get(k) and not detail.get(k, {}).get(p):
                        print("      (not line-addressable: {} hit(s) of {})".format(n, k))
                if not owed:
                    print("    SWEPT -- contributes to no counter")
        return 0
    if a.init:
        if os.path.isfile(a.baseline) and not a.force:
            print("public_prose_gate: FAIL -- a baseline exists at {}; --init --force overwrites it "
                  "(that can raise values silently, so say why in the commit)".format(a.baseline))
            return 1
        dirty = unstaged_measured(a.repo)
        if dirty and not a.force:
            print(unstaged_message(dirty))
            return 1
        save_baseline(a.baseline, counters, a.repo)
        print("public_prose_gate: baseline written ({} counters)".format(len(counters)))
        return 0
    if a.relevel:
        # A widened detector legitimately raises rows, and a ratchet may not grow, so the raise is
        # a deliberate act. Doing it BY HAND is what went wrong: two rows were edited, an
        # intervening sweep had lowered a third, and its stale value left the ratchet six lines
        # loose for one commit -- new debt that could have landed without failing anything. Every
        # row is re-copied from one measurement of one tree, and each move is printed.
        old = (load_baseline(a.baseline) or {}).get("counters", {})
        dirty = unstaged_measured(a.repo)
        if dirty and not a.force:
            print(unstaged_message(dirty))
            return 1
        moved = [(k, old[k], v) for k, v in counters.items()
                 if k in old and v != old[k] and k not in INFORMATIONAL]
        save_baseline(a.baseline, counters, a.repo)
        for k, b, v in sorted(moved):
            print("  {:<30} {:>6} -> {:>6}  {}".format(k, b, v, "RISES" if v > b else "falls"))
        print("public_prose_gate: re-levelled ({} rows moved, {} rows re-copied unchanged)".format(
            len(moved), len(counters) - len(moved)))
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
        dirty = unstaged_measured(a.repo)
        if dirty and not a.force:
            print(unstaged_message(dirty))
            return 1
        lowered = [k for k, v in counters.items() if k in bc and v < bc[k] and k not in INFORMATIONAL]
        save_baseline(a.baseline, counters, a.repo)
        print("public_prose_gate: PASS -- baseline updated ({} lowered: {})".format(
            len(lowered), ", ".join(lowered) or "none"))
        return 0
    print("public_prose_gate: PASS ({} counters at or below the baseline)".format(len(counters)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
