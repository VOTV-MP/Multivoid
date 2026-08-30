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
import io
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


def resolve_cite(path):
    """A cite may be repo-relative or a bare basename. Return (abspath, ambiguous_hits)."""
    direct = os.path.join(REPO, path)
    if os.path.isfile(direct):
        return direct, []
    base = os.path.basename(path)
    hits = []
    for root in ("src", "include", "tools", "research", "reference"):
        full = os.path.join(REPO, root)
        if not os.path.isdir(full):
            continue
        for dirpath, dirnames, filenames in os.walk(full):
            dirnames[:] = [d for d in dirnames if d not in ("__pycache__", ".git")]
            if base in filenames:
                hits.append(os.path.join(dirpath, base))
                if len(hits) > 4:
                    return None, hits
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
        window = range(max(0, lineno - 26), min(len(lines), lineno + 25))
        if any(needle in norm(lines[i]) for i in window):
            continue                      # still where the ledger says it is
        elsewhere = [i + 1 for i, l in enumerate(lines) if needle in norm(l)]
        if elsewhere:
            moved.append((path, lineno, quote[:60], elsewhere[0]))
        else:
            dead.append((path, lineno, quote[:60], None))
    return moved, dead


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", action="store_true", help="list findings, always exit 0")
    ap.add_argument("--ledger", default=LEDGER)
    args = ap.parse_args()

    if not os.path.exists(args.ledger):
        print("lessons_gate: ledger not found: {}".format(args.ledger))
        return 1
    text = io.open(args.ledger, encoding="utf-8").read()
    allowed = load_list(ALLOW)
    allow_files = load_list(ALLOW_FILES)

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
    cites = sorted(set(CITE.findall(text)))
    for path, lineno in cites:
        if path in allow_files or os.path.basename(path) in allow_files:
            external.append((path, lineno))
            continue
        resolved, hits = resolve_cite(path)
        if resolved is None and not hits:
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
