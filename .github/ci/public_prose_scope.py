#!/usr/bin/env python3
"""public_prose_scope -- which files the gate reads, and how it reads them.

The public tree is not one kind of text. Markdown, the mod's own C++, and the
tracked files that are neither (build, CI, ignore rules, scripts) each have their
own membership rule and their own exemptions, and a file that no rule claims is
not measured at all. This module holds those rules, the git queries that list the
tracked tree, and the readers that turn a path into the text a counter sees.

It knows nothing about what counts as a marker; that is public_prose_markers.
"""
import io
import os
import subprocess


HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
BASELINE = os.path.join(HERE, "public_prose_baseline.json")

MD_EXEMPT = ("THIRD-PARTY-NOTICES.md",)          # third-party license texts, reproduced as-is
# The FILENAME says which half a document is in: a tracked one is public and lowercase, an
# untracked one is ours and UPPER_CASE. The `.gitignore` allowlist already decides this, but it
# decides invisibly -- nothing in an `ls`, an editor tree or a grep result shows which half a file
# is in without asking git. The exception class is the files an ecosystem expects in capitals, and
# GitHub gives several of them special treatment, so lowercasing them would add the strangeness the
# convention exists to remove.
DOC_ROOT = "docs/"
DOC_NAME_EXEMPT = ("README", "LICENSE", "CONTRIBUTING", "SECURITY", "THIRD-PARTY-NOTICES",
                   "CHANGELOG")
SRC_ROOTS = ("src/votv-coop/src/", "src/votv-coop/include/")
SRC_EXT = (".cpp", ".h", ".inc")
MD_HARD_CAP = 600
# The tracked text files that are neither markdown nor our own C++: build, CI, ignore rules,
# scripts. Extension-gated so a binary is never read as prose; vendored trees are skipped whole,
# since a third-party file is not ours to rewrite.
OTHER_EXT = (".py", ".ps1", ".yml", ".yaml", ".json", ".toml", ".rs", ".cs", ".txt", ".tsv",
             ".rc", ".in", ".cmake", "CMakeLists.txt", ".gitignore", ".gitattributes")
OTHER_SKIP = ("src/votv-coop/third_party/", "reference/")
# The files whose JOB is these markers: a gate that refuses a word must name it, and a drill
# that proves the refusal must carry a fixture containing it. Counting those would push a sweep
# to break the very checks it is measured by -- the same trap the offset detector hit.
# This gate is a family of modules, not one file, so the first prefix names the
# family: `public_prose_` rather than `public_prose_gate`, so that a sibling is
# exempt for exactly the reason the entry point is, and nothing wider is.
OTHER_MARKER_OWNERS = (".github/ci/public_prose_", ".github/ci/prose_drill_",
                       ".github/ci/public_leak_gate", ".github/ci/public_leak_ack",
                       ".github/ci/commit_msg_check")
# Third-party licence texts are reproduced as-is, and the baseline is generated from the counters,
# so it names them by construction.
OTHER_EXEMPT = ("LICENSE", "THIRD-PARTY", "public_prose_baseline.json")
# In an ignore file the RULE is data and only the comment is prose: a rule cannot ignore a path
# without naming it, so `docs/AGENT_SPAWNING.md` must appear for the rule to work at all.
OTHER_COMMENTS_ONLY = (".gitignore", ".gitattributes")
HALF_COMMENT_MIN_LINES = 300
INFORMATIONAL = ("md.lines", "src.comment_lines", "src.comment_permille", "src.files",
                 "other.files")  # reported, never compared


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


def names_path(p, want):
    """Does tracked path `p` answer to `want`? Anchored on a path SEGMENT: a bare endswith let
    `notes.md` answer for `local_notes.md` and `gate.py` for every *gate.py, so a sweep reading
    the first block reported edited a file it had not asked about."""
    return p == want or p.endswith("/" + want)


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


def measured_md(path):
    return path.endswith(".md") and os.path.basename(path) not in MD_EXEMPT


def measured_src(path):
    return path.startswith(SRC_ROOTS) and path.endswith(SRC_EXT)


def doc_name_fault(path, is_tracked):
    """-> a reason string when a document under docs/ is named for the wrong half, else None.

    A tracked doc is public and reads lowercase; an untracked one is ours and reads UPPER_CASE.
    Only the STEM is judged, and only for the exception class's non-members. Note what this can
    see: in a fresh clone there are no untracked docs at all, so the second half of the rule fires
    only where the working trees coexist, which is the maintainer's disk."""
    if not path.startswith(DOC_ROOT) or not path.endswith(".md"):
        return None
    stem = os.path.basename(path)[: -len(".md")]
    if stem in DOC_NAME_EXEMPT:
        return None
    if is_tracked:
        return None if stem == stem.lower() else "a tracked doc is public: name it lowercase"
    return None if stem == stem.upper() else "an untracked doc is local: name it UPPER_CASE"


def measured_other(path):
    return (not path.endswith(".md") and path.endswith(OTHER_EXT)
            and not path.startswith(SRC_ROOTS) and not path.startswith(OTHER_SKIP)
            and not path.startswith(OTHER_MARKER_OWNERS)
            and not any(x in os.path.basename(path) for x in OTHER_EXEMPT))


def measured(path):
    """One definition of what this gate reads, used by `measure` AND by the unstaged guard.

    They were two definitions once: the guard diffed `SRC_ROOTS` and `*.md` only, so every file
    R-P8 brought in -- the build files, the workflows, the ignore rules, the scripts -- could be
    modified-but-unstaged while a baseline was written from their contents. That is the incident
    R-P12 exists to prevent, and it was open for a whole counter family."""
    return measured_md(path) or measured_src(path) or measured_other(path)
