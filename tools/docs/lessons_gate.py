#!/usr/bin/env python3
"""lessons_gate -- prove that docs/LESSONS.md still points at things that exist.

WHY THIS EXISTS
---------------
A lesson has two halves that rot at DIFFERENT rates. The TAKEAWAY is a statement about
the engine or about how to think; engines and reasoning do not move. The POINTER -- a
symbol name, a file.h:NNN cite -- is a statement about OUR tree, which moves weekly.
So a lesson can be simultaneously true and unusable, and nothing in the reading
experience distinguishes them: a confident takeaway lends its confidence to the dead
symbol beneath it. Worse, the DIG-RULE means the next session TRUSTS that pointer
instead of searching, so a lesson aimed at a dead symbol sends them on a WORSE dig
than no lesson at all.

This was found by ACCIDENT on 2026-08-29 -- docs/LESSONS.md told readers to use
FindBoolFieldBits (reflection.h:277-290); that symbol exists nowhere in the tree
(the real primitive is FindBoolProperty, reflection.h:299) and only a passing
citation in an unrelated design argument surfaced it. /documentize Step 0.5 asks for a
staleness sweep, but a manual instruction over a 5,600-line ledger is not a gate.

WHAT IT CHECKS
--------------
A) file:line citations   -- the file must exist and the line must be within it.
B) backticked symbols    -- the symbol must appear in at least one CODE corpus.
                            Docs are deliberately NOT a corpus: a doc mentioning a
                            symbol must never be what proves that symbol exists, or
                            the ledger validates itself and the gate is theatre.

Two allowlists, because the two checks fail for different legitimate reasons:
  lessons_gate_allow.txt        -- symbols outside every corpus (Win32/DX/CRT APIs,
                                   systemd directives, Unicode property names, IDA
                                   placeholders, doc titles, game classes absent from
                                   the dumped bytecode).
  lessons_gate_allow_files.txt  -- files cited but not in this repo (the UE4SS CXX
                                   header dump, UE4 engine source, upstream vendor
                                   sources read but never committed). Their LINE
                                   NUMBERS are unverifiable and are not checked.

USAGE
    python tools/docs/lessons_gate.py            # gate: exit 1 on any dead pointer
    python tools/docs/lessons_gate.py --report   # list everything, always exit 0
"""
import argparse
import fnmatch
import io
import subprocess
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LEDGER = os.path.join(REPO, "docs", "LESSONS.md")
_HERE = os.path.dirname(os.path.abspath(__file__))
ALLOW = os.path.join(_HERE, "lessons_gate_allow.txt")
ALLOW_FILES = os.path.join(_HERE, "lessons_gate_allow_files.txt")

# The auto-memory lives OUTSIDE the repo. Lessons cite sibling memory files by name,
# so it is a corpus like any other; override with MULTIVOID_MEMORY_DIR.
MEMORY_DIR = os.environ.get("MULTIVOID_MEMORY_DIR", "") or os.path.join(
    os.path.expanduser("~"), ".claude", "projects",
    "D--Projects-Programming-VOTV-MP", "memory")

# Code corpora only. docs/ is excluded ON PURPOSE -- see the module docstring.
CORPORA = {
    "ours":   [("src", (".h", ".hpp", ".cpp", ".c", ".inc", ".py", ".ps1", ".rs")),
               ("include", (".h", ".hpp", ".inc")),
               ("tools", (".py", ".ps1", ".rs", ".h", ".cpp", ".bat"))],
    "memory": [(MEMORY_DIR, (".md",))],
    "mta":    [("reference/mtasa-blue", (".h", ".cpp", ".hpp"))],
    "ue4ss":  [("reference/RE-UE4SS", (".hpp", ".cpp", ".h", ".lua"))],
    "game":   [("research/bp_reflection", (".json", ".txt")),
               ("research/pak_re", (".txt",))],
    "vendor": [("third_party", (".h", ".cpp", ".hpp")),
               ("src/votv-coop/third_party", (".h", ".cpp"))],
}

