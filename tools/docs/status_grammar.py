#!/usr/bin/env python3
"""status_grammar -- what a STATUS LABEL is, and whether what a line cites still resolves.

The grammar half of tools/docs/status_census.py (docs/DOCUMENTIZE_ARC.md WP-1(a), M-1): a LABEL is a
status TAG or TOKEN -- `[?]`, `[SUPERSEDED ...]`, a bold or capitalised status word at line or cell
start, a `Status:` field, a checkbox, a heading carrying `Open questions` / `OPEN` / `TODO` / `NEXT`.
Case-sensitive on purpose: `[V]` / `[A]` / `[RD]` are PROVENANCE tags, `pending-remove` is a name,
`fail CLOSED` mid-sentence is prose. The SUB-STATE column reads the parenthetical or trailing clause
on the label line AND the next line -- where section 2.3 measured the rot hiding under true labels.
The Resolver is the MECHANICAL column: every path:line, path, backticked symbol and commit hash on
the line, with its resolve state (ok / gone / past-eof / ambiguous / external).

The drill (status_census_drill.py) asserts RECALL and PRECISION on the real lines of the 2026-09-02
staleness sample, not on a synthetic RED alone.
"""
import hashlib
import io
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lessons_gate as LG  # noqa: E402  -- build_corpora / CITE_ROOTS / load_list / ALLOW_FILES


def read_text(path):
    try:
        return io.open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return None


def sha1(text):
    return hashlib.sha1(text.encode("utf-8", "replace")).hexdigest()[:12]


# ----------------------------------------------------------------------------- the label grammar
STATUS_WORDS = ("OPEN|DONE|TODO|PENDING|DESIGN|AS-BUILT|BUILT|SHIPPED|VERIFIED|PROVEN|IMPLEMENTED|"
                "DEFERRED|WIP|PARTIAL|CLOSED|FIXED|RETIRED|SUPERSEDED|FUTURE|PLANNED|MITIGATED|STALE|"
                "LIVE|NOT BUILT|NOT WIRED|NOT DONE|NOT hands-on|UNVERIFIED|UNTESTED|BLOCKED|IN PROGRESS")

# THE VOCABULARY, PARTITIONED -- one list, two halves, and an assert that they cover it. The AUTHORING
# lane asks about a claim of COMPLETION, so it needs to know which words assert one; writing that set
# by hand BESIDE `STATUS_WORDS` is how a token list drifts from the grammar it claims to follow (round
# 8 caught exactly that: a filter naming `[V]` as a completion label, when `label_of` says in so many
# words that `[V]`/`[A]`/`[RD]` are PROVENANCE and `TAG_RE` never emits them).
COMPLETION_WORDS = ("DONE", "AS-BUILT", "BUILT", "SHIPPED", "VERIFIED", "PROVEN", "IMPLEMENTED",
                    "CLOSED", "FIXED", "RETIRED", "SUPERSEDED", "MITIGATED", "LIVE")
OPEN_WORDS = ("OPEN", "TODO", "PENDING", "DESIGN", "DEFERRED", "WIP", "PARTIAL", "FUTURE", "PLANNED",
              "STALE", "NOT BUILT", "NOT WIRED", "NOT DONE", "NOT hands-on", "UNVERIFIED", "UNTESTED",
              "BLOCKED", "IN PROGRESS")
assert set(STATUS_WORDS.split("|")) == set(COMPLETION_WORDS) | set(OPEN_WORDS), (
    "the partition must COVER the vocabulary: " +
    str(set(STATUS_WORDS.split("|")) ^ (set(COMPLETION_WORDS) | set(OPEN_WORDS))))

