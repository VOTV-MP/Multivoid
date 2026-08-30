#!/usr/bin/env python3
"""atv_probe_report -- read the [ATVP] lines from two peers and answer three questions.

The ATV probe (coop/dev/atv_probe.cpp, ini [dev] atv_probe=1) logs one line per ATV
per 500 ms sample on every peer. This turns two logs into the three numbers the C1
design is waiting on:

  Q1  Does the suspension actually BREATHE on a live ATV?  -- the spread of
      |wheel - body| over the run. That distance is rotation-invariant, so it isolates
      suspension travel from the body tipping or turning. A rigid frozen rig holds it
      constant to the bit. If it is flat on BOTH peers, "the mirror is a frozen corpse"
      is a weaker claim than the design assumes and P1's justification changes.

  Q2  Do two idle ATVs DIVERGE?  -- P4 says nobody streams an idle ATV and every peer
      ticks it, so fuel/battery/dirt drift apart. This measures the drift and the
      end-of-run gap.

  Q3  Do the two peers' ATVs even agree on WHERE the thing is?  -- body position gap.

Since v146 it ALSO asserts: an ACCEPTANCE section whose every threshold is set so the
b145 baseline FAILS it (docs/vehicles/ATV.md 13) -- rig travel inside the native band
while mirroring, the two copies settling together, the collision guard armed, and at
most one peer owning a given ATV's tick per second. Exit 0 PASS / 1 FAIL /
2 INCONCLUSIVE (nobody drove, so no mirror existed to measure).

    python tools/atv_probe_report.py                 # default HOST + CLIENT_1
    python tools/atv_probe_report.py <hostlog> <clientlog>
"""
import hashlib
import os
import re
import statistics
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_REL = os.path.join("WindowsNoEditor", "VotV", "Binaries", "Win64", "multivoid.log")
DEFAULT = [
    os.path.join(ROOT, "Game_0.9.0n_HOST", _REL),
    os.path.join(ROOT, "Game_0.9.0n_CLIENT_1", _REL),
]

STAMP = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\]")

LINE = re.compile(
    r"\[ATVP\] n=(?P<n>\d+) i=(?P<i>\d+) key='(?P<key>[^']*)' driven=(?P<driven>\d) "
    r"owns=(?P<owns>\d) occ=(?P<occ>\S+) "
    r"body=\((?P<bx>[-\d.]+),(?P<by>[-\d.]+),(?P<bz>[-\d.]+)\) "
    r"rot=\((?P<rp>[-\d.]+),(?P<ry>[-\d.]+),(?P<rr>[-\d.]+)\) "
    # OPTIONAL on purpose: the seven runs archived before 2026-08-30 have no vel= field, and a
    # required group would make every one of them unparseable -- silently, as zero samples.
    r"(?:vel=\((?P<vx>[-\d.]+),(?P<vy>[-\d.]+),(?P<vz>[-\d.]+)\) )?"
    # partZ = the four rig bodies' world Z. Also optional: pre-2026-08-30 runs have none.
    r"(?:partZ=\((?P<pfr>[-\d.]+),(?P<pfl>[-\d.]+),(?P<pbk>[-\d.]+)\) )?"
    r"(?:susFR=(?P<fr>[-\d.]+) susFL=(?P<fl>[-\d.]+) susBK=(?P<bk>[-\d.]+) |(?P<noparts>NOPARTS )?)"
    r"fuel=(?P<fuel>[-\d.]+) batt=(?P<batt>[-\d.]+) dirt=(?P<dirt>[-\d.]+) "
    r"dirtVel=(?P<dv>[-\d.]+) hp=(?P<hp>[-\d.]+)")


def parse(path):
    """-> {key: [sample dicts in order]}"""
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            m = LINE.search(raw)
            if not m:
                continue
            d = m.groupdict()
            rec = {k: (float(v) if v is not None else None)
                   for k, v in d.items() if k not in ("key", "occ", "noparts")}
            rec["key"] = d["key"]
            rec["occ"] = d["occ"]
            ms = STAMP.match(raw)
            rec["t"] = (int(ms.group(1)) * 3600 + int(ms.group(2)) * 60 + int(ms.group(3))
                        if ms else None)
            rec["parts"] = d["noparts"] is None and d["fr"] is not None
            # PRE-PLACEMENT GUARD. The ATV enters GUObjectArray before its components
            # are placed, and those samples read body=(0,0,0) with all three wheel
            # distances 0. Measured 2026-08-29: samples 1..15 of a 173-sample run. They
            # are not a rig at the origin, they are a rig that does not exist yet -- and
            # left in, they drag min to 0 and turn a static rig into a fake "BREATHES"
            # with sd=19.7. Drop them at the parser so no consumer can inherit the bug.
            if abs(rec["bx"]) < 1e-6 and abs(rec["by"]) < 1e-6 and abs(rec["bz"]) < 1e-6:
                continue
            out.setdefault(d["key"], []).append(rec)
    return out