HEXISH = re.compile(r"^[0-9a-fA-F_]+$")
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
# a backticked run that looks like a code symbol: >=5 chars, optionally C++-qualified
SYMBOL = re.compile(r"`([A-Za-z_][A-Za-z0-9_]{4,}(?:::[A-Za-z_][A-Za-z0-9_]*)*)`")
# path/file.ext:NNN  (repo-relative or a bare basename)
CITE = re.compile(r"([A-Za-z0-9_/.\-]+\.(?:h|hpp|cpp|c|inc|py|ps1|rs|json)):(\d+)")


def tokens(text):
    return {t for t in IDENT.findall(text) if not HEXISH.match(t)}


def build_corpora(verbose=False):
    sets, counts = {}, {}
    for label, roots in CORPORA.items():
        ids, nfiles = set(), 0
        for root, exts in roots:
            full = root if os.path.isabs(root) else os.path.join(REPO, root)
            if not os.path.isdir(full):
                continue
            for dirpath, dirnames, filenames in os.walk(full):
                dirnames[:] = [d for d in dirnames
                               if d not in ("__pycache__", ".git", "node_modules")]
                for fname in filenames:
                    if not fname.endswith(exts):
                        continue
                    # A drill's synthetic sentinels are FIXTURES, not code. Tokenising
                    # them puts "the symbol that must not exist" into the corpus, and the
                    # gate then passes its own RED arm. (Measured 2026-08-29: the drill's
                    # dead-symbol arm went green for exactly this reason.)
                    if fname.endswith("_drill.py"):
                        continue
                    try:
                        text = io.open(os.path.join(dirpath, fname),
                                       encoding="utf-8", errors="replace").read()
                    except OSError:
                        continue
                    nfiles += 1
                    ids |= tokens(text)
                    # a memory/ lesson is cited by FILENAME, which is not inside the file
                    ids.add(os.path.splitext(fname)[0])
        sets[label] = ids
        counts[label] = nfiles
        if verbose:
            print("  corpus {:<7} files={:<6} identifiers={}{}".format(
                label, nfiles, len(ids), "   <-- ABSENT" if nfiles == 0 else ""))
    return sets, counts


