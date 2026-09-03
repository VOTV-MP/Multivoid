#!/usr/bin/env python3
"""public_leak_gate -- the leak class that bit TWICE in one session and no instrument ever saw.

WHY THIS EXISTS
---------------
2026-09-04. `docs/signals/HISTORY.md` was published carrying two security sessions, moved verbatim
out of the UNPUBLISHED `CLAUDE.md`; the move lane checked FIDELITY, which has no opinion about
publication. That was found by a hand-run pre-push audit. The audit's own fix was then applied to the
file it named -- and a second sweep of the same range found two more, in `docs/DOCUMENTIZE_ARC.md`
(an example table quoting an OPEN finding with its mechanism and the code range confirming it live)
and `docs/PERF_ARC.md` (an unfixed resource-exhaustion path stated in full, under text that said
"flag for the register" while nothing ever filed it).

Everything else in that arc got a gate. This class got a person, twice.

WHAT IT DOES NOT DO
-------------------
It does NOT decide. `[V]` 2026-09-04 every one of the six pre-existing hits tree-wide is BENIGN on
reading -- an architectural statement, a historical record of a fixed finding, a section-number
string, a feature gap phrased like a weakness ("a client can spend the group's money but not earn
it" is the ABSENCE of a capability). A gate that refused those would be wrong six times out of six
and would be switched off by whoever it annoyed. So it is an ACKNOWLEDGEMENT ratchet: every hit needs
a recorded human verdict in `public_leak_ack.txt`, and an UNACKNOWLEDGED hit fails.

FOUR SIGNALS, AND WHY FOUR
--------------------------
The first version had two, and was then tested against the three real incidents it was written for:
it caught ONE. That is the defect this whole arc keeps finding -- a fix that cannot catch its own
founding case -- so the signals were widened until each incident had one, and no further.

  S1  a citation into the LOCAL security tree carrying a LINE NUMBER (a coordinate). Naming the tree
      is public knowledge; `docs/DOCS_ARC.md` says publicly that it exists and why. 2 hits tree-wide.
      Catches incident 1.
  S2  a security finding ID within 50 characters of OPEN or MITIGATED -- "this specific weakness is
      live", which is `DOCS_ARC` WP-2's cut rule. 3 hits. Also catches incident 1.
  S3  a REFERRAL of something to the security register ("flag for ...security/"). 0 hits tree-wide,
      so it is free to keep armed. Catches incident 2, whose text referred itself to a register that
      never received it.
  S4  VERBATIM overlap with an unpublished tree, as a RATCHET rather than a list. Catches incident 3.

A wider net was considered and REJECTED: the mechanism phrases ("nothing validates", "no count cap")
that would have caught this session's leaks are the ATTACKER'S vocabulary, and the next leak will use
different words -- a gate tuned to the last incident is a site-list. So is a signal on a bare mention
of the register: 15 of the 24 tree-wide mentions are the harmless pointer, and a gate demanding 15
acknowledgements of public knowledge is one nobody keeps.
"""
import argparse
import io
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
ACK = os.path.join(HERE, "public_leak_ack.txt")

S1 = re.compile(r"(?:docs/)?security/[A-Za-z_0-9]+\.md:\d+")
S2 = re.compile(r"\b(?:A\d{1,2}|P\d|W\d{1,2}|F\d)\b[^|\n]{0,50}\b(?:OPEN|MITIGATED)\b")
# S3 -- a public doc REFERRING something to the security register. Incident 2's exact shape:
# `PERF_ARC.md` stated an unfixed exhaustion path in full and ended "flag for
# `docs/security/TRACKER.md`" -- and nothing ever filed it, so the public-bound doc held the ONLY
# copy of it. `[V]` 2026-09-04 this fires ZERO times tree-wide once that one is fixed, so it costs
# nothing to keep armed. A BARE mention of the register is deliberately NOT a signal: `docs/DOCS_ARC.md`
# already says publicly that the tree exists and why, and 15 of the 24 tree-wide mentions are that
# harmless pointer -- a gate demanding 15 acknowledgements of public knowledge is one nobody keeps.
S3 = re.compile(r"(?:flag (?:it )?for|file (?:it )?(?:as|under)|belongs in|should be (?:tracked|filed)"
                r"|track(?:ed)? (?:as|in))[^.\n]{0,60}security/", re.I)

# S4 -- VERBATIM overlap with an unpublished tree: incident 3, where 271 lines moved out of
# `CLAUDE.md` into a tracked file, checked for fidelity and never for publishability. It is a
# RATCHET, not an acknowledgement list. `[V]` 37 lines overlap today across 19 files and every
# sample read is benign (a shared command line, a quoted user sentence, a log excerpt), so per-line
# reasons would be 37 claimed reads nobody made. A count that may not GROW is the honest form.
OVERLAP_BASELINE = 37
# EXCLUDED because it is a deliberate practice, not a leak: copying memory topics into the public
# piles archive. It alone contributes ~1,263 of the raw 1,300 overlaps.
OVERLAP_SKIP = ("docs/piles/_archive/session-log/",)
OVERLAP_MIN = 60          # normalised characters; below this a shared line is a heading, not a claim


