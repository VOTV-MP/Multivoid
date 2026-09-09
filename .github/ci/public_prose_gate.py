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
import subprocess
import sys
from urllib.parse import unquote

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
OTHER_MARKER_OWNERS = (".github/ci/public_prose_gate", ".github/ci/public_leak_gate",
                       ".github/ci/public_leak_ack", ".github/ci/commit_msg_check")
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
# The DAY is optional: "2026-08:" opens a diary entry as surely as "2026-08-25" does, and
# four of them sat in one file while this counter read zero and its burn-down called the
# file swept. Nothing the gate reads matches the shorter form -- measured over every tracked
# doc, script and source. The MONTH is spelled out rather than left as two digits, or a plain
# number range reads as a date: "frames 2000-24 dropped" is not a diary entry. A leading word
# character keeps a hex literal or a version out of it either way, since \b cannot fire
# inside 0x2026 or v2026.
DATE = re.compile(r"\b20\d\d-(?:0[1-9]|1[0-2])(?:-\d\d)?\b")
LINK = re.compile(r"\]\(([^)\s#]+)(?:#[^)]*)?\)")
BACKTICK_PATH = re.compile(r"`((?:docs|tools|src)/[A-Za-z0-9_./-]+\.md)`")
# A document named in a source comment, in the three spellings the tree uses. All resolve the
# same way -- by BASENAME against the tracked set -- because a comment cites `COOP_EVENT_JOIN.md`
# as readily as `docs/COOP_EVENT_JOIN.md`, and both point at the same absent file.
DOC_PATH = re.compile(r"\bdocs/[A-Za-z0-9_./-]+?\.md\b")
# The bare filename, with no directory. The leading `-` in the lookbehind is what keeps this off
# the TAIL of a research finding's name: `votv-x-DESIGN-2026-08-21.md` ends in a token this would
# otherwise read as a document of its own, and that pointer belongs to `ptr_research`.
DOC_BARE = re.compile(r"(?<![\w/.-])([A-Z][A-Z0-9]*(?:[_-][A-Z0-9]+)*\.md)\b")
# The same name with the extension dropped and a locator in its place -- `DEATH_ARC section 3.1`,
# `COOP_SYNCER_MODEL par.2b`. The locator is required: without it the shape is indistinguishable
# from an environment variable, and `COOP_MASTER_PORT` / `COOP_TURN_SECRET` are configuration a
# contributor needs, not prose to sweep away.
DOC_SECTIONED = re.compile(r"\b([A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+)"
                           r"\s+(?:section\b|par\.|paragraph\b|Tier-|S\d)")
# A document with no name at all, which no lookup can resolve for the reader either.
DOC_UNNAMED = re.compile(r"\bdesign doc\b", re.I)
# A citation of a LINE of one of our own sources. This one is judged by resolution rather than by
# shape, because the shape is identical whether it works or not: `session_streams.cpp :198` is a
# pointer a reader can follow and `pe_detour.cpp:645` is not, in a file of 601 lines. Counted only
# when the file is untracked or the line is past its end.
SRC_LINE_CITE = re.compile(r"\b([A-Za-z0-9_]+\.(?:cpp|h|inc))\s*:(\d{1,5})")
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
ANY_INCLUDE = re.compile(r'#\s*include\s+[<"]([^>"]+)[>"]')

# ------------------------------------------------------------ the five citation axes
# A comment that CITES something checkable -- a repository path, a log line, an environment
# variable, an ini row, a member of a type -- makes a claim the tree itself can answer, and one
# nothing ever compiles against. Two of these had no counter at all until the sweep that added
# them, and both were where the rot sat: a renamed method or a reworded log line leaves its
# citation standing with nothing to notice.
MODULE_ROOT = "src/votv-coop/"
WORD = re.compile(r"[A-Za-z_]\w*")
CITED_PATH = re.compile(
    r"(?<![\w./\\-])((?:[\w.+-]+/)+[\w.+-]+"
    r"\.(?:h|hpp|cpp|c|inc|py|ps1|rs|md|txt|json|ya?ml|bat|toml|lua|rc|cmake|tsv|ini|csv|in))"
    r"(?![\w]|\.[A-Za-z])")
