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
    p = subprocess.run([sys.executable, REPORT, h, c], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def healthy(owns_host=1, owns_client=0, bk_client=93.7, gap=0.3):
    """A run the v146 build should produce: host drives, client mirrors inside the band."""
    hs, cs = [], []
    for i in range(12):
        t = stamp(40 + i)
        hs.append(line(t, i, "ATV", 1, owns_host, 100.0, 0.0, 0.0, 93.773, 93.773, 71.914 + 0.01 * i))
        cs.append(line(t, i, "ATV", 0, owns_client, 100.0 + gap, 0.0, 0.0,
                       93.773, 93.773, bk_client + 0.01 * i))
    return hs, cs


ARMS = []


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
    hs, cs = healthy()
    cs = [line(stamp(40 + i), i, "ATV", 0, 0, 100.3, 0.0, 0.0,
               93.773, 93.773, 71.914 + 2.69 * i) for i in range(12)]
    arm("A1 RED: b145 rig travel 29.6cm", "FAIL", run(hs, cs))

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

    # INCONCLUSIVE -- nobody drove, so no mirror ever existed. This must NOT read as PASS:
    # an idle ATV is never mirrored, so a quiet run proves nothing about the corrector.
    hs = [line(stamp(40 + i), i, "ATV", 0, 1, 100.0, 0.0, 0.0, 93.773, 93.773, 71.914)
          for i in range(12)]
    cs = [line(stamp(40 + i), i, "ATV", 0, 0, 100.0, 0.0, 0.0, 93.773, 93.773, 71.914)
          for i in range(12)]
    arm("idle run is not a pass", "INCONCLUSIVE", run(hs, cs))

    bad = [a for a in ARMS if not a[3]]
    print("\n{}/{} arms behaved as specified.".format(len(ARMS) - len(bad), len(ARMS)))
    if bad:
        print("DRILL FAIL")
        return 1
    print("DRILL PASS -- the acceptance rejects the b145 baseline on all four axes.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
