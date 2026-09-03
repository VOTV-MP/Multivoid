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
# WHO is being quoted. Case-INSENSITIVE, and this is the correction: the predicate used to be the
# substring "USER", which measures the presence of an uppercase token rather than a record of what the
# user said. `[V]` 2026-09-03: of the 60 lines in the reading order carrying a quotation, 8 were
# exempt and 14 are under this pattern -- and the six it adds include the user's own Russian rejection
# of the browser design and their hands-on verdict *"obs issue is gone, imgui gets captured in all
# modes possible"*, both introduced by a lowercase "the user" (DIFF pass, round 2 Q3). Two of the six
# are collateral from the neighbour window, and that is the right way to be wrong: over-exempting
# costs an explicit act to move a line, while under-exempting deletes the user's own words silently.
SPEAKER = re.compile(r"(?i)\buser'?s?\b")


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
    """The entry's distinctive units -> [(clause, needle, source line, exempt)].

    A clause is a sentence-ish run of >= 8 words; its NEEDLE is the normalized first 40 characters,
    specific enough that a match is the same claim and short enough to survive the destination
    rewording the tail.

    EXEMPT is computed HERE, once, so every consumer gets the same answer -- the earlier version left
    each caller to re-derive it and only one ever did (see `moved_and_cut`). Two corrections are baked
    in, both measured 2026-09-03: the test is on the SOURCE LINE, not the fragment, because the
    sentence splitter cuts `USER, verbatim: "…"` at its colon and testing the fragment left the
    exemption protecting ZERO lines; and the window includes the NEXT line, because in 3 of the
    reading order's entries (`4a-syncer`, `4e-imgui`, `4e-browser`) the word USER is on one line and
    the quotation it introduces is on the following one."""
    out = []
    for i, line in enumerate(body):
        nxt = body[i + 1] if i + 1 < len(body) else ""
        prv = body[i - 1] if i else ""
        # The window reaches BOTH ways, and the second half is not symmetry for its own sake: when the
        # quotation wraps, the clause that carries the user's actual WORDS is on the second line and
        # has no `USER` on it, so a forward-only window exempts the introduction and leaves the quote
        # itself unprotected -- which is the wrong half to lose.
        exempt = ((bool(SPEAKER.search(line)) and bool(QUOTED.search(line) or QUOTED.search(nxt)))
                  or (bool(QUOTED.search(line)) and bool(SPEAKER.search(prv))))
        for raw in re.split(r"(?<=[.;:])\s+", line):
            n = norm(raw)
            if len(n.split()) >= 8:
                out.append((raw.strip(), n[:40], line, exempt))
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
        for raw, needle, src, is_exempt in cl:
            if is_exempt:
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
      lost   it was EXEMPT and it left anyway -> a record of what the user said, gone.

    The third bucket is the one this function shipped without. The module's own header said a
    `USER`+quotation clause is "never moved and never cut", and the only consumer of that rule was
    `coverage()` -- which the close never calls, so at the one moment the rule could have been
    enforced nothing consulted it (DIFF pass, round 1 Q4). The exemption is not advice about what to
    trim; it is a class of line the reading order may not lose, so its departure is reported apart
    from both ordinary buckets and never counted as a successful move.
    """
    before = {n: (raw, ex) for raw, n, _, ex in clauses(section_of(prev_text))}
    after = {n for _, n, _, _ in clauses(section_of(now_text))}
    gone = [(n, before[n][0]) for n in before if n not in after]
    lost = [(n, raw) for n, raw in gone if before[n][1]]
    gone = [(n, raw) for n, raw in gone if not before[n][1]]
    if not gone and not lost:
        return [], [], []
    blob = []
    for dirpath, dirnames, files in os.walk(repo):
        # `_archive/` is NOT a destination. Archiving is retirement -- RULE 2's "retired info goes,
        # fully" -- so a clause that can only be found there was taken out of service, which is much
        # closer to a CUT than to a move, and counting it as MOVED would let the reading order be
        # emptied into the archive while the number said the facts had relocated.
        dirnames[:] = [d for d in dirnames
                       if d not in (".git", "node_modules", "build", "target", "_archive")]
        for f in files:
            if f.endswith(".md"):
                blob.append(norm(read_text(os.path.join(dirpath, f)) or ""))
    for p in extra_paths:
        blob.append(norm(read_text(p) or ""))
    hay = NL.join(blob)
    moved = [(n, raw) for n, raw in gone if n in hay]
    cut = [(n, raw) for n, raw in gone if n not in hay]
    return moved, cut, lost


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