# A doc that QUOTES the status vocabulary -- a verdict table, a tag legend, this grammar's own token
# list -- is not making the claims it prints. `[V]` 2026-09-03: 20 of the one real close's 23
# `NOT A LABEL` rows sat in FOUR such artifacts (both SKILL.md files 8 each, DOCUMENTIZE_ARC 3, one
# feedback file 1), and all four are touched on EVERY close, so ~13% of every hand pass was the
# instrument re-refusing its own token table. The ACCRETION detector already had an exclusion list;
# the label grammar had none. Same marker the ledger prescribes.
# TWO SCOPES, both explicit -- never positional. `[V]` 2026-09-03: marking only the SECTION holding
# each skill's token table left 7 and 6 rows standing, all of them still mentions ("a doc says
# PROVEN/works/VERIFIED", "what was BUILT", "a tag like `[?]`"): an instruction text ABOUT status
# labels quotes the vocabulary in its prose, not in one table. A design doc like DOCUMENTIZE_ARC is
# the opposite -- 38 rows of real claims with a handful of quotes among them -- so doc scope there
# would throw the claims away. One rule cannot serve both; the author says which.
VOCAB_MARKER = "<!-- corr-vocabulary: quoted -->"          # to the end of THIS section
VOCAB_MARKER_DOC = "<!-- corr-vocabulary: quoted-doc -->"  # the WHOLE doc quotes, never claims
# A NEGATED status word is a different label, not the same one: "ROOT MEASURED, NOT FIXED" captured as
# `FIXED` records the OPPOSITE of the line's claim. Measured 2026-09-03 by a post-ship audit on a real
# census row (research/findings/join-identity/votv-rejoin-loadmap-null-worldsettings-RE-2026-08-31.md:10).
# The four hand-written NOT forms in STATUS_WORDS stay: they cover words that are not status words on
# their own (`NOT hands-on`), which this prefix cannot reach.
STATUS_RE = re.compile(r"(?<![A-Za-z-])((?:NOT\s+)?(?:" + STATUS_WORDS + r"))(?![A-Za-z-])")
TAG_RE = re.compile(r"\[(\?|SUPERSEDED[^\]]*|OPEN|DONE|TODO|WIP|STALE|DEFERRED|CORR)\]")
LEAD_RE = re.compile(r"^[>\s]*(?:(?:[-*+]|\d+[.)])\s+)?")   # a bullet marker must be followed by whitespace: `**bold**` is not one
FIELD_RE = re.compile(r"^\W{0,12}(?:\*\*)?(?:Status|STATUS|Verdict|VERDICT)(?:\*\*)?\s*:\s*(?:\*\*)?([^*|\n]{1,80})")
CELL_RE = re.compile(r"\|\s*(?:\*\*)?((?:NOT\s+)?(?:" + STATUS_WORDS + r"))(?:\*\*)?\s*(?=\(|\||$)")
CHECKBOX_RE = re.compile(r"^\s*[-*]\s+\[( |x|X)\]\s")
HEADING_RE = re.compile(r"^#{1,6}\s+(.*)")
OPEN_PHRASE_RE = re.compile(r"(?<![A-Za-z-])(Open questions?|Open items?|Open bugs?|Open points?|"
                            r"open functional bugs|still open|OPEN|TODO|NEXT|Pending|PENDING)(?![A-Za-z-])")
SUBSTATE_RE = re.compile(r"(commit pending|hands-on-pending|hands-on pending|not yet|pending|uncommitted|"
                         r"unverified|untested|never ran|never run|NOT hands-on|TODO)", re.I)
LOOSE_RE = re.compile(r"OPEN|FUTURE|TODO|PENDING|NEXT|not (yet )?(built|wired|implemented|done|verified)|"
                      r"deferred|unverified|\[ \]|\[\?\]|planned|stub|placeholder", re.I)
