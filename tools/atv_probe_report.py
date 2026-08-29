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

It asserts nothing. At this stage we do not know the right answer; that is the point.

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

LINE = re.compile(
    r"\[ATVP\] n=(?P<n>\d+) i=(?P<i>\d+) key='(?P<key>[^']*)' driven=(?P<driven>\d) occ=(?P<occ>\S+) "
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