def spread(vals):
    vals = [v for v in vals if v is not None]
    if len(vals) < 2:
        return None
    return {"n": len(vals), "min": min(vals), "max": max(vals),
            "range": max(vals) - min(vals),
            "sd": statistics.pstdev(vals)}


def fmt_spread(label, s):
    if s is None:
        return "    {:<8} (no data)".format(label)
    # 0.01 cm = 0.1 mm. Below that is float noise on a settled constraint rig
    # (a resting ATV measured 0.001 cm of travel over 80 s).
    verdict = ("STATIC (at rest / rigid)" if s["range"] < 0.01
               else "BREATHES {:.2f} cm".format(s["range"]))
    return ("    {:<8} n={:<4} min={:9.3f} max={:9.3f} range={:8.4f} sd={:7.4f}   {}"
            .format(label, s["n"], s["min"], s["max"], s["range"], s["sd"], verdict))


def report_peer(name, data):
    print("=" * 78)
    print("PEER: {}".format(name))
    if not data:
        print("  NO [ATVP] LINES -- probe off, no ATV found, or the run never reached gameplay.")
        return
    for key, rows in sorted(data.items()):
        driven = sum(1 for r in rows if r["driven"])
        print("  ATV key='{}'  samples={}  driven-samples={}  parts-resolved={}".format(
            key, len(rows), driven, sum(1 for r in rows if r["parts"])))
        print("  Q1 suspension travel  |wheel - body|, rotation-invariant:")
        for lab, fld in (("frontR", "fr"), ("frontL", "fl"), ("back", "bk")):
            print(fmt_spread(lab, spread([r[fld] for r in rows])))
        print("  Q2 vitals drift over the run:")
        for lab, fld in (("fuel", "fuel"), ("battery", "batt"), ("dirt", "dirt"), ("hp", "hp")):
            vals = [r[fld] for r in rows if r[fld] is not None]
            if not vals:
                continue
            print("    {:<8} first={:9.4f} last={:9.4f} delta={:+9.4f}".format(
                lab, vals[0], vals[-1], vals[-1] - vals[0]))


