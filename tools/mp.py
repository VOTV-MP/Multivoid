"""VOTV coop launcher / autonomous LAN smoke orchestrator.

Replaces the .bat-piped powershell-pipeline chain that buffer-deadlocked under
redirected stdio (cmd.exe + powershell child + ps1 deploy script + VotV
detached spawn -> tail caller never sees output until VotV exits).

Subcommands:
  host        deploy + launch HOST peer (Game_0.9.0n/)
  client      deploy + launch CLIENT #1 peer (Game_0.9.0n_copy/)
  client2     deploy + launch CLIENT #2 peer (Game_0.9.0n_copy2/) -- 2026-05-28
              added for 3-peer LAN tests of the GNS multi-peer wire layer.
  smoke       autonomous LAN smoke: deploy + spawn both peers + monitor + kill
  kill        SIGTERM all VotV-Win64-Shipping instances

Every step prints a [mp] line immediately (flushed) so a Bash caller never has
to guess what the orchestrator is doing. Child VotV is launched DETACHED so
the parent Python exits cleanly without VotV inheriting our pipes.

Used by mp_host_game.bat / mp_client_connect.bat / mp_client2_connect.bat /
mp_smoke.bat which are thin shims at repo root (per the user RULE: "FOR ME
TO RUN YOU MUST MAKE A BAT AND PUT IT PROJECTS ROOT").
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WIN64_REL = "WindowsNoEditor/VotV/Binaries/Win64"
HOST_DIR = ROOT / "Game_0.9.0n" / WIN64_REL
CLIENT_DIR = ROOT / "Game_0.9.0n_copy" / WIN64_REL
CLIENT2_DIR = ROOT / "Game_0.9.0n_copy2" / WIN64_REL  # 2026-05-28 PR-4.2+: 2nd client for 3-peer testing
DEV_DIR = ROOT / "Game_0.9.0n_dev" / WIN64_REL
DEPLOY_ALL = ROOT / "tools" / "deploy-all.ps1"
VOTV_EXE = "VotV-Win64-Shipping.exe"
DEFAULT_PORT = 47621

# Windows CreateProcess flags. DETACHED_PROCESS prevents the child from
# inheriting our console; CREATE_NEW_PROCESS_GROUP isolates Ctrl-C handling.
DETACHED_PROCESS = 0x00000008
CREATE_NEW_PROCESS_GROUP = 0x00000200


def log(msg: str) -> None:
    sys.stdout.write(f"[mp] {msg}\n")
    sys.stdout.flush()


def run_ps(script: str) -> tuple[int, str, str]:
    r = subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return r.returncode, r.stdout, r.stderr


def deploy_all() -> None:
    log("deploying (deploy-all.ps1)...")
    r = subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(DEPLOY_ALL)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if r.returncode != 0:
        log(f"DEPLOY FAILED rc={r.returncode}")
        for line in (r.stdout + "\n" + r.stderr).splitlines()[-25:]:
            log(f"  deploy: {line}")
        sys.exit(1)
    # surface deploy summary lines (one per target)
    for line in r.stdout.splitlines():
        if "===" in line or "deployed loader" in line or "[deploy-all]" in line:
            log(f"  deploy: {line.strip()}")
    log("deploy OK")


def list_votv() -> list[dict]:
    rc, out, _ = run_ps(
        "Get-Process VotV-Win64-Shipping -ErrorAction SilentlyContinue | "
        "ForEach-Object { [PSCustomObject]@{PID=$_.Id; RSS_MB=[math]::Round($_.WorkingSet64/1MB,1); "
        "Title=$_.MainWindowTitle; Path=$_.Path} } | ConvertTo-Json -Compress"
    )
    out = out.strip()
    if not out:
        return []
    data = json.loads(out)
    if isinstance(data, dict):
        data = [data]
    return data


def kill_all() -> int:
    procs = list_votv()
    for p in procs:
        log(f"  killing PID={p['PID']} RSS={p['RSS_MB']}MB title='{p['Title']}'")
    run_ps("Get-Process VotV-Win64-Shipping -ErrorAction SilentlyContinue | Stop-Process -Force")
    time.sleep(1)
    remaining = list_votv()
    if remaining:
        log(f"WARN: {len(remaining)} VotV still alive after kill")
    return len(procs)


def host_owns_udp(pid: int, port: int) -> bool:
    # Note: keep this as a single f-string. A multi-piece string with a non-f
    # second half quietly turns PowerShell's `{ ... }` into `{{ ... }}` (Python
    # f-string escape) which PowerShell parses as a parser error; combined with
    # -ErrorAction SilentlyContinue, the probe fails silently and the smoke
    # never sees the bind. Use Select-Object -ExpandProperty to avoid braces
    # entirely.
    rc, out, _ = run_ps(
        f"Get-NetUDPEndpoint -OwningProcess {pid} -ErrorAction SilentlyContinue | "
        f"Select-Object -ExpandProperty LocalPort"
    )
    return str(port) in out


def launch_peer(role: str, port: int, nick: str, peer: str | None,
                res_x: int, res_y: int, peer_slot: int = 1) -> int:
    # role is the WIRE role (host / client). peer_slot is which CLIENT folder
    # to launch from when role==client: 1 -> Game_0.9.0n_copy, 2 ->
    # Game_0.9.0n_copy2. Host always uses Game_0.9.0n.
    if role == "host":
        game_dir = HOST_DIR
    elif peer_slot == 2:
        game_dir = CLIENT2_DIR
    else:
        game_dir = CLIENT_DIR
    exe = game_dir / VOTV_EXE
    if not exe.exists():
        log(f"FATAL: missing exe {exe}")
        sys.exit(1)
    log(f"role={role} dir={game_dir.parent.parent.parent.name} port={port} nick={nick}"
        + (f" peer={peer}" if peer else ""))
    (game_dir / "scenario.txt").write_text("play")
    log_file = game_dir / "votv-coop.log"
    if log_file.exists():
        try:
            log_file.unlink()
        except PermissionError:
            log(f"WARN: {log_file} locked (another VotV still holds it?)")
    env = os.environ.copy()
    env["VOTVCOOP_SCENARIO"] = "play"
    env["VOTVCOOP_NET_ROLE"] = role
    env["VOTVCOOP_NET_PORT"] = str(port)
    env["VOTVCOOP_NET_NICK"] = nick
    if role == "client" and peer:
        env["VOTVCOOP_NET_PEER"] = peer
    proc = subprocess.Popen(
        [str(exe), "-windowed", f"-ResX={res_x}", f"-ResY={res_y}"],
        cwd=str(game_dir),
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
        close_fds=True,
    )
    log(f"launched PID={proc.pid}")
    return proc.pid


def cmd_host(args) -> None:
    deploy_all()
    pid = launch_peer("host", args.port, args.nick or "Host",
                      peer=None, res_x=args.res_x, res_y=args.res_y)
    log(f"host running PID={pid}")


def cmd_client(args) -> None:
    deploy_all()
    pid = launch_peer("client", args.port, args.nick or "Client",
                      peer=args.peer, res_x=args.res_x, res_y=args.res_y,
                      peer_slot=1)
    log(f"client running PID={pid}")


def cmd_client2(args) -> None:
    deploy_all()
    pid = launch_peer("client", args.port, args.nick or "Client2",
                      peer=args.peer, res_x=args.res_x, res_y=args.res_y,
                      peer_slot=2)
    log(f"client2 running PID={pid}")


def cmd_kill(args) -> None:
    n = kill_all()
    log(f"killed {n} VotV instance(s)")


def tail_log(path: Path, n: int, label: str) -> None:
    log(f"--- {label} LOG (last {n} lines: {path}) ---")
    if not path.exists():
        log(f"  {label}: log not present")
        return
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except Exception as e:
        log(f"  {label}: read failed {e}")
        return
    for line in lines[-n:]:
        log(f"  {label}: {line}")


def cmd_smoke(args) -> None:
    """Autonomous LAN smoke.

    Order:
      1. Deploy
      2. Launch host. Wait until host binds UDP --port (or timeout -> FAIL).
      3. Launch client (--peer 127.0.0.1 by default).
      4. Sample peers every --sample-interval seconds for --duration seconds.
         Per-sample: enumerate VotV processes, log RSS + title. If any peer
         exceeds --ram-kill-mb, kill everything and FAIL (born from the 19 GB
         install-loop incident 2026-05-27).
      5. Tail both logs.
      6. Kill both peers.
      7. Verdict PASS only if BOTH peers were still alive at the last sample
         AND no peer breached the RAM kill threshold.
    """
    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before smoke")

    deploy_all()

    log("--- HOST LAUNCH ---")
    host_pid = launch_peer("host", args.port, "Host",
                           peer=None, res_x=args.res_x, res_y=args.res_y)

    log(f"waiting up to {args.boot_timeout}s for host to bind UDP {args.port}...")
    bound = False
    for i in range(args.boot_timeout):
        time.sleep(1)
        if host_owns_udp(host_pid, args.port):
            log(f"host bound UDP {args.port} after {i+1}s")
            bound = True
            break
        # also check if host died
        if not any(p["PID"] == host_pid for p in list_votv()):
            log(f"HOST DIED before binding UDP (PID {host_pid} gone)")
            tail_log(HOST_DIR / "votv-coop.log", 30, "HOST")
            sys.exit(1)
    if not bound:
        log(f"FAIL: host did NOT bind UDP within {args.boot_timeout}s")
        tail_log(HOST_DIR / "votv-coop.log", 30, "HOST")
        kill_all()
        sys.exit(1)

    log("--- CLIENT LAUNCH ---")
    client_pid = launch_peer("client", args.port, "Client",
                             peer="127.0.0.1", res_x=args.res_x, res_y=args.res_y)

    log(f"--- MONITORING for {args.duration}s (sample every {args.sample_interval}s) ---")
    t0 = time.time()
    last_peers: list[dict] = []
    kill_reason: str | None = None
    while time.time() - t0 < args.duration:
        time.sleep(args.sample_interval)
        peers = list_votv()
        last_peers = peers
        t = int(time.time() - t0)
        if peers:
            desc = ", ".join(f"PID{p['PID']}={p['RSS_MB']}MB '{p['Title']}'" for p in peers)
        else:
            desc = "NONE"
        log(f"  t={t}s peers={len(peers)}: {desc}")
        max_rss = max((p["RSS_MB"] for p in peers), default=0)
        if max_rss > args.ram_kill_mb:
            kill_reason = f"peer RSS={max_rss}MB > kill threshold {args.ram_kill_mb}MB"
            break

    log("--- FINAL STATE ---")
    log(f"peers alive at end: {len(last_peers)}")
    for p in last_peers:
        log(f"  PID={p['PID']} RSS={p['RSS_MB']}MB title='{p['Title']}'")

    tail_log(HOST_DIR / "votv-coop.log", 30, "HOST")
    tail_log(CLIENT_DIR / "votv-coop.log", 30, "CLIENT")

    log("--- KILLING ---")
    kill_all()

    log("--- VERDICT ---")
    if kill_reason:
        log(f"FAIL: {kill_reason}")
        sys.exit(2)
    expected = 2
    if len(last_peers) != expected:
        log(f"FAIL: expected {expected} peers at end, got {len(last_peers)}")
        sys.exit(3)
    log("PASS: both peers stable, no RAM breach, logs tailed")
    sys.exit(0)


def main() -> None:
    ap = argparse.ArgumentParser(description="VOTV coop orchestrator")
    sub = ap.add_subparsers(dest="cmd", required=True)

    # Host window stays 1080p (single big window); clients default to 720p
    # so multiple client windows can fit on a single monitor for 3-peer
    # tests (user directive 2026-05-28, supersedes the earlier "always
    # 1080" rule which assumed only one client window on screen).
    host_res = [
        ("--res-x", {"type": int, "default": 1920}),
        ("--res-y", {"type": int, "default": 1080}),
        ("--port", {"type": int, "default": DEFAULT_PORT}),
    ]
    client_res = [
        ("--res-x", {"type": int, "default": 1280}),
        ("--res-y", {"type": int, "default": 720}),
        ("--port", {"type": int, "default": DEFAULT_PORT}),
    ]

    p_host = sub.add_parser("host", help="launch HOST peer")
    p_host.add_argument("--nick", default=None)
    for flag, kw in host_res: p_host.add_argument(flag, **kw)
    p_host.set_defaults(func=cmd_host)

    p_client = sub.add_parser("client", help="launch CLIENT #1 peer")
    p_client.add_argument("--peer", default="127.0.0.1")
    p_client.add_argument("--nick", default=None)
    for flag, kw in client_res: p_client.add_argument(flag, **kw)
    p_client.set_defaults(func=cmd_client)

    p_client2 = sub.add_parser("client2", help="launch CLIENT #2 peer (3-peer LAN)")
    p_client2.add_argument("--peer", default="127.0.0.1")
    p_client2.add_argument("--nick", default=None)
    for flag, kw in client_res: p_client2.add_argument(flag, **kw)
    p_client2.set_defaults(func=cmd_client2)

    p_kill = sub.add_parser("kill", help="kill all VotV instances")
    p_kill.set_defaults(func=cmd_kill)

    p_smoke = sub.add_parser("smoke", help="autonomous LAN smoke (host+client+monitor+kill)")
    p_smoke.add_argument("--duration", type=int, default=30,
                         help="seconds to monitor after both peers up")
    p_smoke.add_argument("--sample-interval", type=int, default=5,
                         help="seconds between RSS samples")
    p_smoke.add_argument("--boot-timeout", type=int, default=30,
                         help="seconds to wait for host UDP bind")
    p_smoke.add_argument("--ram-kill-mb", type=int, default=8000,
                         help="hard kill threshold (born from 19 GB install-loop incident)")
    for flag, kw in host_res: p_smoke.add_argument(flag, **kw)
    p_smoke.set_defaults(func=cmd_smoke)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