def load_ack(path=ACK):
    """One acknowledged hit per line as `<needle>` -- '#' starts a comment."""
    out = {}
    if not os.path.exists(path):
        return out
    for line in io.open(path, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if line:
            out[line] = True
    return out


def tracked_docs(repo=REPO):
    """Tracked docs and tools, EXCLUDING `*_drill.py`.

    A drill's fixtures are synthetic examples of the very thing the gate hunts, so scanning them
    makes the gate find its own test data and refuse. `lessons_gate_drill.py` already carries this
    lesson in its own words -- its first run in 2026-08-29 found "the symbol that must not exist" in
    itself and passed its own RED arm. Same class, same fix, and it bit here within minutes of the
    drill being written.
    """
    out = subprocess.run(["git", "ls-files", "*.md", "*.py", "*.yml"], cwd=repo,
                         capture_output=True, text=True)
    return [p for p in out.stdout.splitlines()
            if p.strip() and not os.path.basename(p).endswith("_drill.py")]


def scan(repo=REPO, paths=None):
    """-> [(path, lineno, signal, needle, line)] over the TRACKED tree as it stands.

    The tree, not a diff: a range scan answers "did this push add one", and the question that
    matters is "is one standing in the published tree". A rewrite can remove a blob from history;
    only this can tell you the file is clean now.
    """
    hits = []
    for rel in (paths if paths is not None else tracked_docs(repo)):
        full = os.path.join(repo, rel)
        if not os.path.isfile(full):
            continue
        try:
            text = io.open(full, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for n, line in enumerate(text.split(chr(10)), 1):
            for sig, rx in (("S1", S1), ("S2", S2), ("S3", S3)):
                for m in rx.finditer(line):
                    hits.append((rel, n, sig, m.group(0).strip(), line.strip()[:160]))
    return hits


def _norm(s):
    return re.sub(r"\s+", " ", re.sub(r"[`*_>#|\[\]]", "", s)).strip().lower()


def unpublished_lines(repo=REPO):
    """Normalised lines >= OVERLAP_MIN chars from the trees no repository tracks."""
    import glob as _glob
    mem = os.environ.get("MULTIVOID_MEMORY_DIR", "") or os.path.join(
        os.path.expanduser("~"), ".claude", "projects", "D--Projects-Programming-VOTV-MP", "memory")
    out = set()
    srcs = [os.path.join(repo, "CLAUDE.md")]
    srcs += _glob.glob(os.path.join(repo, "docs", "security", "*.md"))
    srcs += _glob.glob(os.path.join(mem, "*.md"))
    for p in srcs:
        if not os.path.isfile(p):
            continue
        for line in io.open(p, encoding="utf-8", errors="replace").read().split(chr(10)):
            n = _norm(line)
            if len(n) >= OVERLAP_MIN:
                out.add(n)
    return out


def overlap_count(repo=REPO):
    """-> (count, {path: n}), or (None, {}) when the unpublished corpora are absent.

    None is not zero. CI has neither `CLAUDE.md` (gitignored) nor the memory directory, so the
    signal reports the INSTRUMENT there rather than a false all-clear -- the same distinction
    `lessons_gate.absent_cite_roots` draws.
    """
    src = unpublished_lines(repo)
    if not src:
        return None, {}
    per = {}
    for rel in tracked_docs(repo):
        if not rel.endswith(".md") or rel.startswith(OVERLAP_SKIP):
            continue
        full = os.path.join(repo, rel)
        if not os.path.isfile(full):
            continue
        for line in io.open(full, encoding="utf-8", errors="replace").read().split(chr(10)):
            n = _norm(line)
            if len(n) >= OVERLAP_MIN and n in src:
                per[rel] = per.get(rel, 0) + 1
    return sum(per.values()), per


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split(chr(10) + chr(10))[0])
    ap.add_argument("--repo", default=REPO)
    ap.add_argument("--ack", default=ACK)
    ap.add_argument("--list", action="store_true", help="print every hit, acknowledged or not")
    args = ap.parse_args(argv)

    ack = load_ack(args.ack)
    hits = scan(args.repo)
    unack = [h for h in hits if h[3] not in ack]
    if args.list:
        for rel, n, sig, needle, line in hits:
            print("  [{}] {:<8} {}:{}  {}".format(
                "ack" if needle in ack else "NEW", sig, rel, n, needle))
    n_over, per_over = overlap_count(args.repo)
    over_bad = n_over is not None and n_over > OVERLAP_BASELINE
    print("public_leak_gate: {} hit(s), {} acknowledged, {} NEW; verbatim overlap with the "
          "unpublished trees: {}".format(
              len(hits), len(hits) - len(unack), len(unack),
              "n/a (corpora absent)" if n_over is None
              else "{} (baseline {})".format(n_over, OVERLAP_BASELINE)))
    if over_bad:
        print("")
        print("RATCHET: verbatim overlap GREW {} -> {}. A tracked doc now repeats a line that lives "
              "in an unpublished tree, which is what a MOVE looks like -- and a move out of an "
              "unpublished file into a tracked one is a PUBLICATION.".format(
                  OVERLAP_BASELINE, n_over))
        for k, v in sorted(per_over.items(), key=lambda kv: -kv[1])[:6]:
            print("    {:3d}  {}".format(v, k))
    if unack:
        print("")
        print("An UNACKNOWLEDGED coordinate into the local security tree, or a finding named as live,")
        print("is standing in the PUBLIC tree. Read each one and decide -- most are benign (an")
        print("architectural statement, a record of a FIXED finding). If it is, add the needle to")
        # `relpath` RAISES across Windows drive letters (`ValueError: path is on mount 'C:', start
        # on mount 'D:'`) -- found by the drill, which puts its scratch ack file under the user's
        # temp on C: while the repo is on D:. A crash inside the message that explains a refusal
        # turns a clear failure into a traceback.
        try:
            shown = os.path.relpath(args.ack, args.repo)
        except ValueError:
            shown = args.ack
        print("{} with its reason. If it is not, cut it per docs/DOCS_ARC.md WP-2".format(shown))
        print("and remember a tip fix does not un-publish: the blob lives in every later commit.")
        for rel, n, sig, needle, line in unack:
            print("")
            print("  {}:{}  [{}]  {}".format(rel, n, sig, needle))
            print("      {}".format(line))
        return 1
    if over_bad:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