# A bare directory, counted only when its first segment is a top-level name of this repository or
# one of the working trees kept beside it. Without that test the pattern reads prose alternation
# -- `fridge/safe/microwave`, `NaN/Inf` -- as a path.
CITED_DIR = re.compile(r"(?<![\w./\\-])((?:[\w.+-]+/){1,6})(?![\w/])")
LOCAL_TREES = ("tools", "research", "memory", "site")
# Trees this repository has never contained: the game's cooked content, UE's own source, the SDK
# header dump, the install layout. A path into one of them is a citation of somewhere else, and
# the tree cannot say whether it resolves either way.
FOREIGN_TREE = ("Content/", "Runtime/", "CXXHeaderDump/", "Engine/", "Mods/", "Saved/",
                "Binaries/", "Config/", "Plugins/")
LOG_CALL = re.compile(r"\bUE_LOG[IWE]\s*\(\s*((?:\"(?:[^\"\\]|\\.)*\"\s*)+)")
LOG_LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')
LOG_SPEC = re.compile(r"%[-+ #0-9.*]*(?:ll|l|h|z|w)?[A-Za-z]")
CITED_MARKER = re.compile(r"`([^`\n]{4,70})`|\"([^\"\n]{6,70})\"")
# What a quoted string must look like before it is read as a LOG marker at all: a bracketed tag in
# capitals, or this tree's `subsystem: lowercase words` line prefix.
MARKER_SHAPE = re.compile(r"^\[[A-Z][A-Z0-9_-]{2,}\]|^[a-z_]+(?:\.[a-z_]+)?: [a-z]")
# What a comment writes where the format writes a %spec: a rendered value, a quoted name, a hex
# word, or the author's own `...` elision.
RENDERED = re.compile(r"\.\.\.|'[^']*'|\b0x[0-9A-Fa-f]+\b|\b\d+\b")
ENV_NAME = re.compile(r"VOTVCOOP_[A-Z0-9_]+")
CITED_INI = re.compile(r"`([a-z][a-z0-9_]*\.[a-z][a-z0-9_.]*)\s*=(?!=)")
REGISTRY_ROW = re.compile(r'CFG_[A-Z_]+\(\s*\w+\s*,\s*"([^"]+)"')
CITED_MEMBER = re.compile(r"`?\b(?:\w+::)*([A-Za-z_]\w*)::(\w+)\b`?")
# Only a type this tree DEFINES. A citation of an engine, DirectX or blueprint member names a
# tree that is not this one -- `IDXGISwapChain::Present` is real and appears in no source here --
# and the repository cannot adjudicate it in either direction.
MIRROR_HEADER = "ue_wrap/core/types.h"
# A BODY, not a forward declaration: `struct IDXGISwapChain;` says only that the name exists,
# and taking it for a definition made this counter answer for DirectX, whose methods are real and
# appear in no source here.
TYPE_DEF = re.compile(
    r"\b(?:class|struct)\s+(?:__declspec\([^)]*\)\s*)?([A-Z]\w+)\s*(?:final\s*)?(?::[^;{]*)?\{")
DECL_SKIP = {"if", "for", "while", "return", "switch", "sizeof", "static_cast", "reinterpret_cast",
             "const_cast", "dynamic_cast", "assert", "catch"}