# Group 3 is the RANGE end (`reflection.cpp:576-677`). It exists because the content rung below
# asks "is the cited thing NEAR this line", and reading only the start of a range calls a citation
# stale for pointing at its own second half -- `[V]` `docs/PERF_ARC.md:366` cites
# `reflection.cpp:576-677` for `CountObjectsByClass`, which is at 647: inside the range, 71 lines
# from its start.
CITE_RE = re.compile(r"([A-Za-z0-9_][A-Za-z0-9_/.\\-]*\.(?:h|hpp|cpp|c|inc|py|ps1|rs|json|md|txt|yml)):(\d+)(?:-(\d+))?")
# The section sign is excluded from the lookbehind: a SECTION reference like "§6c.c" is not a C
# file (measured 2026-09-03 by the first real census on docs/security/LESSONS_SECURITY.md:329, which
# cites "§6c.c + §9b" and was reported as a dead citation).
PATHTOK_RE = re.compile("(?<![A-Za-z0-9_/.\\\\§-])([A-Za-z0-9_][A-Za-z0-9_/.\\\\-]*\\.(?:h|hpp|cpp|c|inc|py|ps1|rs))(?![A-Za-z0-9_/.\\\\-])")
SYMBOL_RE = re.compile(r"`([A-Za-z_][A-Za-z0-9_]{4,}(?:::[A-Za-z_][A-Za-z0-9_]*)*)(?:\(\))?`")
HASH_RE = re.compile(r"(?<![A-Za-z0-9_-])([0-9a-f]{7,40})(?![A-Za-z0-9_-])")   # `-` excluded: a UUID segment is not a commit (2,400 false hashes on the first run)
TOTAL_RE = re.compile(r"\b\d+ of \d+\b|\b\d[\d,]* (?:rows|files|findings|docs|entries|lines|LOC|sites|hits|commits|"
                      r"symbols|classes|lanes)\b|\bb\d{2,3}\b|\bproto(?:col)? \d+\b")
DATE_RE = re.compile(r"\b20\d\d-\d\d-\d\d\b")
CORR_RE = re.compile(r"\[corr 20\d\d-\d\d-\d\d:")
ACCRETION_RE = re.compile(r"CORRECTED|was wrong|is FALSE|SUPERSEDED|no longer|said the opposite|stood here", re.I)
DATED_NAME_RE = re.compile(r"20\d\d-\d\d-\d\d")


def label_of(line):
    """-> (kind, label) or None. Case-sensitive on purpose: `[V]`/`[A]`/`[RD]` are provenance,
    'pending-remove' is a name, 'fail CLOSED' mid-sentence is prose."""
    m = TAG_RE.search(line)
    if m:
        return ("tag", "[" + m.group(1) + "]")
    m = FIELD_RE.match(line)
    if m:
        return ("field", m.group(1).strip())
    m = CHECKBOX_RE.match(line)
    if m:
        return ("checkbox", "done" if m.group(1) in "xX" else "todo")
    m = HEADING_RE.match(line)
    if m:
        p = OPEN_PHRASE_RE.search(m.group(1))
        return ("heading", p.group(1)) if p else None
    m = CELL_RE.search(line)
    if m:
        return ("cell", m.group(1))
    lead = LEAD_RE.sub("", line, count=1)
    if lead.startswith("**"):
        end = lead.find("**", 2)
        span = lead[2:end] if end > 2 else lead[2:80]
        s = STATUS_RE.search(span) or OPEN_PHRASE_RE.search(span)
        if s:
            return ("lead", s.group(1))
    head = lead[:48]
    s = STATUS_RE.search(head)
    if s:
        return ("lead", s.group(1))
    return None


def substate_of(line, next_line):
    found = []
    for src in (line, next_line or ""):
        for m in SUBSTATE_RE.finditer(src):
            t = m.group(1).lower()
            if t not in found:
                found.append(t)
    return found


