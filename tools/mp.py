"""VOTV coop launcher / autonomous LAN smoke orchestrator.

Replaces the .bat-piped powershell-pipeline chain that buffer-deadlocked under
redirected stdio (cmd.exe + powershell child + ps1 deploy script + VotV
detached spawn -> tail caller never sees output until VotV exits).

Subcommands:
  host        deploy + launch HOST peer (Game_0.9.0n/)
  client      deploy + launch CLIENT #1 peer (Game_0.9.0n_copy/)
  client2     deploy + launch CLIENT #2 peer (Game_0.9.0n_copy2/) -- 2026-05-28
              added for 3-peer LAN tests of the GNS multi-peer wire layer.
  client3     deploy + launch CLIENT #3 peer (Game_0.9.0n_dev/) -- 2026-05-30
              the 4th game folder; completes a 4-peer (host + 3 client) set.
  smoke       autonomous 2-peer LAN smoke (non-regression quick-check)
  smoke4      autonomous 4-PEER LAN smoke (Tier 8) -- host + 3 clients, staggered
              connect, then LOG-DRIVEN cross-peer relay verdict. This is the
              only scenario that exercises the Tier 2 host-relay end-to-end:
              with <3 clients the relay fan-out is a no-op (host-only, finds no
              other client). The verdict proves "client A actually sees client
              B" by parsing each client's log for a puppet auto-spawned on
              ANOTHER client's slot -- a marker the old star topology could
              never produce (a client only ever saw the host at slot 0).
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
import atexit
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
                memory_limit_gb: float = 12.0,
                trigger_file: str | None = None) -> int:
    # role is the WIRE role (host / client). peer_slot is which CLIENT folder
    # to launch from when role==client: 1 -> Game_0.9.0n_copy, 2 ->
    # Game_0.9.0n_copy2, 3 -> Game_0.9.0n_dev. Host always uses Game_0.9.0n.
    # NOTE: the _dev folder additionally carries UE4SS (dwmapi.dll proxy slot),
    # but our standalone xinput1_3.dll + votv-coop.dll are byte-identical to the
    # other copies (deploy-all.ps1), so as a 4th peer its coop behaviour matches.
    if role == "host":
        game_dir = HOST_DIR
    elif peer_slot == 3:
        game_dir = DEV_DIR
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
    if trigger_file:
        # dev/spawn_npc watches this file path; when it appears the peer spawns
        # a kerfurOmega NPC once + deletes it. mp.py npctest creates it after
        # all peers connect (so the EntitySpawn broadcast reaches everyone).
        env["VOTVCOOP_SPAWN_TRIGGER"] = trigger_file
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


# --- Log-driven verdict markers (Tier 8) ---
# The 2-peer smoke's liveness-only PASS masked the slow-load flake (a client
# stuck in the menu at 45s still counted as "alive") and could never prove the
# Tier 2 host-relay actually moved cross-peer data. These markers are parsed
# straight out of each peer's votv-coop.log; the exact strings are the UE_LOGI
# calls in src/votv-coop/src/coop/{net/session_status,net/session,net_pump}.cpp
# and player_handshake.cpp -- keep them in sync if those log lines change.
import re  # noqa: E402  (kept local to the verdict feature)

_MARK = {
    # client: the host assigned us a peer slot == we reached the connected
    # handshake (session_status state=2). The definitive "this client is in".
    "assigned_slot":   re.compile(r"host assigned us peer slot (\d+)"),
    # host: it accepted a client connection at a slot.
    "host_accepted":   re.compile(r"host accepted client at slot (\d+)"),
    # THE cross-peer proof: a puppet auto-spawned because a remote pose for
    # slot N arrived. On a client, N != 0 and N != own-slot can ONLY come via
    # the relay (host forwarding another client's pose). The pre-Tier-2 star
    # topology could never log this for a non-host slot.
    "puppet_slot":     re.compile(r"first remote pose on slot (\d+) -> auto-spawning puppet"),
    "puppet_fail":     re.compile(r"slot (\d+) puppet spawn failed"),
    # client: received a relayed PlayerJoined identity for another peer.
    "xpeer_identity":  re.compile(r"client installed cross-peer identity slot=(\d+)"),
    # host: it fanned a PlayerJoined out to the other clients.
    "host_relayed_pj": re.compile(r"host relayed PlayerJoined cross-peer identity"),
    # host: the per-slot connect edge fired (snapshot + flashlight + peer-state replay).
    "connect_edge":    re.compile(r"peer slot (\d+) connect edge -- replaying"),
    "epoch_latched":   re.compile(r"latched senderEpoch=0x[0-9a-fA-F]+ for peer slot (\d+)"),
    "stale_drop":      re.compile(r"stale-gen drop slot=(\d+)"),
    "malformed_drop":  re.compile(r"with senderEpoch=0 \(malformed"),
}


def parse_log_markers(path: Path) -> dict:
    """Extract Tier-2 cross-peer verdict markers from one peer's votv-coop.log.

    Returns a dict; missing/unreadable log -> {'present': False}. Sets of slots
    are returned for the multi-valued markers so the verdict can reason about
    WHICH peers a client saw, not just how many lines matched.
    """
    res = {
        "present": False,
        "assigned_slot": None,        # client: last slot the host gave us
        "host_accepted": set(),       # host: slots it accepted
        "puppet_slots": set(),        # slots we auto-spawned a puppet for
        "puppet_fail": set(),         # slots whose puppet spawn failed
        "xpeer_identity": set(),      # client: relayed PlayerJoined identities installed
        "host_relayed_pj": 0,         # host: PlayerJoined relays fired
        "connect_edges": set(),       # host: per-slot connect edges fired
        "epoch_latched": set(),       # slots we latched an epoch for
        "stale_drops": 0,             # stale-gen drops (benign during churn)
        "malformed_drops": 0,         # senderEpoch=0 drops (a real bug if >0)
    }
    if not path.exists():
        return res
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return res
    res["present"] = True
    for m in _MARK["assigned_slot"].finditer(text):
        res["assigned_slot"] = int(m.group(1))  # last wins (latest assignment)
    res["host_accepted"] = {int(m.group(1)) for m in _MARK["host_accepted"].finditer(text)}
    res["puppet_slots"] = {int(m.group(1)) for m in _MARK["puppet_slot"].finditer(text)}
    res["puppet_fail"] = {int(m.group(1)) for m in _MARK["puppet_fail"].finditer(text)}
    res["xpeer_identity"] = {int(m.group(1)) for m in _MARK["xpeer_identity"].finditer(text)}
    res["host_relayed_pj"] = len(_MARK["host_relayed_pj"].findall(text))
    res["connect_edges"] = {int(m.group(1)) for m in _MARK["connect_edge"].finditer(text)}
    res["epoch_latched"] = {int(m.group(1)) for m in _MARK["epoch_latched"].finditer(text)}
    res["stale_drops"] = len(_MARK["stale_drop"].findall(text))
    res["malformed_drops"] = len(_MARK["malformed_drop"].findall(text))
    return res


def wait_for_client_connect(game_dir: Path, timeout: int, label: str,
                            pid: int) -> int | None:
    """Poll a client's votv-coop.log until it logs 'host assigned us peer slot
    N' (== reached connected). Returns the assigned slot, or None on timeout /
    process death. Staggering launches behind this both spreads the boot-RSS
    peaks and gives deterministic slot ordering (client1->1, client2->2, ...),
    which is what makes the cross-peer verdict legible."""
    log_path = game_dir / "votv-coop.log"
    for i in range(timeout):
        time.sleep(1)
        if not any(p["PID"] == pid for p in list_votv()):
            log(f"  {label}: process PID {pid} died before connecting")
            return None
        mk = parse_log_markers(log_path)
        if mk["assigned_slot"] is not None:
            log(f"  {label}: connected -- host assigned peer slot {mk['assigned_slot']} after {i+1}s")
            return mk["assigned_slot"]
    log(f"  {label}: did NOT reach connected within {timeout}s (slow-load or handshake stall)")
    return None


def set_dev_ue4ss(enabled: bool) -> None:
    """Toggle UE4SS in the _dev folder (CLIENT3's game copy) by parking its
    dwmapi.dll proxy. The _dev copy is the ONLY one carrying UE4SS; loading it
    as a 4th game instance crashes at DLL-load (confirmed 2026-05-30 -- the
    dev peer died with only its log header written), and a 4-peer SHIPPING-
    behaviour validation must be pure-standalone anyway (RULE 3). smoke4
    disables UE4SS for the run and RESTORES it on exit so the user's dev RE
    workflow is untouched. Idempotent; safe if dwmapi.dll is absent."""
    active = DEV_DIR / "dwmapi.dll"
    parked = DEV_DIR / "dwmapi.dll.smoke-off"
    try:
        if enabled:
            if parked.exists() and not active.exists():
                parked.rename(active)
                log("  dev UE4SS: RESTORED (dwmapi.dll back in place)")
        else:
            if active.exists():
                active.rename(parked)
                log("  dev UE4SS: disabled for smoke (dwmapi.dll parked as .smoke-off)")
    except OSError as e:
        log(f"  dev UE4SS toggle (enabled={enabled}) FAILED: {e} "
            f"(a _dev VotV may still hold the dll)")


def cmd_client3(args) -> None:
    deploy_all()
    # Client #3: tile index 2. Launches from the _dev folder (the 4th game copy).
    pid = launch_peer("client", args.port, args.nick or "Client3",
                      peer=args.peer, res_x=args.res_x, res_y=args.res_y,
                      peer_slot=3, monitor=args.monitor, tile_index=2,
                      memory_limit_gb=args.memory_limit_gb)
    log(f"client3 running PID={pid}")


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
    # Liveness alone is NOT connection: the slow-load flake leaves a client
    # stuck in the menu (alive but state != connected) and the old verdict
    # PASSed it. Require the client log to prove it reached the connected
    # handshake AND saw the host's puppet (slot 0).
    cmk = parse_log_markers(CLIENT_DIR / "votv-coop.log")
    if cmk["assigned_slot"] is None:
        log("FAIL: client never reached connected (no 'host assigned us peer slot' "
            "in its log -- slow-load/handshake stall; re-run with a longer --duration)")
        sys.exit(4)
    if 0 not in cmk["puppet_slots"]:
        log(f"FAIL: client connected (slot {cmk['assigned_slot']}) but never spawned "
            "the host puppet (no remote pose on slot 0 -- pose stream not flowing)")
        sys.exit(5)
    if cmk["malformed_drops"] > 0:
        log(f"FAIL: client logged {cmk['malformed_drops']} malformed (senderEpoch=0) drop(s)")
        sys.exit(6)
    log(f"PASS: both peers stable, client connected (slot {cmk['assigned_slot']}), "
        f"host puppet spawned, no RAM breach"
        + (f", {cmk['stale_drops']} benign stale-gen drop(s)" if cmk["stale_drops"] else ""))
    sys.exit(0)


def cmd_smoke4(args) -> None:
    """Autonomous 4-PEER LAN smoke (Tier 8) -- the scenario that validates the
    Tier 2 host-relay end-to-end.

    Order:
      1. Kill stragglers + deploy.
      2. Launch host; wait for UDP bind.
      3. Launch the 3 clients ONE AT A TIME, each behind wait_for_client_connect
         (spreads the boot-RSS peaks that caused the slow-load flake under
         3-simultaneous-boot contention, and pins deterministic slot ordering).
      4. Monitor RSS for --duration (steady state). A per-process Job Object cap
         (launch_peer) is the hard runaway guard; the sampling kill here only
         arms AFTER --settle-grace so the legitimate prop-snapshot boot peak
         (~8 GB, settles ~3.5 GB) no longer trips a false kill -- this retires
         the --ram-kill-mb 12000 workaround.
      5. LOG-DRIVEN verdict (the point of the whole scenario):
         - all 4 peers alive at end,
         - host accepted all 3 clients,
         - every client reached connected (assigned a slot),
         - CROSS-PEER: every client auto-spawned a puppet for the host (slot 0)
           AND for both OTHER clients -- the relay proof,
         - no puppet-spawn failures, no malformed (epoch=0) drops.
    """
    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before smoke4")

    deploy_all()

    # CLIENT3 launches from the _dev folder, the only copy carrying UE4SS.
    # UE4SS as a 4th instance crashes at DLL-load; park it for the run and
    # restore on exit. atexit fires on normal return AND on sys.exit() (the
    # verdict path), so the dev folder is always left as we found it.
    set_dev_ue4ss(False)
    atexit.register(set_dev_ue4ss, True)

    log("--- HOST LAUNCH ---")
    host_pid = launch_peer("host", args.port, "Host",
                           peer=None, res_x=args.res_x, res_y=args.res_y,
                           monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb)
    log(f"waiting up to {args.boot_timeout}s for host to bind UDP {args.port}...")
    bound = False
    for i in range(args.boot_timeout):
        time.sleep(1)
        if host_owns_udp(host_pid, args.port):
            log(f"host bound UDP {args.port} after {i+1}s")
            bound = True
            break
        if not any(p["PID"] == host_pid for p in list_votv()):
            log(f"HOST DIED before binding UDP (PID {host_pid} gone)")
            tail_log(HOST_DIR / "votv-coop.log", 30, "HOST")
            sys.exit(1)
    if not bound:
        log(f"FAIL: host did NOT bind UDP within {args.boot_timeout}s")
        tail_log(HOST_DIR / "votv-coop.log", 30, "HOST")
        kill_all()
        sys.exit(1)

    # Staggered client launch. Each client waits to reach connected before the
    # next launches: spreads boot peaks + makes slot assignment deterministic.
    client_specs = [
        (1, CLIENT_DIR,  "CLIENT1"),
        (2, CLIENT2_DIR, "CLIENT2"),
        (3, DEV_DIR,     "CLIENT3"),
    ]
    client_pids: dict[int, int] = {}
    assigned: dict[int, int | None] = {}
    for slot_arg, game_dir, label in client_specs:
        log(f"--- {label} LAUNCH (peer_slot folder {slot_arg}) ---")
        pid = launch_peer("client", args.port, label.capitalize(),
                          peer="127.0.0.1", res_x=1280, res_y=720,
                          peer_slot=slot_arg, monitor=2, tile_index=slot_arg - 1,
                          memory_limit_gb=args.memory_limit_gb)
        client_pids[slot_arg] = pid
        assigned[slot_arg] = wait_for_client_connect(
            game_dir, args.client_boot_timeout, label, pid)

    log(f"--- MONITORING for {args.duration}s (sample every {args.sample_interval}s, "
        f"RAM kill arms after {args.settle_grace}s) ---")
    t0 = time.time()
    last_peers: list[dict] = []
    rss_series: dict[int, list[float]] = {}
    kill_reason: str | None = None
    while time.time() - t0 < args.duration:
        time.sleep(args.sample_interval)
        peers = list_votv()
        last_peers = peers
        t = int(time.time() - t0)
        desc = ", ".join(f"PID{p['PID']}={p['RSS_MB']}MB '{p['Title']}'" for p in peers) or "NONE"
        log(f"  t={t}s peers={len(peers)}: {desc}")
        for p in peers:
            rss_series.setdefault(p["PID"], []).append(p["RSS_MB"])
        max_rss = max((p["RSS_MB"] for p in peers), default=0)
        if t >= args.settle_grace and max_rss > args.ram_kill_mb:
            kill_reason = f"peer RSS={max_rss}MB > kill threshold {args.ram_kill_mb}MB (post-settle)"
            break

    log("--- FINAL STATE ---")
    log(f"peers alive at end: {len(last_peers)}")
    for p in last_peers:
        log(f"  PID={p['PID']} RSS={p['RSS_MB']}MB title='{p['Title']}'")

    # Tail all four logs.
    tail_log(HOST_DIR / "votv-coop.log", 20, "HOST")
    for slot_arg, game_dir, label in client_specs:
        tail_log(game_dir / "votv-coop.log", 20, label)

    # Parse all four logs for the verdict (after kill is fine -- logs are flushed
    # on each write; parse BEFORE kill to be safe against a crash-on-exit wipe).
    host_mk = parse_log_markers(HOST_DIR / "votv-coop.log")
    client_mks = {slot: parse_log_markers(gd / "votv-coop.log")
                  for slot, gd, _ in client_specs}

    log("--- KILLING ---")
    kill_all()

    # --- Log-driven verdict ---
    log("--- VERDICT (log-driven, 4-peer cross-peer relay) ---")
    num_clients = len(client_specs)
    failures: list[str] = []
    notes: list[str] = []

    if kill_reason:
        failures.append(kill_reason)

    if len(last_peers) != num_clients + 1:
        failures.append(f"expected {num_clients + 1} peers alive at end, got {len(last_peers)}")

    # Host side.
    log(f"host: accepted slots={sorted(host_mk['host_accepted'])} "
        f"relayed_PlayerJoined={host_mk['host_relayed_pj']} "
        f"connect_edges={sorted(host_mk['connect_edges'])} "
        f"epoch_latched={sorted(host_mk['epoch_latched'])}")
    if len(host_mk["host_accepted"]) < num_clients:
        failures.append(f"host accepted only {len(host_mk['host_accepted'])} "
                        f"client(s), expected {num_clients}")
    if host_mk["host_relayed_pj"] < num_clients - 1:
        # With N clients the host fires PlayerJoined relays as each later joiner
        # arrives; <N-1 means cross-peer identity fan-out under-fired.
        notes.append(f"host relayed PlayerJoined only {host_mk['host_relayed_pj']} "
                     f"time(s) (expected >= {num_clients - 1})")

    # Per-client side -- the cross-peer relay proof.
    connected_slots = {s for s, a in assigned.items() if a is not None}
    for slot_arg, game_dir, label in client_specs:
        mk = client_mks[slot_arg]
        own = mk["assigned_slot"]
        log(f"{label}: assigned_slot={own} puppet_slots={sorted(mk['puppet_slots'])} "
            f"xpeer_identity={sorted(mk['xpeer_identity'])} "
            f"puppet_fail={sorted(mk['puppet_fail'])} "
            f"stale_drops={mk['stale_drops']} malformed_drops={mk['malformed_drops']}")
        if own is None:
            failures.append(f"{label} never reached connected (no peer slot assigned)")
            continue
        if 0 not in mk["puppet_slots"]:
            failures.append(f"{label} (slot {own}) never spawned the host puppet (no pose on slot 0)")
        # Cross-peer puppets: any spawned slot that is neither host(0) nor self.
        # In the pre-Tier-2 star topology this set was always empty.
        xpeer = {s for s in mk["puppet_slots"] if s != 0 and s != own}
        if len(xpeer) < num_clients - 1:
            failures.append(f"{label} (slot {own}) saw {len(xpeer)} cross-peer puppet(s) "
                            f"{sorted(xpeer)}, expected {num_clients - 1} "
                            f"(RELAY GAP -- the other clients are invisible to it)")
        else:
            log(f"  {label}: CROSS-PEER OK -- sees host + {sorted(xpeer)} via relay")
        if mk["puppet_fail"]:
            failures.append(f"{label} puppet spawn FAILED for slot(s) {sorted(mk['puppet_fail'])}")
        if mk["malformed_drops"] > 0:
            failures.append(f"{label} logged {mk['malformed_drops']} malformed (epoch=0) drop(s)")
        if mk["stale_drops"] > 0:
            notes.append(f"{label} had {mk['stale_drops']} benign stale-gen drop(s)")

    for n in notes:
        log(f"NOTE: {n}")

    if failures:
        log(f"FAIL ({len(failures)} issue(s)):")
        for f in failures:
            log(f"  - {f}")
        sys.exit(2)
    log(f"PASS: 4 peers stable; host accepted {num_clients} clients; all clients "
        f"connected {sorted(connected_slots)}; every client sees host + both peers "
        f"via relay; no spawn failures, no malformed drops.")
    sys.exit(0)


def _log_count(log_path: Path, needle: str) -> int:
    try:
        return log_path.read_text(errors="replace").count(needle)
    except OSError:
        return 0


def _wait_for_log(log_path: Path, needle: str, timeout: int, label: str) -> bool:
    for i in range(timeout):
        if _log_count(log_path, needle) > 0:
            log(f"  {label}: '{needle[:42]}' seen after {i+1}s")
            return True
        time.sleep(1)
    log(f"  {label}: '{needle[:42]}' NOT seen within {timeout}s")
    return False


def _capture_window(pid: int, out_path: Path) -> bool:
    ps = Path(__file__).resolve().parent / "capture_window.ps1"
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
             "-File", str(ps), "-ProcId", str(pid), "-Out", str(out_path)],
            capture_output=True, text=True, timeout=40)
        if r.returncode == 0:
            log(f"  captured PID {pid} -> {out_path.name}  ({r.stdout.strip()})")
            return True
        log(f"  capture FAILED for PID {pid}: {(r.stderr or r.stdout).strip()[:200]}")
        return False
    except Exception as e:  # noqa: BLE001 (best-effort capture)
        log(f"  capture EXC for PID {pid}: {e}")
        return False


def cmd_npctest(args) -> None:
    """Spawn a kerfurOmega NPC on the HOST and verify it (a) spawns host-side and
    (b) mirrors to every connected client. The ONLY end-to-end test of the
    NPC-sync path (host AllocAndInstall + EntitySpawn broadcast + client mirror
    Install) -- VOTV NPCs never autonomously spawn, so this is how
    PR-FOUNDATION-3 Inc2 gets RUNTIME-validated. Captures each peer's window to a
    PNG for visual confirmation. --peers 1 = host-only (spawn + screenshot);
    --peers 4 = host + 3 clients (full mirror test)."""
    shots_dir = Path(__file__).resolve().parent.parent / "research" / "npctest_shots"
    shots_dir.mkdir(parents=True, exist_ok=True)
    trigger = str(HOST_DIR / "spawn_npc.trigger")
    try:
        Path(trigger).unlink()
    except OSError:
        pass

    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before npctest")
    deploy_all()

    peers = max(1, min(4, args.peers))
    if peers >= 4:
        set_dev_ue4ss(False)
        atexit.register(set_dev_ue4ss, True)

    log("--- HOST LAUNCH (NPC spawn trigger armed) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb, trigger_file=trigger)
    log(f"waiting up to {args.boot_timeout}s for host to bind UDP {args.port}...")
    bound = False
    for i in range(args.boot_timeout):
        time.sleep(1)
        if host_owns_udp(host_pid, args.port):
            log(f"host bound UDP {args.port} after {i+1}s")
            bound = True
            break
        if not any(p["PID"] == host_pid for p in list_votv()):
            log("HOST DIED before binding UDP")
            tail_log(HOST_DIR / "votv-coop.log", 30, "HOST")
            sys.exit(1)
    if not bound:
        log("FAIL: host did not bind UDP")
        kill_all()
        sys.exit(1)

    client_specs = [(1, CLIENT_DIR, "CLIENT1"), (2, CLIENT2_DIR, "CLIENT2"), (3, DEV_DIR, "CLIENT3")]
    client_pids: dict[int, int] = {}
    used_clients: list[tuple[int, Path, str]] = []
    for slot_arg, game_dir, label in client_specs[:peers - 1]:
        log(f"--- {label} LAUNCH ---")
        pid = launch_peer("client", args.port, label.capitalize(), peer="127.0.0.1",
                          res_x=1280, res_y=720, peer_slot=slot_arg, monitor=2,
                          tile_index=slot_arg - 1, memory_limit_gb=args.memory_limit_gb)
        client_pids[slot_arg] = pid
        wait_for_client_connect(game_dir, args.client_boot_timeout, label, pid)
        used_clients.append((slot_arg, game_dir, label))

    # Gate the spawn on the host having finished resolving its NPC refs (the
    # "installed interceptor" line fires only once the gameplay world is up + all
    # 12 NPC classes resolved -- i.e. DevSpawnNpcInFront's refs are ready). This
    # is deterministic regardless of peer count (works for solo + 4-peer).
    host_log = HOST_DIR / "votv-coop.log"
    _wait_for_log(host_log, "npc-suppress: installed interceptor", args.install_timeout, "HOST")
    log(f"--- settling {args.settle}s, then firing spawn trigger ---")
    time.sleep(args.settle)
    log(f"--- writing spawn trigger {trigger} ---")
    Path(trigger).write_text("spawn")
    # Wait for the host to consume the trigger (spawn) + clients to mirror.
    _wait_for_log(host_log, "spawn_npc: spawned 'kerfurOmega_C'", 20, "HOST")
    log(f"--- waiting {args.spawn_wait}s for mirror propagation ---")
    time.sleep(args.spawn_wait)

    log("--- CAPTURING WINDOWS ---")
    shots: dict[str, Path] = {}
    suffix = f"_{peers}p" if peers > 1 else "_solo"
    hp = shots_dir / f"host{suffix}.png"
    if _capture_window(host_pid, hp):
        shots["HOST"] = hp
    for slot_arg, game_dir, label in used_clients:
        cp = shots_dir / f"client{slot_arg}{suffix}.png"
        if _capture_window(client_pids[slot_arg], cp):
            shots[label] = cp

    # Read logs for the verdict (before kill).
    host_spawned = _log_count(host_log, "spawn_npc: spawned 'kerfurOmega_C'")
    host_bcast = _log_count(host_log, "broadcast EntitySpawn class='kerfurOmega")
    host_bound = _log_count(host_log, "[host POST]: bound actor")
    tail_log(host_log, 14, "HOST")
    client_mirror: dict[str, int] = {}
    for slot_arg, game_dir, label in used_clients:
        client_mirror[label] = _log_count(game_dir / "votv-coop.log", "materialized mirror")
        tail_log(game_dir / "votv-coop.log", 8, label)

    log("--- KILLING ---")
    kill_all()

    log("--- NPCTEST VERDICT ---")
    fails: list[str] = []
    log(f"host: dev-spawn={host_spawned}, EntitySpawn-broadcast={host_bcast}, POST-bound={host_bound}")
    if host_spawned < 1:
        fails.append("host did NOT log a dev-spawn of kerfurOmega_C (spawn primitive failed)")
    if peers >= 2:
        if host_bcast < 1:
            fails.append("host did NOT broadcast EntitySpawn for the kerfur (AllocAndInstall/interceptor path)")
        if host_bound < 1:
            fails.append("host POST observer did NOT bind the kerfur actor")
        for slot_arg, game_dir, label in used_clients:
            n = client_mirror[label]
            log(f"{label}: materialized-mirror lines={n}")
            if n < 1:
                fails.append(f"{label} did NOT materialize a kerfur mirror")
    for name, p in shots.items():
        log(f"  screenshot {name}: {p}")
    if fails:
        log(f"FAIL ({len(fails)} issue(s)):")
        for f in fails:
            log(f"  - {f}")
        sys.exit(2)
    if peers >= 2:
        log(f"PASS: host spawned kerfurOmega_C + broadcast it; all {len(used_clients)} "
            f"client(s) materialized the mirror.")
    else:
        log("PASS: host spawned kerfurOmega_C (solo -- screenshot captured).")
    sys.exit(0)


def cmd_ragdollshot(args) -> None:
    """Force the CLIENT's player into ragdoll over the wire (leak-safe -- the test
    driver flips isRagdoll directly, no real ragdollMode / no playerRagdoll_C) and
    capture 2 screenshots of the HOST window showing the client's PUPPET, to verify
    the puppet actually FALLS limp (own-mesh physics flop) rather than staying
    rigid. Reuses the RAGDOLL_TEST (client = driver, host = observer) with an
    extended hold so both shots land during the flop."""
    shots_dir = Path(__file__).resolve().parent.parent / "research" / "ragdoll_shots"
    shots_dir.mkdir(parents=True, exist_ok=True)

    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before ragdollshot")
    deploy_all()

    # Drive the ragdoll e2e test (client drives via direct isRagdoll write -> host
    # puppet flops via own-mesh sim) with a long hold so we can grab 2 host shots.
    os.environ["VOTVCOOP_RUN_RAGDOLL_TEST"] = "1"
    os.environ["VOTVCOOP_RAGDOLL_HOLD_MS"] = str(args.hold_ms)

    log("--- HOST LAUNCH (ragdoll observer) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb)
    log(f"waiting up to {args.boot_timeout}s for host to bind UDP {args.port}...")
    bound = False
    for i in range(args.boot_timeout):
        time.sleep(1)
        if host_owns_udp(host_pid, args.port):
            log(f"host bound UDP {args.port} after {i+1}s"); bound = True; break
        if not any(p["PID"] == host_pid for p in list_votv()):
            log("HOST DIED before binding UDP"); tail_log(HOST_DIR / "votv-coop.log", 30, "HOST"); sys.exit(1)
    if not bound:
        log("FAIL: host did not bind UDP"); kill_all(); sys.exit(1)

    log("--- CLIENT LAUNCH (ragdoll driver) ---")
    client_pid = launch_peer("client", args.port, "Client", peer="127.0.0.1",
                             res_x=1280, res_y=720, peer_slot=1, monitor=2,
                             tile_index=0, memory_limit_gb=args.memory_limit_gb)
    wait_for_client_connect(CLIENT_DIR, args.client_boot_timeout, "CLIENT", client_pid)

    host_log = HOST_DIR / "votv-coop.log"
    shots: list[Path] = []
    # BEFORE shot: the host frames the STANDING puppet (before the ragdoll fires).
    if _wait_for_log(host_log, "BEFORE-SHOT READY", args.ragdoll_timeout, "HOST"):
        time.sleep(1)  # let the camera aim settle
        pb = shots_dir / "host_ragdoll_before.png"
        if _capture_window(host_pid, pb):
            shots.append(pb)
            log(f"  BEFORE shot (standing puppet): {pb.name}")
    else:
        log("WARN: never saw BEFORE-SHOT READY -- skipping the before shot")
    # DURING shots: the driver fires the ragdoll ~12 s after its local player
    # resolves; the host observer logs the rising edge once the puppet flops.
    if not _wait_for_log(host_log, "observed RISING edge", args.ragdoll_timeout, "HOST"):
        log("FAIL: host never observed the ragdoll rising edge")
        tail_log(host_log, 30, "HOST"); kill_all(); sys.exit(2)
    log(f"--- CAPTURING HOST 2x during the flop ({args.shot_gap}s apart) ---")
    for n in (1, 2):
        p = shots_dir / f"host_ragdoll_during_{n}.png"
        if _capture_window(host_pid, p):
            shots.append(p)
        if n == 1:
            time.sleep(args.shot_gap)

    # Verdict markers (read before kill).
    flopping = _log_count(host_log, "physically flopping=1")
    visible = _log_count(host_log, "sim-mesh IsVisible=1")
    tail_log(host_log, 14, "HOST")

    log("--- KILLING ---")
    kill_all()

    log("--- RAGDOLLSHOT VERDICT ---")
    for p in shots:
        log(f"  screenshot: {p}")
    log(f"host: 'physically flopping=1' lines={flopping}, 'sim-mesh IsVisible=1' lines={visible}")
    if len(shots) < 2:
        log(f"FAIL: captured only {len(shots)}/2 host screenshots"); sys.exit(2)
    if flopping < 1:
        log("WARN: host never logged 'physically flopping=1' -- the puppet may be RIGID; inspect the screenshots")
    log(f"DONE: 2 host screenshots captured during the flop -> {shots_dir}")
    sys.exit(0)


def cmd_ragdollspawn(args) -> None:
    """SP-SOLO xray-ragdoll feasibility probe. Launches ONE host instance with
    VOTVCOOP_RUN_RAGDOLL_SPAWN_PROBE=1 (no client). In plain single-player the
    probe spawns playerRagdoll_C MANUALLY (deferred spawn + set Player, NO
    ragdollMode) then triggers a REAL ragdollMode body for comparison. Captures a
    host screenshot of each so we can SEE whether the manual body is visible +
    ragdolling AND whether the screen stays un-faded (no death). The decisive
    verdict is in the log tail ([manual] vs [real] dumps + the Q1b death check)."""
    shots_dir = Path(__file__).resolve().parent.parent / "research" / "ragdoll_shots"
    shots_dir.mkdir(parents=True, exist_ok=True)

    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before ragdollspawn")
    deploy_all()

    os.environ["VOTVCOOP_RUN_RAGDOLL_SPAWN_PROBE"] = "1"

    log("--- HOST LAUNCH (solo xray-ragdoll probe) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb)

    host_log = HOST_DIR / "votv-coop.log"
    shots: list[Path] = []

    # REAL body shot FIRST -- ground truth from VOTV's own ragdollMode (the probe
    # runs the real experiment first to force-load the playerRagdoll_C BP class).
    if _wait_for_log(host_log, "REAL-SHOT READY", args.probe_timeout, "HOST"):
        time.sleep(1)  # let the camera aim settle
        pr = shots_dir / "host_ragdoll_real.png"
        if _capture_window(host_pid, pr):
            shots.append(pr)
            log(f"  REAL body shot: {pr.name}")
    else:
        log("WARN: never saw REAL-SHOT READY -- check the host log tail below")
        tail_log(host_log, 30, "HOST")

    # MANUAL body shot -- the playerRagdoll_C we spawned ourselves (no ragdollMode).
    if _wait_for_log(host_log, "MANUAL-SHOT READY", args.real_timeout, "HOST"):
        time.sleep(1)
        pm = shots_dir / "host_ragdoll_manual.png"
        if _capture_window(host_pid, pm):
            shots.append(pm)
            log(f"  MANUAL body shot: {pm.name}")
    else:
        log("WARN: never saw MANUAL-SHOT READY")

    # Let the probe finish so the verdict (manual-vs-real dumps) is in the log.
    _wait_for_log(host_log, "ragdollspawn: DONE", 40, "HOST")
    tail_log(host_log, 44, "HOST")

    log("--- KILLING ---")
    kill_all()

    log("--- RAGDOLLSPAWN VERDICT ---")
    for p in shots:
        log(f"  screenshot: {p}")
    log("Read the [manual] vs [real] dumps in the tail above: manual is viable iff its body "
        "IsVisible=1 + IsAnyRigidBodyAwake=1 + lowestBoneZ falls AND Q1b says DEATH-FREE.")
    log(f"DONE: {len(shots)} screenshot(s) -> {shots_dir}")
    sys.exit(0 if len(shots) >= 1 else 2)


def cmd_menutravel(args) -> None:
    """SOLO SP BYPASS probe: does arming our transparent ProcessEvent-detour bypass
    let VOTV's transition("/Game/menu") tear down + travel to the menu without our
    layer hanging the swap? Launches ONE host instance with
    VOTVCOOP_RUN_MENUTRAVEL_PROBE=1 (no client). The probe settles in gameplay, arms
    the bypass, issues transition, waits for the fade+teardown+menu load, then logs
    'MENU-SHOT READY'. We CAPTURE the window there: if the shot shows VOTV's MAIN MENU,
    the bypass works and the production death-flee design is sound."""
    shots_dir = Path(__file__).resolve().parent.parent / "research" / "menutravel_shots"
    shots_dir.mkdir(parents=True, exist_ok=True)

    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before menutravel")
    deploy_all()

    os.environ["VOTVCOOP_RUN_MENUTRAVEL_PROBE"] = "1"

    log("--- HOST LAUNCH (solo menu-travel BYPASS probe) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb)

    host_log = HOST_DIR / "votv-coop.log"
    shot: Path | None = None
    t0 = time.time()
    seen_pause = False
    seen_ready = False
    t_shot = 0.0
    rss_peak = 0.0
    rss_at_shot = 0.0
    ram_guard_mb = 14000  # protect the user's RAM -- the leak is the bug under test
    while time.time() - t0 < args.probe_timeout:
        time.sleep(3)
        peers = list_votv()
        if not peers:
            log("  (no VotV process -- exited/crashed)")
            break
        rss = max((p["RSS_MB"] for p in peers), default=0)
        rss_peak = max(rss_peak, rss)
        log(f"  t+{int(time.time()-t0)}s host RSS={rss}MB")
        if rss > ram_guard_mb:
            log(f"  RAM GUARD: RSS={rss}MB > {ram_guard_mb}MB -- killing now (LEAK reproduced)")
            break
        try:
            tailtext = host_log.read_text(errors="ignore")[-4000:]
        except Exception:
            tailtext = ""
        if not seen_pause and "PAUSE-SHOT READY" in tailtext:
            seen_pause = True
            time.sleep(1)
            pshot = shots_dir / "menutravel_pause.png"
            if _capture_window(host_pid, pshot):
                log(f"  pause shot: {pshot.name} (RSS={rss}MB -- did the pause menu open?)")
        if not seen_ready and "MENU-SHOT READY" in tailtext:
            seen_ready = True
            t_shot = time.time()
            rss_at_shot = rss
            time.sleep(1)
            shot = shots_dir / "menutravel_result.png"
            if _capture_window(host_pid, shot):
                log(f"  result shot: {shot.name} (RSS={rss}MB at shot)")
        if seen_ready and time.time() - t_shot > 12:  # confirm RSS stays flat ~12s at menu
            break
    if not seen_ready:
        log("WARN: never saw 'MENU-SHOT READY' before exit/guard -- travel hung or leaked")
    tail_log(host_log, 30, "HOST")

    log(f"--- KILLING (rss_peak={rss_peak}MB, rss_at_shot={rss_at_shot}MB) ---")
    kill_all()

    log("--- MENUTRAVEL VERDICT ---")
    if shot and shot.exists():
        log(f"Inspect {shot}: if it shows VOTV's MAIN MENU, the transparent-bypass flee "
            "WORKS -> wire it into the death path. If it shows a frozen gameplay/black "
            "screen, the teardown hangs even with our layer bypassed -> menu infeasible, "
            "fall back to exit-to-desktop.")
    else:
        log("No screenshot captured -- the travel hung before MENU-SHOT READY.")
    sys.exit(0)


def cmd_clumpvis(args) -> None:
    """SOLO clump-visibility probe. Launches ONE host with VOTVCOOP_RUN_CLUMPVIS_PROBE=1;
    the probe spawns a bare prop_garbageClump_C ~150cm in front of the player + logs
    whether its StaticMesh asset is null (empty) or named (visible), then holds it for
    a screenshot. Gates the mannequin-model clump rework: visible -> no mesh copy
    needed; empty -> the mirror must copy the source mesh over the wire."""
    shots_dir = Path(__file__).resolve().parent.parent / "research" / "clumpvis_shots"
    shots_dir.mkdir(parents=True, exist_ok=True)

    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before clumpvis")
    deploy_all()

    os.environ["VOTVCOOP_RUN_CLUMPVIS_PROBE"] = "1"

    log("--- HOST LAUNCH (solo clump-visibility probe) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb)

    host_log = HOST_DIR / "votv-coop.log"
    if _wait_for_log(host_log, "CLUMPVIS READY", args.probe_timeout, "HOST"):
        time.sleep(2)
        p = shots_dir / "host_clumpvis.png"
        if _capture_window(host_pid, p):
            log(f"  clumpvis shot: {p}")
    else:
        log("WARN: never saw CLUMPVIS READY")
    # Surface the decisive 'bare clump ... HAS A MESH / EMPTY' line.
    time.sleep(2)
    tail_log(host_log, 12, "HOST")
    log("--- KILLING ---")
    kill_all()
    sys.exit(0)


def cmd_fogprobe(args) -> None:
    """SOLO SP fog ON/OFF model + clear-path probe. Launches ONE host instance with
    VOTVCOOP_RUN_FOG_PROBE=1 (no client). The probe forces fog ON via the cycle's own
    spawnFog()/superFogEvent() verbs, samples finalFogDensity/thickFog/actor-presence
    for ~12 s (the density-vs-target model that gates the wire design), then runs the
    RE'd CLEAR sequence (destroy the rolling-fog + super-fog actors + zero density +
    SetFogDensity()) and confirms it stays cleared. Captures a FOGGY then a CLEARED
    screenshot; the decisive verdict (density evolution + 'VERDICT cleared=N') is in
    the log tail. Confirms the clear-path mechanics + the thickFog-target question
    before any protocol change is written."""
    shots_dir = Path(__file__).resolve().parent.parent / "research" / "fog_shots"
    shots_dir.mkdir(parents=True, exist_ok=True)

    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before fogprobe")
    deploy_all()

    os.environ["VOTVCOOP_RUN_FOG_PROBE"] = "1"

    log("--- HOST LAUNCH (solo fog probe) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb)

    host_log = HOST_DIR / "votv-coop.log"
    shots: list[Path] = []

    # FOGGY shot -- fog forced on (rolling + super).
    if _wait_for_log(host_log, "FOG-FORCED READY", args.probe_timeout, "HOST"):
        time.sleep(1)
        pf = shots_dir / "host_fog_forced.png"
        if _capture_window(host_pid, pf):
            shots.append(pf)
            log(f"  FOGGY shot: {pf.name}")
    else:
        log("WARN: never saw FOG-FORCED READY -- check the host log tail below")
        tail_log(host_log, 30, "HOST")

    # CLEARED shot -- after the clear sequence.
    if _wait_for_log(host_log, "FOG-CLEARED READY", args.clear_timeout, "HOST"):
        time.sleep(1)
        pc = shots_dir / "host_fog_cleared.png"
        if _capture_window(host_pid, pc):
            shots.append(pc)
            log(f"  CLEARED shot: {pc.name}")
    else:
        log("WARN: never saw FOG-CLEARED READY")

    # Let the probe finish so the VERDICT + sample evolution are in the log.
    _wait_for_log(host_log, "fogprobe: DONE", 40, "HOST")
    tail_log(host_log, 44, "HOST")

    log("--- KILLING ---")
    kill_all()

    log("--- FOGPROBE VERDICT ---")
    for p in shots:
        log(f"  screenshot: {p}")
    log("Read the [active #N] samples in the tail: if thickFog holds a stable high target "
        "while finalFogDensity ramps toward it, the wire streams thickFog (single broadcast). "
        "The 'VERDICT cleared=1' line proves destroy-actors+SetFogDensity is the host-auth clear.")
    log(f"DONE: {len(shots)} screenshot(s) -> {shots_dir}")
    sys.exit(0 if len(shots) >= 1 else 2)


def cmd_menushot(args) -> None:
    """SOLO screenshot proof of the Dear ImGui F1 menu. Launches ONE host with
    VOTVCOOP_MENU_OPEN=1 so the menu starts visible (an autonomous run can't press
    F1), waits for the overlay's DX11 bring-up, and captures the window -- the shot
    should show the nested category menu drawn over the game (even the VOTV main
    menu, since the overlay hooks Present). Proves the DXGI present hook renders
    ImGui WITHOUT crashing the game's render thread (the riskiest part)."""
    shots_dir = Path(__file__).resolve().parent.parent / "research" / "menu_shots"
    shots_dir.mkdir(parents=True, exist_ok=True)
    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before menushot")
    deploy_all()
    os.environ["VOTVCOOP_MENU_OPEN"] = "1"

    log("--- HOST LAUNCH (solo ImGui menu shot) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb)
    host_log = HOST_DIR / "votv-coop.log"
    shot = None
    if _wait_for_log(host_log, "imgui_overlay: DX11 bring-up OK", args.probe_timeout, "HOST"):
        time.sleep(3)  # let a few menu frames render
        p = shots_dir / "host_menu.png"
        if _capture_window(host_pid, p):
            shot = p
            log(f"  menu shot: {p.name}")
    else:
        log("WARN: never saw 'imgui_overlay: DX11 bring-up OK' -- DX12 RHI or a hook failure; see tail")
    tail_log(host_log, 30, "HOST")
    log("--- KILLING ---")
    kill_all()
    log("--- MENUSHOT VERDICT ---")
    if shot and shot.exists():
        log(f"Inspect {shot}: it should show the 'VOTV Coop  -  Menu (F1)' window with the "
            "nested category tree (Player/Game/Network/Cosmetics) over the game.")
    else:
        log("No screenshot -- overlay didn't bring up (DX12 RHI, or a hook fault -- read the tail).")
    sys.exit(0 if shot else 2)


def cmd_scoreshot(args) -> None:
    """2-PEER screenshot proof of the TAB player list. Launches host + client with
    VOTVCOOP_SCOREBOARD_OPEN=1 (an autonomous run can't hold/press TAB), waits for the
    client to connect + announce its nick over the Join reliable, then captures the
    HOST window -- the shot should show the roster table with BOTH rows ('Host (You)'
    + 'Client'). Proves the roster snapshot reads real per-peer connection + nick
    state and renders as a second overlay surface."""
    shots_dir = Path(__file__).resolve().parent.parent / "research" / "menu_shots"
    shots_dir.mkdir(parents=True, exist_ok=True)
    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before scoreshot")
    deploy_all()
    os.environ["VOTVCOOP_SCOREBOARD_OPEN"] = "1"

    log("--- HOST LAUNCH (2-peer scoreboard shot) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb)
    host_log = HOST_DIR / "votv-coop.log"
    log(f"waiting up to {args.boot_timeout}s for host to bind UDP {args.port}...")
    bound = False
    for i in range(args.boot_timeout):
        time.sleep(1)
        if host_owns_udp(host_pid, args.port):
            log(f"host bound UDP {args.port} after {i+1}s"); bound = True; break
        if not any(p["PID"] == host_pid for p in list_votv()):
            log("HOST DIED before binding UDP"); tail_log(host_log, 30, "HOST"); sys.exit(1)
    if not bound:
        log(f"FAIL: host did not bind UDP within {args.boot_timeout}s")
        tail_log(host_log, 30, "HOST"); kill_all(); sys.exit(1)

    log("--- CLIENT LAUNCH ---")
    client_pid = launch_peer("client", args.port, "Client", peer="127.0.0.1",
                             res_x=1280, res_y=720, monitor=2, tile_index=0,
                             memory_limit_gb=args.memory_limit_gb)
    client_log = CLIENT_DIR / "votv-coop.log"

    log(f"waiting up to {args.probe_timeout}s for the client to connect...")
    connected = False
    for i in range(args.probe_timeout):
        time.sleep(1)
        if parse_log_markers(client_log)["assigned_slot"] is not None:
            log(f"client connected after {i+1}s"); connected = True; break
        if not any(p["PID"] == client_pid for p in list_votv()):
            log("CLIENT DIED before connecting"); break
    if not connected:
        log("WARN: client never reached connected -- roster may show host only")
    time.sleep(4)  # let the Join nickname + a roster refresh reach the host

    shot = shots_dir / "host_scoreboard.png"
    captured = _capture_window(host_pid, shot)
    if captured:
        log(f"  scoreboard shot: {shot.name}")
    tail_log(host_log, 20, "HOST")
    log("--- KILLING ---")
    kill_all()
    log("--- SCORESHOT VERDICT ---")
    if captured and shot.exists():
        log(f"Inspect {shot}: it should show 'Players  (2)' with rows 'Host (You)' and 'Client'.")
    sys.exit(0 if captured else 2)


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

    p_client3 = sub.add_parser("client3", help="launch CLIENT #3 peer (4-peer LAN; _dev folder)")
    p_client3.add_argument("--peer", default="127.0.0.1")
    p_client3.add_argument("--nick", default=None)
    p_client3.add_argument("--monitor", type=int, default=2,
                           help="1-based monitor index; defaults to secondary")
    _add_mem_limit(p_client3)
    for flag, kw in client_res: p_client3.add_argument(flag, **kw)
    p_client3.set_defaults(func=cmd_client3)

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

    p_smoke4 = sub.add_parser("smoke4",
                              help="autonomous 4-PEER LAN smoke (Tier 8 cross-peer relay verdict)")
    p_smoke4.add_argument("--duration", type=int, default=45,
                          help="seconds to monitor after all peers connect")
    p_smoke4.add_argument("--sample-interval", type=int, default=5,
                          help="seconds between RSS samples")
    p_smoke4.add_argument("--boot-timeout", type=int, default=40,
                          help="seconds to wait for host UDP bind")
    p_smoke4.add_argument("--client-boot-timeout", type=int, default=75,
                          help="per-client seconds to reach connected (staggered)")
    p_smoke4.add_argument("--settle-grace", type=int, default=25,
                          help="seconds before the sampling RAM-kill arms (lets the "
                               "prop-snapshot boot peak pass; Job Object cap is the "
                               "real runaway guard)")
    p_smoke4.add_argument("--ram-kill-mb", type=int, default=12000,
                          help="post-settle hard kill threshold per peer")
    p_smoke4.add_argument("--memory-limit-gb", type=float, default=12.0,
                          help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_smoke4.add_argument(flag, **kw)
    p_smoke4.set_defaults(func=cmd_smoke4)

    p_npc = sub.add_parser("npctest",
                           help="spawn a kerfurOmega NPC on the host + verify it mirrors to all clients (+ screenshots)")
    p_npc.add_argument("--peers", type=int, default=4,
                       help="total peers: 1 = host-only spawn+screenshot, 4 = host + 3 clients (full mirror test)")
    p_npc.add_argument("--boot-timeout", type=int, default=40,
                       help="seconds to wait for host UDP bind")
    p_npc.add_argument("--client-boot-timeout", type=int, default=75,
                       help="per-client seconds to reach connected (staggered)")
    p_npc.add_argument("--install-timeout", type=int, default=90,
                       help="seconds to wait for the host's NPC interceptor to install (gameplay loaded)")
    p_npc.add_argument("--settle", type=int, default=6,
                       help="seconds after install before firing the spawn trigger")
    p_npc.add_argument("--spawn-wait", type=int, default=6,
                       help="seconds after spawn for mirror propagation before capture")
    p_npc.add_argument("--memory-limit-gb", type=float, default=12.0,
                       help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_npc.add_argument(flag, **kw)
    p_npc.set_defaults(func=cmd_npctest)

    p_rag = sub.add_parser("ragdollshot",
                           help="force the client's player into ragdoll + capture 2 host screenshots of the puppet falling")
    p_rag.add_argument("--boot-timeout", type=int, default=40, help="seconds to wait for host UDP bind")
    p_rag.add_argument("--client-boot-timeout", type=int, default=75, help="seconds for the client to connect")
    p_rag.add_argument("--ragdoll-timeout", type=int, default=70, help="seconds to wait for the host to observe the ragdoll rising edge")
    p_rag.add_argument("--hold-ms", type=int, default=20000, help="how long the client holds the ragdoll (ms) -- must cover both shots")
    p_rag.add_argument("--shot-gap", type=int, default=5, help="seconds between the 2 host screenshots")
    p_rag.add_argument("--memory-limit-gb", type=float, default=12.0, help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_rag.add_argument(flag, **kw)
    p_rag.set_defaults(func=cmd_ragdollshot)

    p_ragspawn = sub.add_parser("ragdollspawn",
                                help="SOLO SP probe: spawn playerRagdoll_C manually (no ragdollMode) vs real ragdollMode -- decide xray-ragdoll feasibility")
    p_ragspawn.add_argument("--probe-timeout", type=int, default=150,
                            help="seconds to wait for MANUAL-SHOT READY (covers the save-load into gameplay)")
    p_ragspawn.add_argument("--real-timeout", type=int, default=45,
                            help="seconds to wait for REAL-SHOT READY after the manual shot")
    p_ragspawn.add_argument("--memory-limit-gb", type=float, default=12.0,
                            help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_ragspawn.add_argument(flag, **kw)
    p_ragspawn.set_defaults(func=cmd_ragdollspawn)

    p_menutravel = sub.add_parser("menutravel",
                                  help="SOLO SP probe: find which command travels gameplay->menu (for the client-death flee-to-menu fix)")
    p_menutravel.add_argument("--probe-timeout", type=int, default=200,
                              help="seconds to wait for 'menutravel: DONE' (boot + 4 candidates x ~6s)")
    p_menutravel.add_argument("--memory-limit-gb", type=float, default=12.0,
                              help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_menutravel.add_argument(flag, **kw)
    p_menutravel.set_defaults(func=cmd_menutravel)

    p_clumpvis = sub.add_parser("clumpvis",
                                help="SOLO probe: spawn a bare prop_garbageClump_C + report whether its StaticMesh asset is null (empty) or named (visible)")
    p_clumpvis.add_argument("--probe-timeout", type=int, default=150,
                            help="seconds to wait for CLUMPVIS READY (covers boot into gameplay)")
    p_clumpvis.add_argument("--memory-limit-gb", type=float, default=12.0,
                            help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_clumpvis.add_argument(flag, **kw)
    p_clumpvis.set_defaults(func=cmd_clumpvis)

    p_fogprobe = sub.add_parser("fogprobe",
                                help="SOLO SP probe: force fog on, sample density/target/actors, run the RE'd clear sequence -- gates the host-authoritative weather fix")
    p_fogprobe.add_argument("--probe-timeout", type=int, default=150,
                            help="seconds to wait for FOG-FORCED READY (covers boot into gameplay)")
    p_fogprobe.add_argument("--clear-timeout", type=int, default=60,
                            help="seconds to wait for FOG-CLEARED READY after the foggy shot")
    p_fogprobe.add_argument("--memory-limit-gb", type=float, default=12.0,
                            help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_fogprobe.add_argument(flag, **kw)
    p_fogprobe.set_defaults(func=cmd_fogprobe)

    p_menushot = sub.add_parser("menushot",
                                help="SOLO: screenshot proof the Dear ImGui F1 menu renders over the game (VOTVCOOP_MENU_OPEN=1)")
    p_menushot.add_argument("--probe-timeout", type=int, default=150,
                            help="seconds to wait for the overlay DX11 bring-up")
    p_menushot.add_argument("--memory-limit-gb", type=float, default=12.0,
                            help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_menushot.add_argument(flag, **kw)
    p_menushot.set_defaults(func=cmd_menushot)

    p_scoreshot = sub.add_parser("scoreshot",
                                 help="2-PEER: screenshot proof the TAB player list shows the server roster (VOTVCOOP_SCOREBOARD_OPEN=1)")
    p_scoreshot.add_argument("--probe-timeout", type=int, default=150,
                             help="seconds to wait for the client to connect")
    p_scoreshot.add_argument("--boot-timeout", type=int, default=90,
                             help="seconds to wait for host UDP bind")
    p_scoreshot.add_argument("--memory-limit-gb", type=float, default=12.0,
                             help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_scoreshot.add_argument(flag, **kw)
    p_scoreshot.set_defaults(func=cmd_scoreshot)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
