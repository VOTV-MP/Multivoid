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
NLC = chr(10)

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

# Checks C and D (WP-4, 2026-09-03) need the memory corpus; their RED arms run only where it exists.
import lessons_gate as _LG  # noqa: E402
if os.path.isdir(_LG.MEMORY_DIR):
    ARMS += [
        ("dead wikilink",
         "- A row bounded by [[zz-no-such-lesson-{}]] as its prior art.\n".format(SENTINEL[:12].lower()),
         "DEAD WIKILINKS"),
        ("dead memory ref",
         "- A row whose detail lives in `memory/zz_no_such_file_{}.md`.\n".format(SENTINEL[:12].lower()),
         "DEAD MEMORY REFERENCES"),
    ]

# Classes the gate deliberately does NOT fail on -- assert it stays green for these,
# or the gate is too noisy to keep in CI and will be disabled by whoever it annoys.
QUIET = [
    ("git sha", "- Fixed in `a290a466`, follow-up `de249463`, revert `f03c04f0`.\n"),
    ("partial cite", "- The atlas GC is pressure-triggered; `DiscardBakes` runs then.\n"),
    # E: a running total is a WARN the gate must PRINT and must not FAIL on.
    ("running total", "- **A COUNT THAT ROTS** -- 3 of 5 sites were fixed (2026-09-03).\n"),
]
# The E arm must also be VISIBLE: the gate's output has to name it (a warning nobody prints is silent).
WARN_NEEDLE = ("running total", "3 of 5")



def drill_allowlist():
    """The allowlist's three new refusal paths, none of which a LEDGER INJECTION can reach.

    Round 4 added `allow_match`, `masking_entries` and `hpp_premise_holds` (+154 lines carrying three
    refusals) to a gate whose drill has only ledger-injection arms and which `[V]` nothing in
    `.github` ran at all. Round 5 then found two defects living in exactly that gap: the census and
    the ledger gate read ONE list with TWO matchers and disagreed about the same citation, and the
    premise check asked `git ls-files` (0) while the decider walked the filesystem (297). An unrun,
    unarmed sibling is how both got through (round 5 Q4).

    These arms are unit-level on purpose -- the gate subprocess costs ~24 s per call, and what needs
    proving here is a predicate, not a pipeline.
    """
    import os as _os
    import shutil as _shutil
    import tempfile as _tempfile
    import status_grammar as SG
    fails = []

    def check(ok, msg):
        print("  [{}] {}".format("PASS" if ok else "FAIL", msg))
        if not ok:
            fails.append(msg)

    # --- 1. ONE MATCHER: the census and the gate must answer the SAME thing about the same cite ---
    class _E(object):
        repo = REPO
    allow = set(_LG.load_list(_LG.ALLOW_FILES))
    res = SG.Resolver(_E())
    cases = ["trashBitsPile.hpp", "engine.hpp", "Engine.hpp", "udp.cpp", "UDP.CPP",
             "reflection.h", "zz_made_up_never_exists.cpp"]
    disagree = [c for c in cases if res.external(c) != _LG.allow_match(c, allow)]
    check(not disagree, "census and ledger gate agree on every allowlist case ({})".format(
        disagree or "7/7 agree"))
    # ...and the arm DISCRIMINATES: the retired exact-string predicate disagrees on three of them.
    old = [c for c in cases
           if (c in allow or _os.path.basename(c) in allow) != _LG.allow_match(c, allow)]
    check(len(old) >= 3, "RED control: the RETIRED exact-string predicate disagrees on {} of "
                         "them, so this arm can fail".format(len(old)))

    # --- 2. masking_entries: an entry naming a file we HAVE must refuse ---
    root = _tempfile.mkdtemp(prefix="lga_")
    saved_repo, saved_idx = _LG.REPO, _LG._BASENAME_INDEX
    try:
        _os.makedirs(_os.path.join(root, "src"))
        io.open(_os.path.join(root, "src", "real_thing.h"), "w").write("x" + NLC)
        _LG.REPO, _LG._BASENAME_INDEX = root, None
        check(_LG.masking_entries({"real_thing.h": "r"}) == ["real_thing.h"],
              "RED: an allowlist entry naming a file the repo HAS is reported as masking")
        check(_LG.masking_entries({"zz_absent_file.h": "r"}) == [],
              "GREEN: an entry naming a file the repo does NOT have is not masking")

        # --- 3. hpp_premise_holds must ask the FILESYSTEM over the trees we own ---
        _LG._BASENAME_INDEX = None
        ok, own = _LG.hpp_premise_holds()
        check(ok and not own, "GREEN: no .hpp under the trees we own -> the premise holds")
        io.open(_os.path.join(root, "src", "ours.hpp"), "w").write("x" + NLC)
        ok, own = _LG.hpp_premise_holds()
        check((not ok) and len(own) == 1,
              "RED: one .hpp under a tree we own BREAKS the premise -- the check the "
              "git-ls-files version could never make, since a submodule is never in the index")
    finally:
        _LG.REPO, _LG._BASENAME_INDEX = saved_repo, saved_idx
        _shutil.rmtree(root, ignore_errors=True)
    return fails