class Resolver:
    """The mechanical column: does each token on the line still resolve in the tree?"""

    def __init__(self, env):
        self.env = env
        self._corpora = None
        self._hashes = {}
        self._files = {}
        self._index = None
        LG.REPO = env.repo   # the corpora walk under LG.REPO

    def corpora(self):
        if self._corpora is None:
            sets, _ = LG.build_corpora()
            self._corpora = set().union(*sets.values()) if sets else set()
        return self._corpora

    def index(self):
        """basename -> [relpath]; built ONCE. (lessons_gate.resolve_cite walks the tree per call, which
        is fine for ~125 ledger citations and is minutes for a census: measured 2026-09-03.)"""
        if self._index is None:
            idx = {}
            for root in LG.CITE_ROOTS:
                full = os.path.join(self.env.repo, root)
                if not os.path.isdir(full):
                    continue
                for dirpath, dirnames, filenames in os.walk(full):
                    dirnames[:] = [d for d in dirnames if d not in ("__pycache__", ".git", "node_modules", "build", "target")]
                    for f in filenames:
                        idx.setdefault(f, []).append(os.path.join(dirpath, f))
            self._index = idx
        return self._index

    def resolve(self, path):
        direct = os.path.join(self.env.repo, path)
        if os.path.isfile(direct):
            return direct, []
        hits = self.index().get(os.path.basename(path), [])
        if len(hits) == 1:
            return hits[0], []
        return None, hits

    def lines_of(self, abspath):
        if abspath not in self._files:
            t = read_text(abspath)
            self._files[abspath] = None if t is None else t.split("\n")
        return self._files[abspath]

    def external(self, path):
        """Files cited from OUTSIDE this repo (the UE4SS header dump, engine source, upstream vendor
        sources).

        ONE MATCHER, not merely one list. This used to do its own `base in self._ext or path in
        self._ext` while `lessons_gate` matched the same file case-insensitively and by fnmatch --
        so a shared list was read two ways and the two instruments disagreed about the same
        citation. `[V]` 2026-09-04: `trashBitsPile.hpp`, `engine.hpp` and `Engine.hpp` all answered
        external=False here and external=True there, which is how the census raised ten `cite` rows
        the ledger gate did not consider dead. Sharing the DATA and forking the PREDICATE is the
        same defect as two copies of the data (DIFF pass, round 5 Q1).
        """
        if not hasattr(self, "_ext"):
            self._ext = set(LG.load_list(LG.ALLOW_FILES))
        return LG.allow_match(path, self._ext)

    def cite(self, path, line, end=None, quote=None, symbol=None):
        resolved, hits = self.resolve(path)
        if not resolved:
            if self.external(path):
                return "external", None
            return ("ambiguous" if hits else "gone"), None
        lines = self.lines_of(resolved)
        if lines is None:
            return "gone", None
        if line > len(lines):
            return "past-eof", None
        return self.content(resolved, line, end, quote, symbol)

    # --- the CONTENT rung ---------------------------------------------------------------------
    # A line number is a POSITION; the claim is about CONTENT. `lessons_gate.check_quoted_cites`
    # says exactly that and has checked it since 2026-08-30, when an extraction moved five cited
    # facts and the positional gate passed all five in the same run that created the rot. But it
    # fires ONLY on the explicit `file:line` says "..." form -- and `[V]` 2026-09-03 that form
    # occurs FIVE times in the whole 1,613-doc read set and ZERO times in `docs/LESSONS.md`, the
    # ledger it guards. The check built for that defect has never had an input.
    #
    # What this corpus actually writes is a citation beside a BACKTICKED SYMBOL: `[V]` 1,816 of the
    # 5,302 resolving citations. So the content rung reads there too -- at a DIFFERENT strength,
    # because the two pairings are not equally certain:
    #
    #   QUOTE   the doc states which words the line carries, with an explicit verb. Unambiguous by
    #           construction, so a miss is DEAD and refuses a STILL TRUE like any dead citation.
    #   SYMBOL  the pairing is INFERRED from adjacency. A hand check of four cases found one false
    #           pair (`docs/LESSONS.md:1590` cites `config.cpp:508` for `resize(255)`; `ToUtf8`
    #           belongs to a later citation in the same sentence) and one range read as stale for
    #           pointing inside itself. Both are fixed below -- and it STILL only emits a row, so
    #           the drift enters the bounded hand check and is judged there. It never refuses. A
    #           gate that is mostly right is one people learn to ignore, which is `QUOTED_CITE`'s
    #           own comment ("narrow beats noisy") applied to its successor.
    def content(self, resolved, line, end, quote, symbol):
        """-> (state, detail). `moved` / `content-gone` are DEAD; `drift` is advisory."""
        lines = self.lines_of(resolved)
        lo, hi = line, (end or line)
        if quote:
            needle = LG.norm(quote)[:48]
            if len(needle) >= 20:
                if needle in LG.quote_window(lines, lo, hi):
                    return "ok", None
                at = LG.find_quote(lines, needle)
                return ("moved", at) if at else ("content-gone", None)
        if symbol:
            s0 = symbol.split("::")[-1]
            rx = re.compile(r"\b" + re.escape(s0) + r"\b")
            at = [i + 1 for i, l in enumerate(lines) if rx.search(l)]
            # A symbol NOT in the cited file is no evidence at all: a doc may name a caller and
            # cite the callee's site, or name a concept the file never spells. Only a symbol that
            # IS there, exactly once, and nowhere near the cited line, says the number moved.
            if at and not any(lo - 25 <= i <= hi + 25 for i in at) and len(at) == 1:
                return "drift", at[0]
        return "ok", None

    @staticmethod
    def pair_symbol(cites, syms, cb, ce):
        """The symbol ADJACENT to the citation at [cb, ce), with no OTHER citation between them and
        at most 3 characters of separation (a comma, a space, a backtick). Returns None when the
        line gives no unambiguous partner -- which is most lines, on purpose."""
        best = None
        for sb, se, v in syms:
            if se <= cb:
                gap = cb - se
                if any(o_ce <= cb and o_ce > se for o_cb, o_ce, _, _, _ in cites if o_cb != cb):
                    continue
            elif sb >= ce:
                gap = sb - ce
                if any(o_cb >= ce and o_cb < sb for o_cb, o_ce, _, _, _ in cites if o_cb != cb):
                    continue
            else:
                continue
            if gap <= 3 and (best is None or gap < best[0]):
                best = (gap, v)
        return best[1] if best else None

    def path(self, path):
        resolved, hits = self.resolve(path)
        if resolved:
            return "ok"
        if self.external(path):
            return "external"
        return "ambiguous" if hits else "gone"

    def symbol(self, sym):
        head = sym.split("::")[0]
        return "ok" if head in self.corpora() else "gone"

    def commit(self, h):
        if h not in self._hashes:
            ok = False
            for cwd in (self.env.repo, self.env.research):
                if cwd and subprocess.run(["git", "cat-file", "-e", h + "^{commit}"], cwd=cwd,
                                          capture_output=True).returncode == 0:
                    ok = True
                    break
            self._hashes[h] = ok
        return "ok" if self._hashes[h] else "gone"

    def tokens(self, line):
        out = []
        seen = set()
        # The content rung needs to know WHICH symbol (or quote) belongs to WHICH citation, so both
        # sides of the line are collected before either is judged.
        cites = [(m.start(), m.end(), m.group(1), int(m.group(2)),
                  int(m.group(3)) if m.group(3) else None)
                 for m in CITE_RE.finditer(line) if not m.group(1).endswith(".md")]
        syms = [(m.start(), m.end(), m.group(1)) for m in SYMBOL_RE.finditer(line)
                if m.group(1).upper() != m.group(1) and m.group(1) not in STATUS_WORDS
                and not re.fullmatch(r"[0-9a-f]{7,40}", m.group(1))]
        quoted = {m.group("path"): m.group("quote") for m in LG.QUOTED_CITE.finditer(line)}
        for cb, ce, p, n, end in cites:
            if p in seen:
                continue
            seen.add(p)
            sym = self.pair_symbol(cites, syms, cb, ce)
            if sym and self.symbol(sym) != "ok":
                sym = None                      # an unresolvable symbol proves nothing about a line
            state, at = self.cite(p, n, end, quoted.get(p), sym)
            tok = "{}:{}".format(p, n) + ("-{}".format(end) if end else "")
            if at:
                tok += "->{}".format(at)        # the repair, named: where the cited thing now is
            out.append((tok, state))
        for m in PATHTOK_RE.finditer(line):
            p = m.group(1)
            if p in seen or "://" in p:
                continue
            # `atv_sync\.cpp` is a REGEX quoted in prose, not a Windows path: a backslash separator
            # is followed by a path segment, never by a dot. Without this the doc that quotes a
            # `git grep` pattern is charged a dead citation for the pattern -- measured once in the
            # corpus (a lesson quoting the very grep that proved its point), and the row would then
            # be answered NOT A LABEL, spending the label grammar's precision measure on a regex.
            if "\\." in p:
                continue
            if re.search(r"\.(?:h|hpp)/\.(?:cpp|c)$", p):      # the docs' `x.h/.cpp` shorthand = the header
                p = p.rsplit("/", 1)[0]
            seen.add(p)
            out.append((p, self.path(p)))
        for m in SYMBOL_RE.finditer(line):
            s = m.group(1)
            if s in seen or s.upper() == s or s in STATUS_WORDS:
                continue
            seen.add(s)
            if re.fullmatch(r"[0-9a-f]{7,40}", s) and re.search(r"\d", s):
                out.append((s, self.commit(s)))       # a backticked commit hash is a hash, not a symbol
                continue
            out.append(("`{}`".format(s), self.symbol(s)))
        for m in HASH_RE.finditer(line):
            h = m.group(1)
            if h in seen or not re.search(r"\d", h) or not re.search(r"[a-f]", h):
                continue
            seen.add(h)
            out.append((h, self.commit(h)))
        return out


