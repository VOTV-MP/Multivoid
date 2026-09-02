"""Census of DECLARED-BUT-NEVER-CALLED public API in the Multivoid tree.

THE CLASS THIS EXISTS FOR: a capability whose plumbing is complete except for the
one call that would switch it on. It compiles, it reviews clean, it is documented,
and it never runs. Two shipped examples, both on the join path, both found
2026-09-02 and fixed in `fff4032b`:

  * `LobbyAnnouncer::SetPlayerCountFn` -- born 2026-06-07 in `8dd62916`, never
    called, so every lobby in the server browser reported "1/4" for three months
    regardless of who was in it.
  * `save_transfer::GetProgress` -- its own comment says "Download progress for the
    loading screen (bytes)", never called, so the longest stage of a real join
    rendered as an indeterminate "Connecting..." marquee.

See `docs/DEAD_CAPABILITY_REGISTER.md` for the confirmed list and the triage rules.

USAGE
    python tools/dead_api_census.py            # census + both self-tests
    python tools/dead_api_census.py --list     # census only, no self-test gate

TREAT THE RAW LIST AS TRIAGE, NOT TRUTH. It over-reports constructors, thunks and
inline one-liners. ALWAYS hand-validate a hit before acting on it:
    grep -rn "\\bNAME\\b" --include=*.cpp --include=*.h src/votv-coop/{src,include}
Two references (a declaration and a definition) means genuinely dead; three or more
means it has a caller this scanner's line classifier could not see.

THE SELF-TESTS ARE THE POINT, AND THERE ARE TWO OF THEM ON PURPOSE.
v1 of this script could not find `SetPlayerCountFn` (a one-line inline definition
in a header read as a call) -- an instrument blind to the very thing it hunts.
v2 found it and then reported SIX LIVE `Tick*` functions as dead, because
`subsystems.cpp` packs a scope guard, a walk timer and the call onto one line and
the prefix scored as a return type. RECALL alone would have passed v2. So the gate
asserts BOTH directions: it must FIND a known-dead name and must NOT flag a
known-live one.
"""
import re, pathlib, collections, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent / "src" / "votv-coop"
INC, SRC = ROOT / "include", ROOT / "src"

# Canaries. RE-POINT THESE when the named function's status legitimately changes --
# a canary that has been fixed makes the gate fail forever and trains you to ignore it.
CANARY_DEAD = "ClientArmed"        # save_transfer.h -- declared + defined, no callers
CANARY_LIVE = "TickClientNpcs"     # called from subsystems.cpp's packed one-liner

DECL = re.compile(r"([A-Za-z_]\w*)\s*\(")
KEYWORDS = {"if","while","for","switch","return","sizeof","catch","else","do",
            "case","new","delete","throw","co_return","static_assert","assert"}
SKIP = {"operator","explicit","virtual","template","typename","std","struct","class",
        "enum","union","namespace","public","private","protected","friend","using"}


def files(base, pats):
    for pat in pats:
        for p in base.rglob(pat):
            if "third_party" not in p.parts:
                yield p


def classify(line, name):
    """'declsite' or 'use' for an occurrence of NAME( on this line."""
    m = re.search(r"\b" + re.escape(name) + r"\s*\(", line)
    if not m:
        return None
    prefix = line[:m.start()]
    stripped = prefix.strip()
    first = stripped.split()[0] if stripped.split() else ""
    if first.rstrip("(") in KEYWORDS:
        return "use"
    # preceded by an expression context -> a call
    if any(t in prefix for t in ("=", "(", ".", "->", ",", "!", "&&", "||", "?", "+")):
        return "use"
    # ...or by a STATEMENT boundary. subsystems.cpp:575-581 packs three statements
    # onto one line; without this the calls there read as declarations and six live
    # Tick* functions get reported dead. CANARY_LIVE guards this branch.
    if any(t in prefix for t in (";", "{", "}")):
        return "use"
    if not stripped:
        return "use"                       # bare `Foo(...)` statement = unqualified call
    # `void Foo(` / `bool Session::Foo(` / `static inline int ns::Foo(`
    if re.search(r"[A-Za-z_][\w:<>\*&\[\]]*\s", prefix):
        return "declsite"
    return "use"                           # `ns::Foo(` with no return type = qualified call


def census():
    declared = {}
    for h in files(INC, ("*.h",)):
        try:
            lines = h.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for i, ln in enumerate(lines, 1):
            s = ln.strip()
            if not s or s.startswith(("//", "*", "/*", "#")):
                continue
            if not (s.endswith(";") or "{" in s):
                continue
            m = DECL.search(ln)
            if not m:
                continue
            name = m.group(1)
            if name in SKIP or name in KEYWORDS or len(name) < 4:
                continue
            if classify(ln, name) != "declsite":
                continue
            declared.setdefault(name, (h, i, s[:105]))

    rx = {n: re.compile(r"\b" + re.escape(n) + r"\s*\(") for n in declared}
    uses = collections.Counter()
    for p in list(files(SRC, ("*.cpp", "*.h"))) + list(files(INC, ("*.h",))):
        try:
            lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for ln in lines:
            s = ln.strip()
            if not s or s.startswith(("//", "*", "/*")):
                continue
            for name, r in rx.items():
                if r.search(ln) and classify(ln, name) == "use":
                    uses[name] += 1

    dead = sorted(((n, f.relative_to(ROOT), ln, txt)
                   for n, (f, ln, txt) in declared.items() if uses[n] == 0),
                  key=lambda r: str(r[1]))
    return declared, dead


def main():
    declared, dead = census()
    print(f"=== DECLARED BUT NEVER CALLED: {len(dead)} of {len(declared)} declared names ===\n")
    for name, f, ln, txt in dead:
        print(f"{f}:{ln}\n    {txt}")

    if "--list" in sys.argv:
        return 0

    names = {n for n, *_ in dead}
    recall = CANARY_DEAD in names
    precision = CANARY_LIVE not in names
    print("\n=== SELF-TEST (both directions -- see the module docstring) ===")
    print(f"  RECALL    -- known-dead '{CANARY_DEAD}' flagged:     "
          f"{'PASS' if recall else '*** FAIL: blind to its own target ***'}")
    print(f"  PRECISION -- known-live '{CANARY_LIVE}' NOT flagged: "
          f"{'PASS' if precision else '*** FAIL: over-reporting live code ***'}")
    return 0 if (recall and precision) else 1


if __name__ == "__main__":
    sys.exit(main())