# name -> (regex, what it counts). Each is applied per LINE of markdown / per comment line.
LINE_MARKERS = collections.OrderedDict([
    ("cyrillic",     (CYRILLIC, "lines with Cyrillic")),
    # The same attribution in lower case. `\bUSER\b` reads 3 lines of source, 3 of the build and CI
    # files and 1 markdown line; "per the user", `user: "..."`, "the user-requested X", "a user
    # report" and "user 2026-07-08" add 21 source lines and 4 build/CI lines on the tree this was
    # measured against. One habit, one counter.
    # The attribution has to be EXPLICIT, and the capitals alternative stays case-SENSITIVE: this
    # tree writes about the person playing the game in the same word, so "the user types into a
    # user widget as user 0" and "a per-user setting" are correct prose, and a detector that
    # flagged them would push a sweep to damage them.
    # The noun group is the ten words that actually occur, with their inflections -- a group that
    # claims more than the drill tests is a group whose mutant proves nothing. THE GAP, stated
    # rather than closed (R-P11): "the user wants/wanted/picked/chose", "the user's call/premise/
    # verdict", a word between the possessive and the noun ("the user's key ask"), a noun that
    # wraps onto the next comment line, and `user-mandated` / `user-retest` are unread.
    ("user",         (re.compile(r"\bUSER\b"
                                 r"|(?i:\bper (?:the )?user\b)"
                                 r"|(?i:\buser'?s?\s*:\s*\S)"
                                 r"|(?i:\buser-request(?:ed)?\b)"
                                 r"|(?i:\buser'?s?\s+(?:ask(?:s|ed)?"
                                 r"|report(?:s|ed)?"
                                 r"|req(?:s|uest(?:s|ed)?)?"
                                 r"|retest(?:s|ed)?"
                                 r"|say(?:s)?|said"
                                 r"|choice|decision|rule)\b)"
                                 r"|(?i:\buser \d{4}-\d{2}-\d{2})"),
                     "lines attributing a decision to the user")),
    ("verbatim",     (re.compile(r"\bverbatim\b", re.I), "lines saying verbatim")),
    ("qf",           (re.compile(r"(?<![\w/])/qf\b|\bqf\b(?!\.)"), "lines naming the /qf ritual")),
    ("agent",        (re.compile(r"\b(?:sub)?agents?\b", re.I), "lines naming an agent")),
    ("dated",        (DATE, "lines carrying a date")),
    # Both notations for the same pointer. `memory/x.md` is the path; `[[x]]` is the wiki link the
    # memory files use among themselves, and 84 of them sat in public source naming 49 slugs, none
    # of which is or will be a tracked file. One habit, one counter.
    #
    # The bracket form is the fussy one, because `[[...]]` is also C++ attribute syntax and TOML
    # array-of-table syntax. A slug is THREE OR MORE words joined by `-` or `_` (either separator:
    # the files are named with underscores and cited with hyphens), optionally followed by `|alias`
    # or `#anchor`. That shape excludes every TOML header (`[[bin]]`, `[[test]]`) and all but the
    # three-word attributes, which are named outright -- `[[no_unique_address]]` is otherwise
    # indistinguishable from a slug, and flagging an attribute would push a sweep to damage code.
    ("ptr_memory",   (re.compile(r"(?<![\w.])memory/"
                                 r"|\[\[(?!(?:no_unique_address|carries_dependency|maybe_unused"
                                 r"|nodiscard|fallthrough|noreturn|deprecated|likely|unlikely)"
                                 r"[\]|#])[A-Za-z0-9]+(?:[-_][A-Za-z0-9]+){2,}(?:[|#][^\]]*)?\]\]"),
                      "pointers into the memory directory")),
    ("ptr_research", (re.compile(r"(?<![\w.])research/"), "pointers into research/")),
    ("ptr_claude",   (re.compile(r"\bCLAUDE\.md\b|\.claude/"), "pointers to CLAUDE.md or .claude/")),
    ("ptr_security", (re.compile(r"\bdocs/security/"), "pointers into docs/security/")),
])
SRC_EXTRA = collections.OrderedDict([
    ("lesson", (re.compile(r"\bLESSONS\b|\blessons?\b", re.I), "comment lines citing a lesson")),
    ("sha",    (re.compile(r"`[0-9a-f]{8,10}`|\bcommit [0-9a-f]{7,10}\b"), "comment lines citing a commit")),
    # "audit F-3", "audit MINOR-6", "the audit's third category": a review of ours, named in a
    # comment. A maintainer cannot look any of them up -- the reviews are not in the tree and
    # never will be -- so the citation carries no information and the finding it stands for has
    # to be restated as what the code does. The word IS the vocabulary, as with `lesson`.
    ("review", (re.compile(r"\baudit(?:s|ed|ing)?\b", re.I), "comment lines citing an internal review")),
    # `[V]` / `[?]` / `[RD]` / `[A]`: the evidence tags of our own working docs, which carry a
    # legend on the docs index. Source has no legend, so in a comment the tag is noise around
    # the fact.
    ("evidence", (re.compile(r"\[(?:V|\?|RD|A)\]"), "comment lines carrying an evidence tag")),
    # The same citation without the word: a work item, a review finding or a register row named
    # by its label. `CRIT-1`, `security A34`, `Inc-2`, `Inc3`, `Increment 2b`, `take-9`, `WP-2`,
    # `s28 cut`, `K-5`, `R-2`
    # all name a document outside the tree, and the security register is deliberately
    # unpublished, so those rows name something a reader is not meant to have. The named
    # families are exact; the one-letter form counts only where it OPENS the comment or carries
    # a colon, which is how a label is written and how arithmetic is not: `leave N-1 host
    # props`, `X -> X-93` and `1->(P-2)` all read as prose and none of them counts.
    # `v133`, `v85`, `v42` -- the BUILD a change landed in, which is the same citation again: join
    # compatibility is byte-equality on the version pair, so there is no "from build N onward"
    # semantics for a comment to be stating, only a diary entry. Two to three digits, and not after
    # a word character, a slash or a dot, so `IPv4`, a path and a decimal are left alone.
    ("label", (re.compile(r"\b(?:CRIT|MAJOR|MINOR|HIGH|MED|LOW|IMP)-\d+\b"
                          r"|\bsecurity\s+[A-Z]\d+\b|\bA\d\d/A\d\d\b"
                          r"|(?i:\binc(?:rement)?[-\s]?\d+[a-z]?\b)"
                          r"|\btake-\d+\b|\bWP-?\d+\b|\bs\d\d cut\b|\bfinding \d+\b"
                          r"|(?<![\w/.])v\d{2,3}\b"
                          r"|//[\s*-]*[A-Z]-\d{1,2}\b|\b[A-Z]-\d{1,2}:"),
               "comment lines citing a work item by its label")),
    # The third spelling of the same citation: a ROW of a table that lives in a document outside
    # the tree. `islive-zeroav row :79`, `census row kerfur_command:138`, `census rows
    # engine_mainplayer:192/:196` -- each names a line of a survey a maintainer has no way to
    # open, and ten of the eighteen files carrying one were reported swept, because neither
    # `review` nor `label` reads this shape. The row number must follow the word directly, with
    # at most a slug between, so `the row is a MIRROR -> a 1:1` and other prose stay clear.
    # The second half of the same shape is a line of a FILE VERSION that no longer exists, which
    # a reader can resolve even less than a row of an absent document: `net_pump's old :436-783`,
    # `the pre-cut :869 gate`, `the pre-extraction :404 correlation`. A version marker has to open
    # it, because the bare ` :NNN` notation is also how a port is written -- `a deliberate :7777`,
    # `never the default :80/:443` are correct prose and a sweep obeying a detector that flagged
    # them would delete the reason a migration exists. A MARKED port -- "the old :80 default" --
    # would still be flagged, since a marker plus a bare number is one shape whether the number is
    # a line or a port; there is none in this tree, and the answer if one appears is to write the
    # port without the marker. The `<file>.ext:NNN` form needs no such heuristic, because it is
    # judged by whether it RESOLVES.
    ("doc_row", (re.compile(r"\brows?\s+[\w./-]*:\d+"
                            r"|\b(?:old|former|previous|pre-cut|pre-extraction|pre-split)"
                            r"\s+(?:[\w'-]+\s+)?:\d{2,5}"),
                 "comment lines citing a row, or a line, a reader cannot open")),
])

