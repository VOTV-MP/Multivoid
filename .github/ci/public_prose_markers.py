#!/usr/bin/env python3
"""public_prose_markers -- the vocabulary the gate refuses, as patterns.

Every counter that names a habit -- a diary date, an attribution to the user, a
pointer into a local tree, a citation that resolves to nothing -- is one entry
here: a key, the pattern that recognises it, and the sentence that describes it
in a report. Keeping them together is what makes the set readable as a list of
habits rather than as scattered regular expressions, and it is the surface a
mutant is aimed at when the drill asks whether a detector is still awake.

It is standalone by construction: patterns only, no file access, no decisions.
"""
import collections
import re


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