# ---------------------------------------------------------------------------
# ACCEPTANCE (v146, the simulate-and-correct mirror model). Every threshold below
# is chosen so the PREVIOUS build FAILS it -- a verdict that passes on the broken
# build measures nothing. The b145 baseline is the autonomous two-peer run of
# 2026-08-29 (commit ca64d098), recorded in docs/vehicles/ATV.md 13.
# A1's thresholds, and WHY they are not the resting band any more.
#
# 4.0 cm was measured on a PARKED ATV (docs/vehicles/ATV.md 13). Grading a DRIVEN mirror
# against it is a category error, and the 2026-08-30 run proved it: over a 20 s driven
# window the mirror travelled 6.81 / 5.50 / 4.12 cm and "failed", while the AUTHOR's own
# natively-driven rig -- the thing the mirror is supposed to resemble -- travelled
# 4.68 / 4.19 / 6.59 cm in the same run. A test the authority itself fails is not a test of
# the mirror.
#
# So A1 asks the two questions that actually matter, against the AUTHOR in the SAME window:
#   A1a NOT FROZEN  -- the original question (docs/vehicles/ATV.md 11.1). A rigid rig holds
#                      its wheel offsets to the bit: a resting one measured 0.001 cm, and the
#                      b145 freeze model produced a corpse. 1.0 cm sits an order of magnitude
#                      above that and below the 2-4 cm a merely PARKED rig breathes.
#   A1b SAME REGIME -- the mirror may breathe more than the author (it is being corrected),
#                      but not unboundedly. 2.5x is a DESIGN threshold, not a fitted one: it
#                      is stated here so a later run can move it with evidence rather than
#                      having it quietly track whatever was measured last.
# A5's ceiling. A2 compares the two copies only at the END of the run, so it is blind to
# what a watching player would actually see: how far the mirror TRAILS while the thing is
# moving. Measured 2026-08-30 over a 20 s driven window: mean 129.5 cm, max 437.9 cm.
#
# That is not an accident, it is the corrector's own threshold. atv_sync's warp fires past
# `kWarpBaseCm + kWarpPerSpeedS * |v|` = 200 + 0.5*|v| cm, which at ~480 cm/s is ~440 cm --
# so a 437.9 cm trail is INSIDE the tolerance and no warp ever fired. MTA's equivalent
# (CClientVehicle::UpdateTargetPosition:3867) is `15 + 10*|v|`: a small base and a large
# speed term, i.e. the opposite shape, tightening as the vehicle goes faster where ours
# stays flat.
#
# 150 cm is a stated DESIGN ceiling, not a fitted one: about one vehicle length, the point
# at which a passenger would see the other player's ATV in the wrong place.
#
# KNOWN-WEAK SHAPE, stated so nobody quotes an A5 verdict as a lane property. A second run
# the same night trailed max 70 cm and PASSED -- not because anything was fixed, but because
# the ATV happened to drive 33 m at ~780 cm/s instead of 78 m at ~1300 cm/s. The arm steers
# nothing, so route and speed are uncontrolled between runs, and a FIXED cm ceiling is partly
# measuring the route. The honest form is speed-scaled, like the warp it is grading; it is not
# written because nothing has yet measured what trail is ACHIEVABLE at speed. Until then A5 is
# a tripwire, not a metric: a FAIL is worth reading, a PASS proves less than it looks.
TRAIL_MAX_CM = 150.0
FROZEN_FLOOR_CM = 1.0
REGIME_RATIO_MAX = 2.5
BAND_CM = 4.0        # RESTING travel only -- reported for context, no longer a gate
SETTLE_GAP_CM = 20.0 # end-of-run body gap; baseline was 109.9 cm and PERMANENT
# A6's ceiling. MEASURED 2026-08-30: at the authority handoff the peer that LOSES
# authority drops 23.3 / 24.2 / 39.8 cm of Z within one 500 ms sample, in all three
# driven runs, while the peer that GAINS it rises ~6 cm. 10 cm fails all three and is
# still four times the rig's own 2-4 cm suspension travel, so a PASS means the drop is
# gone rather than merely smaller.
HANDOFF_SAG_CM = 10.0
# How long after the release the pair is allowed to take to settle. The coast-down measured
# 2026-08-30 was under 3 s; 8 s leaves room without reaching the next event.
HANDOFF_SETTLE_S = 20
MIN_MIRROR_SAMPLES = 6


ARM_LINE = re.compile(r"\[ATVP\] ARM (?P<phase>[a-z-]+):(?P<rest>.*)")
ARM_GATES = re.compile(
    r"driven=(?P<driven>\d) empty=(?P<empty>\d) brake=(?P<brake>\d) broken=(?P<broken>\d) "
    r"underwater=(?P<uw>\d) batt=(?P<batt>[-\d.]+) fwd=(?P<fwd>\d) torq=(?P<torq>[-\d.]+) "
    r"speed=(?P<speed>[-\d.]+)")


def arm_trace(logs):
    """Every [ATVP] ARM line across both logs, in file order.

    The arm is the only thing in this rig that can make the run MEAN anything -- an idle
    ATV is never mirrored -- so when the driven window is empty the arm's own trace is the
    evidence that says which of its steps did not happen. Without this, a run that fails
    for four different reasons prints one identical sentence.
    """
    out = []
    for path in logs:
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                for raw in fh:
                    m = ARM_LINE.search(raw)
                    if m:
                        out.append((os.path.basename(os.path.dirname(path)),
                                    m.group("phase"), m.group("rest").strip()))
        except OSError:
            pass
    return out


def arm_diagnosis(logs):
    """One sentence naming WHY there is no driven window, from the arm's own trace."""
    trace = arm_trace(logs)
    phases = {p for _, p, _ in trace}
    if not trace:
        return ("the arm left NO trace at all -- either [dev] atv_probe_sit is off on the "
                "host, or no peer ever reached world-ready (the arm waits for one, because "
                "a drive window with nobody mirroring measures nothing)")
    if "sit" in phases and not ({"drive-start", "driving"} & phases):
        refused = [r for _, p, r in trace if p == "sit"]
        return ("the seat verb was CALLED and REFUSED ({}) -- actionName('sit') is gated on "
                "|fallVeloc.Z|<800, empty hands, and an unoccupied playerHit "
                "[disasm @46420/@46522/@46645]".format("; ".join(refused[-2:])))
    if "pre-drive" in phases and "driving" not in phases:
        return "the player was seated but the drive never started -- check the pre-drive gates"
    return "the arm ran but no sample carried driven=1 -- check the sample cadence vs the window"