# The marker families a STRING LITERAL may not carry either. The comment counters read comments,
# and a literal in this tree is often the more public surface of the two: the config registry's
# descriptions render in the mod's own settings panel, and a log line is read by anyone who opens
# a log or pastes one into a bug report. 27 of them carried a build tag, a security-register row,
# an increment number, a date or an attribution while every counter read zero -- one habit, one
# POSITION over.
#
# FOUR families are exempt, because in a literal they are ordinary product English rather than
# our vocabulary, and this counter is ratcheted at zero -- so flagging one would refuse a push
# over correct code. `review` is the word "audit" ("the audit of slot %d failed", an include path
# with `audit` in it); `agent` is the "User-Agent" header and a muted voice agent; `lesson` is the
# word itself. None of the three fired on the 27 this axis was built from. `cyrillic` is exempt
# because the codec and fold selftests MUST hold Cyrillic to test what they test, and flagging
# those would push a sweep to delete its own fixtures.
#
# THE GAP, stated rather than closed: a Cyrillic string that is prose rather than a fixture is
# unread, and so is our vocabulary spelled in one of those four words inside a literal.
STRING_EXEMPT = ("cyrillic", "review", "agent", "lesson")
STRING_MARKERS = [(k, rx) for k, (rx, _) in
                  list(LINE_MARKERS.items()) + list(SRC_EXTRA.items()) if k not in STRING_EXEMPT]


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


