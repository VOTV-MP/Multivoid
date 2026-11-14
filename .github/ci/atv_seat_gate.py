#!/usr/bin/env python3
"""atv_seat_gate -- an ATV entry's seat, syncer and rider have one writer.

`AtvEntry::seatOccupant`, `seatSyncer`, `seatRider` and `seatRiderActor` are written only by
`coop/interactables/atv_seat_table.cpp` (SetSeat, SetRider), which tells the subscribers of every
change. A write from anywhere else would change the seat without announcing it. Over `src/votv-coop/src`
and `src/votv-coop/include` (comments and string literals set aside) this gate refuses an assignment (plain or compound) to
one of the four names outside the owner, except the default member initializers of `AtvEntry` in
`atv_sync_internal.h`.

  python .github/ci/atv_seat_gate.py                  # PASS/FAIL over the tree
  python .github/ci/atv_seat_gate.py --tree <dir>     # the same over <dir>/src/votv-coop
  python .github/ci/atv_seat_gate.py --drill          # the gate must fail each planted fault
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

OWNER = "src/coop/interactables/atv_seat_table.cpp"
DECLARING_HEADER = "include/coop/interactables/atv_sync_internal.h"

# A plain or compound assignment (=, |=, &=, ^=, +=, -=, *=, /=, %=); never == (an equality test).
WRITE = re.compile(r"\b(seatOccupant|seatSyncer|seatRider|seatRiderActor)\s*[|&^+\-*/%]?=[^=]")
DECLARATION_PREFIX = re.compile(r"^\s*(?:uint8_t|uint32_t|void\s*\*)\s*$")
SUFFIXES = (".cpp", ".h", ".hpp", ".inc")
HEX_DIGITS = frozenset("0123456789abcdefABCDEF")  # a set: the empty string at a text edge is not a member


def number_run(text, i):
    """Whether the word behind text[i] starts with a digit: a number (1, 700, 0xFF, 0xAB'CD'EF; the walk
    crosses the earlier separators), where a quote is a separator; case'a' and u8'a' are words, and
    their quote opens a character literal."""
    j = i
    while j > 0 and (text[j - 1].isalnum() or text[j - 1] in "_'"):
        j -= 1
    return text[j:j + 1].isdigit()


def strip(text):
    """The code of a C++ text, its comments and string and character literals blanked (newlines kept)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            i = j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c == "'" and text[i - 1:i] in HEX_DIGITS and text[i + 1:i + 2] in HEX_DIGITS and number_run(text, i):
            out.append(c)  # a C++14 digit separator (1'700'000'000), not the start of a character literal
            i += 1
        elif c == '"' or c == "'":
            m = re.match(r'([^()\\ ]{0,16})\(', text[i + 1:i + 19]) if c == '"' and text[i - 1:i] == "R" else None
            if m:
                end = text.find(")" + m.group(1) + '"', i)
                j = n if end < 0 else end + len(m.group(1)) + 2
            else:
                j = i + 1
                while j < n and text[j] != c:
                    j += 2 if text[j] == "\\" else 1
                j = min(n, j + 1)
            out.append("\n" * text.count("\n", i, j))
            out.append('""' if c == '"' else "''")
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def check(files):
    """files: {path relative to src/votv-coop, '/'-separated: text}. Returns the faults."""
    faults = []
    for path, text in sorted(files.items()):
        if path == OWNER:
            continue
        code = strip(text)
        for m in WRITE.finditer(code):
            line_start = code.rfind("\n", 0, m.start()) + 1
            if path == DECLARING_HEADER and DECLARATION_PREFIX.match(code[line_start:m.start()]):
                continue
            faults.append("%s:%d: a write of %s outside %s (SetSeat and SetRider announce the change)"
                          % (path, code.count("\n", 0, m.start()) + 1, m.group(1), OWNER))
    return faults


