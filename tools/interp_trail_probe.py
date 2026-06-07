"""Interp-trail probe -- verifies the puppet interp-starvation fix (2026-06-06).

THE BUG: a remote player's puppet trailed a MOVING source by 1-2.5 s because
net_pump::Tick called RemotePlayer::SetTargetPose then Tick() back-to-back in the
SAME frame -> the interp window's alpha was ~0 -> curPos never advanced. Props
snapped real-time, so only PLAYERS lagged. Every prior autonomous smoke had a
STATIC host -> pose-diag trail was trivially ~0 cm -> the bug hid.

THE FIX (MTA CClientPed::SetTargetPosition shape): advance the open window to NOW
as the first step of SetTargetPose (advance-before-rebase). Steady-state trail is
then bounded at ~ speed * window.

THIS PROBE launches a host running the TEST-ONLY movement oscillator
(VOTVCOOP_RUN_MOVE_OSC=1 -> autotest_move_osc.cpp circles the host player at
~235 cm/s) + a client OBSERVER. It then reads the CLIENT log's
`pose-diag[slot 0] ... trail=` line (slot 0 = the host puppet) over the
oscillation window and reports the trail.

  FIXED   -> trail bounded ~10-30 cm while the source moves (speed*75ms ~= 18 cm).
  STARVED -> trail grows to HUNDREDS of cm (the puppet crawls behind).

Run: python tools/interp_trail_probe.py
"""

from __future__ import annotations

import re
import sys
import time
from pathlib import Path

import mp

PORT = 47621
BOOT_TIMEOUT_S = 40       # the host game boot + UDP bind
WATCH_S = 120             # 30 s osc-settle + 45 s osc + boot/connect + tail
RSS_KILL_MB = 9000        # two worlds on one box sit ~3-5 GB each; a balloon climbs past this

# pose-diag[slot 0]: fresh=57/s targetSpeed=235 target=(-1234,5678) puppet=(-1240,5670) trail=18cm
POSE_RE = re.compile(
    r"pose-diag\[slot 0\]: fresh=(\d+)/s targetSpeed=(-?\d+) "
    r"target=\((-?\d+),(-?\d+)\) puppet=\((-?\d+),(-?\d+)\) trail=(\d+)cm"
)


def grep_count(path: Path, needle: str) -> int:
    try:
        return path.read_text(encoding="utf-8", errors="replace").count(needle)
    except OSError:
        return 0


def parse_pose_samples(path: Path):
    """Return a list of dicts {fresh, speed, tx, ty, px, py, trail} for every
    pose-diag[slot 0] line in the client log."""
    out = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return out
    for m in POSE_RE.finditer(text):
        out.append({
            "fresh": int(m.group(1)), "speed": int(m.group(2)),
            "tx": int(m.group(3)), "ty": int(m.group(4)),
            "px": int(m.group(5)), "py": int(m.group(6)),
            "trail": int(m.group(7)),
        })
    return out