class DocIndex:
    """What a citation resolves against: tracked paths, tracked basenames, and how long each
    source is, since a line citation past a file's end resolves no better than an absent file."""

    def __init__(self, repo, tracked, subs=()):
        self.names = set(tracked) | {t.rsplit("/", 1)[-1] for t in tracked}
        self.repo, self.subs, self._vendored = repo, list(subs), None
        self._subs_readable = 0
        self.lengths = {}
        self.paths = set(tracked)
        self.dirs = set()
        for t in tracked:
            parts = t.split("/")
            for i in range(1, len(parts)):
                self.dirs.add("/".join(parts[:i]))
        self.tops = {t.split("/")[0] for t in tracked} | set(LOCAL_TREES)
        self.includes = {}
        self.log_formats, self.envs, self.ini_keys = [], set(), set()
        self.our_types, self.code_words = set(), set()
        for t in tracked:
            if not t.endswith(SRC_EXT):
                continue
            text = read(repo, t)
            if text is None:
                continue
            base = t.rsplit("/", 1)[-1]
            self.lengths[base] = max(self.lengths.get(base, 0), text.count("\n") + 1)
            # The citation axes read OUR sources only: a vendored library's log lines and types
            # are not what our comments cite, and indexing them would answer for a tree we do not
            # write.
            if not measured_src(t):
                continue
            bare = code_only(text)
            self.includes[t] = set(ANY_INCLUDE.findall(bare))
            for m in LOG_CALL.finditer(bare):
                self.log_formats.append("".join(LOG_LITERAL.findall(m.group(1))))
            self.envs |= set(ENV_NAME.findall(bare))
            self.ini_keys |= set(REGISTRY_ROW.findall(bare))
            if not t.endswith(MIRROR_HEADER):
                self.our_types |= set(TYPE_DEF.findall(bare))
            self.code_words |= set(WORD.findall(bare))
        # The build file names env twins in a spelling no C++ line carries.
        cmake = read(repo, MODULE_ROOT + "CMakeLists.txt")
        if cmake:
            self.envs |= set(ENV_NAME.findall(cmake))
        self.log_skeletons = [LOG_SPEC.sub("\x01", f) for f in self.log_formats]
        self.ini_sections = {k.split(".")[0] for k in self.ini_keys}

    def resolves(self, ref):
        return ref in self.names or ref.rsplit("/", 1)[-1] in self.names

    def vendored(self):
        """Every basename in the checked-out submodules, read once and only if asked.

        A submodule is ONE gitlink entry in `git ls-files`, so its contents are invisible to the
        tracked set even though they sit on disk beside us. A citation into one resolves for a
        reader, and it cannot rot the way ours does, because the submodule is pinned by SHA."""
        if self._vendored is None:
            self._vendored = set()
            for sub_path in self.subs:
                here = os.path.join(self.repo, sub_path)
                # A checked-out submodule has a `.git` of its own. Without that test an EMPTY
                # placeholder directory answers `ls-files` with the PARENT repository's list, exit
                # 0, and this loop believes it read a submodule it never opened.
                if not os.path.exists(os.path.join(here, ".git")):
                    continue                      # not checked out; see vendored_known()
                try:
                    out = git(["ls-files"], here)
                except Exception:
                    continue
                self._subs_readable += 1
                self._vendored |= {p.rsplit("/", 1)[-1] for p in out.split("\n") if p.strip()}
        return self._vendored

    def vendored_known(self):
        """False when a declared submodule is not on disk, which is every plain CI checkout.

        The carve-out below then has no input, and an empty set is not the same answer as "this
        name is not vendored" -- reading it as one turned every GNS, imgui and MTA citation into
        debt, 0 locally and 13 in CI. A counter that cannot tell must not accuse."""
        self.vendored()
        return self._subs_readable == len(self.subs)

    def path_resolves(self, cand, owner):
        """Can a reader open `cand`, written in a comment inside `owner`?

        Every spelling this tree legitimately uses: a path from the repository root, one relative
        to the citing file, and the MODULE-ROOT form (`src/ue_wrap/x.cpp`, relative to the CMake
        project root) that hundreds of citations here use and none spell out in full. A path
        inside a submodule resolves on its PREFIX, without reading the submodule, so this answers
        the same in a plain CI checkout that has none."""
        c = cand.rstrip("/")
        if c in self.paths or c in self.dirs:
            return True
        if c in self.includes.get(owner, ()):
            return True
        if any(c.startswith(s + "/") for s in self.subs):
            return True
        here = owner.rsplit("/", 1)[0] if "/" in owner else ""
        rel = os.path.normpath(os.path.join(here, c)).replace(os.sep, "/")
        if rel in self.paths or rel in self.dirs:
            return True
        for base in (MODULE_ROOT, MODULE_ROOT + "include/", MODULE_ROOT + "src/"):
            if base + c in self.paths or base + c in self.dirs:
                return True
            if any((base + c).startswith(s + "/") for s in self.subs):
                return True
        return False

    def log_marker_resolves(self, marker):
        """Is `marker` a RENDERED instance of some UE_LOG format in the tree?

        Compared in the one direction that works. The comment writes what a reader will SEE
        (`active=1`, `'starRain'`) and the format writes `%d` and `%s`, so each side's
        substitutions become wildcards and the marker's remaining literal runs must appear in ONE
        format, in order. Matching the marker's text against the format's -- the obvious way
        round, and the way this was first written -- calls every rendered value a stale
        citation."""
        toks = [t for t in RENDERED.split(marker) if t.strip()]
        if not toks:
            return True
        for fs in self.log_skeletons:
            pos, ok = 0, True
            for t in toks:
                i = fs.find(t, pos)
                if i < 0:
                    ok = False
                    break
                pos = i + len(t)
            if ok:
                return True
        return False

    def line_resolves(self, name, first):
        """A `<file>:NNN` citation resolves when the tree carries that file AND it is that long.
        Both halves matter: some of the tree's citations name a file that is gone, and others name
        a line a shortened file no longer has. A vendored file resolves on its name alone."""
        if name not in self.lengths:
            return name in self.vendored() or not self.vendored_known()
        return first <= self.lengths[name]


