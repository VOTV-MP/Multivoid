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
import ctypes
import json
import os
import subprocess
import sys
import time
from ctypes import wintypes
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


# --- Monitor enumeration (Win32 EnumDisplayMonitors via ctypes) ---
# Used to place client windows on the second monitor by default, so the user's
# primary monitor (host window) stays visually separate from the client(s) in
# multi-peer tests. Returns a list of dicts with 'primary', 'rect', 'work'.

class _RECT(ctypes.Structure):
    _fields_ = [('left', ctypes.c_long), ('top', ctypes.c_long),
                ('right', ctypes.c_long), ('bottom', ctypes.c_long)]


class _MONITORINFO(ctypes.Structure):
    _fields_ = [('cbSize', wintypes.DWORD), ('rcMonitor', _RECT),
                ('rcWork', _RECT), ('dwFlags', wintypes.DWORD)]


_MONITORINFOF_PRIMARY = 0x00000001


def enumerate_monitors() -> list[dict]:
    """Returns a list of monitors ordered: primary first, then secondaries
    in EnumDisplayMonitors order (top-left to bottom-right typically).

    Each entry: {'primary': bool, 'x': int, 'y': int, 'w': int, 'h': int}
    where x/y/w/h come from rcMonitor (full bounds, including taskbar).
    """
    user32 = ctypes.windll.user32
    # Without argtypes, ctypes defaults to c_int for pointer-typed args; HMONITOR
    # on x64 is 64-bit and that mismatch raises OverflowError when the C callback
    # tries to forward the handle. Set the signatures explicitly.
    user32.GetMonitorInfoW.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(_MONITORINFO)
    ]
    user32.GetMonitorInfoW.restype = wintypes.BOOL
    user32.EnumDisplayMonitors.restype = wintypes.BOOL
    found: list[dict] = []

    MONITORENUMPROC = ctypes.WINFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
        ctypes.POINTER(_RECT), ctypes.c_void_p)

    def cb(hmonitor, hdc, lprc, lparam):
        # Must return 1 even on internal failure: returning 0 (or None via an
        # exception) tells EnumDisplayMonitors to STOP enumerating, which would
        # silently drop secondary monitors.
        try:
            mi = _MONITORINFO()
            mi.cbSize = ctypes.sizeof(_MONITORINFO)
            if user32.GetMonitorInfoW(hmonitor, ctypes.byref(mi)):
                found.append({
                    'primary': bool(mi.dwFlags & _MONITORINFOF_PRIMARY),
                    'x': mi.rcMonitor.left,
                    'y': mi.rcMonitor.top,
                    'w': mi.rcMonitor.right - mi.rcMonitor.left,
                    'h': mi.rcMonitor.bottom - mi.rcMonitor.top,
                })
        except Exception:
            pass
        return 1

    user32.EnumDisplayMonitors(None, None, MONITORENUMPROC(cb), 0)
    # Sort: primary first, then by (y, x) for stable left-to-right ordering.
    found.sort(key=lambda m: (not m['primary'], m['y'], m['x']))
    return found


def pick_monitor(index_1based: int) -> dict | None:
    """index 1 -> primary, 2 -> first secondary, etc. Returns None if N/A."""
    mons = enumerate_monitors()
    if index_1based < 1 or index_1based > len(mons):
        return None
    return mons[index_1based - 1]


# --- Per-process commit limit via Win32 Job Objects ---
# Safety net for the kind of UE4 runaway-allocation pathology we hit on
# 2026-05-28 (host launched with -WinX=0 -WinY=0 on a multi-monitor virtual
# desktop ate 18 GB before the user could kill it). A Job Object with
# JOB_OBJECT_LIMIT_PROCESS_MEMORY caps the per-process commit. The kernel
# fails further VirtualAlloc once the limit is hit -- UE4 either retries
# at a smaller size, logs an OOM, or crashes. Either way the process stops
# growing past the cap. Critically: the limit is a property of the job
# object, NOT of our handle, so it stays in effect after mp.py exits and
# VotV is left running standalone.
#
# References:
# - https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information
# - JOB_OBJECT_LIMIT_PROCESS_MEMORY = 0x00000100
# - JobObjectExtendedLimitInformation = 9

