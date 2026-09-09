#!/usr/bin/env python3
"""prose_drill_mutants -- the ways a detector could be written wrong.

Every counter is proved by a COUNT, and a count only proves a detector is not
silent. Each row here breaks one detector on a copy of the gate, and the drill
must go RED; a row that stays green names a regression that would ship.

Deliberately not named public_prose_*: this file quotes every anchor, so the
mutation pass must never search it for one.
"""

# Every counter above is proved by a COUNT, and a count only proves the detector is not silent.
# These prove it is not deaf either: each row breaks one detector on a COPY of the gate, and the
# drill must go RED. A row that stays green names a regression that would ship -- which is how
# `[A]` and the whole named-label family were found to have no canary at all.
MUTANTS = [
    # The size pass reads `other_file`, which is the membership WITHOUT the marker
    # exemption. Narrowing it back to `measured_other` is the exact blindness that
    # let this gate reach 1,317 lines: markers exempt, and length exempt with them.
    ("size: the cap stops reading the gates",
     'for p in [q for q in files if other_file(q)]:',
     'for p in [q for q in files if measured_other(q)]:'),
    ("size: the cap is never crossed", 'if n > OTHER_HARD_CAP:', 'if n > 10 ** 9:'),
    # --file's own answers: calling an unread file swept, and matching a basename by suffix.
    ("file: an unread file is called swept", "if not measured(p):", "if False:"),
    ("file: a basename matches by suffix", 'return p == want or p.endswith("/" + want)',
     "return p.endswith(want)"),
    ("review: no inflections", 'r"\\baudit(?:s|ed|ing)?\\b", re.I', 'r"\\baudit\\b", re.I'),
    ("review: case-sensitive", 'r"\\baudit(?:s|ed|ing)?\\b", re.I', 'r"\\baudit(?:s|ed|ing)?\\b"'),
    ("review: matches nothing", 'r"\\baudit(?:s|ed|ing)?\\b", re.I', 'r"\\bZZZZ\\b", re.I'),
    ("review: no trailing boundary", 'r"\\baudit(?:s|ed|ing)?\\b", re.I', 'r"\\baudit", re.I'),
    ("evidence: drops [RD]", 'r"\\[(?:V|\\?|RD|A)\\]"', 'r"\\[(?:V|\\?|A)\\]"'),
    ("evidence: drops [?]", 'r"\\[(?:V|\\?|RD|A)\\]"', 'r"\\[(?:V|RD|A)\\]"'),
    ("evidence: drops [V]", 'r"\\[(?:V|\\?|RD|A)\\]"', 'r"\\[(?:\\?|RD|A)\\]"'),
    ("evidence: drops [A]", 'r"\\[(?:V|\\?|RD|A)\\]"', 'r"\\[(?:V|\\?|RD)\\]"'),
    ("evidence: any bracket token", 'r"\\[(?:V|\\?|RD|A)\\]"', 'r"\\[[A-Za-z?]{1,3}\\]"'),
    ("user: drops the capitals form", 'r"\\bUSER\\b"', 'r"\\bZZZZ\\b"'),
    ("user: drops the per-the-user form", 'r"|(?i:\\bper (?:the )?user\\b)"', 'r""'),
    ("user: drops the colon form", 'r"|(?i:\\buser\'?s?\\s*:\\s*\\S)"', 'r""'),
    ("user: colon form needs a quote", "\\s*:\\s*\\S)", '\\s*:\\s*[\\"\\u00ab])'),
    ("user: drops the requested form", 'r"|(?i:\\buser-request(?:ed)?\\b)"', 'r""'),
    # One mutant per noun family, each a BALANCED substring: cutting the group
    # mid-parenthesis made the gate fail to import, and an arm that only asks for a
    # non-zero exit passes on the crash instead of on the detector going blind.
    ("user: drops the ask family", "ask(?:s|ed)?", "zzzk(?:s|ed)?"),
    ("user: drops the report family", "report(?:s|ed)?", "zzzort(?:s|ed)?"),
    ("user: drops the request family", "req(?:s|uest(?:s|ed)?)?", "zzz(?:s|uest(?:s|ed)?)?"),
    ("user: drops the retest family", "retest(?:s|ed)?", "zzzest(?:s|ed)?"),
    ("user: drops the say family", "say(?:s)?|said", "zzy(?:s)?|zzid"),
    ("user: drops the noun family", "|choice|decision|rule)", "|zzzice|zzzision|zzle)"),
    ("user: drops the dated attribution", 'r"|(?i:\\buser \\d{4}-\\d{2}-\\d{2})"', 'r""'),
    ("user: any mention of the word", 'r"|(?i:\\bper (?:the )?user\\b)"', 'r"|(?i:\\buser\\b)"'),
    # The five citation axes. Each row breaks the detector one way it could plausibly be written
    # wrong: the path axis losing a spelling the tree uses, the marker axis compared in the
    # direction that reads every rendered value as dead, the member axis taking a forward
    # declaration for a definition.
    ("path: drops the module-root spelling",
     '(MODULE_ROOT, MODULE_ROOT + "include/", MODULE_ROOT + "src/")', '("zzz/",)'),
    ("path: drops the submodule prefix",
     'if any(c.startswith(s + "/") for s in self.subs):\n            return True',
     'if False:\n            return True'),
    ("path: stops at the shorter name", r'(?![\w]|\.[A-Za-z])', r'(?![\w])'),
    ("path: counts a foreign tree too", '"Runtime/", "CXXHeaderDump/"', '"zzzRuntime/", "CXXHeaderDump/"'),
    ("path: an include spelling is a path",
     'if c in self.includes.get(owner, ()):\n            return True',
     'if False:\n            return True'),
    ("marker: a rendered value is literal", r"|\b\d+\b", r"|\bzzz\b"),
    ("marker: any quoted string is a marker", 'if MARKER_SHAPE.search(cand)', 'if cand'),
    ("env: a family counts as a name", 'not n.endswith("_") and ', ''),
    ("ini: every row resolves", 'k not in docs.ini_keys', 'k in ()'),
    ("member: a forward declaration is a definition",
     r'([A-Z]\w+)\s*(?:final\s*)?(?::[^;{]*)?\{', r'([A-Z]\w+)'),
    ("member: every member resolves", 'm not in docs.code_words', 'm in ()'),
    ("member: the pattern does not overlap", r'(?:\w+::)*([A-Za-z_]', r'(?:zzz::)*([A-Za-z_]'),
    ("member: a mirror type is ours",
     'if not t.endswith(MIRROR_HEADER):', 'if True:'),
    ("ini: a comparison is a row", r'\s*=(?!=)")', r'\s*=")'),
    ("path: a foreign tree counts only at the start",
     'return any(cand.startswith(f) or ("/" + f) in cand for f in FOREIGN_TREE)',
     'return cand.startswith(FOREIGN_TREE)'),
    ("label: drops the named families",
     'r"\\b(?:CRIT|MAJOR|MINOR|HIGH|MED|LOW|IMP)-\\d+\\b"', 'r"\\bZZZZ\\b"'),
    ("label: increment needs its dash",
     'r"|(?i:\\binc(?:rement)?[-\\s]?\\d+[a-z]?\\b)"', 'r"|\\bInc-\\d+[a-z]?\\b"'),
    ("label: increment is capitals-only",
     'r"|(?i:\\binc(?:rement)?[-\\s]?\\d+[a-z]?\\b)"', 'r"|\\bInc(?:rement)?[-\\s]?\\d+[a-z]?\\b"'),
    ("label: increment spelt out unread",
     'r"|(?i:\\binc(?:rement)?[-\\s]?\\d+[a-z]?\\b)"', 'r"|(?i:\\binc[-\\s]?\\d+[a-z]?\\b)"'),
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
    # The naming rule needs one mutant per DIRECTION, because its two halves read different
    # inputs: the tracked half reads git, the untracked half reads the directory. A rule with one
    # arm would ship half-blind and the burn-down would call the tree named.
    ("name_case: tracked half blind",
     'return None if stem == stem.lower() else "a tracked doc is public: name it lowercase"',
     'return None'),
    ("name_case: untracked half blind",
     'return None if stem == stem.upper() else "an untracked doc is local: name it UPPER_CASE"',
     'return None'),
    ("name_case: exceptions swallow every name", "    if stem in DOC_NAME_EXEMPT:\n",
     "    if True:\n"),
    ("name_case: never sees an untracked doc",
     '            if name.endswith(".md") and rel not in tracked_set:\n',
     '            if False:\n'),
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
    # A citation into a submodule is not a rotted one: the tree is pinned by SHA and checked out
    # beside us, so it cannot be shortened under the citation the way one of ours can.
    # A submodule that is not checked out is not a submodule full of absent files. Without the
    # `.git` presence test, `git -C <empty dir> ls-files` answers with the PARENT repository's
    # list and exit 0, so the loop believes it read a tree it never opened -- which is what made
    # this counter read 0 locally and 13 in CI.
    ("doc_row: an unopened submodule reads as read",
     '                if not os.path.exists(os.path.join(here, ".git")):\n                    continue',
     '                if False:\n                    continue'),
    ("doc_row: a vendored file does not resolve",
     "            return name in self.vendored()", "            return False"),
    ("doc_row: an absent file resolves",
     "        if name not in self.lengths:\n            return name in self.vendored()",
     "        if name not in self.lengths:\n            return True"),
    ("doc_row: any line number resolves",
     "        return first <= self.lengths[name]", "        return True"),
    ("doc_row: line citations unread",
     'SRC_LINE_CITE = re.compile(r"\\b([A-Za-z0-9_]+\\.(?:cpp|h|inc))\\s*:(\\d{1,5})")',
     'SRC_LINE_CITE = re.compile(r"ZZZZNOMATCH()()")'),
    # The dead-document predicate in the other.* family: it ran only over our C++ once, while 33
    # lines in ten tracked scripts and workflows named an absent doc with nothing reading them.
    # The two carve-outs the doc predicate needs outside our C++, each able to damage the tree
     # from the other side: reading a whole line makes a path a script OPENS a citation, and
     # reading an ignore file's comments counts the reason every rule in it exists.
    ("other: a path in code reads as a citation",
     "    return line[i:] if i >= 0 else None", "    return line"),
    ("other: an ignore file's comments count as citations",
     "    if path.endswith(OTHER_COMMENTS_ONLY):\n        return None",
     "    if False:\n        return None"),
    ("other: dead documents unread",
     "            prose = other_prose(p, line)",
     "            prose = None"),
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
    # The string axis. Each row breaks one part of it -- the counting, the table it derives
    # from, the lexer, the per-line dedupe, the explainer -- and the exemption that keeps it off
    # the fold fixtures, which is the one way widening it would damage the tree.
    ("dated: a month is not a date", r'r"\b20\d\d-(?:0[1-9]|1[0-2])(?:-\d\d)?\b"',
     r'r"\b20\d\d-(?:0[1-9]|1[0-2])-\d\d\b"'),
    ("dated: a day is not required", r'r"\b20\d\d-(?:0[1-9]|1[0-2])(?:-\d\d)?\b"',
     r'r"\b20\d\d\b"'),
    ("dated: any two digits are a month",
     r'r"\b20\d\d-(?:0[1-9]|1[0-2])(?:-\d\d)?\b"', r'r"\b20\d\d-\d\d(?:-\d\d)?\b"'),
    ("string: literals unread", "smark = string_marker_lines(text)", "smark = []"),
    ("string: nothing is exempt", 'STRING_EXEMPT = ("cyrillic", "review", "agent", "lesson")',
     'STRING_EXEMPT = ()'),
    ("string: ordinary product English counts",
     'STRING_EXEMPT = ("cyrillic", "review", "agent", "lesson")', 'STRING_EXEMPT = ("cyrillic",)'),
    ("string: reads only the shared marker table",
     'list(LINE_MARKERS.items()) + list(SRC_EXTRA.items()) if k not in STRING_EXEMPT',
     'list(LINE_MARKERS.items()) if k not in STRING_EXEMPT'),
    # The four shapes a line-based lexer got wrong. Each is a literal the counter must SEE; the
    # mutant blinds it one way and the count falls.
    ("string: adjacent literals are never joined", "        if len(run) > 1:", "        if False:"),
    ("string: a comment ends a concatenation run",
     '        if text.startswith("//", i):\n            k = text.find("\\n", i)',
     '        if text.startswith("//", i):\n            flush()\n            k = text.find("\\n", i)'),
    ("string: a block comment eats the rest of its line",
     '            k = text.find("*/", i + 2)\n            end = n if k < 0 else k + 2',
     '            k = text.find("\\n", i)\n            end = n if k < 0 else k'),
    ("string: a raw string is not one literal", "        m = RAW_OPEN.match(text, i)", "        m = None"),
    ("string: a line with two literals counts twice",
     "        if no in seen:\n            continue", "        if False:\n            continue"),
    ("string: counted but never explained",
     '        for no, body in string_marker_lines(text):\n'
     '            out.append((no, "src.string_marker", \'"\' + body + \'"\'))\n',
     ''),
    ("markers: blind to trailing comments",
     "        for _no, line in comments + tails:\n", "        for _no, line in comments:\n"),
]
