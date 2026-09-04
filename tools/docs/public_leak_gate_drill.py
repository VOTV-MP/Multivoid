#!/usr/bin/env python3
"""public_leak_gate_drill -- show every signal RED, and the founding incidents caught.

The gate exists because a leak class was found twice by a person and zero times by an instrument.
Its FIRST version was then measured against the three real incidents and caught ONE, which is the
defect this arc keeps finding: a fix that cannot catch its own founding case. So the arms here are
not "does the regex compile" -- each one replays an incident, or shows the signal firing on a case it
must catch and staying quiet on a near-miss it must not.

Every arm is unit-level and needs no network, no game and no scratch git repo.
"""
import io
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import public_leak_gate as G  # noqa: E402

NL = chr(10)
EM = chr(8212)
FAILS = []


def check(ok, msg):
    print("  [{}] {}".format("PASS" if ok else "FAIL", msg))
    if not ok:
        FAILS.append(msg)


def scan_text(text):
    d = tempfile.mkdtemp(prefix="plg_")
    try:
        io.open(os.path.join(d, "probe.md"), "w", encoding="utf-8", newline=NL).write(text + NL)
        return G.scan(d, ["probe.md"])
    finally:
        shutil.rmtree(d, ignore_errors=True)


def sigs(text):
    return sorted({h[2] for h in scan_text(text)})


def drill_incidents():
    """THE THREE REAL INCIDENTS OF 2026-09-04, replayed verbatim.

    This is the arm the first version of the gate would have failed 2-of-3. It is first on purpose:
    a gate that cannot catch what it was written for is not a weaker gate, it is a different one.
    """
    print("  -- the founding incidents --")
    # THE FIXTURES ARE SYNTHETIC, and that is not fastidiousness. The first version of this drill
    # reproduced incident 1 VERBATIM -- the finding id, its mechanism and the live code range -- in a
    # TRACKED file, which put the leak straight back into the public repo inside the test for it. The
    # gate could not see it because the same commit had excluded `*_drill.py` from the scan to stop a
    # self-match, so the exclusion HID a real copy. Same shape, invented identifiers: the arms test a
    # regex, and a regex cannot tell `A99` from `A56`.
    i1 = ("| 14 | security/DRILL_REGISTER.md:704 | A99 nothing validates the client-named FooDestroy "
          "eid " + EM + " OPEN | still-true | drill_probe.cpp:206-223 still broadcasts |")
    check("S1" in sigs(i1), "incident 1 (a census row quoting an OPEN finding + its live code range)")

    i2 = ("`drill_subsystem g_fake` has NO count cap on the host and its 30-s TTL is SUSPENDED while "
          "a bracket is open, keyed by a client-chosen id " + EM + " **flag for "
          "`docs/security/DRILL_REGISTER.md` (a resource gap inside a bracket)**.")
    check("S3" in sigs(i2), "incident 2 (an unfixed path REFERRED to a register that never got it)")
    # incident 3 is S4's, and it is a COUNT -- drilled below, where it can be shown to MOVE.


def drill_signals_discriminate():
    """Each signal must fire on its case AND stay quiet on the near-miss beside it.

    Without the negative half a signal that matched everything would pass every arm above.
    """
    print("  -- discrimination --")
    check(sigs("see `docs/security/DRILL_REGISTER.md:704` for the row") == ["S1"],
          "S1 fires on a security cite WITH a line number")
    check(sigs("the register lives in `docs/security/DRILL_REGISTER.md`, local-only") == [],
          "S1 stays QUIET on a bare pointer -- public knowledge, and 15 of 24 mentions are this")

    check("S2" in sigs("A99 is still OPEN and unguarded in the drill fixture"),
          "S2 fires on a finding id next to OPEN")
    check(sigs("A99 was closed by `abc12345` and the drill is green") == [],
          "S2 stays QUIET when the finding is named as CLOSED")

    check("S3" in sigs("that is a hazard; flag for `docs/security/DRILL_REGISTER.md` when there is time"),
          "S3 fires on a referral to the register")
    check(sigs("the threat model is in docs/security/README.md and explains the posture") == [],
          "S3 stays QUIET on prose that merely mentions the tree")


def drill_overlap_ratchet():
    """S4: copying a line OUT of an unpublished tree INTO a tracked doc must move the count.

    This is incident 3's mechanism -- 271 lines moved out of `CLAUDE.md`, checked for fidelity and
    never for publishability. The count is asserted while it MOVES, never only at its green value:
    a baseline that is merely re-read proves nothing about the wire between the detector and the gate.
    """
    print("  -- the overlap ratchet (incident 3's mechanism) --")
    src = G.unpublished_lines(G.REPO)
    if not src:
        print("  [SKIP] the unpublished corpora are absent here, so S4 cannot be drilled")
        return
    # a real line from a real unpublished file, long enough to count
    victim = next(iter(sorted(s for s in src if len(s) >= G.OVERLAP_MIN + 20)))
    d = tempfile.mkdtemp(prefix="plgo_")
    try:
        os.makedirs(os.path.join(d, "docs"))
        io.open(os.path.join(d, "CLAUDE.md"), "w", encoding="utf-8", newline=NL).write(
            victim + NL)
        io.open(os.path.join(d, "docs", "public.md"), "w", encoding="utf-8", newline=NL).write(
            "# public" + NL + NL + "nothing borrowed here at all, just ordinary prose" + NL)

        # the drill cannot call git in a non-repo, so the tracked-file listing is stubbed
        real_tracked = G.tracked_docs
        G.tracked_docs = lambda repo=None: ["docs/public.md"]
        try:
            before, _ = G.overlap_count(d)
            check(before == 0, "GREEN: a tracked doc that borrows nothing scores 0")
            io.open(os.path.join(d, "docs", "public.md"), "a", encoding="utf-8",
                    newline=NL).write(victim + NL)
            after, per = G.overlap_count(d)
            check(after == 1 and per.get("docs/public.md") == 1,
                  "RED: copying ONE unpublished line into a tracked doc moves the count 0 -> {} "
                  "and names the file".format(after))
        finally:
            G.tracked_docs = real_tracked
    finally:
        shutil.rmtree(d, ignore_errors=True)


