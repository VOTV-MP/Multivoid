#!/usr/bin/env python3
"""verify_proof.py -- the /qf critic's proof-of-read gate, made mechanical (docs/QF_ARC.md WP-3).

    verify_proof.py --qf "<fragment>" --opus "<fragment>" [--prior "<fragment>" [--exclude "<identity>"]...]
    verify_proof.py --reply reply.json [--exclude "<identity>"]...     # takes proofOfRead from a critic reply

Each fragment (4..12 words) must occur VERBATIM in its source, compared on text normalised for
whitespace and markdown (`*`, backticks, `_` stripped; dashes and quotes unified) so a fragment that
straddles a wrapped or bold line still matches.  A prior-art fragment must occur inside a ledger ROW
(docs/LESSONS.md + docs/security/LESSONS_SECURITY.md) whose identity is NOT in --exclude -- the brief's own
PRIOR ART list -- so a fragment copied from the brief proves nothing.

Exit 0 = every fragment found (each is printed with where).  Exit 1 = a miss (printed).  Exit 2 = usage.
The skill treats exit 1 as "discard the reply and re-spawn the critic".
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QF_DOC = ROOT / "docs" / "QUESTION_FORM_AGENT.md"
OPUS_DOC = ROOT / "docs" / "OPUS_48_DISCIPLINE.md"
LEDGERS = [ROOT / "docs" / "LESSONS.md", ROOT / "docs" / "security" / "LESSONS_SECURITY.md"]

_MD = re.compile(r"[*`_]")
_WS = re.compile(r"\s+")
_UNI = str.maketrans({"—": "-", "–": "-", "’": "'", "‘": "'", "“": '"', "”": '"',
                      " ": " "})


def normalise(text: str) -> str:
    return _WS.sub(" ", _MD.sub("", text.translate(_UNI))).strip().lower()


def word_count(fragment: str) -> int:
    return len(normalise(fragment).split())


def ledger_rows(path: Path):
    """Yield (identity, text) per lessons-ledger row.  A row starts at a line beginning `- **` or `**`
    and runs to the next such line or a header; identity = '<section>/<first 60 chars of the title>'.
    Shared shape with tools/qf/prior_art.py (QF_ARC WP-2)."""
    if not path.exists():
        return
    section = "0"
    title, buf = None, []
    for line in path.read_text(encoding="utf-8", errors="replace").split("\n"):
        if line.startswith("#"):
            if title is not None:
                yield f"{section}/{title[:60]}", "\n".join(buf)
                title, buf = None, []
            m = re.match(r"^##\s+(\d+)", line)
            if m:
                section = m.group(1)
            continue
        m = re.match(r"^(?:- )?\*\*(.+?)\*\*", line)
        if m:
            if title is not None:
                yield f"{section}/{title[:60]}", "\n".join(buf)
            title, buf = m.group(1).strip(), [line]
        elif title is not None:
            buf.append(line)
    if title is not None:
        yield f"{section}/{title[:60]}", "\n".join(buf)


def check_doc(label: str, fragment: str, path: Path) -> bool:
    n = word_count(fragment)
    if not (4 <= n <= 12):
        print(f"MISS {label}: fragment must be 4..12 words, got {n}: {fragment!r}")
        return False
    if not path.exists():
        print(f"MISS {label}: source not on disk: {path}")
        return False
    hay = normalise(path.read_text(encoding="utf-8", errors="replace"))
    if normalise(fragment) in hay:
        print(f"OK   {label}: found in {path.relative_to(ROOT)}")
        return True
    print(f"MISS {label}: not found verbatim in {path.relative_to(ROOT)}: {fragment!r}")
    return False


def check_prior(fragment: str, exclude: set[str]) -> bool:
    n = word_count(fragment)
    if not (4 <= n <= 12):
        print(f"MISS prior: fragment must be 4..12 words, got {n}: {fragment!r}")
        return False
    needle = normalise(fragment)
    hits, excluded = [], []
    for ledger in LEDGERS:
        for identity, text in ledger_rows(ledger):
            if needle in normalise(text):
                (excluded if identity in exclude else hits).append(identity)
    if hits:
        print(f"OK   prior: found in row {hits[0]!r}" + (f" (+{len(hits) - 1} more)" if len(hits) > 1 else ""))
        return True
    if excluded:
        print(f"MISS prior: fragment only matches rows the brief already listed: {excluded[:3]}")
    else:
        print(f"MISS prior: not found in any ledger row: {fragment!r}")
    return False


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qf", help="fragment from docs/QUESTION_FORM_AGENT.md")
    ap.add_argument("--opus", help="fragment from docs/OPUS_48_DISCIPLINE.md")
    ap.add_argument("--prior", action="append", default=[], help="fragment from a lessons-ledger row (repeatable)")
    ap.add_argument("--exclude", action="append", default=[], help="row identity the brief already listed (repeatable)")
    ap.add_argument("--reply", help="a critic reply JSON; its proofOfRead supplies --qf/--opus/--prior")
    a = ap.parse_args(argv)
    qf, opus, priors = a.qf, a.opus, list(a.prior)
    if a.reply:
        try:
            reply = json.load(sys.stdin if a.reply == "-" else open(a.reply, encoding="utf-8"))
        except (OSError, ValueError) as e:
            print(f"usage: cannot read reply: {e}")
            return 2
        p = reply.get("proofOfRead") or {}
        qf, opus = p.get("qfDoc", qf), p.get("opusDoc", opus)
        priors += [x.get("fragment", "") for x in p.get("priorArt", []) if isinstance(x, dict)]
    if not qf or not opus:
        ap.print_usage()
        print("usage: both --qf and --opus fragments are required (or --reply)")
        return 2
    ok = check_doc("qf", qf, QF_DOC)
    ok = check_doc("opus", opus, OPUS_DOC) and ok
    for frag in priors:
        ok = check_prior(frag, set(a.exclude)) and ok
    print("PROOF OK" if ok else "PROOF FAILED -- discard the reply and re-spawn the critic")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