class _JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ('PerProcessUserTimeLimit', ctypes.c_int64),
        ('PerJobUserTimeLimit',     ctypes.c_int64),
        ('LimitFlags',              wintypes.DWORD),
        ('MinimumWorkingSetSize',   ctypes.c_size_t),
        ('MaximumWorkingSetSize',   ctypes.c_size_t),
        ('ActiveProcessLimit',      wintypes.DWORD),
        ('Affinity',                ctypes.c_void_p),
        ('PriorityClass',           wintypes.DWORD),
        ('SchedulingClass',         wintypes.DWORD),
    ]


class _IO_COUNTERS(ctypes.Structure):
    _fields_ = [
        ('ReadOperationCount',  ctypes.c_uint64),
        ('WriteOperationCount', ctypes.c_uint64),
        ('OtherOperationCount', ctypes.c_uint64),
        ('ReadTransferCount',   ctypes.c_uint64),
        ('WriteTransferCount',  ctypes.c_uint64),
        ('OtherTransferCount',  ctypes.c_uint64),
    ]


class _JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ('BasicLimitInformation', _JOBOBJECT_BASIC_LIMIT_INFORMATION),
        ('IoInfo',                _IO_COUNTERS),
        ('ProcessMemoryLimit',    ctypes.c_size_t),
        ('JobMemoryLimit',        ctypes.c_size_t),
        ('PeakProcessMemoryUsed', ctypes.c_size_t),
        ('PeakJobMemoryUsed',     ctypes.c_size_t),
    ]


_JOB_OBJECT_LIMIT_PROCESS_MEMORY = 0x00000100
_JobObjectExtendedLimitInformation = 9


def apply_process_memory_limit(pid: int, limit_bytes: int) -> bool:
    """Wrap `pid` in a Job Object with a per-process commit limit of
    `limit_bytes`. Returns True on success. The job persists as long as
    `pid` is alive even after this Python process exits."""
    kernel32 = ctypes.windll.kernel32
    PROCESS_SET_QUOTA = 0x0100
    PROCESS_TERMINATE = 0x0001
    proc = kernel32.OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE,
                                False, pid)
    if not proc:
        log(f"  memory-limit: OpenProcess({pid}) failed err={kernel32.GetLastError()}")
        return False
    try:
        job = kernel32.CreateJobObjectW(None, None)
        if not job:
            log(f"  memory-limit: CreateJobObjectW failed err={kernel32.GetLastError()}")
            return False
        info = _JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
        info.BasicLimitInformation.LimitFlags = _JOB_OBJECT_LIMIT_PROCESS_MEMORY
        info.ProcessMemoryLimit = limit_bytes
        if not kernel32.SetInformationJobObject(
                job, _JobObjectExtendedLimitInformation,
                ctypes.byref(info), ctypes.sizeof(info)):
            log(f"  memory-limit: SetInformationJobObject failed err={kernel32.GetLastError()}")
            kernel32.CloseHandle(job)
            return False
        if not kernel32.AssignProcessToJobObject(job, proc):
            err = kernel32.GetLastError()
            log(f"  memory-limit: AssignProcessToJobObject failed err={err}")
            kernel32.CloseHandle(job)
            return False
        # NOTE: we deliberately DO NOT CloseHandle(job) here. The job
        # object lives in the kernel; closing our handle is fine because
        # the limit applies to the assigned process as long as the process
        # is alive (the job is GC'd when its last process exits). We don't
        # set JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE so closing won't kill VotV.
        kernel32.CloseHandle(job)
        return True
    finally:
        kernel32.CloseHandle(proc)


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