def symbol_check_available():
    """Does the gate's SYMBOL half run here at all?

    It does not when any corpus in `LG.CORPORA` is empty -- the gate says so itself ("The symbol
    check did not run") rather than reporting ~100 game symbols as dead. CI has no memory corpus, so
    the `dead symbol` arm cannot fire there, and asserting it anyway makes the drill red for a reason
    about the ENVIRONMENT rather than about the gate. Same distinction `absent_cite_roots` draws: a
    check whose corpus is missing reports the instrument, not the ledger.
    """
    for name, roots in _LG.CORPORA.items():
        if not any(os.path.isdir(r if os.path.isabs(r) else os.path.join(REPO, r))
                   for r, _exts in roots):
            return False, name
    return True, None


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

    # PREMISE CHECK, before any arm. Check A files a bare-basename citation that resolves nowhere as
    # "unverifiable" instead of DEAD whenever ANY cite root is missing -- a deliberate CI accommodation
    # whose own comment claims "with every root present this branch cannot be taken". On a full local
    # checkout that must be true, or every arm below is measuring a gate that cannot fail. It was NOT
    # true from the gate's first commit until 2026-09-03: CITE_ROOTS listed `include`, which has never
    # existed here, so the branch was taken on every run and the `dead file` arm silently passed.
    missing = _LG.absent_cite_roots()
    full = not missing
    print("  [{}] PREMISE  {:<16} absent cite roots={} (the LEDGER half needs a full checkout)".format(
        "PASS" if full else "SKIP", "cite roots", missing or "none"))
    if not full:
        # NOT a failure: the corpus is missing, not the ledger -- the distinction `absent_cite_roots`
        # itself draws. This drill was written for a full LOCAL checkout and then wired into CI on
        # 2026-09-04 after being tested with only the memory corpus absent; CI also has no
        # `research/` (local-only), so its premise failed and every corpus-dependent arm failed with
        # it, reporting the ENVIRONMENT as six defects in the gate. The ledger-injection half is
        # skipped where its corpus is incomplete; the unit-level half runs everywhere.
        print("  [SKIP] the ledger-injection arms, the QUIET checks and the CONTROL all need the "
              "cite roots {} -- without them a bare-basename citation resolves nowhere for the "
              "WRONG reason and the arms cannot discriminate. Run locally for that half."
              .format(missing))

    sym_ok, sym_missing = symbol_check_available()
    if full and not sym_ok:
        print("  [SKIP] the '{}' corpus is absent, so the gate's SYMBOL half does not run here; "
              "arms depending on it are SKIPPED rather than failed".format(sym_missing))
    for name, injection, needle in (ARMS if full else []):
        if name == "dead symbol" and not sym_ok:
            print("  [SKIP] {:<24} needs the symbol corpus".format(name))
            continue
        path = os.path.join(tmpdir, "ledger_red.md")
        io.open(path, "w", encoding="utf-8", newline="\n").write(base + "\n" + injection)
        code, out = run(path)
        ok = (code == 1) and (needle in out)
        print("  [{}] RED arm  {:<16} exit={} names-the-defect={}".format(
            "PASS" if ok else "FAIL", name, code, needle in out))
        if not ok:
            failures.append("RED arm '{}' did not fail for its own reason".format(name))

    for name, injection in (QUIET if full else []):
        path = os.path.join(tmpdir, "ledger_quiet.md")
        io.open(path, "w", encoding="utf-8", newline="\n").write(base + "\n" + injection)
        code, out = run(path)
        ok = code == 0
        if name == WARN_NEEDLE[0]:
            ok = ok and WARN_NEEDLE[1] in out       # quiet on exit code, LOUD in the output
        print("  [{}] QUIET    {:<16} exit={} (must not fail{})".format(
            "PASS" if ok else "FAIL", name, code, "; must WARN" if name == WARN_NEEDLE[0] else ""))
        if not ok:
            failures.append("QUIET check '{}' wrongly failed the gate or stayed silent".format(name))

    if full:
        code, _ = run(LEDGER)
        ok = code == 0
        print("  [{}] CONTROL  {:<16} exit={} (the real ledger)".format(
            "PASS" if ok else "FAIL", "unmodified", code))
        if not ok:
            failures.append("the real ledger does not pass")

    print("")
    print("  -- the allowlist predicates (unit-level; a ledger injection cannot reach them) --")
    failures += drill_allowlist()

    print("")
    if failures:
        for f in failures:
            print("  FAILURE: {}".format(f))
        print("\nlessons_gate_drill: FAIL")
        return 1
    if not full:
        print("lessons_gate_drill: ALL PASS (unit half only -- the ledger-injection half needs a "
              "full checkout, which this environment is not)")
        return 0
    print("lessons_gate_drill: ALL PASS -- the gate was shown RED on every defect class")
    print("it claims to catch, quiet on the classes it deliberately tolerates, and green")
    print("on the real ledger.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