def load_list(path):
    """One entry per line, '#' starts a comment. Returns {entry: reason}."""
    out = {}
    if not os.path.exists(path):
        return out
    for line in io.open(path, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        entry, _, reason = line.partition("#")
        entry = entry.strip()
        if entry:
            out[entry] = reason.strip() or "(no reason given)"
    return out


# Every entry must be a directory this repository ACTUALLY has, because absent_cite_roots() turns a
# missing one into a blanket "unverifiable" for bare basenames. `include` was in this tuple from the
# gate's first commit and has NEVER existed at the repo root (our headers live under
# src/votv-coop/include/, already covered by `src`; `git log -- 'include/*'` = 0 commits). So
# missing_roots was ALWAYS non-empty, the branch below that its own comment says "cannot be taken"
# locally was taken on every run, and no dead bare-basename citation could fail the gate on any
# machine -- measured 2026-09-03 when the drill's `dead file` arm was run for the first time since
# that branch was added and reported exit=0. Adding a root here without checking it exists disables
# half of check A silently.
CITE_ROOTS = ("src", "tools", "research", "reference", ".github")


def allow_match(path, allow_files):
    """Is this UNRESOLVABLE citation into an external corpus we deliberately do not ship?

    Entries match the full cite or its basename, CASE-INSENSITIVELY (a hand-written citation and a
    generated header disagree on case routinely -- `engine.hpp` vs `Engine.hpp` cost three false
    verdicts on 2026-09-03), and an entry containing a wildcard is an fnmatch PATTERN, so a corpus
    can be described by what it IS rather than by which of its files someone happened to cite.

    A pattern is only safe because the caller has already failed to resolve the path.
    """
    base = os.path.basename(path)
    for entry in allow_files:
        e = entry.lower()
        for cand in (path.lower(), base.lower()):
            if e == cand or (("*" in e or "?" in e) and fnmatch.fnmatch(cand, e)):
                return True
    return False


def masking_entries(explicit):
    """Explicit allowlist entries that DO resolve in this repo.

    An allowlist exists to excuse citations into a corpus we do not ship. An entry naming a file we
    DO have would silently excuse real rot in our own tree, which is why the entries are checked
    before `resolve_cite` only once this refusal exists to keep them honest.
    """
    return [e for e in explicit if resolve_cite(e)[0] is not None]


def hpp_premise_holds():
    """`*.hpp` stands in for the CXX dump ONLY while no `.hpp` we OWN sits in the search roots.

    The first version asked `git ls-files '*.hpp'` -- the INDEX -- and got 0, while the thing that
    actually decides whether the pattern is ever consulted is `_basename_index`, which walks the
    FILESYSTEM over `CITE_ROOTS`, where `[V]` 2026-09-04 there are **297** `.hpp` files. All 297 are
    vendored (`reference/mtasa-blue/vendor/unrar` 64, `reference/RE-UE4SS/.../LuaType` 34, ...) and a
    submodule's files are NEVER in `git ls-files`, so the check was green by construction and would
    have stayed green forever. Two witnesses, two trees, and the honesty check beside it
    (`masking_entries`) was already asking the filesystem through `resolve_cite` (round 5 Q3).

    So it asks the same tree, over the roots we own. The RESIDUAL, stated rather than hidden: a
    citation to a VENDORED `.hpp` that upstream renamed resolves nowhere and is then excused by the
    pattern as if it were a dump header. Resolve-first ordering keeps that to renamed-or-deleted
    vendored headers only -- every one still on disk resolves and never reaches the allowlist.
    """
    own = []
    for root in ("src", "tools"):
        full = os.path.join(REPO, root)
        if not os.path.isdir(full):
            continue
        for dirpath, dirnames, filenames in os.walk(full):
            dirnames[:] = [d for d in dirnames if d not in ("__pycache__", ".git")]
            own += [os.path.join(dirpath, f) for f in filenames if f.endswith(".hpp")]
    return (not own), own

def absent_cite_roots(roots=CITE_ROOTS):
    """Which of CITE_ROOTS this checkout does not have.

    `research/` is gitignored and `reference/` is a submodule CI deliberately never
    fetches ("reference/* never fetched", build-core.yml). A citation into either
    therefore resolves to NOTHING on CI and to a real file locally -- and until
    2026-09-01 the gate called that DEAD and failed the build. Ten MTA citations, none
    of them rot, none of them fixable by editing the ledger.

    This is the same distinction the SYMBOL half already draws with its `absent` corpus
    list: a check whose corpus is missing reports the instrument, not the ledger.
    """
    return [r for r in roots if not os.path.isdir(os.path.join(REPO, r))]


_BASENAME_INDEX = None


def _basename_index():
    """basename -> [abspath, ...] over CITE_ROOTS, walked ONCE.

    `resolve_cite` used to walk the roots per citation. `[V]` 2026-09-04: those roots hold ~53,000
    files (research 24.4k, reference 12.4k, src 11.2k, tools 5.0k) and the ledger carries ~1,500
    citations, so the gate spent 66 SECONDS doing the same walk over and over -- the shape
    `docs/PERF_ARC.md` records for `FindFunction` walking GUObjectArray per lookup. One walk and a
    dict lookup is the same answer at a cost that lets the gate actually be run.

    Order is preserved: the roots are visited in CITE_ROOTS order, so a caller comparing hit counts
    or taking hits[0] sees exactly what the per-call walk produced.
    """
    global _BASENAME_INDEX
    if _BASENAME_INDEX is None:
        idx = {}
        for root in CITE_ROOTS:
            full = os.path.join(REPO, root)
            if not os.path.isdir(full):
                continue
            for dirpath, dirnames, filenames in os.walk(full):
                dirnames[:] = [d for d in dirnames if d not in ("__pycache__", ".git")]
                for f in filenames:
                    idx.setdefault(f, []).append(os.path.join(dirpath, f))
        _BASENAME_INDEX = idx
    return _BASENAME_INDEX


def resolve_cite(path):
    """A cite may be repo-relative or a bare basename. Return (abspath, ambiguous_hits)."""
    direct = os.path.join(REPO, path)
    if os.path.isfile(direct):
        return direct, []
    hits = _basename_index().get(os.path.basename(path), [])
    # The old walk bailed out once it had FIVE, so the reported list is capped the same way: an
    # ambiguous cite is ambiguous, and the count past five carried no meaning to any caller.
    if len(hits) > 4:
        return None, hits[:5]
    if len(hits) == 1:
        return hits[0], []
    return None, hits


# A citation followed closely by a QUOTE of the cited text. Conservative on purpose: it fires
# only when the ledger actually quotes what the line says, which is where the strongest claims
# live and where a silent move does the most damage.
QUOTED_CITE = re.compile(
    r"`(?P<path>[A-Za-z0-9_./\\-]+\.(?:cpp|h|hpp|inc|py|ps1|rs|md|json|txt)):(?P<line>\d+)"
    r"(?:-\d+)?`"                       # file:line or file:line-line
    # ONLY the explicit quote-the-line form: `file:line` says/reads/states "...".
    # A looser gap matched prose that merely CONTAINED a quotation and produced six
    # false positives on the first run -- and a gate people learn to ignore is worse than
    # no gate. Narrow beats noisy: this fires on the rows making the strongest claims.
    r"\s+(?:says|said|reads|states|carries|records)\s+"
    r"[*_]{0,2}[\"“](?P<quote>[^\"”\n]{20,160})[\"”]")


def norm(t):
    return " ".join(t.split()).lower()


# A quoted fact WRAPS in source, so a per-line match cannot see it. `[V]` 2026-09-03, the first time
# a quoted citation outside this ledger was ever checked: `nick_color.h:3` carries "The COLOR AXIS
# has ONE owner: this module" across lines 3 and 4 -- the words are exactly where the doc says, and
# a per-line matcher called it content-gone. Joining is necessary but NOT sufficient: the second
# line starts with its own `//`, so the naive join reads "this // module" and misses anyway. The
# comment marker is punctuation of the medium, not of the sentence -- so it is stripped, for the
# same reason `norm` collapses whitespace. The claim is about the WORDS.
LEAD_COMMENT = re.compile(r"^\s*(?://+|#+|--|;+|\*+|/\*+)\s?")


def _joined(lines):
    return norm(" ".join(LEAD_COMMENT.sub("", l) for l in lines))


def quote_window(lines, lo, hi, pad=25):
    return _joined(lines[max(0, lo - pad - 1):hi + pad])


def find_quote(lines, needle, span=3):
    """-> the 1-based line where a joined run of `span` lines first contains the needle, or None."""
    for i in range(len(lines)):
        if needle in _joined(lines[i:i + span]):
            return i + 1
    return None


def check_quoted_cites(text):
    """-> (moved, dead) where each entry is (path, cited_line, quote, found_line_or_None).

    THE HOLE THIS CLOSES. Check A verifies only that a cited line is INSIDE the file, so any
    citation whose target moves but stays in the same file passes forever. On 2026-08-30 an
    extraction moved five cited facts out of atv_sync.cpp -- two of them into a different file
    entirely -- and the gate reported PASS on all five, in the same run that created the rot.
    A line number is a POSITION; the claim is about CONTENT, and only content can check it.
    """
    moved, dead = [], []
    for m in QUOTED_CITE.finditer(text):
        path, lineno, quote = m.group("path"), int(m.group("line")), m.group("quote")
        resolved, hits = resolve_cite(path)
        cand = resolved or (hits[0] if hits else None)
        if not cand:
            continue                      # check A already reports a dead path
        try:
            lines = io.open(cand, encoding="utf-8", errors="replace").read().split("\n")
        except OSError:
            continue
        # Match on a distinctive prefix: the ledger often elides the tail with "..." or trims.
        needle = norm(quote)[:48]
        if len(needle) < 20:
            continue
        if needle in quote_window(lines, lineno, lineno):
            continue                      # still where the ledger says it is
        elsewhere = find_quote(lines, needle)
        if elsewhere:
            moved.append((path, lineno, quote[:60], elsewhere))
        else:
            dead.append((path, lineno, quote[:60], None))
    return moved, dead


# ---- checks C / D / E (docs/DOCUMENTIZE_ARC.md WP-4, 2026-09-03) ----------------------------------
# The memory directory uses TWO slug conventions (lesson_x_y.md and lesson-x-y.md); a link under
# either must resolve. These checks need the memory corpus, which CI does not have: there they print
# UNVERIFIABLE and never fail -- the numbers travel in the Docs-Census trailer instead
# (wikilinks-dead / pairing-unref / pairing-dead), ratcheted by the close and by docs_census_gate.
WIKILINK = re.compile(r"\[\[([^\]|#]+)(?:[|#][^\]]*)?\]\]")
MEMREF = re.compile(r"memory/([A-Za-z0-9_\-.]+)\.md")
PAIRED_PREFIX = re.compile(r"^(lesson|feedback)[_-]")
RUNNING_TOTAL = re.compile(r"\b\d[\d,]* (?:rows|files|findings|of \d+)\b")
DATE = re.compile(r"20\d\d-\d\d-\d\d")


def _slug(s):
    return s.strip().replace("_", "-").lower()


def memory_slugs(memory_dir=MEMORY_DIR):
    """-> (set of normalised slugs, list of paired filenames) or (None, None) when the corpus is absent."""
    if not os.path.isdir(memory_dir):
        return None, None
    names = [f[:-3] for f in os.listdir(memory_dir) if f.endswith(".md")]
    return {_slug(n) for n in names}, [n for n in names if PAIRED_PREFIX.match(n)]


def check_wikilinks(text, memory_dir=MEMORY_DIR):
    """C: every [[slug]] resolves to a memory file under either convention. -> list of dead links, or None."""
    have, _ = memory_slugs(memory_dir)
    if have is None:
        return None
    dead = []
    for m in WIKILINK.finditer(text):
        link = m.group(1).strip()
        if not link or set(link) <= set(".<>"):
            continue                                   # the convention's own `[[...]]` placeholder
        if _slug(link) not in have and link not in dead:
            dead.append(link)
    return dead


def check_pairing(text, memory_dir=MEMORY_DIR):
    """D: the two-set diff of /documentize Step 3.5. -> (unreferenced memory files, dead references) or (None, None).

    A lesson/feedback file with no row in the ledger is INVISIBLE to the browsable digest; a row whose
    `memory/<slug>.md` points at no file has no detail. Both halves are reported; the trailer counts them."""
    have, paired = memory_slugs(memory_dir)
    if have is None:
        return None, None
    ledger_norm = _slug(text)
    unref = sorted(n for n in paired if _slug(n) not in ledger_norm)
    refs = {r for r in MEMREF.findall(text) if r != "<slug>"}
    dead = sorted(r for r in refs if _slug(r) not in have)
    return unref, dead


def check_running_totals(text):
    """E: a row carrying a running total ('N rows', 'N of M', 'N files') is a count that rots by
    construction (the ledger's own lesson). Listed with the row's date; a WARN, never PASS-silent."""
    out = []
    for line in text.split("\n"):
        if line.startswith("- **") and RUNNING_TOTAL.search(line):
            d = DATE.findall(line)
            out.append((d[0] if d else "undated", RUNNING_TOTAL.search(line).group(0), line[4:84]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", action="store_true", help="list findings, always exit 0")
    ap.add_argument("--ledger", default=LEDGER)
    ap.add_argument("--pairing", action="store_true",
                    help="print the memory<->ledger pairing diff (Step 3.5) as two lists and exit")
    args = ap.parse_args()

    if not os.path.exists(args.ledger):
        print("lessons_gate: ledger not found: {}".format(args.ledger))
        return 1
    text = io.open(args.ledger, encoding="utf-8").read()
    allowed = load_list(ALLOW)
    allow_files = load_list(ALLOW_FILES)

    # ---- checks C / D / E need no corpus build; --pairing is the Step 3.5 diff and exits here ----
    dead_links = check_wikilinks(text)
    unref, dead_refs = check_pairing(text)
    totals = check_running_totals(text)
    if args.pairing:
        if unref is None:
            print("pairing: UNVERIFIABLE here -- the memory corpus is absent ({})".format(MEMORY_DIR))
            return 0
        print("pairing: {} memory lesson/feedback files without a ledger row, {} ledger references "
              "without a file".format(len(unref), len(dead_refs)))
        for n in unref:
            print("   unreferenced   memory/{}.md".format(n))
        for r in dead_refs:
            print("   DEAD REFERENCE memory/{}.md".format(r))
        return 0

    print("lessons_gate: {} ({} lines)".format(args.ledger, len(text.splitlines())))
    sets, counts = build_corpora(verbose=True)
    # A corpus that is absent cannot testify. research/ is gitignored and the auto-memory
    # lives outside the repo, so on a fresh CI checkout the game-bytecode and memory
    # corpora are simply not there -- and every game BP class and every cited memory file
    # would be reported DEAD. That is not a finding, it is the instrument missing. Fail
    # CLOSED: skip check B entirely and say so, rather than emit ~100 false deaths that
    # would train everyone to ignore this gate. Check A still runs and is still useful.
    absent = [k for k, v in counts.items() if v == 0]

    # ---- check A: file:line citations ------------------------------------------------
    dead_cites, ambiguous, external = [], [], []
    unreachable = []                      # cites into a corpus this checkout does not have
    explicit_allow = {k: v for k, v in allow_files.items() if "*" not in k and "?" not in k}
    pattern_allow = {k: v for k, v in allow_files.items() if "*" in k or "?" in k}
    masked = masking_entries(explicit_allow)
    if masked:
        print("lessons_gate: FAIL -- {} allowlist entr(ies) name a file this repo DOES have, so they "
              "would mask real rot: {}".format(len(masked), ", ".join(sorted(masked)[:5])))
        return 1
    ok_hpp, tracked_hpp = hpp_premise_holds()
    if not ok_hpp and any("*.hpp" in e for e in allow_files):
        print("lessons_gate: FAIL -- the allowlist carries `*.hpp`, which stands in for the CXX "
              "dump only while this repo tracks no .hpp of its own. It now tracks {}: {}".format(
                  len(tracked_hpp), ", ".join(tracked_hpp[:5])))
        return 1
    missing_roots = absent_cite_roots()
    cites = sorted(set(CITE.findall(text)))
    # EXPLICIT entries short-circuit BEFORE `resolve_cite`, because resolving is a corpus walk and
    # doing it for every citation cost this gate 3s -> 69s when the order was simply inverted
    # (measured 2026-09-04, the fix for round 4 Q2 before it was made cheap). Masking is not a risk
    # for them: `masking_entries()` refuses at load time any explicit entry that DOES resolve here.
    # PATTERN entries are the ones that could mask, so they alone wait until resolution has failed.
    for path, lineno in cites:
        if allow_match(path, explicit_allow):
            external.append((path, lineno))
            continue
        resolved, hits = resolve_cite(path)
        # `[V]` this repo tracks ZERO `.hpp` files while the corpus cites dozens, so the four
        # hand-listed CXX headers were structurally guaranteed to rot: the census verdicted
        # `trashBitsPile.hpp` (never listed) and `engine.hpp` (listed as `Engine.hpp`, matched by
        # EXACT STRING) as dead-on-purpose -- which is how a rung's false-positive rate reads 0
        # while three of its ten rows are instrument error.
        if resolved is None and not hits and allow_match(path, pattern_allow):
            external.append((path, lineno))
            continue
        if resolved is None and not hits:
            # A BARE BASENAME that resolves nowhere, in a checkout that is MISSING one of
            # the search roots, is UNVERIFIABLE rather than dead -- the file may well be
            # sitting in the root that was not fetched. An explicit repo-relative path is
            # still dead, because its root is named and either present or not.
            #
            # LOCAL STRICTNESS IS UNCHANGED: with every root present `missing_roots` is
            # empty and this branch cannot be taken, so the full-corpus run still fails on
            # real rot. That matters -- this bucket must never become the place citations
            # go to stop being checked.
            if missing_roots and os.path.basename(path) == path:
                unreachable.append((path, lineno))
            else:
                dead_cites.append((path, lineno, []))
            continue
        # An ambiguous basename is reported, but its LINE is still checked -- against
        # every candidate. Skipping the check on ambiguity is how a `reflection.h:999999`
        # slips through, and reflection.h is exactly the file this gate was born from.
        candidates = [resolved] if resolved else hits
        counts = []
        for cand in candidates:
            try:
                counts.append(sum(1 for _ in io.open(cand, encoding="utf-8", errors="replace")))
            except OSError:
                counts.append(-1)
        if resolved is None:
            ambiguous.append((path, lineno, hits))
        if counts and all(c >= 0 and int(lineno) > c for c in counts):
            dead_cites.append((path, lineno, ["line past EOF in {}; longest is {} lines".format(
                "all {} candidates".format(len(counts)) if len(counts) > 1 else "the file",
                max(counts))]))

    # ---- check A2: a QUOTED citation must still find its quote near the line ----------
    moved_q, dead_q = check_quoted_cites(text)

    # ---- check B: backticked symbols -------------------------------------------------
    # A git SHA is not a symbol. The ledger cites commits constantly and they are all
    # hex, so filter them before anything else rather than allowlisting 36 of them.
    symbols = sorted(s for s in set(SYMBOL.findall(text))
                     if not (7 <= len(s) <= 40 and HEXISH.match(s)))
    dead_syms, partial_syms = [], []
    everything = set()
    for ids in sets.values():
        everything |= ids
    for sym in ([] if absent else symbols):
        if sym in allowed:
            continue
        tail = sym.split("::")[-1]
        if sym in everything or tail in everything:
            continue
        # The ledger sometimes cites a SUFFIX of the real name (NoLoadGlyphs for
        # ImFontFlags_NoLoadGlyphs). That is not rot -- the thing exists -- but it is a
        # weaker pointer than it could be, so report it and name the full symbol.
        full = [t for t in everything if t.endswith(tail) and len(t) > len(tail)]
        if full:
            partial_syms.append((sym, sorted(full, key=len)[:3]))
            continue
        dead_syms.append(sym)

    print("")
    print("citations: {} checked, {} cite allowlisted out-of-repo files "
          "(line numbers unverifiable)".format(len(cites), len(external)))
    if unreachable:
        print("           {} UNVERIFIABLE here -- absent search root(s): {}. Run locally "
              "for the full gate.".format(len(unreachable), ", ".join(missing_roots)))
    if absent:
        print("symbols:   CHECK SKIPPED -- corpus absent: {}".format(", ".join(absent)))
        print("           (research/ is gitignored; the auto-memory dir lives outside the")
        print("            repo. Without them a symbol check reports the instrument, not")
        print("            the ledger. Run this locally for the full gate.)")
    else:
        used_allow = sum(1 for x in symbols if x in allowed)
        print("symbols:   {} checked, {} matched the allowlist ({} entries)".format(
            len(symbols), used_allow, len(allowed)))

    if ambiguous:
        print("")
        print("AMBIGUOUS cites ({}) -- basename matches >1 file; reported, not failed:"
              .format(len(ambiguous)))
        for path, lineno, hits in ambiguous[:15]:
            rel = [os.path.relpath(h, REPO) for h in hits[:3]]
            print("   {}:{} -> {}".format(path, lineno, rel))

    if partial_syms:
        print("")
        print("PARTIAL citations ({}) -- the symbol exists only as a suffix of a longer"
              .format(len(partial_syms)))
        print("name; the ledger would point better if fully qualified. Not failed:")
        for sym, full in partial_syms[:20]:
            print("   {:<28} -> {}".format(sym, ", ".join(full)))

    bad = False
    if dead_cites:
        bad = True
        print("")
        print("DEAD CITATIONS ({}):".format(len(dead_cites)))
        for path, lineno, why in dead_cites:
            print("   {}:{}   {}".format(path, lineno,
                                         why[0] if why else "file does not exist"))
    if moved_q or dead_q:
        bad = True
        print("")
        print("MOVED/ROTTED QUOTED CITATIONS ({}) -- the line is inside the file, but what the"
              .format(len(moved_q) + len(dead_q)))
        print("ledger QUOTES is no longer there. Check A cannot see this: a line number is a")
        print("POSITION and the claim is about CONTENT.")
        for path, lineno, quote, found in moved_q:
            print("   {}:{}  -> now at :{}   \"{}...\"".format(path, lineno, found, quote))
        for path, lineno, quote, _ in dead_q:
            print("   {}:{}  -> NOT IN THAT FILE AT ALL   \"{}...\"".format(path, lineno, quote))
        print("")
        print("   A `-> now at :N` is the corrected line: re-cite it. A `NOT IN THAT FILE`")
        print("   means the fact moved to another file or is gone -- find it before re-citing.")

    if dead_syms:
        bad = True
        print("")
        print("DEAD SYMBOLS ({}) -- named in the ledger, present in no code corpus:"
              .format(len(dead_syms)))
        for sym in dead_syms:
            print("   {}".format(sym))
        print("")
        print("   Each is either (a) real rot -> fix the ledger row and re-cite the live")
        print("   symbol, or (b) legitimately external -> add it to")
        print("   tools/docs/lessons_gate_allow.txt with a one-line reason.")
        print("   Do NOT allowlist to silence real rot.")

    # ---- C / D / E reporting -----------------------------------------------------------
    print("")
    n_links = len(set(WIKILINK.findall(text)))
    if dead_links is None:
        print("wikilinks: {} found, UNVERIFIABLE here -- memory corpus absent".format(n_links))
        print("pairing:   UNVERIFIABLE here -- memory corpus absent (run locally, or read the "
              "Docs-Census trailer's wikilinks-dead / pairing-unref / pairing-dead)")
    else:
        print("wikilinks: {} checked, {} dead".format(n_links, len(dead_links)))
        print("pairing:   {} memory lesson/feedback files without a ledger row (--pairing lists them), "
              "{} ledger references without a file".format(len(unref), len(dead_refs)))
    if totals:
        print("running totals: {} row(s) carry a count that rots by construction -- WARN, never silent:"
              .format(len(totals)))
        for date, count, head in totals:
            print("   {:<10} {:<12} {}".format(date, count, head))
    if dead_links:
        bad = True
        print("")
        print("DEAD WIKILINKS ({}) -- [[slug]] with no memory file under either convention:".format(len(dead_links)))
        for l in dead_links:
            print("   [[{}]]".format(l))
    if dead_refs:
        bad = True
        print("")
        print("DEAD MEMORY REFERENCES ({}) -- a row points at a memory file that does not exist:".format(len(dead_refs)))
        for r in dead_refs:
            print("   memory/{}.md".format(r))

    if not bad:
        print("")
        if absent:
            print("lessons_gate: PASS (citations only) -- every cited file:line resolves. "
                  "The symbol check did not run.")
        else:
            print("lessons_gate: PASS -- every cited file:line resolves, every quoted "
                  "citation still says what the ledger claims, and every symbol exists.")
        return 0
    if args.report:
        print("")
        print("lessons_gate: --report, exiting 0")
        return 0
    print("")
    print("lessons_gate: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