def main() -> None:
    if mp.kill_all() > 0:
        mp.log("note: pre-existing VotV instances killed before probe")
    mp.deploy_all()

    mp.log("--- HOST LAUNCH (with VOTVCOOP_RUN_MOVE_OSC=1 -> the host player circles) ---")
    host_pid = mp.launch_peer("host", PORT, "Host", peer=None,
                              res_x=1920, res_y=1080, monitor=1, center=True,
                              extra_env={"VOTVCOOP_RUN_MOVE_OSC": "1"})

    mp.log(f"waiting up to {BOOT_TIMEOUT_S}s for host to bind UDP {PORT}...")
    bound = False
    for i in range(BOOT_TIMEOUT_S):
        time.sleep(1)
        if mp.host_owns_udp(host_pid, PORT):
            mp.log(f"host bound UDP {PORT} after {i+1}s")
            bound = True
            break
        if not any(p["PID"] == host_pid for p in mp.list_votv()):
            mp.log(f"HOST DIED before binding UDP (PID {host_pid} gone)")
            mp.tail_log(mp.HOST_DIR / "votv-coop.log", 30, "HOST")
            sys.exit(1)
    if not bound:
        mp.log(f"FAIL: host did NOT bind UDP within {BOOT_TIMEOUT_S}s")
        mp.tail_log(mp.HOST_DIR / "votv-coop.log", 30, "HOST")
        mp.kill_all()
        sys.exit(1)

    mp.log("--- CLIENT LAUNCH (observer) ---")
    mp.launch_peer("client", PORT, "Client", peer="127.0.0.1",
                   res_x=1280, res_y=720, monitor=2, tile_index=0)

    mp.log(f"--- MONITORING for {WATCH_S}s (host osc settles 30s, then circles 45s) ---")
    t0 = time.time()
    kill_reason = None
    while time.time() - t0 < WATCH_S:
        time.sleep(5)
        peers = mp.list_votv()
        t = int(time.time() - t0)
        max_rss = max((p["RSS_MB"] for p in peers), default=0)
        mp.log(f"  t={t}s peers={len(peers)} maxRSS={max_rss}MB")
        if max_rss > RSS_KILL_MB:
            kill_reason = f"peer RSS={max_rss}MB > {RSS_KILL_MB}MB"
            break
        if len(peers) < 2:
            kill_reason = f"a peer died (only {len(peers)} alive)"
            break

    host_log = mp.HOST_DIR / "votv-coop.log"
    client_log = mp.CLIENT_DIR / "votv-coop.log"

    # --- gather evidence BEFORE killing ---
    osc_started = grep_count(host_log, "move_osc: base=")
    osc_done = grep_count(host_log, "move_osc: DONE")
    samples = parse_pose_samples(client_log)

    mp.tail_log(host_log, 12, "HOST")
    mp.kill_all()

    mp.log("--- ANALYSIS ---")
    mp.log(f"host move_osc markers: started={osc_started} done={osc_done}")
    mp.log(f"client pose-diag[slot 0] samples: {len(samples)}")
    if kill_reason:
        mp.log(f"FAIL: {kill_reason}")
        sys.exit(2)
    if osc_started == 0:
        mp.log("FAIL: host never started the oscillator (no 'move_osc: base=' -- "
               "not in gameplay? env not set?)")
        sys.exit(3)
    if not samples:
        mp.log("FAIL: client logged NO pose-diag[slot 0] -- it never connected / "
               "never spawned the host puppet (no pose stream to measure)")
        sys.exit(4)

    # Moving samples: the streamed target moved >20 cm since the previous sample.
    moving = []
    prev = None
    for s in samples:
        if prev is not None:
            d = ((s["tx"] - prev["tx"]) ** 2 + (s["ty"] - prev["ty"]) ** 2) ** 0.5
            if d > 20.0:
                moving.append(s)
        prev = s

    mp.log(f"moving samples (target shifted >20cm): {len(moving)} / {len(samples)}")
    if not moving:
        mp.log("FAIL: the source never appeared to move on the wire (target stayed put) "
               "-- the oscillator didn't drive the streamed position")
        sys.exit(5)

    trails = [s["trail"] for s in moving]
    fresh = [s["fresh"] for s in moving]
    speeds = [s["speed"] for s in moving]
    tmin, tmax = min(trails), max(trails)
    tmean = sum(trails) / len(trails)
    fmean = sum(fresh) / len(fresh)
    mp.log(f"  trail while moving: min={tmin}cm mean={tmean:.0f}cm max={tmax}cm")
    mp.log(f"  fresh poses/s while moving: mean={fmean:.0f}/s "
           f"(streamed targetSpeed range {min(speeds)}..{max(speeds)})")
    # show the worst few
    worst = sorted(moving, key=lambda s: -s["trail"])[:5]
    for s in worst:
        mp.log(f"    trail={s['trail']}cm fresh={s['fresh']}/s "
               f"target=({s['tx']},{s['ty']}) puppet=({s['px']},{s['py']})")

    mp.log("--- VERDICT ---")
    # FIXED predicts trail ~= speed*0.075 ~= 18 cm, bounded. Allow generous headroom
    # for the 1 Hz sampling catching mid-correction + jitter. STARVED would be hundreds.
    PASS_MAX = 80     # cm -- a bounded trail; the bug produced 200-400 cm
    PASS_MEAN = 45    # cm
    if fmean < 30:
        mp.log(f"INCONCLUSIVE: fresh poses only {fmean:.0f}/s while moving -- the pose "
               "stream was thin (network/cadence issue), trail not a clean interp read")
        sys.exit(7)
    if tmax <= PASS_MAX and tmean <= PASS_MEAN:
        mp.log(f"PASS: puppet trail stayed BOUNDED (mean={tmean:.0f}cm max={tmax}cm) while the "
               f"source moved at ~{max(speeds)}cm/s with {fmean:.0f} fresh poses/s. "
               "Interp-starvation fix CONFIRMED (a static-source smoke would show ~0; the "
               "pre-fix bug showed hundreds of cm).")
        sys.exit(0)
    mp.log(f"FAIL: puppet trail too large (mean={tmean:.0f}cm max={tmax}cm) -- the interp is "
           "STILL behind the moving source. Fix not effective (or regressed).")
    sys.exit(8)


if __name__ == "__main__":
    main()
