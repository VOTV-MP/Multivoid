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
CITE_RE = re.compile(r"([A-Za-z0-9_][A-Za-z0-9_/.\\-]*\.(?:h|hpp|cpp|c|inc|py|ps1|rs|json|md|txt|yml)):(\d+)")
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
        sources) -- the ledger gate's lessons_gate_allow_files.txt, one list for both instruments."""
        if not hasattr(self, "_ext"):
            self._ext = set(LG.load_list(LG.ALLOW_FILES))
        base = os.path.basename(path)
        return base in self._ext or path in self._ext

    def cite(self, path, line):
        resolved, hits = self.resolve(path)
        if not resolved:
            if self.external(path):
                return "external"
            return "ambiguous" if hits else "gone"
        lines = self.lines_of(resolved)
        if lines is None:
            return "gone"
        return "ok" if line <= len(lines) else "past-eof"

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
        for m in CITE_RE.finditer(line):
            p, n = m.group(1), int(m.group(2))
            if p.endswith(".md") or p in seen:
                continue
            seen.add(p)
            out.append(("{}:{}".format(p, n), self.cite(p, n)))
        for m in PATHTOK_RE.finditer(line):
            p = m.group(1)
            if p in seen or "://" in p:
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


def dead_cites(tokens):
    return [(t, s) for t, s in tokens if s in ("gone", "past-eof") and is_cite_tok(t)]


def scan_doc(key, abspath, resolver, loose=False):
    lines = resolver.lines_of(abspath)
    if lines is None:
        return []
    return scan_lines(key, lines, resolver, loose)


def scan_text(key, text, resolver, loose=False):
    """Same scan over a text buffer -- used to read a doc's BASELINE version out of git, so a touched
    doc contributes only the rows this session introduced or changed (status_census, 2026-09-03)."""
    return scan_lines(key, text.split(chr(10)), resolver, loose)


def scan_lines(key, lines, resolver, loose=False):
    rows = []
    in_fence = False
    for i, line in enumerate(lines):
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
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
            "hash": sha1(line.strip()), "text": line.strip()[:160], "verdict": "",
        })
    return rows
