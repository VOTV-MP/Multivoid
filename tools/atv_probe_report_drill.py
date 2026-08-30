#!/usr/bin/env python3
"""atv_probe_report_drill -- prove the ACCEPTANCE section can FAIL.

An acceptance verdict that passes on the build it was written to reject measures
nothing (docs/LESSONS.md, "a verdict that already passes on the broken build").
The real b145 baseline logs cannot serve as the negative control here: they predate
the `owns=` field, so the parser matches nothing in them and the run comes back
INCONCLUSIVE rather than FAIL -- which is exactly the shape of a silent pass.

So the control is SYNTHESISED from the numbers the b145 run actually measured
(docs/vehicles/ATV.md 13): a mirror whose rig travelled 29.58 cm and whose copy
settled 109.9 cm from the host's. Each arm below must come back with the verdict
named; a green arm where RED is expected fails the drill.

    python tools/atv_probe_report_drill.py
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPORT = os.path.join(ROOT, "tools", "atv_probe_report.py")

HDR = "==== Multivoid log ====\n"
ARMED = "[19:54:40] [INFO ] atv: hit guard armed -- 7/7 ComponentHit delegates intercepted\n"


def line(t, n, key, driven, owns, x, y, z, fr, fl, bk):
    return ("[{}] [INFO ] [ATVP] n={} i=0 key='{}' driven={} owns={} occ=0x1 "
            "body=({:.1f},{:.1f},{:.1f}) rot=(0.0,0.0,0.0) "
            "susFR={:.3f} susFL={:.3f} susBK={:.3f} "
            "fuel=100.000 batt=100.000 dirt=0.0000 dirtVel=0.0000 hp=100.00\n"
            ).format(t, n, key, driven, owns, x, y, z, fr, fl, bk)


def write(path, rows, armed=True, counters=None):
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(HDR)
        if armed:
            fh.write(ARMED)
        for r in rows:
            fh.write(r)
        if counters:
            fh.write("[19:55:10] [INFO ] atv: OnDisconnect -- cleared 1 ATV(s); "
                     "hit guard: {} cancelled / {} allowed this session\n".format(*counters))


def stamp(sec):
    return "19:54:{:02d}".format(sec)


def run(host_rows, client_rows, host_armed=True, client_armed=True, counters=(3, 9)):
    d = tempfile.mkdtemp(prefix="atvdrill")
    h, c = os.path.join(d, "h.log"), os.path.join(d, "c.log")
    write(h, host_rows, host_armed, counters)
    write(c, client_rows, client_armed, counters)
    # --no-archive OR THE DRILL POISONS THE EVIDENCE. Without it every arm files its
    # 14-line synthetic fixture into research/atv_runs/ under a timestamp indistinguishable
    # from a real two-peer run: on 2026-08-30 four of the seven directories there were this
    # drill, and the only thing separating them from the runs the ATV design cites was a
    # tempdir path inside a MANIFEST nobody opens. An archive you cannot trust by its name
    # is not an archive.
    p = subprocess.run([sys.executable, REPORT, h, c, "--no-archive"],
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


# A DRIVEN rig BREATHES, and the fixture has to say so.
#
# Until 2026-08-30 `healthy()` emitted a mirror whose front wheels never moved at all
# (93.773 every sample) and whose back wheel moved 0.11 cm over the window -- i.e. the
# frozen corpse the whole mirror model exists to prevent -- and the drill called it the
# healthy control. It passed because the old A1 only had a CEILING: it could catch a mirror
# that travelled too much and was blind to one that did not travel at all. The fixture now
# breathes on both peers, in the same regime, at the amplitude a real driven run measured
# (2026-08-30: author 4.68/4.19/6.59 cm, mirror 6.81/5.50/4.12 cm over 20 driven seconds).
import math


def breathe(base, amp, i, phase=0.0):
    return base + amp * math.sin(i * 0.9 + phase)


def healthy(owns_host=1, owns_client=0, gap=0.3, mirror_amp=2.4, author_amp=2.2):
    """A run the v146 build should produce: host drives, client mirrors in the same regime."""
    hs, cs = [], []
    for i in range(12):
        t = stamp(40 + i)
        hs.append(line(t, i, "ATV", 1, owns_host, 100.0, 0.0, 0.0,
                       breathe(93.773, author_amp, i),
                       breathe(93.773, author_amp, i, 0.3),
                       breathe(71.914, author_amp * 1.4, i, 0.6)))
        cs.append(line(t, i, "ATV", 0, owns_client, 100.0 + gap, 0.0, 0.0,
                       breathe(93.773, mirror_amp, i, 0.15),
                       breathe(93.773, mirror_amp, i, 0.45),
                       breathe(71.914, mirror_amp * 1.4, i, 0.75)))
    return hs, cs


ARMS = []


def armline(t, phase, rest):
    return "[{}] [INFO ] [ATVP] ARM {}: {}\n".format(t, phase, rest)


def gates(driven=1, empty=0, brake=0, broken=0, uw=0, batt=100.0, fwd=1, torq=0.0, speed=0.0):
    return ("driven={} empty={} brake={} broken={} underwater={} batt={:.2f} fwd={} "
            "torq={:.3f} speed={:.2f}".format(driven, empty, brake, broken, uw, batt,
                                              fwd, torq, speed))


def arm_text(name, expect, rc_and_out, must_say):
    """Assert the VERDICT and the ATTRIBUTION.

    A run with no driven window is INCONCLUSIVE whatever went wrong, so the return code
    alone cannot tell "the arm was never enabled" from "the seat verb was refused". These
    arms exist because the report used to print one identical sentence for both, and a
    verdict that cannot name a subsystem sends the next session to rewrite working code
    (docs/LESSONS.md, "a counter you never print is not an instrument").
    """
    rc, out = rc_and_out
    got = {0: "PASS", 1: "FAIL", 2: "INCONCLUSIVE"}.get(rc, "rc=%d" % rc)
    ok = got == expect and must_say in out
    ARMS.append((name, expect, got, ok))
    why = "ok" if ok else ("DRILL FAIL (verdict)" if got != expect else "DRILL FAIL (no attribution)")
    print("  {:<34} expect {:<12} got {:<12} {}".format(name, expect, got, why))
    if not ok:
        print("      | wanted text: {!r}".format(must_say))
        print("\n".join("      | " + l for l in out.splitlines()[-25:]))


def arm(name, expect, rc_and_out):
    rc, out = rc_and_out
    got = {0: "PASS", 1: "FAIL", 2: "INCONCLUSIVE"}.get(rc, "rc=%d" % rc)
    ok = got == expect
    ARMS.append((name, expect, got, ok))
    print("  {:<34} expect {:<12} got {:<12} {}".format(name, expect, got, "ok" if ok else "DRILL FAIL"))
    if not ok:
        print("\n".join("      | " + l for l in out.splitlines()[-25:]))


def main():
    print("atv_probe_report_drill -- the acceptance must be able to fail\n")

    # CONTROL: a healthy v146 run passes. Without this the drill could "pass" by
    # rejecting everything.
    arm("control: healthy run", "PASS", run(*healthy()))

    # A1 RED -- the b145 mirror's measured rig travel (29.58 cm over the driven window).
    # Under the ratio rule this is ~13x the author's, far past REGIME_RATIO_MAX.
    hs, _ = healthy()
    cs = [line(stamp(40 + i), i, "ATV", 0, 0, 100.3, 0.0, 0.0,
               93.773, 93.773, 71.914 + 2.69 * i) for i in range(12)]
    arm("A1 RED: b145 rig travel 29.6cm", "FAIL", run(hs, cs))

    # A1 RED -- THE FROZEN CORPSE, which the pre-2026-08-30 acceptance could not detect at
    # all: a mirror whose wheel offsets never change while the ATV is being driven. This is
    # the failure docs/vehicles/ATV.md 11.1 was written to ask about, and it was the drill's
    # own "healthy" fixture until the ratio rule exposed it.
    hs, _ = healthy()
    cs = [line(stamp(40 + i), i, "ATV", 0, 0, 100.3, 0.0, 0.0,
               93.773, 93.773, 71.914) for i in range(12)]
    arm("A1 RED: mirror is a frozen corpse", "FAIL", run(hs, cs))

    # A2 RED -- the b145 settled gap: the release launched the copy and nothing
    # corrected it again.
    hs, cs = healthy(gap=109.9)
    arm("A2 RED: b145 settled gap 109.9cm", "FAIL", run(hs, cs))

    # A3 RED -- the guard never armed, so the lane should never have run at all.
    hs, cs = healthy()
    arm("A3 RED: hit guard never armed", "FAIL", run(hs, cs, client_armed=False))

    # A4 RED -- both peers claim the same ATV's tick: the single-syncer invariant broken.
    hs, cs = healthy(owns_client=1)
    arm("A4 RED: two owners in one second", "FAIL", run(hs, cs))

    # A5 RED -- the mirror trails the author by metres while the ATV moves, then catches up.
    # A2 cannot see this: both copies END together. This is the 2026-08-30 measurement
    # (max 437.9 cm) turned into a control, and it is the number a watching player sees.
    hs, cs = healthy()
    cs = [line(stamp(40 + i), i, "ATV", 0, 0, 100.0 + (0.0 if i >= 10 else 300.0), 0.0, 0.0,
               breathe(93.773, 2.4, i, 0.15), breathe(93.773, 2.4, i, 0.45),
               breathe(71.914, 3.4, i, 0.75)) for i in range(12)]
    arm("A5 RED: mirror trails 3 m while driving", "FAIL", run(hs, cs))

    # A6 RED -- the client authors, releases, and its OWN copy sinks 24 cm where it stands.
    # This is the 2026-08-30 measurement (23.3 / 24.2 / 39.8 cm across three runs) turned into
    # a control. A2 alone cannot produce this arm: A2 asks whether the copies END apart, and a
    # lane could pass it by drifting back together while still dropping the rig at every
    # handoff -- which is the shape a corrector that cuts every 10 s actually has. Asserted by
    # TEXT, because this fixture also fails A2 and a bare rc=1 would not prove A6 saw anything.
    def handoff(z_after_client, x_step=0.0, z_step=0.0):
        hs = [line(stamp(40 + i), i, "ATV", 0, 0 if i < 6 else 1,
                   100.0 + x_step * i, 0.0, 400.0 - z_step * i,
                   breathe(93.773, 2.2, i), breathe(93.773, 2.2, i, 0.3),
                   breathe(71.914, 3.1, i, 0.6)) for i in range(12)]
        cs = [line(stamp(40 + i), i, "ATV", 1 if i < 6 else 0, 1 if i < 6 else 0,
                   100.3 + x_step * i, 0.0,
                   400.0 - z_step * i - (0.0 if i < 6 else z_after_client),
                   breathe(93.773, 2.4, i, 0.15), breathe(93.773, 2.4, i, 0.45),
                   breathe(71.914, 3.4, i, 0.75)) for i in range(12)]
        return hs, cs

    arm_text("A6 RED: mirror sinks 24cm at handoff", "FAIL", run(*handoff(24.0)),
             "lost authority and its own copy sank")

    # A6 REST GUARD -- the same `owns` 1->0 edge, but the ATV is being DRIVEN AWAY down a
    # slope, so its Z change is the terrain and not a defect. Both copies stay together, so
    # every other arm passes and only the guard is under test: 15 cm per sample over the 3 s
    # window is a 45 cm "sag" that WOULD fail A6 outright, and 120 cm per sample of horizontal
    # travel is what tells the arm this is a peer driving off rather than a parked handoff.
    # Without the guard A6 would fail every run in which somebody mounts and drives away.
    arm_text("A6 guard: driving away is not a sag", "PASS",
             run(*handoff(0.0, x_step=120.0, z_step=15.0)),
             "skipped -- the ATV travelled")

    # INCONCLUSIVE -- nobody drove, so no mirror ever existed. This must NOT read as PASS:
    # an idle ATV is never mirrored, so a quiet run proves nothing about the corrector.
    hs = [line(stamp(40 + i), i, "ATV", 0, 1, 100.0, 0.0, 0.0, 93.773, 93.773, 71.914)
          for i in range(12)]
    cs = [line(stamp(40 + i), i, "ATV", 0, 0, 100.0, 0.0, 0.0, 93.773, 93.773, 71.914)
          for i in range(12)]
    arm("idle run is not a pass", "INCONCLUSIVE", run(hs, cs))

    # ATTRIBUTION 1 -- the seat verb was CALLED and REFUSED. Same verdict as a run where
    # the arm was never enabled at all; only the sentence distinguishes them, and the
    # difference decides whether the next session edits the ini or the game's gates.
    hs = [line(stamp(40 + i), i, "ATV", 0, 1, 100.0, 0.0, 0.0, 93.773, 93.773, 71.914)
          for i in range(12)]
    cs = [line(stamp(40 + i), i, "ATV", 0, 0, 100.0, 0.0, 0.0, 93.773, 93.773, 71.914)
          for i in range(12)]
    refused = hs + [armline(stamp(52), "sit",
                            "actionName(local=0x1, 'sit') on atv=0x2 -> called, try 3/3, driven=0")]
    arm_text("attrib: seat verb REFUSED", "INCONCLUSIVE", run(refused, cs),
             "the seat verb was CALLED and REFUSED")

    # ATTRIBUTION 2 -- the arm seated and drove, but the handbrake was still on, so the
    # torque block at @34866 bailed and the rig never moved. Without this the run reads as
    # a clean PASS while having exercised the corrector at rest -- the exact false-green
    # the 2026-08-29 baseline already showed (both peers agree to 0.3 cm when parked).
    hs, cs = healthy()
    hs = hs + [armline(stamp(52), "driving", gates(brake=1, torq=0.0)),
               armline(stamp(53), "driving", gates(brake=1, torq=0.0))]
    arm_text("attrib: seated but ZERO torque", "PASS", run(hs, cs),
             "produced ZERO torque")

    bad = [a for a in ARMS if not a[3]]
    print("\n{}/{} arms behaved as specified.".format(len(ARMS) - len(bad), len(ARMS)))
    if bad:
        print("DRILL FAIL")
        return 1
    print("DRILL PASS -- the acceptance rejects the b145 baseline on all four axes, catches a\n      FROZEN mirror as well as a wild one, and names WHY when the arm itself failed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