# How prose opens in each of the other.* file kinds. A citation is prose, so it is read from a
# comment; a `.md` path in code is a file the script opens.
OTHER_COMMENT_LEAD = {".py": "#", ".ps1": "#", ".yml": "#", ".yaml": "#", ".toml": "#",
                      ".cmake": "#", ".txt": "#", "CMakeLists.txt": "#", ".gitignore": "#",
                      ".gitattributes": "#", ".rs": "//", ".cs": "//"}


def other_prose(path, line):
    """-> the comment part of one line of a non-md, non-source file, or None when it has none.

    An ignore file is not read at all here: its comments exist to say why a path is NOT tracked,
    so counting them for naming an absent document measures the file's whole purpose.

    THE GAP this leaves, stated rather than closed: a citation inside a STRING a script PRINTS is
    a pointer a contributor follows, and it is not a comment. Two of those shipped here -- an
    `abi_gate` failure message naming a document that is not in the tree -- and they were found by
    reading, not by counting. Telling a printed message from a path a script opens needs more than
    a pattern, so this counter does not try."""
    if path.endswith(OTHER_COMMENTS_ONLY):
        return None
    lead = next((v for k, v in OTHER_COMMENT_LEAD.items() if path.endswith(k)), None)
    if lead is None:
        return None
    i = line.find(lead)
    return line[i:] if i >= 0 else None


def doc_faults(line, docs):
    """True when one line cites a document a reader cannot open, in any of its four spellings.

    One implementation, so the same habit is counted the same wherever it sits -- our C++
    comments, a build file's comment, a workflow's. It was the source counter's alone once, and
    33 lines in ten tracked scripts and workflows named an absent document with nothing reading
    them, two of those in a `print()` a contributor sees at runtime."""
    cited = (DOC_PATH.findall(line) + DOC_BARE.findall(line)
             + [m + ".md" for m in DOC_SECTIONED.findall(line)])
    return any(not docs.resolves(m) for m in cited) or bool(DOC_UNNAMED.search(line))


def foreign(cand):
    """A tree this repository has never held. Matched anywhere in the path, not only at its
    start: the game's own install directory carries `Content/` several segments in."""
    return any(cand.startswith(f) or ("/" + f) in cand for f in FOREIGN_TREE)


