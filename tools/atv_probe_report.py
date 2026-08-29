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
import os
import re
import statistics
import sys

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
BAND_CM = 4.0        # native suspension travel is 2-4 cm; baseline mirror hit 29.58
SETTLE_GAP_CM = 20.0 # end-of-run body gap; baseline was 109.9 cm and PERMANENT
MIN_MIRROR_SAMPLES = 6


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
    else:
        notes.append("NO driven=1 sample on either peer -- the sit arm never fired, so nothing "
                     "below tested a MIRROR. An idle ATV is never mirrored (the lane is invisible "
                     "at rest); this run proves nothing about the corrector.")

    # A1 -- a MIRROR's rig stays inside the native band while someone drives.
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
                ok = sp["range"] <= BAND_CM
                print("  A1 {:<9} {:<6} n={:<4} range={:8.4f} cm  {}".format(
                    nm, lab, sp["n"], sp["range"], "PASS" if ok else "FAIL"))
                if not ok:
                    fails.append("A1 {} {} rig travel {:.2f} cm > {:.1f} cm while mirroring".format(
                        nm, lab, sp["range"], BAND_CM))

    # A2 -- the two copies END the run together. The baseline's gap was not merely
    # large, it was PERMANENT: the release launched the mirror and nothing corrected
    # it again. A settled gap is the direct test of that.
    if len(peers) > 1:
        for key in sorted(set(peers[0]) & set(peers[1])):
            ra, rb = peers[0][key][-1], peers[1][key][-1]
            dx, dy, dz = ra["bx"] - rb["bx"], ra["by"] - rb["by"], ra["bz"] - rb["bz"]
            gap = (dx * dx + dy * dy + dz * dz) ** 0.5
            ok = gap <= SETTLE_GAP_CM
            print("  A2 settled gap key='{}' {:.3f} cm  {}".format(key, gap, "PASS" if ok else "FAIL"))
            if not ok:
                # ATTRIBUTE the failure. A gap the corrector never tried to close is a sync
                # defect; a gap it CUT repeatedly, at a constant distance, is not -- the rig was
                # teleported onto the authority's exact pose and fell back, which means the two
                # peers' worlds differ under the vehicle and no pose lane can fix it. Blaming the
                # wrong subsystem is worse than no verdict.
                cuts = []
                for path in logs:
                    try:
                        cuts += re.findall(r"correction stalled at ([\d.]+) cm", 
                                           open(path, encoding="utf-8", errors="replace").read())
                    except OSError:
                        pass
                vals = sorted({round(float(c), 1) for c in cuts})
                if len(cuts) >= 2 and len(vals) == 1 and abs(vals[0] - gap) < 1.0:
                    fails.append("A2 key='{}' settled {:.1f} cm apart -- but the corrector CUT to "
                                 "the authority's pose {} times and it fell back to the same "
                                 "{:.1f} cm every time. The pose lane is doing its job; the two "
                                 "peers' WORLDS differ under this vehicle. Not a C1 defect -- "
                                 "file it against the world/save-transfer lane.".format(
                                     key, gap, len(cuts), vals[0]))
                else:
                    fails.append("A2 key='{}' settled {:.1f} cm apart > {:.1f}".format(
                        key, gap, SETTLE_GAP_CM))

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
        for bad in ("explode", "ejectWheel"):
            if bad in txt:
                notes.append("A3 {}: log mentions '{}' -- check WHICH peer authored it".format(nm, bad))

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


def main():
    paths = sys.argv[1:3] if len(sys.argv) >= 3 else DEFAULT
    names = ["HOST", "CLIENT_1"] if paths == DEFAULT else [p for p in paths]
    peers = [parse(p) for p in paths]
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
