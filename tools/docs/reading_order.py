#!/usr/bin/env python3
"""reading_order -- CLAUDE.md's reading order, its entries' DESTINATIONS, and what they already carry.

WHY THIS EXISTS. The reading order is the first thing a reset session opens, and it grows every close
because writing one more sentence there is always cheaper than finding the doc that should own it.
`ro-bytes` has been ratcheted since 2026-09-02 and the target is 58 KB; it stands at ~119 KB, and a
single entry (`4e.`, the signals log) is 24 KB of it -- a per-session changelog living inside an index.

Shrinking it by eye is how facts get lost. So the cut is MEASURED first: every entry names a
DESTINATION (the doc or directory in its first line), and this module reports what fraction of the
entry's distinctive clauses already appear there. A high-coverage entry is a summary of a doc that
says the same thing -- it can become a pointer. A low-coverage entry is the ONLY copy of its facts and
must be MOVED before it is cut, never cut first.

  covered   the clause's needle is already in the destination -- cutting it loses nothing
  missing   the clause exists ONLY here -- move it, then cut

THE EXEMPTION, stated computably before any cut: a line carrying `USER` together with a quotation
(`verbatim`, a quoted string, or the guillemets this project uses for Russian) is never moved and
never cut, because it is a record of what the user actually said and the destination doc is not where
they said it. It is reported separately so a shrink can be checked against it.

    python tools/docs/reading_order.py                 # the coverage table, largest entry first
    python tools/docs/reading_order.py --entry 4e      # every clause of one entry, with its verdict
"""
import argparse
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from status_grammar import read_text  # noqa: E402

NL = chr(10)
HEADING = "## Reading order after a session reset"
ENTRY = re.compile(r"^([0-9]+[a-z-]*)\. ")
DEST = re.compile(r"`((?:docs|research|src|include|tools)/[A-Za-z0-9_/.-]+)`")
# A quotation the user is recorded as making: the two ASCII forms and the guillemets used for Russian.
QUOTED = re.compile(r"verbatim|[«»“”]|\"[^\"]{8,}\"|\*\"")


def section(repo):
    t = read_text(os.path.join(repo, "CLAUDE.md")) or ""
    lines = t.split(NL)
    start = next((i for i, l in enumerate(lines) if l.startswith(HEADING)), None)
    return lines[start:] if start is not None else []


def entries(repo):
    """-> [(tag, [lines])] in document order."""
    out, cur, tag = [], None, None
    for l in section(repo):
        m = ENTRY.match(l)
        if m:
            if cur is not None:
                out.append((tag, cur))
            tag, cur = m.group(1), [l]
        elif cur is not None:
            cur.append(l)
    if cur is not None:
        out.append((tag, cur))
    return out


def destinations(repo, body):
    """The docs this entry points at: every backticked repo path in it that EXISTS. A directory
    contributes every `.md` in it -- `docs/signals/` is a destination the same way a file is."""
    out = []
    for p in dict.fromkeys(DEST.findall(NL.join(body))):
        full = os.path.join(repo, p)
        if os.path.isfile(full):
            out.append(p)
        elif os.path.isdir(full):
            for dirpath, dirnames, files in os.walk(full):
                dirnames[:] = [d for d in dirnames if d != "_archive"]
                out.extend(os.path.relpath(os.path.join(dirpath, f), repo).replace("\\", "/")
                           for f in files if f.endswith(".md"))
    return list(dict.fromkeys(out))


def norm(t):
    return " ".join(re.sub(r"[`*_>|\[\]]", " ", t).split()).lower()


def clauses(body):
    """The entry's distinctive units -> [(clause, needle, source line)].

    A clause is a sentence-ish run of >= 8 words; its NEEDLE is the normalized first 40 characters,
    specific enough that a match is the same claim and short enough to survive the destination
    rewording the tail. The SOURCE LINE travels with it because the `USER`+quotation exemption is a
    property of the line, not of the fragment: `[V]` 2026-09-03 the sentence splitter cuts
    `USER, verbatim: "…"` at the colon, so testing the fragment left the exemption protecting ZERO
    lines -- a guard that was on the page and off in the code."""
    out = []
    for line in body:
        for raw in re.split(r"(?<=[.;:])\s+", line):
            n = norm(raw)
            if len(n.split()) >= 8:
                out.append((raw.strip(), n[:40], line))
    return out