def is_cite_tok(tok):
    """A CITATION is a path:line or a path -- what the doc's refusal names. A symbol (backticked)
    or a commit hash is an informational column: it may be a game/BP name outside every corpus,
    or an upstream hash, and neither can certify a claim false on its own."""
    return not tok.startswith("`") and "." in tok


DEAD_STATES = ("gone", "past-eof", "moved", "content-gone")
DRIFT_STATES = ("drift",)


def dead_cites(tokens):
    """`drift` is deliberately NOT here. It is the SYMBOL rung's advisory: strong enough to put the
    line in front of the hand, not strong enough to refuse a close, because its pairing is inferred
    (see `Resolver.content`). `moved` and `content-gone` come from the QUOTE rung, whose pairing is
    explicit, so they are dead like a vanished path."""
    return [(t, s) for t, s in tokens if s in DEAD_STATES and is_cite_tok(t)]


def drift_cites(tokens):
    return [(t, s) for t, s in tokens if s in DRIFT_STATES and is_cite_tok(t)]


def scan_doc(key, abspath, resolver, loose=False):
    lines = resolver.lines_of(abspath)
    if lines is None:
        return []
    return scan_lines(key, lines, resolver, loose)


def scan_text(key, text, resolver, loose=False):
    """Same scan over a text buffer -- used to read a doc's BASELINE version out of git, so a touched
    doc contributes only the rows this session introduced or changed (status_census, 2026-09-03).

    `VOCAB_MARKER` is handled per SECTION inside `scan_lines`, not per doc."""
    return scan_lines(key, text.split(chr(10)), resolver, loose)