def tile_offset(tile_index: int, mon: dict | None,
                res_x: int, res_y: int) -> tuple[int, int]:
    """Compute non-overlapping (offset_x, offset_y) within `mon` for the
    Nth (0-indexed) tile of a `res_x` x `res_y` window. Tries side-by-side
    first (if the monitor is wide enough for two windows), then stacked
    vertically (if tall enough), then a 40-px cascade fallback.

    A small top margin is reserved on the first row so the window's title
    bar lands comfortably below the monitor's top edge. User-observed
    issue 2026-05-28: client #1 at y=0 on a portrait secondary monitor
    had its title bar clipped/hidden against the monitor's top edge --
    fix is to push everything down a touch so the title bar grab area
    is always visible.

    Monitor shape examples (with TILE_TOP_MARGIN_Y=40):
      2560x1440 landscape secondary: cols=2 (2560//1280=2) -> tile 0
        lands at (0, 40), tile 1 at (1280, 40).
      1440x2560 portrait secondary: cols=1, rows=3 -> tile 0 at (0, 40),
        tile 1 at (0, 760), tile 2 at (0, 1480). All title bars visible.
      1280x720 single monitor: cols=1, rows=1 -> cascade with margin.
    """
    TILE_TOP_MARGIN_Y = 40
    if mon is None:
        return (tile_index * 40, TILE_TOP_MARGIN_Y + tile_index * 40)
    cols = max(1, mon['w'] // res_x)
    # Subtract the top margin from available height before deciding rows
    # so we don't claim more rows than actually fit with the margin in.
    avail_h = max(res_y, mon['h'] - TILE_TOP_MARGIN_Y)
    rows = max(1, avail_h // res_y)
    if cols >= 2:
        col = tile_index % cols
        row = (tile_index // cols) % max(1, rows)
        return (col * res_x, TILE_TOP_MARGIN_Y + row * res_y)
    if rows >= 2:
        return (0, TILE_TOP_MARGIN_Y + (tile_index % rows) * res_y)
    return (tile_index * 40, TILE_TOP_MARGIN_Y + tile_index * 40)


def launch_peer(role: str, port: int, nick: str, peer: str | None,
                res_x: int, res_y: int, peer_slot: int = 1,
                monitor: int = 1, tile_index: int = 0,
                center: bool = False,
                memory_limit_gb: float = 12.0) -> int:
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
    # Pick target monitor + compute window placement. UE4 accepts -WinX / -WinY
    # for windowed-mode placement (in virtual-screen coords). 0,0 = primary
    # monitor top-left; secondary monitor coords come from EnumDisplayMonitors.
    mon = pick_monitor(monitor)
    if mon is None:
        # Requested monitor doesn't exist (e.g. user said --monitor 2 but only
        # has one screen). Silently fall back to the primary monitor.
        mon = pick_monitor(1)
    if center and mon is not None:
        # Center the window on the chosen monitor. Used by the host launcher
        # so the single big host window lands in the middle of the primary
        # screen instead of the top-left corner. max(0, ...) guards against
        # window bigger than monitor (shouldn't happen at 1920x1080 on
        # modern monitors, but no negative offsets either).
        ox = max(0, (mon['w'] - res_x) // 2)
        oy = max(0, (mon['h'] - res_y) // 2)
    else:
        ox, oy = tile_offset(tile_index, mon, res_x, res_y)
    win_x = (mon['x'] if mon else 0) + ox
    win_y = (mon['y'] if mon else 0) + oy
    log(f"role={role} dir={game_dir.parent.parent.parent.name} port={port} nick={nick}"
        f" monitor={monitor} win=({win_x},{win_y}) res={res_x}x{res_y}"
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
        [str(exe), "-windowed",
         f"-ResX={res_x}", f"-ResY={res_y}",
         f"-WinX={win_x}", f"-WinY={win_y}"],
        cwd=str(game_dir),
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
        close_fds=True,
    )
    log(f"launched PID={proc.pid}")
    # Apply per-process commit limit via Job Object. The OS will fail
    # further VirtualAlloc once the cap is hit, preventing the runaway
    # growth we hit 2026-05-28 (host with -WinX=0 -WinY=0 ate 18 GB
    # before user could kill it). Limit applies for the lifetime of the
    # process even after this Python orchestrator exits.
    if memory_limit_gb > 0:
        limit_bytes = int(memory_limit_gb * 1024 * 1024 * 1024)
        if apply_process_memory_limit(proc.pid, limit_bytes):
            log(f"  memory-limit applied: {memory_limit_gb:.1f} GB per-process commit cap")
        else:
            log(f"  memory-limit FAILED -- process not protected from runaway alloc")
    return proc.pid


def cmd_host(args) -> None:
    deploy_all()
    # Host: single big window, centered on the chosen monitor (primary by
    # default). Not tiled -- the client launchers handle multi-window tiling.
    pid = launch_peer("host", args.port, args.nick or "Host",
                      peer=None, res_x=args.res_x, res_y=args.res_y,
                      monitor=args.monitor, center=True,
                      memory_limit_gb=args.memory_limit_gb)
    log(f"host running PID={pid}")


def cmd_client(args) -> None:
    deploy_all()
    # Client #1: tile index 0 (top-left of the target monitor).
    pid = launch_peer("client", args.port, args.nick or "Client",
                      peer=args.peer, res_x=args.res_x, res_y=args.res_y,
                      peer_slot=1, monitor=args.monitor, tile_index=0,
                      memory_limit_gb=args.memory_limit_gb)
    log(f"client running PID={pid}")


def cmd_client2(args) -> None:
    deploy_all()
    # Client #2: tile index 1. tile_offset() picks side-by-side / stacked /
    # cascade automatically based on the chosen monitor's dimensions.
    pid = launch_peer("client", args.port, args.nick or "Client2",
                      peer=args.peer, res_x=args.res_x, res_y=args.res_y,
                      peer_slot=2, monitor=args.monitor, tile_index=1,
                      memory_limit_gb=args.memory_limit_gb)
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
    # Host: 1920x1080 centered on the primary monitor. center=True avoids
    # the (0,0)+multi-monitor UE4 swap-chain pathology documented in
    # [[feedback-never-winxy-zero-multimonitor]].
    host_pid = launch_peer("host", args.port, "Host",
                           peer=None, res_x=args.res_x, res_y=args.res_y,
                           monitor=1, center=True)

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
    # Client: 720p on secondary monitor (per established hands-on pattern --
    # see [[feedback-user-prefers-1080-windows]]). If only one monitor is
    # connected, mp.pick_monitor silently falls back to the primary. Tile
    # index 0 puts the client at the top of the secondary monitor with a
    # 40-px title-bar margin.
    client_pid = launch_peer("client", args.port, "Client",
                             peer="127.0.0.1", res_x=1280, res_y=720,
                             monitor=2, tile_index=0)

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

    # --monitor: 1 = primary, 2 = first secondary, etc. Host stays on the
    # primary monitor; clients default to monitor 2 so the user's main screen
    # isn't crowded by 2+ client windows during multi-peer tests. If only one
    # monitor is connected the code silently falls back to monitor 1.
    # --memory-limit-gb: per-process commit cap enforced by Win32 Job Object.
    # Default 12 GB sits well above legitimate use (host 3.5 GB steady,
    # client 6.5 GB during snapshot fan-out peak) but well below the 18 GB
    # the (0,0) UE4 hazard hit, so any future runaway-alloc bug hits the
    # cap and stops growing instead of locking the machine. 0 disables.
    def _add_mem_limit(p):
        p.add_argument("--memory-limit-gb", type=float, default=12.0,
                       help="per-process commit cap in GB (0 = disabled)")

    p_host = sub.add_parser("host", help="launch HOST peer")
    p_host.add_argument("--nick", default=None)
    p_host.add_argument("--monitor", type=int, default=1,
                        help="1-based monitor index; primary=1, secondary=2")
    _add_mem_limit(p_host)
    for flag, kw in host_res: p_host.add_argument(flag, **kw)
    p_host.set_defaults(func=cmd_host)

    p_client = sub.add_parser("client", help="launch CLIENT #1 peer")
    p_client.add_argument("--peer", default="127.0.0.1")
    p_client.add_argument("--nick", default=None)
    p_client.add_argument("--monitor", type=int, default=2,
                          help="1-based monitor index; defaults to secondary")
    _add_mem_limit(p_client)
    for flag, kw in client_res: p_client.add_argument(flag, **kw)
    p_client.set_defaults(func=cmd_client)

    p_client2 = sub.add_parser("client2", help="launch CLIENT #2 peer (3-peer LAN)")
    p_client2.add_argument("--peer", default="127.0.0.1")
    p_client2.add_argument("--nick", default=None)
    p_client2.add_argument("--monitor", type=int, default=2,
                           help="1-based monitor index; defaults to secondary")
    _add_mem_limit(p_client2)
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