def coverage(repo, tag=None):
    """-> [(tag, bytes, lines, n_clauses, n_covered, n_exempt, [dests], [(clause, needle, found)])]."""
    rows = []
    for t, body in entries(repo):
        if tag and t != tag:
            continue
        dests = destinations(repo, body)
        blob = norm(NL.join(read_text(os.path.join(repo, d)) or "" for d in dests))
        cl = clauses(body)
        detail, covered, exempt = [], 0, 0
        for raw, needle, src in cl:
            if "USER" in src and QUOTED.search(src):
                exempt += 1
                detail.append((raw, needle, "exempt"))
                continue
            found = needle in blob
            covered += 1 if found else 0
            detail.append((raw, needle, "covered" if found else "missing"))
        rows.append((t, len(NL.join(body).encode("utf-8")), len(body), len(cl), covered, exempt,
                     dests, detail))
    return rows


def section_of(text):
    lines = text.split(NL)
    start = next((i for i, l in enumerate(lines) if l.startswith(HEADING)), None)
    return lines[start:] if start is not None else []


def moved_and_cut(repo, prev_text, now_text, extra_paths=()):
    """Clauses that LEFT the reading order since `prev_text`, split by where they went.

    A shrink is only good news if the facts went somewhere. `[V]` 2026-09-03 the reading order stood
    at 119 KB against a 58 KB target while NO entry's clauses were more than 11 % present in the doc
    it points at -- so it is not a redundant index that can be trimmed, it is the ONLY copy of most of
    what it says, and every byte of the ~60 KB owed is a MOVE. This is what tells the two apart:

      moved  the clause's needle is now findable in some doc under the repo -> it relocated
      cut    it is nowhere -> a claim was destroyed. Printed in full, never silently counted.
    """
    before = {n: raw for raw, n, _ in clauses(section_of(prev_text))}
    after = {n for _, n, _ in clauses(section_of(now_text))}
    gone = [(n, before[n]) for n in before if n not in after]
    if not gone:
        return [], []
    blob = []
    for dirpath, dirnames, files in os.walk(repo):
        dirnames[:] = [d for d in dirnames if d not in (".git", "node_modules", "build", "target")]
        for f in files:
            if f.endswith(".md"):
                blob.append(norm(read_text(os.path.join(dirpath, f)) or ""))
    for p in extra_paths:
        blob.append(norm(read_text(p) or ""))
    hay = NL.join(blob)
    moved = [(n, raw) for n, raw in gone if n in hay]
    cut = [(n, raw) for n, raw in gone if n not in hay]
    return moved, cut


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split(NL + NL)[0])
    ap.add_argument("--repo", default=os.path.dirname(os.path.dirname(HERE)))
    ap.add_argument("--entry", help="print every clause of ONE entry with its verdict")
    ap.add_argument("--top", type=int, default=20)
    args = ap.parse_args(argv)
    rows = coverage(args.repo, args.entry)
    if args.entry:
        for t, b, n, nc, cov, ex, dests, detail in rows:
            print("{}  {} B / {} lines  -> {}".format(t, b, n, ", ".join(dests) or "(no destination)"))
            for raw, needle, verdict in detail:
                print("  {:<8} {}".format(verdict, raw[:150]))
            print("  {} clause(s): {} covered, {} exempt, {} missing".format(
                nc, cov, ex, nc - cov - ex))
        return 0
    total = sum(r[1] for r in rows)
    print("reading order: {} entries, {} bytes".format(len(rows), total))
    print("{:<14} {:>7} {:>6} {:>8} {:>9} {:>7}  {}".format(
        "entry", "bytes", "lines", "clauses", "covered", "exempt", "destination(s)"))
    for t, b, n, nc, cov, ex, dests, _ in sorted(rows, key=lambda r: -r[1])[:args.top]:
        pct = "{:.0f}%".format(100.0 * cov / nc) if nc else "-"
        print("{:<14} {:>7} {:>6} {:>8} {:>9} {:>7}  {}".format(
            t, b, n, nc, pct, ex, (", ".join(dests)[:60] or "(none)")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