def row_hash(key, line, dupes):
    """A row's identity: the DOC, the line's text, and -- only when the SAME text appears more than
    once in that doc -- its occurrence number. `[V]` 2026-09-03: `sha1(line.strip())` alone collided
    on FOUR of the corpus's 6,081 rows, so the verdict carry could copy one row's verdict onto a
    different row. The ordinal is scoped to IDENTICAL lines in ONE doc, so a unique line always hashes
    to occurrence 1 and never churns -- unlike a global ordinal, which round 14 measured moving on
    0.5-7% of untouched rows."""
    t = line.strip()
    dupes[t] = dupes.get(t, 0) + 1
    n = dupes[t]
    return sha1(key + chr(0) + t + (chr(0) + str(n) if n > 1 else ""))


def line_hashes(key, lines):
    """The identity of EVERY line in the doc, computed by exactly the procedure that identifies a ROW.

    Two things need this to be one function of one input. `retired_verdicts` asks "is the line this
    verdict named still in the file", and asking the ROW set instead answers a different question:
    `[V]` 2026-09-03 a `cite` row exists only while its citation resolves dead, so RESTORING a cited
    file makes the row vanish while the doc line is byte-identical -- and the verdict would be
    recorded as acted-on for a line nobody touched, the same bias the scope fix removed one commit
    earlier (DIFF pass, round 2 Q1).

    It also closes a latent one. The occurrence ordinal used to advance only when a ROW was created,
    so two identical lines that both produced rows were 1 and 2 -- and if the FIRST stopped producing
    one, the second silently became 1, changing its hash, losing its verdict AND recording it as
    retired. Advancing per LINE makes the ordinal a property of the text's position in the file, which
    is what it was always meant to be."""
    dupes, out = {}, []
    for l in lines:
        out.append(row_hash(key, l, dupes))
    return out