def arm_torque_note(logs):
    """If the arm drove, did the throttle actually produce torque? Name the blocker if not."""
    peak, blockers = 0.0, set()
    for _, phase, rest in arm_trace(logs):
        if phase not in ("driving", "drive-start", "drive-stop"):
            continue
        m = ARM_GATES.search(rest)
        if not m:
            continue
        peak = max(peak, abs(float(m.group("torq"))))
        for name, key in (("empty", "empty"), ("brake", "brake"), ("brokenn", "broken"),
                          ("underwater", "uw")):
            if m.group(key) == "1":
                blockers.add(name)
        if float(m.group("batt")) <= 0.0:
            blockers.add("battery<=0")
    if not peak and blockers:
        return ("the ATV was seated but produced ZERO torque -- the block at uber @34866 bails "
                "on {}".format(" | ".join(sorted(blockers))))
    if peak:
        return "peak torqAlpha while driving = {:.3f}".format(peak)
    return None


def driven_seconds(peers):
    """The set of wall-clock seconds in which SOME peer reported driven=1."""
    out = set()
    for d in peers:
        for rows in d.values():
            for r in rows:
                if r["driven"] and r["t"] is not None:
                    out.add(r["t"])
    return out


def acceptance(names, peers, logs):
    print("\n" + "=" * 78)
    print("ACCEPTANCE vs the b145 baseline (docs/vehicles/ATV.md 13)")
    fails, notes = [], []
    win = driven_seconds(peers)
    if win:
        print("  driven window: {} sample-second(s), {}..{}".format(len(win), min(win), max(win)))
        tq = arm_torque_note(logs)
        if tq:
            print("  arm: " + tq)
            if tq.startswith("the ATV was seated but produced ZERO torque"):
                notes.append("A1 caveat: " + tq + " -- the rig was authored but never MOVED, so "
                             "the corrector was still only exercised at rest")
    else:
        notes.append("NO driven=1 sample on either peer, so nothing below tested a MIRROR "
                     "(an idle ATV is never mirrored -- the lane is invisible at rest). "
                     "WHY: " + arm_diagnosis(logs))

    # A1 -- does the MIRROR's rig behave like the AUTHOR's while someone drives?
    # Both sides of the comparison come from the same run and the same seconds, so a
    # difference is a statement about the mirror rather than about the terrain, the
    # throttle, or which build measured the baseline.
    author = {}   # key -> {wheel: range} measured on whoever OWNED the tick in-window
    for nm, d in zip(names, peers):
        for key, rows in d.items():
            own = [r for r in rows if r["owns"] and r["parts"] and r["t"] in win]
            if len(own) < MIN_MIRROR_SAMPLES:
                continue
            for lab, fld in (("frontR", "fr"), ("frontL", "fl"), ("back", "bk")):
                sp = spread([r[fld] for r in own])
                if sp is not None:
                    author.setdefault(key, {})[lab] = (nm, sp["range"], sp["n"])

    for nm, d in zip(names, peers):
        for key, rows in sorted(d.items()):
            mir = [r for r in rows if not r["owns"] and r["parts"] and r["t"] in win]
            if len(mir) < MIN_MIRROR_SAMPLES:
                notes.append("A1 {}/{}: only {} mirror sample(s) inside the driven window -- "
                             "not enough to judge".format(nm, key, len(mir)))
                continue
            for lab, fld in (("frontR", "fr"), ("frontL", "fl"), ("back", "bk")):
                sp = spread([r[fld] for r in mir])
                if sp is None:
                    continue
                ref = author.get(key, {}).get(lab)
                frozen = sp["range"] < FROZEN_FLOOR_CM
                ratio = (sp["range"] / ref[1]) if (ref and ref[1] > 0) else None
                regime_bad = ratio is not None and ratio > REGIME_RATIO_MAX
                verdict = "FAIL" if (frozen or regime_bad) else "PASS"
                print("  A1 {:<9} {:<6} n={:<4} mirror={:7.3f} cm  author={:>9}  {:>7}  {}".format(
                    nm, lab, sp["n"], sp["range"],
                    "{:.3f} cm".format(ref[1]) if ref else "(none)",
                    "x{:.2f}".format(ratio) if ratio is not None else "-", verdict))
                if frozen:
                    fails.append("A1 {} {} mirror rig travelled only {:.3f} cm (< {:.1f}) while "
                                 "the ATV was driven -- a FROZEN rig".format(
                                     nm, lab, sp["range"], FROZEN_FLOOR_CM))
                elif regime_bad:
                    fails.append("A1 {} {} mirror rig travelled {:.2f} cm = {:.2f}x the author's "
                                 "{:.2f} cm (> {:.1f}x)".format(
                                     nm, lab, sp["range"], ratio, ref[1], REGIME_RATIO_MAX))
            if key not in author:
                notes.append("A1 {}/{}: no peer owned the tick for {}+ samples in the driven "
                             "window, so there is no author to compare against -- the FROZEN "
                             "floor still applied".format(nm, key, MIN_MIRROR_SAMPLES))

    # A2 -- the two copies END the run together. The baseline's gap was not merely
    # large, it was PERMANENT: the release launched the mirror and nothing corrected
    # it again. A settled gap is the direct test of that.
    if len(peers) > 1:
        for key in sorted(set(peers[0]) & set(peers[1])):
            # TIME-ALIGN THE PAIR. Taking each peer's own last sample compares two DIFFERENT
            # instants: on 2026-08-30 the client's log ended 70 s before the host's while the
            # host's ATV crept 0.27 cm/s the whole time, so the reported gap included drift the
            # client could never have mirrored. Cut both sides at the last second they share.
            ta, tb = peers[0][key][-1]["t"], peers[1][key][-1]["t"]
            if ta is None or tb is None:
                notes.append("A2 key='{}': unstamped samples -- cannot time-align, skipped"
                             .format(key))
                continue
            tEnd = min(ta, tb)
            pick = []
            for side in (peers[0][key], peers[1][key]):
                cand = [r for r in side if r["t"] is not None and r["t"] <= tEnd]
                pick.append(cand[-1] if cand else None)
            if pick[0] is None or pick[1] is None or \
               abs(pick[0]["t"] - tEnd) > 3 or abs(pick[1]["t"] - tEnd) > 3:
                notes.append("A2 key='{}': no sample pair within 3 s of the shared end t={} -- "
                             "one peer stopped logging early, skipped".format(key, tEnd))
                continue
            ra, rb = pick[0], pick[1]
            dx, dy, dz = ra["bx"] - rb["bx"], ra["by"] - rb["by"], ra["bz"] - rb["bz"]
            gap = (dx * dx + dy * dy + dz * dz) ** 0.5
            ok = gap <= SETTLE_GAP_CM
            print("  A2 settled gap key='{}' {:.3f} cm  {}".format(key, gap, "PASS" if ok else "FAIL"))
            if not ok:
                # ATTRIBUTE the failure. RETRACTED 2026-08-30: this branch used to conclude that
                # a gap the corrector CUT repeatedly, at a constant distance, meant "the two
                # peers' WORLDS differ under the vehicle and no pose lane can fix it". That was
                # measured FALSE. The logs show the cut LANDING -- run 1 client, 00:21:06, body
                # Z 5405.2 -> 5430.5 the instant after the cut -- and the rig falling back to the
                # identical Z within the next 500 ms sample, every time. The mirror is pulled
                # back down by this lane's own per-packet root-velocity write, not held down by
                # its world. A repeated cut at a constant distance is therefore evidence FOR a
                # lane defect, not against one. See A6, which names the moment it starts.
                cuts = []
                for path in logs:
                    try:
                        cuts += re.findall(r"correction stalled at ([\d.]+) cm", 
                                           open(path, encoding="utf-8", errors="replace").read())
                    except OSError:
                        pass
                vals = sorted({round(float(c), 1) for c in cuts})
                if len(cuts) >= 2 and len(vals) == 1 and abs(vals[0] - gap) < 1.0:
                    fails.append("A2 key='{}' settled {:.1f} cm apart, and the corrector CUT to "
                                 "the authority's pose {} times, each time landing and each time "
                                 "falling back to the same {:.1f} cm. The cut works; something in "
                                 "THIS lane pulls the mirror back down after it. Read A6."
                                 .format(key, gap, len(cuts), vals[0]))
                else:
                    fails.append("A2 key='{}' settled {:.1f} cm apart > {:.1f}".format(
                        key, gap, SETTLE_GAP_CM))

    # A6 -- THE AUTHORITY HANDOFF MUST NOT OPEN A GAP. A2 says the two copies end the run
    # apart; this says WHEN the gap is born, which is the half A2 cannot see.
    #
    # MEASURED 2026-08-30, four driven runs: the instant a peer stops owning the tick its own
    # copy drops 23-40 cm of Z within one 500 ms sample while the peer that TAKES ownership
    # rises ~6 cm. Both peers lose the occupant at that same moment, so occupancy cannot
    # explain opposite signs -- the variable whose sign tracks the direction is which peer is
    # being corrected.
    #
    # WHAT THIS MEASURES, AND WHY IT IS THE PAIR AND NOT ONE COPY. The first version of this
    # arm graded one peer's own Z before vs after, and needed a guard to skip the handoff where
    # a peer mounts and drives away -- because there the Z change is the terrain being crossed.
    # That guard then SKIPPED the case the arm exists for: a rig coasting to a stop travelled
    # 68 cm, over a 50 cm threshold picked from the few cm a SETTLED rig creeps. A threshold
    # fitted between two observed regimes is the same mistake as A1's old resting band.
    # Grading the DIFFERENCE BETWEEN THE TWO COPIES removes the whole question: terrain,
    # coasting and slope move both copies together and cancel, so a peer driving away scores
    # ~0 with no guard at all, and what is left is exactly the divergence the handoff opens.
    if len(peers) > 1:
        for key in sorted(set(peers[0]) & set(peers[1])):
            for side in (0, 1):
                rows = peers[side][key]
                other = peers[1 - side][key]
                for i in range(1, len(rows)):
                    if not (rows[i - 1]["owns"] and not rows[i]["owns"]):
                        continue
                    t0 = rows[i - 1]["t"]
                    if t0 is None:
                        continue

                    def at(seq, t):
                        c = [r for r in seq if r["t"] is not None and r["t"] <= t]
                        return c[-1] if c else None

                    before_o = at(other, t0)
                    if before_o is None:
                        notes.append("A6 {}/{}: handoff at t={} unmeasured -- the other peer has "
                                     "no sample before it".format(names[side], key, t0))
                        continue

                    # SETTLE, THEN MEASURE. The pair gap is a lane fact only once both copies
                    # have stopped; while the ATV coasts or is driven away it varies for honest
                    # reasons. So walk FORWARD to the first instant both are still rather than
                    # sampling at a fixed offset -- a fixed offset makes the verdict depend on
                    # how long that particular rig took to roll to a halt. And "still" is a
                    # state at measurement time, not a distance travelled since: the first
                    # version of this guard skipped a handoff because the rig coasted 68 cm,
                    # which is the very case the arm exists for.
                    def still(seq, r):
                        p = [x for x in seq if x["t"] is not None and x["t"] < r["t"]]
                        if not p:
                            return False
                        q = p[-1]
                        return ((r["bx"] - q["bx"]) ** 2 + (r["by"] - q["by"]) ** 2) ** 0.5 < 5.0

                    after_self = after_o = None
                    for r in rows[i:]:
                        if r["t"] is None or r["t"] - t0 > HANDOFF_SETTLE_S:
                            break
                        o = at(other, r["t"])
                        if o is None or o["t"] < t0:
                            continue
                        if still(rows, r) and still(other, o):
                            after_self, after_o = r, o
                            break
                    if after_self is None:
                        notes.append("A6 {}/{}: handoff at t={} unmeasured -- the two copies "
                                     "never both came to rest within {} s, so the pair gap is "
                                     "not yet a lane fact"
                                     .format(names[side], key, t0, HANDOFF_SETTLE_S))
                        continue
                    gap0 = abs(rows[i - 1]["bz"] - before_o["bz"])
                    gap1 = abs(after_self["bz"] - after_o["bz"])
                    opened = gap1 - gap0
                    ok = opened <= HANDOFF_SAG_CM
                    print("  A6 handoff {}/{} at t={}  pair dZ {:.1f} -> {:.1f} cm  "
                          "opened={:+.1f}  {}".format(names[side], key, t0, gap0, gap1,
                                                      opened, "PASS" if ok else "FAIL"))
                    if not ok:
                        fails.append("A6 {} key='{}' released authority at t={} and the two "
                                     "copies went from {:.1f} cm apart in Z to {:.1f} cm "
                                     "({:+.1f}) within {} s > {:.1f}. The rigs agreed while one "
                                     "of them authored and diverged the moment it became a "
                                     "mirror.".format(names[side], key, t0, gap0, gap1, opened,
                                                      HANDOFF_SETTLE_S, HANDOFF_SAG_CM))

    # A3 -- the collision guard armed, and whether its CANCEL path was ever exercised.
    for nm, path in zip(names, logs):
        try:
            txt = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        armed = "atv: hit guard armed" in txt
        print("  A3 {:<9} hit guard armed: {}".format(nm, "PASS" if armed else "FAIL"))
        if not armed:
            fails.append("A3 {}: the hit guard did not arm (ATV sync stayed inert)".format(nm))
        m = re.search(r"hit guard: (\d+) cancelled / (\d+) allowed", txt)
        if m:
            c, a = int(m.group(1)), int(m.group(2))
            print("     counters: {} cancelled / {} allowed".format(c, a))
            if c == 0 and a == 0:
                notes.append("A3 {}: the guard armed but NEVER FIRED -- no ATV collision happened, "
                             "so its cancel path is still UNPROVEN by this run".format(nm))
        # A SUBSTRING IS NOT AN EVENT. This scan fired on both peers of the 2026-08-30 run
        # and neither had exploded: the only line containing either word was OUR OWN
        # "hit guard armed -- ... (a non-owner cannot author damage/explode/ejectWheel)".
        # An instrument that reports its own log text as a finding manufactures work, so the
        # scan now excludes the lines this module writes about itself.
        for bad in ("explode", "ejectWheel"):
            hits = [ln for ln in txt.splitlines()
                    if bad in ln and "hit guard armed" not in ln]
            if hits:
                notes.append("A3 {}: log mentions '{}' ({} line(s), e.g. {!r}) -- check WHICH "
                             "peer authored it".format(nm, bad, len(hits), hits[0].strip()[:120]))

    # A5 -- TRAIL. How far behind the author is the mirror WHILE THE ATV IS MOVING? A2 looks
    # only at the last sample, so a mirror that lags metres through the whole drive and then
    # catches up when everything stops passes it. This is the number a watching player sees.
    if len(peers) > 1:
        for key in sorted(set(peers[0]) & set(peers[1])):
            by_peer = []
            for d in peers:
                m = {}
                for r in d[key]:
                    if r["t"] is not None and r["t"] in win:
                        m.setdefault(r["t"], r)
                by_peer.append(m)
            secs = sorted(set(by_peer[0]) & set(by_peer[1]))
            # Only seconds where exactly one side owned the tick: with no author there is no
            # "behind" to measure, and a handoff second compares two authorities.
            trails = []
            for t in secs:
                a, b = by_peer[0][t], by_peer[1][t]
                if int(a["owns"]) + int(b["owns"]) != 1:
                    continue
                dx, dy, dz = a["bx"] - b["bx"], a["by"] - b["by"], a["bz"] - b["bz"]
                trails.append((dx * dx + dy * dy + dz * dz) ** 0.5)
            if len(trails) < MIN_MIRROR_SAMPLES:
                notes.append("A5 {}: only {} single-author second(s) in the driven window -- "
                             "no trail measurement".format(key, len(trails)))
                continue
            mean = sum(trails) / len(trails)
            worst = max(trails)
            ok = worst <= TRAIL_MAX_CM
            print("  A5 trail key='{}' n={} mean={:.1f} cm  max={:.1f} cm  {}".format(
                key, len(trails), mean, worst, "PASS" if ok else "FAIL"))
            if not ok:
                fails.append("A5 key='{}' the mirror trailed the author by up to {:.0f} cm while "
                             "driving (mean {:.0f}) > {:.0f} -- atv_sync's warp fires only past "
                             "kWarpBaseCm + kWarpPerSpeedS*|v|, which at drive speed is wider "
                             "than this".format(key, worst, mean, TRAIL_MAX_CM))

    # A4 -- SINGLE SYNCER. At most one peer may own a given ATV's tick in the same second.
    if len(peers) > 1:
        owners = {}
        for nm, d in zip(names, peers):
            for key, rows in d.items():
                for r in rows:
                    if r["owns"] and r["t"] is not None:
                        owners.setdefault((key, r["t"]), set()).add(nm)
        clashes = {k: v for k, v in owners.items() if len(v) > 1}
        print("  A4 single-syncer: {} second(s) with >1 owner  {}".format(
            len(clashes), "PASS" if not clashes else "FAIL"))
        if clashes:
            ex = sorted(clashes.items())[:3]
            fails.append("A4 two peers owned the same ATV's tick in {} second(s), e.g. {}".format(
                len(clashes), [(k[0], k[1], sorted(v)) for k, v in ex]))

    print("-" * 78)
    for n in notes:
        print("  NOTE: {}".format(n))
    if fails:
        for f in fails:
            print("  FAIL: {}".format(f))
        print("  ACCEPTANCE: FAIL")
        return 1
    if notes and not win:
        print("  ACCEPTANCE: INCONCLUSIVE (see NOTE)")
        return 2
    print("  ACCEPTANCE: PASS")
    return 0


