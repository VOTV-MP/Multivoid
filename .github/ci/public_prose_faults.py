#!/usr/bin/env python3
"""public_prose_faults -- reading one line, or one file, and naming what is wrong.

The detectors themselves. Given a line and an index of what the repository
actually contains, each answers with the counter keys that line trips: a marker
from public_prose_markers, a link or citation that resolves against nothing, a
comment block past its cap, a string literal carrying a habit a comment may not.
DocIndex is the index they resolve against -- tracked paths, tracked basenames,
and the lengths that decide whether a cited line number still exists.

It reaches the tree only through public_prose_scope.
"""
import os
import re
from urllib.parse import unquote
from public_prose_markers import (
    ANY_INCLUDE, BACKTICK_PATH, CITED_DIR, CITED_INI, CITED_MARKER, CITED_MEMBER, CITED_PATH,
    DOC_BARE, DOC_PATH, DOC_SECTIONED, DOC_UNNAMED, ENV_NAME, FOREIGN_TREE, LINE_MARKERS, LINK,
    LOCAL_TREES, LOG_CALL, LOG_LITERAL, LOG_SPEC, LONG_COMMENT_BLOCK, MARKER_SHAPE, MIRROR_HEADER,
    MODULE_ROOT, RAW_OFFSET, REGISTRY_ROW, RENDERED, SRC_EXTRA, SRC_LINE_CITE, STRING_MARKERS,
    TYPE_DEF, WORD)
from public_prose_scope import (
    OTHER_COMMENTS_ONLY, SRC_EXT, code_only, git, measured_src, read, tracked)


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