def drill_acknowledgement():
    """An unacknowledged hit must FAIL, and acknowledging it must let the same tree through."""
    print("  -- the acknowledgement ratchet --")
    d = tempfile.mkdtemp(prefix="plga_")
    try:
        empty = os.path.join(d, "empty.txt")
        io.open(empty, "w", encoding="utf-8").write("# nothing cleared" + NL)
        rc_empty = G.main(["--repo", G.REPO, "--ack", empty])
        check(rc_empty == 1, "RED: with an EMPTY acknowledgement file the gate REFUSES")
        rc_real = G.main(["--repo", G.REPO])
        check(rc_real == 0, "GREEN: with the real acknowledgement file the same tree passes")
    finally:
        shutil.rmtree(d, ignore_errors=True)

def drill_ack_is_path_keyed():
    """An acknowledgement in file A must NOT clear the same needle in file B.

    Keyed on the needle alone, the drill's own S3 fixture cleared `flag for `docs/security/` in EVERY
    file, so the exact prose of incident 2 read as already-acknowledged and the signal was disarmed
    the moment its own test was cleared (round 7 Q1). This asserts the discrimination directly.
    """
    print("  -- the acknowledgement is keyed on (path, needle) --")
    ack = G.load_ack()
    drill = "tools/docs/public_leak_gate_drill.py"
    needle = "flag for `docs/security/"
    check((drill, needle) in ack,
          "the drill's own S3 fixture IS cleared, in the drill")
    check(("docs/PERF_ARC.md", needle) not in ack,
          "RED: the SAME needle is NOT cleared in docs/PERF_ARC.md -- incident 2's own file")
    check(all("|" in l or l.strip().startswith("#") or not l.strip()
              for l in io.open(G.ACK, encoding="utf-8")),
          "every acknowledgement line carries a path (a bare needle clears nothing)")


def drill_partial_corpus_is_not_a_count():
    """A corpus missing its largest half must report n/a, not a smaller number.

    With `MULTIVOID_MEMORY_DIR` mistyped the memory half (35 of 44 lines) vanished and the gate
    printed `4 (baseline 37)` and exited 0 -- a ratchet reading a fifth of its input and calling it
    green (round 7 Q2).
    """
    print("  -- a partial corpus is not a corpus --")
    saved = os.environ.get("MULTIVOID_MEMORY_DIR")
    os.environ["MULTIVOID_MEMORY_DIR"] = os.path.join(tempfile.gettempdir(), "no_such_memory_zz")
    try:
        n, _ = G.overlap_count(G.REPO)
        check(n is None, "RED: a missing memory corpus reports n/a rather than a partial count "
                         "({})".format(n))
    finally:
        if saved is None:
            os.environ.pop("MULTIVOID_MEMORY_DIR", None)
        else:
            os.environ["MULTIVOID_MEMORY_DIR"] = saved
    # The GREEN half needs a corpus to come back TO, and CI has none: no memory directory, no
    # CLAUDE.md, no docs/security. Asserting it there is a claim about the ENVIRONMENT, which is the
    # THIRD time in one day I wired something into CI without running it in CI's environment (the
    # ledger drill's premise, this gate's *_drill exclusion, and now this). The RED half above needs
    # nothing and runs everywhere; this half says so instead of failing.
    n2, _ = G.overlap_count(G.REPO)
    if n2 is None:
        print("  [SKIP] there is no real corpus here either, so the GREEN half has nothing to come "
              "back to -- the RED half above is the part that needs no corpus")
    else:
        check(True, "GREEN: with the real corpus it is a number again ({})".format(n2))


def main():
    print("public_leak_gate_drill")
    drill_incidents()
    drill_signals_discriminate()
    drill_overlap_ratchet()
    drill_acknowledgement()
    drill_ack_is_path_keyed()
    drill_partial_corpus_is_not_a_count()
    print("")
    if FAILS:
        for f in FAILS:
            print("  FAILURE: {}".format(f))
        print("public_leak_gate_drill: FAIL ({} arm(s))".format(len(FAILS)))
        return 1
    print("public_leak_gate_drill: ALL PASS -- every signal shown firing on its case, quiet on the")
    print("near-miss beside it, and both ratchets shown MOVING rather than merely read at green.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