def scan_lines(key, lines, resolver, loose=False):
    if any(l.strip() == VOCAB_MARKER_DOC for l in lines):
        return []
    rows = []
    # ONE pass, up front: a row's hash is the hash of its LINE, so the ordinal cannot
    # depend on which lines happened to produce rows (see `line_hashes`).
    hashes = line_hashes(key, lines)
    in_fence = False
    quoting_vocab = False
    for i, line in enumerate(lines):
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        # The marker suppresses rows to the END OF ITS SECTION -- a legend or a verdict table lives
        # under its own heading, and a doc that quotes the vocabulary in ONE table still makes real
        # claims everywhere else (this arc holds 38 label rows and 3 quoted ones). Doc-level would
        # throw the claims away with the legend; the marker must also BE the line, so a doc that
        # merely mentions it in prose does not silently opt out.
        # A `[corr YYYY-MM-DD: ...]` stamp is a CORRECTION, never a claim, and its job is to NAME what
        # was wrong -- routinely including a path that is now gone. `[V]` 2026-09-03: the first stamp
        # written under this design came straight back as a LABEL row carrying `tools/inject.ps1=gone`,
        # which the close then refuses to verdict STILL TRUE: the census flagging its own remedy, the
        # shape docs/LESSONS.md already names for the accretion detector (which is why `CORR_RE`
        # exists and was already excluded THERE, at status_census.py:424, but not here).
        if not in_fence and CORR_RE.search(line):
            continue
        if not in_fence and line.strip() == VOCAB_MARKER:
            quoting_vocab = True
            continue
        if not in_fence and HEADING_RE.match(line):
            quoting_vocab = False
        if quoting_vocab:
            continue
        if in_fence:
            continue
        lab = label_of(line)
        toks = resolver.tokens(line) if (lab or CITE_RE.search(line) or PATHTOK_RE.search(line)
                                         or HASH_RE.search(line)) else []
        dead = dead_cites(toks)
        kind = None
        if lab:
            kind = lab[0]
        elif dead:
            kind = "cite"
        elif drift_cites(toks):
            # The symbol rung found the cited thing elsewhere in the cited file. That is not proof
            # (the pairing is inferred), so it does not join `cite` and cannot refuse -- it is its
            # own kind, and the hand answers it with a verdict like any other row.
            kind = "drift"
        elif loose and LOOSE_RE.search(line):
            kind = "loose"
        if not kind:
            continue
        nxt = lines[i + 1] if i + 1 < len(lines) else ""
        dates = DATE_RE.findall(line)
        rows.append({
            "key": key, "line": i + 1, "kind": kind, "label": lab[1] if lab else "",
            "substate": substate_of(line, nxt), "tokens": toks,
            "date": max(dates) if dates else "", "total": bool(TOTAL_RE.search(line)),
            "hash": hashes[i], "text": line.strip()[:160], "verdict": "",
        })
    return rows