def archive(paths, names):
    """Keep the lines this verdict rests on, because the next run deletes them.

    mp.py unlinks each peer's multivoid.log at launch, so the evidence behind a printed
    verdict survives only until somebody starts a game -- which on a shared box can be a
    different session, two minutes later, with no warning. That happened on 2026-08-30: a
    run's A1 numbers were printed and its host log was gone before they could be examined.
    A verdict whose evidence cannot be re-read is an anecdote.

    Only the ATV-relevant lines are kept (a full pair is ~2 M lines and mostly unrelated),
    plus a MANIFEST naming the source files and the deployed DLL, so a later reader can tell
    which build produced them.
    """
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out = os.path.join(ROOT, "research", "atv_runs", stamp)
    try:
        os.makedirs(out, exist_ok=True)
    except OSError as e:
        print("  (archive skipped: {})".format(e))
        return None
    keep = re.compile(r"\[ATVP\]|^\[[\d:]+\] \[\w+ *\] atv[:_]|atv_probe|hit guard")
    manifest = ["run archived {}".format(stamp), ""]
    for nm, p in zip(names, paths):
        dst = os.path.join(out, "{}.log".format(nm.lower()))
        n = 0
        try:
            with open(p, encoding="utf-8", errors="replace") as fh, \
                 open(dst, "w", encoding="utf-8") as w:
                for ln in fh:
                    if keep.search(ln):
                        w.write(ln)
                        n += 1
        except OSError as e:
            manifest.append("{}: UNREADABLE ({})".format(nm, e))
            continue
        manifest.append("{}: {} line(s) kept from {}".format(nm, n, p))
        dll = os.path.join(os.path.dirname(p), "Mods", "Multivoid", "dlls", "main.dll")
        if os.path.exists(dll):
            h = hashlib.sha256(open(dll, "rb").read()).hexdigest()[:16].upper()
            manifest.append("{}: deployed main.dll sha256[0:16] = {}".format(nm, h))
    with open(os.path.join(out, "MANIFEST.txt"), "w", encoding="utf-8") as w:
        w.write("\n".join(manifest) + "\n")
    print("  archived to research/atv_runs/{}".format(stamp))
    return out