def src_comment_faults(line, docs, read_offsets, owns_offsets, path=""):
    """-> the counter key for every rule one line of source COMMENT breaks.

    One implementation, called by the counting pass and by the line explainer, for the same reason
    `md_link_faults` is: the two disagreeing is the failure `--lines` exists to make impossible.
    A key the caller does not count is still returned here -- `measure` filters, so retiring a
    counter cannot leave the explainer naming lines for a number nobody prints.

    `docs` is the DocIndex: the doc rules resolve by basename, and a line citation by length."""
    faults = ["src.comment_" + k for k, (rx, _) in
              list(LINE_MARKERS.items()) + list(SRC_EXTRA.items()) if rx.search(line)]
    if doc_faults(line, docs):
        faults.append("src.comment_dead_docpath")
    if any(not docs.line_resolves(n, int(k)) for n, k in SRC_LINE_CITE.findall(line)):
        faults.append("src.comment_doc_row")
    if not owns_offsets and {int(h, 16) for h in RAW_OFFSET.findall(line)} - read_offsets:
        faults.append("src.comment_pinned_offset")
    # Documents belong to `dead_docpath`, which resolves them by BASENAME -- the right rule for a
    # doc and the wrong one for a source path. Counting a `.md` here too would put one habit under
    # two numbers and let a sweep clear one of them.
    cited = [m.group(1) for m in CITED_PATH.finditer(line) if not m.group(1).endswith(".md")]
    cited += [d for d in CITED_DIR.findall(line)
              if d.split("/")[0] in docs.tops and not any(d in c for c in cited)]
    if any(not foreign(c) and not docs.path_resolves(c, path) for c in cited):
        faults.append("src.comment_dead_path")
    # A trailing underscore is a FAMILY -- `VOTVCOOP_RUN_*`, `..._{X,Y,Z}` -- not a name.
    if docs.envs and any(not n.endswith("_") and n not in docs.envs
                         for n in ENV_NAME.findall(line)):
        faults.append("src.comment_dead_env")
    if docs.ini_keys and any(k.split(".")[0] in docs.ini_sections and k not in docs.ini_keys
                             for k in CITED_INI.findall(line)):
        faults.append("src.comment_dead_ini_key")
    if any(t in docs.our_types and m not in docs.code_words
           for t, m in CITED_MEMBER.findall(line)):
        faults.append("src.comment_dead_member")
    # An empty format index is not the answer "no format carries it"; reading it as one would
    # accuse every marker in the tree. The same shape as vendored_known below.
    if docs.log_formats:
        for m in CITED_MARKER.finditer(line):
            cand = (m.group(1) or m.group(2)).strip()
            if MARKER_SHAPE.search(cand) and "/" not in cand \
                    and not docs.log_marker_resolves(cand):
                faults.append("src.comment_dead_log_marker")
                break
    return faults


def md_link_faults(line, base, tracked_set, subs):
    """-> the counter key for every dead link and dead backticked path on one line of a doc.

    One implementation, called by the counting pass and by the line explainer, so `--lines` cannot
    disagree with the number it is explaining. A link into a submodule is live: its content is
    public too."""
    faults = []
    for m in LINK.finditer(line):
        target = unquote(m.group(1))
        if re.match(r"^[a-z]+:", target, re.I) or target.startswith("/"):
            continue
        if "\\" in target:                       # a backslash path is dead on Linux
            faults.append("md.dead_links")
            continue
        rel = os.path.normpath(os.path.join(base, target)).replace("\\", "/")
        if rel in tracked_set or any(t.startswith(rel + "/") for t in tracked_set) \
                or any(rel == s or rel.startswith(s + "/") or s.startswith(rel + "/") for s in subs):
            continue
        faults.append("md.dead_links")
    for m in BACKTICK_PATH.finditer(line):
        rel = m.group(1)
        if rel in tracked_set or any(t.startswith(rel + "/") for t in tracked_set):
            continue
        faults.append("md.dead_paths")
    return faults


