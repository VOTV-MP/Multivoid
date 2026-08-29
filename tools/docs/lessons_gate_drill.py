#!/usr/bin/env python3
"""lessons_gate_drill -- show the gate RED before trusting it green.

`memory/lesson_an_instrument_never_shown_failing_passes_by_construction.md`: a gate
that has only ever been observed passing has not been shown to detect anything. This
builds a synthetic ledger carrying one instance of each defect class the gate claims
to catch, runs the real gate against it (via --ledger), and asserts it FAILS for the
right reason -- then asserts the unmodified ledger still passes.

    python tools/docs/lessons_gate_drill.py
"""
import io
import os
import subprocess
import sys
import tempfile
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
GATE = os.path.join(HERE, "lessons_gate.py")
LEDGER = os.path.join(REPO, "docs", "LESSONS.md")

# The sentinel is MINTED AT RUNTIME, never written down. A literal here would live in
# tools/, which the gate tokenises as a corpus -- so the gate would find "the symbol that
# must not exist" in this very file and pass its own RED arm. That is not hypothetical:
# it is what the first run of this drill measured (2026-08-29). The gate also skips
# *_drill.py now, but a self-describing sentinel is the fix that cannot regress.
SENTINEL = "Zz" + uuid.uuid4().hex[:16] + "NotARealSymbol"

# (name, injected markdown, substring that MUST appear in the gate's failure output)
ARMS = [
    ("dead symbol",
     "- A row citing `{}` as the primitive to use.\n".format(SENTINEL),
     SENTINEL),
    ("dead file",
     "- A row citing `zz_no_such_file_at_all.cpp:12` for the call site.\n",
     "zz_no_such_file_at_all.cpp:12"),
    # reflection.h is AMBIGUOUS by basename (ours + abseil's). The first run of this
    # drill proved the gate skipped the line check on ambiguity, so this arm is aimed
    # at that hole on purpose, not merely at "a big number".
    ("line past EOF (ambiguous)",
     "- A row citing `reflection.h:999999` for the bitfield reader.\n",
     "reflection.h:999999"),
    ("line past EOF (unique)",
     "- A row citing `hud.cpp:999999` for the overlay predicate.\n",
     "hud.cpp:999999"),
]

# Classes the gate deliberately does NOT fail on -- assert it stays green for these,
# or the gate is too noisy to keep in CI and will be disabled by whoever it annoys.
QUIET = [
    ("git sha", "- Fixed in `a290a466`, follow-up `de249463`, revert `f03c04f0`.\n"),
    ("partial cite", "- The atlas GC is pressure-triggered; `DiscardBakes` runs then.\n"),
]


def run(ledger_path):
    proc = subprocess.run([sys.executable, GATE, "--ledger", ledger_path],
                          capture_output=True, text=True, cwd=REPO)
    return proc.returncode, proc.stdout + proc.stderr


def main():
    base = io.open(LEDGER, encoding="utf-8").read()
    failures = []
    tmpdir = tempfile.mkdtemp(prefix="lessons_drill_")

    print("lessons_gate_drill: {} arms + {} quiet checks + 1 control".format(
        len(ARMS), len(QUIET)))
    print("  sentinel this run: {}\n".format(SENTINEL))

    for name, injection, needle in ARMS:
        path = os.path.join(tmpdir, "ledger_red.md")
        io.open(path, "w", encoding="utf-8", newline="\n").write(base + "\n" + injection)
        code, out = run(path)
        ok = (code == 1) and (needle in out)
        print("  [{}] RED arm  {:<16} exit={} names-the-defect={}".format(
            "PASS" if ok else "FAIL", name, code, needle in out))
        if not ok:
            failures.append("RED arm '{}' did not fail for its own reason".format(name))

    for name, injection in QUIET:
        path = os.path.join(tmpdir, "ledger_quiet.md")
        io.open(path, "w", encoding="utf-8", newline="\n").write(base + "\n" + injection)
        code, _ = run(path)
        ok = code == 0
        print("  [{}] QUIET    {:<16} exit={} (must not fail)".format(
            "PASS" if ok else "FAIL", name, code))
        if not ok:
            failures.append("QUIET check '{}' wrongly failed the gate".format(name))

    code, _ = run(LEDGER)
    ok = code == 0
    print("  [{}] CONTROL  {:<16} exit={} (the real ledger)".format(
        "PASS" if ok else "FAIL", "unmodified", code))
    if not ok:
        failures.append("the real ledger does not pass")

    print("")
    if failures:
        for f in failures:
            print("  FAILURE: {}".format(f))
        print("\nlessons_gate_drill: FAIL")
        return 1
    print("lessons_gate_drill: ALL PASS -- the gate was shown RED on every defect class")
    print("it claims to catch, quiet on the classes it deliberately tolerates, and green")
    print("on the real ledger.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