def main():
    paths = sys.argv[1:3] if len(sys.argv) >= 3 else DEFAULT
    names = ["HOST", "CLIENT_1"] if paths == DEFAULT else [p for p in paths]
    peers = [parse(p) for p in paths]
    if "--no-archive" not in sys.argv:
        archive(paths, names)
    for nm, p, d in zip(names, paths, peers):
        print("\nsource: {}".format(p))
        report_peer(nm, d)

    print("\n" + "=" * 78)
    print("CROSS-PEER (Q2 divergence / Q3 position agreement)")
    a, b = peers[0], peers[1] if len(peers) > 1 else {}
    common = sorted(set(a) & set(b))
    if not common:
        print("  no ATV key seen on BOTH peers -- keys: {} vs {}".format(
            sorted(a) or "[]", sorted(b) or "[]"))
        return 0
    for key in common:
        ra, rb = a[key][-1], b[key][-1]
        print("  ATV key='{}'  (comparing each peer's LAST sample)".format(key))
        for lab, fld in (("fuel", "fuel"), ("battery", "batt"), ("dirt", "dirt"), ("hp", "hp")):
            if ra[fld] is None or rb[fld] is None:
                continue
            gap = ra[fld] - rb[fld]
            print("    {:<8} host={:10.4f}  client={:10.4f}  GAP={:+10.4f} {}".format(
                lab, ra[fld], rb[fld], gap, "" if abs(gap) < 1e-6 else "  <-- DIVERGED"))
        dx, dy, dz = ra["bx"] - rb["bx"], ra["by"] - rb["by"], ra["bz"] - rb["bz"]
        dist = (dx * dx + dy * dy + dz * dz) ** 0.5
        print("    body     host=({:.1f},{:.1f},{:.1f}) client=({:.1f},{:.1f},{:.1f})  "
              "GAP={:.3f} cm {}".format(ra["bx"], ra["by"], ra["bz"], rb["bx"], rb["by"], rb["bz"],
                                        dist, "" if dist < 1.0 else "  <-- APART"))
    return acceptance(names, peers, paths)


if __name__ == "__main__":
    sys.exit(main())