def comment_lines(text):
    """-> ([(1-based line number, text)], code_line_count, long_blocks). A line counts as COMMENT
    when it holds nothing but comment (and whitespace); quotes are respected so a `//` inside a
    string is code. `long_blocks` LISTS the first line of each run of more than LONG_COMMENT_BLOCK
    consecutive comment lines (blank lines do not break a run; a code line does), so its length is
    the count the gate wants and its contents are the essays `--lines` points a human at. The line
    number rides along with each comment for the same reason.

    `tails` is the fourth value: the comment TRAILING a code line, which is a comment the marker
    rules apply to and the volume rules do not. A citation is a citation wherever it sits, so the
    markers read these; the ratio counters measure comment MASS as whole lines, because the rule
    they serve is about essays and a trailing `// why` is part of its own code line's statement."""
    lines = text.split("\n")
    comments, code, long_blocks, tails, run = [], 0, [], [], 0
    in_block = False

    def comment(line):
        nonlocal run
        comments.append((no, line))
        run += 1
        if run == LONG_COMMENT_BLOCK + 1:
            long_blocks.append(comments[-run][0])

    for no, line in enumerate(lines, 1):
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
                tails.append((no, s[i:]))
                break
            elif s.startswith("/*", i):
                # A block comment that CLOSES on this line contributes only its own span; taking
                # the rest of the line with it charges the code after `*/` to a comment counter,
                # and `--lines` would then point a sweep at a string literal it cannot edit away.
                e = s.find("*/", i + 2)
                if e < 0:
                    tails.append((no, s[i:]))
                    in_block = True
                    break
                tails.append((no, s[i:e + 2]))
                i = e + 2
                continue
            i += 1
        code += 1
    return comments, code, long_blocks, tails


RAW_OPEN = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t]{0,16})\(')


def string_literals(text):
    """-> [(1-based line number, body)] for every "..." that is CODE, and for every RUN of
    adjacent literals the compiler concatenates, the joined body under the run's first line.

    The run matters as much as the parts. This tree wraps a long log line by splitting its format
    across two adjacent literals, so a marker can sit astride the join -- `"... security " "A65"`
    -- where testing each half alone sees nothing. A sweep that re-wraps such a line would drive
    this counter to zero without removing anything.

    Walked over the whole text rather than line by line, because the three shapes that broke a
    line-based scan all cross or contain line boundaries: a raw string's body (where `//` is
    ordinary text), a block comment that closes mid-line (whose tail is code), and a literal
    continued with a trailing backslash."""
    out, run, run_line = [], [], 0
    i, n, line = 0, len(text), 1

    def emit(body, at):
        nonlocal run_line
        out.append((at, body))
        if not run:
            run_line = at
        run.append(body)

    def flush():
        nonlocal run
        if len(run) > 1:
            out.append((run_line, "".join(run)))
        run = []

    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c in " \t\r":
            i += 1
            continue
        # Neither a comment nor whitespace ends a concatenation run: `"a" /* why */ "b"` is one
        # string to the compiler, so it must be one string here too.
        if text.startswith("//", i):
            k = text.find("\n", i)
            i = n if k < 0 else k
            continue
        if text.startswith("/*", i):
            k = text.find("*/", i + 2)
            end = n if k < 0 else k + 2
            line += text.count("\n", i, end)
            i = end
            continue
        m = RAW_OPEN.match(text, i)
        if m:
            close = ")" + m.group(1) + '"'
            k = text.find(close, m.end())
            end = n if k < 0 else k + len(close)
            emit(text[m.end():(n if k < 0 else k)], line)
            line += text.count("\n", i, end)
            i = end
            continue
        if c == '"':
            k, buf, at = i + 1, [], line
            while k < n and text[k] != '"':
                if text[k] == "\\":
                    buf.append(text[k:k + 2])
                    if text[k + 1:k + 2] == "\n":
                        line += 1
                    k += 2
                elif text[k] == "\n":
                    break                       # unterminated: stop at the line end
                else:
                    buf.append(text[k])
                    k += 1
            emit("".join(buf), at)
            i = k + 1
            continue
        if c == "'":
            k = i + 1
            while k < n and text[k] not in ("'", "\n"):
                k += 2 if text[k] == "\\" else 1
            flush()
            i = k + 1
            continue
        flush()
        i += 1
    flush()
    return out


def string_marker_lines(text):
    """-> [(line number, the literal)] ONCE PER LINE, for every string carrying a marker.

    One implementation for the counting pass and the line explainer, as `src_comment_faults` is:
    a line carrying two offending literals must be one number in both, or `--lines` would name
    more lines than the gate counts and a sweep would chase a hit that is not there."""
    seen, out = set(), []
    for no, body in string_literals(text):
        if no in seen:
            continue
        if any(rx.search(body) for _k, rx in STRING_MARKERS):
            seen.add(no)
            out.append((no, body))
    return out


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
        tracked_files, tracked_subs = tracked(a.repo) if a.lines else ((), ())
        tracked_set = set(tracked_files)
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