def tree_files(base):
    out = {}
    for sub in ("src", "include"):
        root = base / sub
        for p in sorted(root.rglob("*")):
            if p.is_file() and p.suffix in SUFFIXES:
                out[p.relative_to(base).as_posix()] = p.read_text(encoding="utf-8", errors="replace")
    return out


def drill():
    cases = {
        "a seat write in another file": {"src/coop/interactables/atv_body.cpp": "e.seatOccupant = 3;\n"},
        "a syncer write through a pointer": {"src/coop/x/a.cpp": "e->seatSyncer = who;\n"},
        "a rider write": {"src/coop/x/a.cpp": "e.seatRider = eid;\n"},
        "a rider actor write": {"src/coop/x/a.cpp": "e.seatRiderActor=actor;\n"},
        "a write in a header": {"include/coop/x/a.h": "inline void F(E& e) { e.seatSyncer = 0; }\n"},
        "a write after a digit separator": {
            "src/coop/x/a.cpp": "constexpr long k = 1'700'000'000;\ne.seatOccupant = 1;\n// it's\n"},
        "a write over two lines": {"src/coop/x/a.cpp": "e.seatRider\n    = 7;\n"},
        "a compound write": {"src/coop/x/a.cpp": "e->seatRider += 1;\n"},
        "a write in the declaring header that is no initializer": {
            DECLARING_HEADER: "uint8_t seatOccupant = 0xFF;\ninline void F(E& e) { e.seatOccupant = 1; }\n"},
        "a local named like the field": {"src/coop/x/a.cpp": "uint32_t seatRider = 5;\n"},
    }
    clean = {
        "an equality test": {"src/coop/x/a.cpp": "if (e.seatOccupant == 3 && e.seatSyncer == 0) {}\n"},
        "an inequality and an order test": {
            "src/coop/x/a.cpp": "if (e.seatRider != 0 || e.seatOccupant <= 2 || e.seatSyncer >= 1) {}\n"},
        "a read": {"src/coop/x/a.cpp": "const uint8_t s = e.seatSyncer; auto r = e->seatRider;\n"},
        "the owner writes": {OWNER: "e.seatOccupant = o; e.seatSyncer = s; e.seatRider = r;\n"},
        "the default member initializers": {
            DECLARING_HEADER: "uint8_t  seatOccupant = 0xFF;\nuint8_t  seatSyncer   = 0;\n"
                              "uint32_t seatRider    = 0;\nvoid*    seatRiderActor = nullptr;\n"},
        "a write in a comment": {"src/coop/x/a.cpp": "// e.seatOccupant = 1;\n/* seatRider = 2; */\n"},
        "a write in a string": {"src/coop/x/a.cpp": 'const char* s = "seatSyncer = 1";\n'},
        "the wire payload's own names": {"src/coop/x/a.cpp": "p.occupantSlot = 1; p.syncerSlot = 2;\n"},
    }
    bad = 0
    for name, files in cases.items():
        if not check(files):
            print("drill: the gate PASSED a planted fault: %s" % name)
            bad += 1
    for name, files in clean.items():
        if check(files):
            print("drill: the gate FAILED a clean file: %s" % name)
            bad += 1
    print("atv_seat_gate drill: %s (%d planted fault(s), %d clean case(s))"
          % ("PASS" if not bad else "FAIL", len(cases), len(clean)))
    return 1 if bad else 0


def main():
    if "--drill" in sys.argv:
        return drill()
    base = ROOT / "src" / "votv-coop"
    if "--tree" in sys.argv:
        base = pathlib.Path(sys.argv[sys.argv.index("--tree") + 1]) / "src" / "votv-coop"
    if not (base / "src").is_dir() or not (base / "include").is_dir():
        print("atv_seat_gate: FAIL (no source or include folder under %s)" % base)
        return 1
    files = tree_files(base)
    faults = check(files)
    for f in faults:
        print(f)
    print("atv_seat_gate: %s (%d file(s) read, %d fault(s))"
          % ("PASS" if not faults else "FAIL", len(files), len(faults)))
    return 1 if faults else 0


if __name__ == "__main__":
    sys.exit(main())
