"""VOTV coop launcher / autonomous LAN smoke orchestrator.

Replaces the .bat-piped powershell-pipeline chain that buffer-deadlocked under
redirected stdio (cmd.exe + powershell child + ps1 deploy script + VotV
detached spawn -> tail caller never sees output until VotV exits).

Subcommands:
  host        deploy + launch HOST peer (Game_0.9.0n_HOST/)
  client      deploy + launch CLIENT #1 peer (Game_0.9.0n_CLIENT_1/)
  client2     deploy + launch CLIENT #2 peer (Game_0.9.0n_CLIENT_2/) -- 2026-05-28
              added for 3-peer LAN tests of the GNS multi-peer wire layer.
  client3     deploy + launch CLIENT #3 peer (Game_0.9.0n_CLIENT_3/) -- 2026-05-30
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
HOST_DIR = ROOT / "Game_0.9.0n_HOST" / WIN64_REL
CLIENT_DIR = ROOT / "Game_0.9.0n_CLIENT_1" / WIN64_REL
CLIENT2_DIR = ROOT / "Game_0.9.0n_CLIENT_2" / WIN64_REL  # 2026-05-28 PR-4.2+: 2nd client for 3-peer testing
DEV_DIR = ROOT / "Game_0.9.0n_CLIENT_3" / WIN64_REL
DEPLOY_ALL = ROOT / "tools" / "deploy-all.ps1"
VOTV_EXE = "VotV-Win64-Shipping.exe"
DEFAULT_PORT = 47621

# Windows CreateProcess flags. DETACHED_PROCESS prevents the child from
# inheriting our console; CREATE_NEW_PROCESS_GROUP isolates Ctrl-C handling.
DETACHED_PROCESS = 0x00000008
CREATE_NEW_PROCESS_GROUP = 0x00000200


# Windows consoles default to a legacy codepage (cp1251/cp866). Subprocess output
# echoed through log() can carry characters that codepage cannot encode (e.g. the
# U+FFFD replacements from decoding a localized PowerShell error as utf-8) -- an
# unguarded write then raises UnicodeEncodeError and kills the launcher BEFORE the
# game starts, with the .bat window closing unseen (2026-07-02, pak-locked deploy).
# Logging must never kill the launch: replace unencodable characters instead.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")
    sys.stderr.reconfigure(errors="replace")


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
        if ("===" in line or "deployed mod" in line or "[deploy-all]" in line
                or "client pak" in line):
            log(f"  deploy: {line.strip()}")
    log("deploy OK")


def list_votv() -> list[dict]:
    """Live VotV processes.

    THE @() IS LOAD-BEARING. Without it a pipeline over zero processes prints
    NOTHING, so empty stdout meant BOTH "no game is running" and "the PowerShell
    query itself failed" -- and the caller could not tell them apart. Wrapping in
    an array subexpression makes zero processes emit a literal `[]`, which leaves
    empty output meaning exactly one thing: the query did not run.

    That ambiguity was not theoretical. 2026-08-26: two consecutive `mp.py
    browser` runs reported "(no VotV process -- exited/crashed)" three seconds
    after launch and abandoned the run, while `kill_all()` moments later found
    the process alive and warned about it -- the harness declared a crash that
    had not happened and threw away the measurement. So a failed query now
    RETRIES rather than being read as an answer.
    """
    # -InputObject, NOT a pipe. Piping an empty array sends ZERO objects downstream, so
    # `@() | ConvertTo-Json` prints nothing and the ambiguity survives the @(). Passing the
    # array as an argument is what makes zero processes serialise to a literal `[]`
    # (verified at the prompt before this was trusted).
    script = (
        "ConvertTo-Json -Compress -Depth 3 -InputObject @(Get-Process VotV-Win64-Shipping "
        "-ErrorAction SilentlyContinue | ForEach-Object { [PSCustomObject]@{PID=$_.Id; "
        "RSS_MB=[math]::Round($_.WorkingSet64/1MB,1); Title=$_.MainWindowTitle; "
        "Path=$_.Path} })"
    )
    for attempt in range(3):
        _rc, out, _err = run_ps(script)
        out = out.strip()
        if out:
            try:
                data = json.loads(out)
            except json.JSONDecodeError:
                data = None
            if data is not None:
                if isinstance(data, dict):
                    data = [data]
                return data
        if attempt < 2:
            time.sleep(0.5)
    log("WARN: could not query VotV processes after 3 attempts -- reporting none, "
        "which may be wrong; treat a 'crashed' verdict from this run with suspicion")
    return []


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
                trigger_file: str | None = None,
                set_net_role: bool = True,
                set_scenario: str | None = "play",
                extra_env: dict | None = None) -> int:
    # role is the WIRE role (host / client). peer_slot is which CLIENT folder
    # to launch from when role==client: 1 -> Game_0.9.0n_CLIENT_1, 2 ->
    # Game_0.9.0n_CLIENT_2, 3 -> Game_0.9.0n_CLIENT_3. Host always uses Game_0.9.0n_HOST.
    # WP-2 (2026-08-22): every copy runs the UE4SS lane -- the mod is
    # Mods\Multivoid\dlls\main.dll started via start_mod(); deploy-all.ps1
    # ships byte-identical bytes to all four, so any copy is an equivalent peer.
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
    # Scenario is driven by the VOTVCOOP_SCENARIO env var (set below) -- NOT a
    # scenario.txt file. A leftover scenario.txt in the game dir aliased later
    # NATIVE launches into auto-gameplay (user bug 2026-06-06); the harness no
    # longer reads it. Proactively delete any stale one this launcher left before.
    try:
        (game_dir / "scenario.txt").unlink()
    except FileNotFoundError:
        pass
    log_file = game_dir / "multivoid.log"
    if log_file.exists():
        try:
            log_file.unlink()
        except PermissionError:
            log(f"WARN: {log_file} locked (another VotV still holds it?)")
    env = os.environ.copy()
    # set_scenario=None -> NO VOTVCOOP_SCENARIO -> the harness boots to the MENU (the
    # real native-launch / save-picker context). Default "play" auto-loads the ini save.
    if set_scenario:
        env["VOTVCOOP_SCENARIO"] = set_scenario
    # set_net_role=False boots plain "play" with NO env net role -> the harness non-net
    # branch runs + RunPlayLoop waits for a browser-initiated start (used by the Tier-2
    # browser-boot probe, where the session starts via session_manager, not the env).
    if set_net_role:
        env["VOTVCOOP_NET_ROLE"] = role
        if role == "client" and peer:
            env["VOTVCOOP_NET_PEER"] = peer
    env["VOTVCOOP_NET_PORT"] = str(port)
    env["VOTVCOOP_NET_NICK"] = nick
    # Test-infra world selection (2026-06-09, user request): the HOST always loads save 's_1234';
    # every CLIENT always boots a FRESH New Game and NEVER loads a save. These are env overrides the
    # harness honors over multivoid.ini (see BootStorySaveBlocking). Keeps every run deterministic:
    # one fixed host world streamed onto blank clients (the ephemeral-client baseline).
    if role == "host":
        env["VOTVCOOP_SAVE"] = "s_test_screens2"
    else:
        env["VOTVCOOP_FRESH"] = "1"
    if trigger_file:
        # dev/spawn_npc watches this file path; when it appears the peer spawns
        # a kerfurOmega NPC once + deletes it. mp.py npctest creates it after
        # all peers connect (so the EntitySpawn broadcast reaches everyone).
        env["VOTVCOOP_SPAWN_TRIGGER"] = trigger_file
    if extra_env:
        # Per-peer overrides/additions (e.g. the P2P topology + identity env the
        # p2p_smoke orchestrator injects). Applied last so they win.
        for k, v in extra_env.items():
            env[k] = v
        # Redact secrets (TURN pass, signaling token) from the log -- they land in
        # console scrollback / CI capture otherwise.
        _sens = ("PASS", "TOKEN", "SECRET")
        _shown = {k: ("***" if any(s in k.upper() for s in _sens) else v)
                  for k, v in extra_env.items()}
        log("  extra_env: " + ", ".join(f"{k}={v}" for k, v in _shown.items()))
    # VOTVCOOP_RHI=dx12 appends UE4's -dx12 switch. The overlay has a separate
    # render half per RHI (ui/overlay_backend_dx12.cpp), and "the emoji draw" is a
    # claim about a real frame -- so it has to be made on BOTH backends or it is a
    # claim about one of them.
    _argv = [str(exe), "-windowed",
             f"-ResX={res_x}", f"-ResY={res_y}",
             f"-WinX={win_x}", f"-WinY={win_y}"]
    _rhi = os.environ.get("VOTVCOOP_RHI", "").lower()
    if _rhi in ("dx12", "d3d12"):
        _argv.append("-dx12")
    elif _rhi in ("dx11", "d3d11"):
        _argv.append("-dx11")
    # Crash forensics (diagnostic, 2026-08-22): UE4.27's in-process ReportCrash
    # honors -fullcrashdump -> the UE4CC UE4Minidump.dmp becomes a FULL-memory
    # dump (code pages + heap readable; multi-GB). Opt-in only -- set
    # MP_FULLCRASHDUMP=1 in the wrapper that wants forensic dumps.
    if os.environ.get("MP_FULLCRASHDUMP") == "1":
        _argv.append("-fullcrashdump")
    proc = subprocess.Popen(
        _argv,
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
# straight out of each peer's multivoid.log; the exact strings are the UE_LOGI
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
    "xpeer_identity":  re.compile(r"roster: client installed cross-peer identity slot=(\d+)"),
    # host: it fanned a PlayerJoined out to the other clients.
    "host_relayed_pj": re.compile(r"roster: host asserted the full roster to joiner"),
    # host: the per-slot connect edge fired (snapshot + flashlight + peer-state replay).
    "connect_edge":    re.compile(r"peer slot (\d+) connect edge -- replaying"),
    "epoch_latched":   re.compile(r"latched senderEpoch=0x[0-9a-fA-F]+ for peer slot (\d+)"),
    "stale_drop":      re.compile(r"stale-gen drop slot=(\d+)"),
    "malformed_drop":  re.compile(r"with senderEpoch=0 \(malformed"),
}


def parse_log_markers(path: Path) -> dict:
    """Extract Tier-2 cross-peer verdict markers from one peer's multivoid.log.

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
    """Poll a client's multivoid.log until it logs 'host assigned us peer slot
    N' (== reached connected). Returns the assigned slot, or None on timeout /
    process death. Staggering launches behind this both spreads the boot-RSS
    peaks and gives deterministic slot ordering (client1->1, client2->2, ...),
    which is what makes the cross-peer verdict legible."""
    log_path = game_dir / "multivoid.log"
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
            tail_log(HOST_DIR / "multivoid.log", 30, "HOST")
            sys.exit(1)
    if not bound:
        log(f"FAIL: host did NOT bind UDP within {args.boot_timeout}s")
        tail_log(HOST_DIR / "multivoid.log", 30, "HOST")
        kill_all()
        sys.exit(1)

    # Optional host-solo settle BEFORE the client launches (default 0 = unchanged). A pre-connect
    # window so a host-drift scenario (VOTVCOOP_RUN_PILE_DRIFT) can diverge the host's world before
    # the snapshot is taken -- the only way to seed real orphans for the client's [PILE-CENSUS].
    if getattr(args, "host_settle", 0) and args.host_settle > 0:
        log(f"host-settle {args.host_settle}s (solo window for a host-drift scenario before connect)...")
        time.sleep(args.host_settle)

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

    def sample_once() -> bool:
        """One monitor sample. Returns False to stop (RAM breach)."""
        nonlocal last_peers, kill_reason
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
            return False
        return True

    while time.time() - t0 < args.duration:
        if not sample_once():
            break

    # Join-aware grace (2026-06-12, cold-cache flake). The menu-mode
    # save-transfer join (client boot + connect + ~18 MB download + save load +
    # connect replay) can outrun --duration on the day's first launch after a
    # deploy (cold DLL/shader/disk caches) -- the fixed budget then kills the
    # peers mid-download and the verdict FAILs on "never spawned the host
    # puppet" with nothing actually wrong (it PASSes on the warm re-run).
    # If the puppet marker is still missing at budget end while both peers are
    # alive and under the RAM cap, keep sampling up to --join-grace extra
    # seconds; once the marker appears, run one more fixed steady-state stretch
    # so the verdict still covers post-join stability, not just the join.
    if not kill_reason and len(last_peers) == 2:
        client_log = CLIENT_DIR / "multivoid.log"
        if 0 not in parse_log_markers(client_log)["puppet_slots"]:
            log(f"--- JOIN GRACE: host puppet not up at budget end; extending up to {args.join_grace}s ---")
            g0 = time.time()
            joined = False
            while time.time() - g0 < args.join_grace:
                if not sample_once():
                    break
                if len(last_peers) != 2:
                    break  # a peer died -- let the normal verdict report it
                if 0 in parse_log_markers(client_log)["puppet_slots"]:
                    joined = True
                    break
            if joined and not kill_reason:
                log("--- JOIN GRACE: puppet spawned; 15s post-join steady-state ---")
                s0 = time.time()
                while time.time() - s0 < 15:
                    if not sample_once():
                        break

    log("--- FINAL STATE ---")
    log(f"peers alive at end: {len(last_peers)}")
    for p in last_peers:
        log(f"  PID={p['PID']} RSS={p['RSS_MB']}MB title='{p['Title']}'")

    tail_log(HOST_DIR / "multivoid.log", 30, "HOST")
    tail_log(CLIENT_DIR / "multivoid.log", 30, "CLIENT")

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
    cmk = parse_log_markers(CLIENT_DIR / "multivoid.log")
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
    # Machine assertion for the config-selftest runs (ini arc 4, release ritual
    # step 0): when the selftest env gate is on, the host log MUST carry the
    # green completion line -- a generator/lexer regression fails THIS verdict,
    # not a human's memory of grepping.
    if os.environ.get("VOTVCOOP_RUN_CONFIG_SELFTEST") == "1":
        try:
            host_text = (HOST_DIR / "multivoid.log").read_text(encoding="utf-8",
                                                               errors="replace")
        except OSError:
            host_text = ""
        if "config-selftest: DONE fail=0" not in host_text:
            log("FAIL: VOTVCOOP_RUN_CONFIG_SELFTEST=1 but the host log has no "
                "'config-selftest: DONE fail=0' (selftest failed or never ran)")
            sys.exit(8)
        log("config-selftest: DONE fail=0 confirmed in the host log")
    # v141 (security A52): the movement ledger self-checks its own arithmetic on every
    # session start, un-gated, on BOTH peers. It is asserted here rather than left to a
    # human grep because its failure mode is SILENT -- a wrong verdict merely reads wrong,
    # and the field transcript it produces would be believed. The FIRST build of that
    # module refused the game's own ATV on its first sample; this is the gate that keeps
    # that caught.
    ledger_bad: list[str] = []
    for lbl, d in (("HOST", HOST_DIR), ("CLIENT", CLIENT_DIR)):
        try:
            txt = (d / "multivoid.log").read_text(encoding="utf-8", errors="replace")
        except OSError:
            txt = ""
        if "movement_ledger selftest: ALL PASS" not in txt:
            if "movement_ledger selftest: FAIL" in txt:
                fails = [ln.strip() for ln in txt.splitlines()
                         if "movement_ledger selftest: FAIL" in ln]
                ledger_bad.append(f"{lbl}: {len(fails)} failing check(s): " + " | ".join(fails[:6]))
            else:
                ledger_bad.append(f"{lbl}: no 'movement_ledger selftest' line at all "
                                  "(the selftest never ran -- OnSessionStart not reached?)")
    if ledger_bad:
        log("FAIL: movement-ledger selftest did not pass on every peer:")
        for h in ledger_bad:
            log(f"  - {h}")
        sys.exit(11)
    log("movement_ledger selftest: ALL PASS confirmed on host + client")
    # A54 (2026-08-26): the intent authorizer self-checks its reach arithmetic on every session
    # start, un-gated, on BOTH peers -- same reasoning as the ledger above, and the same failure
    # mode. A wrong reach verdict does not crash: it either refuses a real player or authorizes the
    # whole map, and both read as "working" from outside. Generalized rather than copy-pasted,
    # because a third selftest is coming and three near-identical blocks is how the first one rots.
    selftest_bad: list[str] = []
    for lbl, d in (("HOST", HOST_DIR), ("CLIENT", CLIENT_DIR)):
        try:
            txt = (d / "multivoid.log").read_text(encoding="utf-8", errors="replace")
        except OSError:
            txt = ""
        if "intent_authority selftest: ALL PASS" not in txt:
            fails = [ln.strip() for ln in txt.splitlines()
                     if "intent_authority selftest FAIL" in ln]
            if fails:
                selftest_bad.append(f"{lbl}: {len(fails)} failing check(s): " + " | ".join(fails[:6]))
            else:
                selftest_bad.append(f"{lbl}: no 'intent_authority selftest' line at all "
                                    "(the selftest never ran -- OnSessionStart not reached?)")
    if selftest_bad:
        log("FAIL: intent-authority selftest did not pass on every peer:")
        for h in selftest_bad:
            log(f"  - {h}")
        sys.exit(11)
    log("intent_authority selftest: ALL PASS confirmed on host + client")
    # WP-2 boot-lane assertion: both peers booted via UE4SS start_mod
    # (entry=cppmod, no REFUSE, no retired-proxy line; MISSING log = FAIL).
    lane: list[str] = []
    for lbl, d in (("HOST", HOST_DIR), ("CLIENT", CLIENT_DIR)):
        lane += _lane_check(lbl, d)
    if lane:
        log(f"FAIL: {len(lane)} boot-lane issue(s):")
        for h in lane:
            log(f"  - {h}")
        sys.exit(10)
    # LOG HEALTH, ON EVERY SMOKE -- not just the i18n one. These checks (strict
    # UTF-8, a line that formatted to nothing, any 'selftest: FAIL', and the
    # POSITIVE 'font selftest: DONE fail=0') were written for smoke_i18n and
    # wired only there, so the plain smoke could not see a font selftest fail at
    # all. Measured 2026-07-30: a build with two deliberately-failing selftest
    # rows and 'fail=2' in both peer logs printed PASS from this very function.
    health: list[str] = []
    for lbl, d in (("HOST", HOST_DIR), ("CLIENT", CLIENT_DIR)):
        health += _peer_log_health(lbl, d / "multivoid.log")
    if health:
        log(f"FAIL: {len(health)} log-health issue(s):")
        for h in health:
            log(f"  - {h}")
        sys.exit(9)
    log(f"PASS: both peers stable, client connected (slot {cmk['assigned_slot']}), "
        f"host puppet spawned, no RAM breach"
        + (f", {cmk['stale_drops']} benign stale-gen drop(s)" if cmk["stale_drops"] else ""))
    sys.exit(0)


# --- Physics-divergence test signals (smoke_phystele, 2026-06-09) -------------
# Reproduces + auto-detects the client world-divergence physics failure (the
# bug two builds shipped blind because the plain smoke can't see it):
#   - host loads s_1234 (populated), client boots FRESH -> the asymmetry IS the
#     bug (host disk-asleep props vs client independent-RNG New Game).
#   - THE deterministic signal: count `diverged (d=...cm)` lines in the client
#     log = how many divergent RNG props the client GENERATED and had to
#     teleport-reconcile. Current build -> hundreds (FAIL/reproduces). After the
#     spawner-suppression fix -> ~0 (client generates none; host props arrive via
#     Path C fresh-spawn). This does NOT depend on RNG FPS luck.
#   - safety nets: `posted task FAULT` (the grep the last smoke MISSED), the
#     game's own Fatal error / EXCEPTION_ACCESS_VIOLATION (the PhysX worker-thread
#     crash is process-death, NOT caught by our game-thread SEH -> peer-count<2 is
#     the primary catcher), post-settle FPS lock, RSS growth.
_PHYS = {
    "frames":   re.compile(r"frames=(\d+)/s"),
    "diverged": re.compile(r"diverged \(d="),            # remote_prop_spawn diverged-converge teleport
    "pathc":    re.compile(r"OnSpawn: spawned .* of '"),  # Path C fresh-spawn from host data
    "fault":    re.compile(r"game_thread: posted task FAULT"),
    "drain":    re.compile(r"snapshot: drain complete for slot \d+ \((\d+) candidates"),
    "cancel":   re.compile(r"client-cancel spawner|cancelling BP body on client"),
}
_GAME_FATAL = re.compile(r"Fatal error|Assertion failed|EXCEPTION_ACCESS_VIOLATION")


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return ""


def _game_log(win64_dir: Path) -> Path:
    # .../VotV/Binaries/Win64 -> .../VotV/Saved/Logs/VotV.log
    return win64_dir.parent.parent / "Saved" / "Logs" / "VotV.log"


def _lane_check(label: str, win64_dir: Path) -> list[str]:
    """WP-2 boot-lane assertion: the peer must have booted via the UE4SS
    start_mod lane. launch_peer unlinks multivoid.log pre-spawn, so a MISSING
    log means the loader never started the mod (never a stale-content pass);
    the entry= line survives kill-teardown because start_mod's started legs
    flush (lesson-kill-teardown-discards-buffered-info-log-lines)."""
    lp = win64_dir / "multivoid.log"
    if not lp.exists():
        return [f"{label}: multivoid.log MISSING -- the loader never started the mod"]
    txt = _read_text(lp)
    probs: list[str] = []
    if "entry=cppmod" not in txt:
        probs.append(f"{label}: no 'entry=cppmod' boot line (wrong lane or boot failed)")
    if "entry=proxy-dllmain" in txt:
        probs.append(f"{label}: retired proxy-lane boot line present")
    # Match the loader's actual refuse lines ("cppmod: REFUSE reason=...") -- a bare
    # "REFUSE" substring false-positived on the novelty ledger's unrelated
    # "REFUSED a text field" W11 line (caught 2026-08-22, smoke crash loop).
    if "cppmod: REFUSE" in txt:
        probs.append(f"{label}: cppmod REFUSE line in log (dup/predecessor guard fired)")
    return probs


def _steady_fps(fps: list) -> tuple:
    """Steady-state FPS = median + min over the LAST HALF of positive samples
    (past the one-time connect-drain dip). Returns (median, min, n_tail)."""
    pos = [f for f in fps if f > 0]
    if not pos:
        return 0, 0, 0
    tail = pos[len(pos) // 2:]            # drop boot ramp + connect-drain transient
    s = sorted(tail)
    return s[len(s) // 2], min(tail), len(tail)


def parse_phys_signals() -> dict:
    cl, hl = _read_text(CLIENT_DIR / "multivoid.log"), _read_text(HOST_DIR / "multivoid.log")
    cgame, hgame = _read_text(_game_log(CLIENT_DIR)), _read_text(_game_log(HOST_DIR))
    cfps = [int(m.group(1)) for m in _PHYS["frames"].finditer(cl)]
    hfps = [int(m.group(1)) for m in _PHYS["frames"].finditer(hl)]
    cmed, cmin, cn = _steady_fps(cfps)
    hmed, _hmin, _hn = _steady_fps(hfps)
    drains = [int(m.group(1)) for m in _PHYS["drain"].finditer(hl)]
    return {
        "diverged":     len(_PHYS["diverged"].findall(cl)),  # info only now (harmless kinematic reconciles)
        "pathc":        len(_PHYS["pathc"].findall(cl)),
        "cancels":      len(_PHYS["cancel"].findall(cl)),
        "faults":       len(_PHYS["fault"].findall(cl)) + len(_PHYS["fault"].findall(hl)),
        "max_drain":    max(drains, default=0),
        # P1 metric: the bug is a PHYSICS-WAKE DRAG -- measured by host-vs-client
        # FPS parity (host runs the IDENTICAL DLL on the SAME prop scene loaded
        # asleep from disk; pre-fix host ~110 / client ~44 = 0.40). The client is
        # 720p (lighter) so it should match-or-beat the host once the woken-body
        # drag is gone. This is the diagnosis agent's killer metric.
        "client_fps_med":   cmed,
        "client_fps_min":   cmin,
        "client_fps_n":     cn,
        "host_fps_med":     hmed,
        "fps_ratio":        (cmed / hmed) if hmed else 0.0,
        "client_fatal": bool(_GAME_FATAL.search(cgame)),
        "host_fatal":   bool(_GAME_FATAL.search(hgame)),
    }


def cmd_smoke_phystele(args) -> None:
    """Autonomous client world-divergence PHYSICS test. Host loads s_1234, client
    boots FRESH; reproduce divergence, then judge by the deterministic `diverged`
    count + crash/fatal/FPS/RSS safety nets. Designed to FAIL RED on a build that
    still teleport-reconciles divergent props, and PASS only when the client
    generates none (spawner suppression working)."""
    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before phystele")
    deploy_all()

    log("--- HOST LAUNCH (s_1234) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True)
    log(f"waiting up to {args.boot_timeout}s for host UDP {args.port}...")
    bound = False
    for i in range(args.boot_timeout):
        time.sleep(1)
        if host_owns_udp(host_pid, args.port):
            log(f"host bound after {i+1}s"); bound = True; break
        if not any(p["PID"] == host_pid for p in list_votv()):
            log("HOST DIED before binding UDP"); tail_log(HOST_DIR / "multivoid.log", 30, "HOST"); sys.exit(1)
    if not bound:
        log(f"FAIL: host did not bind UDP within {args.boot_timeout}s"); kill_all(); sys.exit(1)

    # Wait for the host to finish loading + STABILIZE its prop registry before the
    # client connects. The host's story-load re-issues 'open untitled_1', which tears
    # down the boot-world's prop elements (they linger as "dying" for a while) and
    # spawns the save world's. Connecting mid-transition caught a DEGENERATE snapshot
    # (88 live / 1383 dying) -> client got a near-empty world, its player died, UI
    # looped, RSS ballooned to 9 GB (2026-06-10). Gate on the host's "PLAY READY"
    # marker, then a settle window for the dying elements to drain.
    log(f"waiting up to {args.boot_timeout}s for host PLAY READY...")
    for i in range(args.boot_timeout):
        if "==== PLAY READY ====" in _read_text(HOST_DIR / "multivoid.log"):
            log(f"host PLAY READY after {i}s"); break
        time.sleep(1)
        if not any(p["PID"] == host_pid for p in list_votv()):
            log("HOST DIED before PLAY READY"); tail_log(HOST_DIR / "multivoid.log", 30, "HOST"); sys.exit(1)
    log(f"host-settle {args.host_settle}s (let the boot-world's dying prop elements drain before connect)...")
    time.sleep(args.host_settle)

    log("--- CLIENT LAUNCH (FRESH world) ---")
    client_pid = launch_peer("client", args.port, "Client", peer="127.0.0.1",
                             res_x=1280, res_y=720, monitor=2, tile_index=0)

    log(f"--- MONITORING {args.duration}s (the divergent snapshot + physics settle) ---")
    t0 = time.time(); last_peers: list[dict] = []; kill_reason = None; rss0 = {}
    while time.time() - t0 < args.duration:
        time.sleep(args.sample_interval)
        peers = list_votv(); last_peers = peers; t = int(time.time() - t0)
        desc = ", ".join(f"PID{p['PID']}={p['RSS_MB']}MB" for p in peers) if peers else "NONE"
        log(f"  t={t}s peers={len(peers)}: {desc}")
        for p in peers:
            rss0.setdefault(p["PID"], p["RSS_MB"])
        mx = max((p["RSS_MB"] for p in peers), default=0)
        if mx > args.ram_kill_mb:
            kill_reason = f"RSS={mx}MB > {args.ram_kill_mb}MB"; break

    rss_growth = max((p["RSS_MB"] - rss0.get(p["PID"], p["RSS_MB"]) for p in last_peers), default=0)
    # Leak/doubling signal = client-vs-HOST final RSS parity, NOT growth-from-boot.
    # P1 KEEPS the client's full world (divergent props become kinematic, not removed),
    # so the client legitimately loads a ~4.5 GB world that PLATEAUS (1.7->4.5 GB during
    # load, then flat) -- same as the host. Growth-from-boot-baseline therefore reads
    # ~3 GB of NORMAL world-load and false-fails. A real doubling/leak shows as the
    # client RSS running well ABOVE the host's (both hold the same prop scene). P2
    # (claim-tracking) further trims the client's small excess by dropping the doubles.
    host_rss   = next((p["RSS_MB"] for p in last_peers if p["PID"] == host_pid), 0.0)
    client_rss = next((p["RSS_MB"] for p in last_peers if p["PID"] == client_pid), 0.0)
    rss_ratio  = (client_rss / host_rss) if host_rss else 0.0
    tail_log(HOST_DIR / "multivoid.log", 12, "HOST")
    tail_log(CLIENT_DIR / "multivoid.log", 12, "CLIENT")
    sig = parse_phys_signals()
    log("--- KILLING ---"); kill_all()

    log("--- PHYS SIGNALS ---")
    log(f"  client steady FPS = {sig['client_fps_med']} (min {sig['client_fps_min']}, n={sig['client_fps_n']})"
        f"   host steady FPS = {sig['host_fps_med']}   ratio = {sig['fps_ratio']:.2f}  <-- the FPS-parity signal")
    log(f"  diverged kinematic reconciles = {sig['diverged']} (info only -- harmless after the SP-parity fix)")
    log(f"  pathC fresh-spawns from host data = {sig['pathc']}   spawner-cancels = {sig['cancels']}")
    log(f"  host snapshot candidates = {sig['max_drain']}   posted-task FAULTs = {sig['faults']}")
    log(f"  game Fatal/AV: host={sig['host_fatal']} client={sig['client_fatal']}")
    log(f"  client RSS = {client_rss:.0f}MB  host RSS = {host_rss:.0f}MB  ratio = {rss_ratio:.2f}"
        f"   (growth-from-boot {rss_growth:.0f}MB = world-load, info only)")

    log("--- VERDICT ---")
    # P1 (2026-06-09): the failure is a PHYSICS-WAKE DRAG, NOT a reconcile count.
    # Gate on host-vs-client steady FPS parity (the diagnosis agent's killer
    # metric: identical DLL, same prop scene; pre-fix host 110 / client 44 = 0.40).
    # The client is 720p (lighter) so it should reach >= 60% of host once the
    # woken-body drag is gone. The diverged count is INFO only now.
    FPS_RATIO_FAIL = 0.60
    if kill_reason:
        log(f"FAIL: {kill_reason}"); sys.exit(2)
    if len(last_peers) < 2:
        log("FAIL: a peer DIED (process-death = the PhysX worker-thread crash signature)"); sys.exit(3)
    # The PhysX crash leaves a lingering crash-reporter window ("...has crashed and will
    # close") so the process count stays 2 -- catch it by title (the death-check misses it).
    crashed = [p for p in last_peers if "crash" in str(p.get("Title", "")).lower()]
    if crashed:
        log(f"FAIL: a peer CRASHED (window title: '{crashed[0]['Title']}')"); sys.exit(10)
    if sig["host_fatal"] or sig["client_fatal"]:
        log("FAIL: 'Fatal error'/AV in a game log (crash)"); sys.exit(4)
    if sig["faults"] > 0:
        log(f"FAIL: {sig['faults']} 'posted task FAULT' line(s) (DLL use-after-free / SEH-caught AV)"); sys.exit(5)
    if sig["max_drain"] < 500:
        log(f"FAIL(inconclusive): host streamed only {sig['max_drain']} props -- s_1234 not loaded / snapshot "
            "didn't run. The scenario didn't arm; re-check the host save + --duration."); sys.exit(7)
    if sig["host_fps_med"] <= 0 or sig["client_fps_n"] < 3:
        log(f"FAIL(inconclusive): not enough steady FPS samples (host_med={sig['host_fps_med']}, "
            f"client_n={sig['client_fps_n']}) -- extend --duration or check perf_probe is on."); sys.exit(7)
    if sig["fps_ratio"] < FPS_RATIO_FAIL:
        log(f"FAIL(reproduced): client steady FPS {sig['client_fps_med']} is only "
            f"{sig['fps_ratio']*100:.0f}% of host {sig['host_fps_med']} (< {FPS_RATIO_FAIL*100:.0f}%) -- the "
            "woken-physics drag is still present. THIS is the bug.")
        sys.exit(8)
    RSS_RATIO_FAIL = 1.50   # client holding the same prop scene as the host should be ~parity;
                            # >1.5x host = doubled/leaked props (P2 not removing divergent extras)
    if host_rss > 0 and rss_ratio > RSS_RATIO_FAIL:
        log(f"FAIL: client RSS {client_rss:.0f}MB is {rss_ratio:.2f}x host {host_rss:.0f}MB "
            f"(> {RSS_RATIO_FAIL}x) -- doubled/leaked props (P2 claim-tracking needed)."); sys.exit(9)
    log(f"PASS: client steady FPS {sig['client_fps_med']} = {sig['fps_ratio']*100:.0f}% of host "
        f"{sig['host_fps_med']} (>= {FPS_RATIO_FAIL*100:.0f}%); client RSS {rss_ratio:.2f}x host "
        f"(<= {RSS_RATIO_FAIL}x); {sig['diverged']} harmless kinematic reconciles, {sig['pathc']} "
        "fresh spawns, no crash/fault.")
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

    # ARC B drill: --nick-all makes every peer ASK for the same name, which is the
    # only way to exercise the arbiter end-to-end; --scoreboard opens the TAB list
    # so the assignment is PHOTOGRAPHABLE (an autonomous run cannot hold TAB).
    # Only the plain smoke4 --scoreboard forces the board from boot. smoke_i18n
    # presses the real key at capture time instead (see above).
    if getattr(args, "scoreboard", False) and not getattr(args, "chat", None):
        os.environ["VOTVCOOP_SCOREBOARD_OPEN"] = "1"

    # ARC D2: per-peer names. Split ONCE here so an index error is a launch-time
    # failure rather than a silently short list that renames a peer mid-drill.
    per_nick = [n for n in (getattr(args, "nicks", None) or "").split(",") if n != ""]
    def nick_for(i: int, fallback: str) -> str:
        if per_nick:
            if i >= len(per_nick):
                log(f"FAIL: --nicks has {len(per_nick)} names, peer index {i} needs one")
                sys.exit(1)
            return per_nick[i]
        return getattr(args, "nick_all", None) or fallback

    log("--- HOST LAUNCH ---")
    host_pid = launch_peer("host", args.port, nick_for(0, "Host"),
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
            tail_log(HOST_DIR / "multivoid.log", 30, "HOST")
            sys.exit(1)
    if not bound:
        log(f"FAIL: host did NOT bind UDP within {args.boot_timeout}s")
        tail_log(HOST_DIR / "multivoid.log", 30, "HOST")
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
        pid = launch_peer("client", args.port, nick_for(slot_arg, label.capitalize()),
                          peer="127.0.0.1", res_x=1280, res_y=720,
                          peer_slot=slot_arg, monitor=2, tile_index=slot_arg - 1,
                          memory_limit_gb=args.memory_limit_gb)
        client_pids[slot_arg] = pid
        assigned[slot_arg] = wait_for_client_connect(
            game_dir, args.client_boot_timeout, label, pid)

    # i18n: every peer SAYS something in its own script, typed through the real
    # keyboard path. Done before the monitoring window so the messages have the
    # whole window to reach every other peer's feed and disk.
    chat_msgs = [m for m in (getattr(args, "chat", None) or "").split("|") if m != ""]
    if chat_msgs:
        log("--- i18n CHAT (typed via WM_CHAR into the real chat bar) ---")
        peers_for_chat = [(0, HOST_DIR, host_pid, "HOST")] + [
            (sa, gd, client_pids[sa], lbl) for sa, gd, lbl in client_specs]
        # GATE ON BEING IN-WORLD, not on having connected. `T` is swallowed while
        # any interactive surface owns input, and the LOADING SCREEN is one of
        # them (imgui_overlay.cpp CaptureActive) -- the first run of this
        # scenario typed into a peer that was still loading and lost the message
        # silently. The peer's own "Joined X's game" feed line is the first thing
        # it prints from inside the world.
        for _idx, gd, _pid, lbl in peers_for_chat[1:]:
            _wait_for_log(gd / "multivoid.log", "Joined ", 60, lbl)
        time.sleep(4)   # let the last loading cover actually come down
        # VERIFY THE SEND, DO NOT ASSUME IT. "Joined " + a flat sleep is not a
        # readiness signal: `T` is swallowed while any interactive surface owns
        # input, and a peer that joined late is still settling when the flat 4s
        # expires -- so the keystrokes go nowhere and the message is lost
        # SILENTLY. Measured 2026-07-30: two consecutive runs of this scenario on
        # identical bytes gave FAIL(6) then PASS, differing only in how long the
        # last client took to join (35s vs 21s), and the 6 "never saw X" rows
        # sent a session hunting a font regression that did not exist.
        #
        # The sender renders its own line too, so its own log is the receipt.
        # Wait for it and retype if it is not there: a lost keystroke becomes a
        # retry instead of a verdict. [[lesson-an-instrument-can-fail-the-feature-it-tests]]
        for idx, gd, pid, lbl in peers_for_chat:
            if idx >= len(chat_msgs):
                continue
            msg = chat_msgs[idx]
            log(f"  {lbl}: typing {msg!r}")
            for attempt in range(1, 4):
                _type_chat(pid, msg, lbl)
                if _log_count(gd / "multivoid.log", msg) > 0:
                    if attempt > 1:
                        log(f"  {lbl}: landed on attempt {attempt}")
                    break
                log(f"  {lbl}: not in its OWN log after attempt {attempt} -- "
                    f"the bar never took the keystrokes; retyping")
                time.sleep(2)
            else:
                log(f"  {lbl}: FAILED to submit its own chat line after 3 attempts -- "
                    f"the verdict below is about the INSTRUMENT, not the lane")
            time.sleep(1.5)
        time.sleep(4)

    # WINDOW ANCHOR (2026-08-23): the monitoring window used to start at the
    # LAST client's wire-connect -- but the cross-peer verdict needs the last
    # joiner to finish its save transfer + world load AND stream its first
    # pose (~13 s after world-ready, measured) INSIDE that window. The host
    # smoke save has grown to ~19 MB, so a wall-clock window anchored on
    # connect silently starved the last joiner and produced a false
    # "RELAY GAP" verdict: measured 2026-08-23, FAIL at 45 s post-connect,
    # clean PASS on the SAME bytes once the window covered the join pipeline
    # (drill/smoke4_window120_try2.txt). Anchor the steady-state window on
    # EVERY client's world-ready edge in the HOST log instead; --duration
    # then means what it says (steady-state dwell), independent of save
    # size. A timeout is logged and falls through -- the verdict then fails
    # WITH that attribution instead of a misleading relay-gap row.
    log(f"--- WAITING for all clients to reach world-ready in the host log "
        f"(save transfer + world load; up to {args.ready_timeout}s each) ---")
    for slot_arg, _gd, label in client_specs:
        slot = assigned.get(slot_arg)
        if slot is None:
            log(f"  {label}: never connected -- skipping its world-ready wait "
                f"(the verdict will fail with that attribution)")
            continue
        _wait_for_log(HOST_DIR / "multivoid.log",
                      f"net: slot {slot} world-ready", args.ready_timeout, label)

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

    # ARC B: photograph every board BEFORE the kill -- the arbitration is a
    # DISPLAY fact and a log line is not the thing the user sees.
    if getattr(args, "scoreboard", False):
        shots_dir = Path(__file__).resolve().parent.parent / "research" / "nickarb_shots"
        shots_dir.mkdir(parents=True, exist_ok=True)
        # Press the REAL scoreboard key (VK_OEM_3, the tilde-position key) rather
        # than relying on VOTVCOOP_SCOREBOARD_OPEN. Two reasons: the forced board
        # counts as an interactive surface on the HOST and swallows the chat bind,
        # and pressing the actual bind tests the actual bind. The host TOGGLES on
        # keydown; a client HOLDS, so its key must stay down across the capture.
        _press_scoreboard(host_pid, hold=False)
        for slot_arg, _gd, _lbl in client_specs:
            _press_scoreboard(client_pids[slot_arg], hold=True)
        time.sleep(1.0)
        _capture_window(host_pid, shots_dir / "host.png")
        for slot_arg, game_dir, label in client_specs:
            _capture_window(client_pids[slot_arg], shots_dir / f"client{slot_arg}.png")
        log(f"arc B: four boards captured -> {shots_dir}")
        for slot_arg, game_dir, label in [(0, HOST_DIR, "HOST")] + client_specs:
            ini = game_dir / "multivoid.ini"
            if ini.exists():
                for line in ini.read_text(encoding="utf-8", errors="replace").splitlines():
                    if line.strip().lower().startswith("net.nick"):
                        log(f"arc B ini {label}: {line.strip()}")

    log("--- FINAL STATE ---")
    log(f"peers alive at end: {len(last_peers)}")
    for p in last_peers:
        log(f"  PID={p['PID']} RSS={p['RSS_MB']}MB title='{p['Title']}'")

    # Tail all four logs.
    tail_log(HOST_DIR / "multivoid.log", 20, "HOST")
    for slot_arg, game_dir, label in client_specs:
        tail_log(game_dir / "multivoid.log", 20, label)

    # Parse all four logs for the verdict (after kill is fine -- logs are flushed
    # on each write; parse BEFORE kill to be safe against a crash-on-exit wipe).
    host_mk = parse_log_markers(HOST_DIR / "multivoid.log")
    client_mks = {slot: parse_log_markers(gd / "multivoid.log")
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

    # WP-2 boot-lane assertion, all four peers (CLIENT2's first-ever UE4SS
    # boots ride this check too).
    for lbl, d in [("HOST", HOST_DIR)] + [(lbl2, gd) for _s, gd, lbl2 in client_specs]:
        failures.extend(_lane_check(lbl, d))

    # --- i18n assertions (--assert-i18n). Every peer must be able to SEE every
    # other peer's name and message, byte-for-byte, and no lane may have emitted
    # ill-formed or empty text on the way. This is the half a relay verdict
    # cannot reach: the relay can be perfect while the NAMES arrive blanked.
    if getattr(args, "assert_i18n", False):
        want_names = [n for n in (getattr(args, "nicks", None) or "").split(",") if n != ""]
        want_msgs = [m for m in (getattr(args, "chat", None) or "").split("|") if m != ""]
        all_peers = [(0, HOST_DIR, "HOST")] + list(client_specs)
        for idx, gd, lbl in all_peers:
            # A peer must carry every OTHER peer's name and message; its own name
            # is excluded because the host may legitimately have renamed it.
            must = ([n for j, n in enumerate(want_names) if j != idx] +
                    [m for j, m in enumerate(want_msgs) if j != idx])
            failures.extend(_peer_log_health(lbl, gd / "multivoid.log", must))
        # The host is the only peer that logs an arbitration decision, and that
        # line is the one the C-locale defect deleted. Its ABSENCE is the finding.
        htext, _ = _read_log_strict(HOST_DIR / "multivoid.log")
        decided = htext.count("nickname_arbiter: slot ")
        if decided < len(client_specs):
            failures.append(f"host: only {decided} arbitration decision line(s) for "
                            f"{len(client_specs)} clients -- either the arbiter did not "
                            f"run or the log line was destroyed by its own arguments")
        else:
            notes.append(f"i18n: {decided} arbitration decisions logged, all names "
                         f"round-tripped to every peer")

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
    # encoding="utf-8" IS THE ASSERTION. The mod writes its log as UTF-8; without
    # this, read_text() uses the process default (cp1251 on a RU Windows box), so
    # every non-ASCII needle is compared against mojibake and can NEVER match.
    # Measured 2026-07-29: smoke_i18n reported 6 FAILs -- "HOST: never saw
    # 'привет всем'" -- against a log that contained every line byte-intact and a
    # chat lane that was working end-to-end on all four scripts. The instrument
    # failed the feature. Every other _read_text call in this file already passes
    # the encoding; this one was the single escapee, and it is the one the i18n
    # verdict is built on. [[lesson-an-instrument-blind-to-the-phenomenon-always-passes]]
    try:
        return log_path.read_text(encoding="utf-8", errors="replace").count(needle)
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



# ---------------------------------------------------------------------------
# i18n SCENARIO SUPPORT (2026-07-28, user request: "a smoke test like when
# players with jap cn eng cyrillic connect and play, to catch issues").
#
# Names alone were never the whole lane. Today's session found a defect on the
# LOG path and one on the EGRESS path that no name-only drill could see, and
# both only fired for non-ASCII peers. So this drives the full chain a real
# mixed-script lobby exercises -- keyboard -> ImGui InputText -> UTF-8 buffer ->
# wire -> the other peers' feed -> disk -- and then asserts on the ARTIFACTS
# rather than on "it didn't crash".
# ---------------------------------------------------------------------------

def _peer_hwnd(pid: int):
    """Top-level visible window owned by `pid` (the game's render window)."""
    import ctypes
    from ctypes import wintypes
    u32 = ctypes.WinDLL("user32", use_last_error=True)
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _lp):
        owner = wintypes.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid or not u32.IsWindowVisible(hwnd):
            return True
        n = u32.GetWindowTextLengthW(hwnd)
        if n > 0:
            found.append(hwnd)
            return False
        return True

    u32.EnumWindows(cb, 0)
    return found[0] if found else None


def _press_vk(pid: int, vk: int, label: str = "") -> bool:
    """One down+up of a virtual key into a peer's window."""
    import ctypes
    u32 = ctypes.WinDLL("user32", use_last_error=True)
    WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
    hwnd = _peer_hwnd(pid)
    if not hwnd:
        if label:
            log(f"  {label}: no window for pid {pid} -- cannot press vk 0x{vk:02X}")
        return False
    u32.PostMessageW(hwnd, WM_KEYDOWN, vk, 0)
    u32.PostMessageW(hwnd, WM_KEYUP, vk, 0)
    return True


def _type_chat(pid: int, text: str, label: str, submit: bool = True) -> bool:
    """Open the chat bar with T, type `text`, press Enter -- as real messages.

    submit=False stops BEFORE Enter, leaving the bar open with the text in it.
    That is the only way to drive the chat-history reveal from a fixture: the
    reveal is gated on the surface being OPEN, and every other path through this
    function closes it. `text` may be empty to just hold the bar open.

    To CLOSE a bar held this way, press Enter on an empty field (chat_input.cpp
    closes on submit either way) -- NOT Escape, which also falls through to the
    game and raises the pause menu, and the pause menu suppresses the whole HUD
    pass, so the reveal's own close marker would never be written.

    WM_CHAR carries UTF-16 code units, so an astral character arrives as a
    surrogate PAIR and ImGui reassembles it (AddInputCharacterUTF16). That path
    only works because IMGUI_USE_WCHAR32 is on -- before arc D2 the reassembled
    codepoint was DISCARDED (imgui.cpp:1512), i.e. an emoji could not be typed
    at all. Driving it from here is what keeps that true.
    """
    import ctypes
    u32 = ctypes.WinDLL("user32", use_last_error=True)
    WM_KEYDOWN, WM_KEYUP, WM_CHAR = 0x0100, 0x0101, 0x0102
    VK_RETURN = 0x0D
    hwnd = _peer_hwnd(pid)
    if not hwnd:
        log(f"  {label}: no window for pid {pid} -- cannot type")
        return False
    u32.PostMessageW(hwnd, WM_KEYDOWN, ord('T'), 0)
    u32.PostMessageW(hwnd, WM_KEYUP, ord('T'), 0)
    time.sleep(0.8)                       # let the bar open + take focus
    # One WM_CHAR per UTF-16 CODE UNIT -- an astral character is therefore two
    # messages, a high surrogate then a low one, which is exactly what a real
    # keyboard/IME delivers and what ImGui has to reassemble.
    units = text.encode("utf-16-le")
    for k in range(0, len(units), 2):
        u32.PostMessageW(hwnd, WM_CHAR, units[k] | (units[k + 1] << 8), 0)
        time.sleep(0.01)
    time.sleep(0.4)
    if not submit:
        return True
    u32.PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0)
    u32.PostMessageW(hwnd, WM_KEYUP, VK_RETURN, 0)
    time.sleep(0.6)
    return True


def _press_scoreboard(pid: int, hold: bool) -> None:
    """VK_OEM_3 (the key left of 1) opens the player list -- toggle on the host,
    hold-to-show on a client (imgui_overlay.cpp:174-186)."""
    import ctypes
    u32 = ctypes.WinDLL("user32", use_last_error=True)
    WM_KEYDOWN, WM_KEYUP, VK_OEM_3 = 0x0100, 0x0101, 0xC0
    hwnd = _peer_hwnd(pid)
    if not hwnd:
        return
    u32.PostMessageW(hwnd, WM_KEYDOWN, VK_OEM_3, 0)
    if not hold:
        u32.PostMessageW(hwnd, WM_KEYUP, VK_OEM_3, 0)


def _read_log_strict(path: Path):
    """(text, utf8_error) -- the log MUST decode as strict UTF-8.

    This is the cheapest total check on every text lane at once: a CESU-8 pair
    from a hand-rolled encoder, a byte cut mid-sequence, an ANSI narrow, all land
    here as a decode error. errors='replace' would hide every one of them.
    """
    raw = path.read_bytes() if path.exists() else b""
    try:
        return raw.decode("utf-8"), None
    except UnicodeDecodeError as e:
        return raw.decode("utf-8", errors="replace"), f"{e.reason} at byte {e.start}"


_EMPTY_LINE = re.compile(r"^\[\d\d:\d\d:\d\d\] \[\w+\s*\]\s*$")


def _peer_log_health(label: str, path: Path, must_contain: list[str] | None = None) -> list[str]:
    """Every assertion one peer's log can answer. Returns failure strings.

    NOT i18n-SPECIFIC, and the name it used to carry (`_i18n_checks`) was the
    whole defect. Nothing in here is about mixed scripts: strict-UTF-8 decoding,
    a line that formatted to nothing, a selftest FAIL, the positive font-selftest
    line -- those are health questions about ANY peer log. But it was only ever
    called from smoke_i18n, so the plain `smoke` verdict never asked them, and on
    2026-07-30 a deliberately-mutated font selftest logged two ERROR rows and
    `fail=2` while the smoke printed PASS. The check existed, was correct, and
    was wired to one scenario -- [[lesson-a-gate-on-one-verb-reads-as-a-gate-on-
    the-path]] in its purest form.

    `must_contain` stays optional because only the i18n run has specific strings
    it demands survive the wire; every other caller wants the health half.
    """
    must_contain = must_contain or []
    fails = []
    text, err = _read_log_strict(path)
    if err:
        fails.append(f"{label}: log is NOT well-formed UTF-8 ({err}) -- a text lane "
                     f"emitted ill-formed bytes")
    # A line that formatted to nothing. This is the exact shape of the 2026-07-28
    # logger defect: %ls + the C locale returned -1 and left the buffer empty, so
    # every line naming a non-ASCII peer became a bare timestamp.
    blank = [ln for ln in text.splitlines() if _EMPTY_LINE.match(ln)]
    if blank:
        fails.append(f"{label}: {len(blank)} log line(s) formatted to NOTHING "
                     f"(first: {blank[0]!r}) -- an argument killed the message")
    if "[args unformattable]" in text:
        fails.append(f"{label}: the formatter fell back to the format string -- "
                     f"an argument could not be converted")
    for st in ("selftest: FAIL",):
        if st in text:
            bad = [ln for ln in text.splitlines() if st in ln]
            fails.append(f"{label}: {len(bad)} selftest FAILURE(s): {bad[0][:120]}")
    # THE FONT SELFTEST IS ASSERTED POSITIVELY, and the negative grep above is no
    # longer enough for it. Until 2026-07-30 that selftest ran unconditionally in
    # ui::fonts::Load(), so "no FAIL line" implied "it ran and passed". The ImGui
    # flip made it CONDITIONAL -- it fires on an atlas texture-id edge, once per
    # BUILD -- and at that moment "passed" and "never ran" produce the identical
    # log. An instrument whose absence is indistinguishable from its success is
    # not an instrument. This is the same shape the config selftest already uses
    # (see 'config-selftest: DONE fail=0' below).
    if "font selftest: DONE fail=0" not in text:
        fails.append(f"{label}: no 'font selftest: DONE fail=0' line -- the per-build font "
                     f"selftest either failed or NEVER RAN (it is conditional on an atlas "
                     f"texture-id edge; silence is not a pass)")
    # THE CASE TABLE, same argument as the font selftest above and a sharper one:
    # it is GENERATED, and a generated fold table that arrives empty folds
    # nothing -- which produces a lobby where no two names collided, exactly what
    # a healthy lobby produces. There is no failure symptom to grep for, so the
    # positive line is the evidence, and it is demanded on EVERY smoke.
    if "case-fold selftest: PASS" not in text:
        fails.append(f"{label}: no 'case-fold selftest: PASS' line -- the generated "
                     f"case table either failed its rows or never ran; an empty "
                     f"fold table is silent by construction")
    for needle in must_contain:
        if needle not in text:
            fails.append(f"{label}: never saw {needle!r} -- it was blanked, squashed "
                         f"or truncated somewhere on the way")
    return fails


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


# The four scripts, one peer each, plus an astral emoji so the typed-input path
# (WM_CHAR surrogate pair -> AddInputCharacterUTF16 -> IMGUI_USE_WCHAR32) is
# exercised rather than assumed. Names and messages are DIFFERENT strings on
# purpose: a name travels the Join/RosterRow lane, a message travels the chat
# lane, and this session found a defect on a third lane (the log) that neither
# would have shown on its own.
#
# ONE OF THE FOUR IS A COMBINING-MARK SCRIPT (2026-07-30, commit 2). Until that
# commit every mark was excluded from the atlas, so Thaana -- which is written
# ENTIRELY in Mn -- could not be drawn at all, and Thai, Arabic, Hebrew and Tamil
# drew their base letters with boxes where the marks belong. Nothing in this set
# would have noticed: Japanese and Chinese exercise the SAME sentinel path as each
# other, so the second of them bought no coverage, and none of the six strings
# contained a mark. The Thaana nick and the pointed-Hebrew message are here so the
# feature has an end-to-end assertion instead of a claim.
I18N_NICKS = ["Pelmentor", "Пельмень",
              "张伟明", "ދިވެހި"]
I18N_CHAT = ["hello everyone 😀",
             "привет всем",
             "你好世界",
             "שָׁלוֹם עולם"]


def cmd_smoke_i18n(args) -> None:
    """The mixed-script lobby, as one word."""
    args.nicks = ",".join(I18N_NICKS)
    args.chat = "|".join(I18N_CHAT)
    args.assert_i18n = True
    # NOT the VOTVCOOP_SCOREBOARD_OPEN env: a forced board is an interactive
    # surface on the host and swallows `T`, so the host could never chat. The
    # capture phase presses the real key instead (and thereby tests it).
    args.scoreboard = True
    log("--- i18n LOBBY: " + " / ".join(I18N_NICKS) + " ---")
    cmd_smoke4(args)


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
            tail_log(HOST_DIR / "multivoid.log", 30, "HOST")
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
    host_log = HOST_DIR / "multivoid.log"
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
        client_mirror[label] = _log_count(game_dir / "multivoid.log", "materialized mirror")
        tail_log(game_dir / "multivoid.log", 8, label)

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


def cmd_kerfurtoggle(args) -> None:
    """Verify the CLIENT kerfur conversion-adopt fix end-to-end. Both peers boot FRESH
    (so the spawned kerfur is the ONLY one -> 'nearest kerfur' is unambiguous); the host
    dev-spawns one kerfur, it mirrors to the client, then the client toggles it OFF then
    ON via the kerfur_toggle trigger (a reflection stand-in for the radial menu). PASS
    requires the client to CLAIM its own local conversion ghost + ADOPT it as the host
    mirror -- prop via the Gap-I-1 fuzzy match, NPC via npc-mirror[adopt] 'bound EXISTING'
    -- with NO orphan-destroy and NO 'outside the host range' / untracked-held cascade.
    That combination = no dupe + no destroy/respawn pop (the exact bug)."""
    spawn_trigger = str(HOST_DIR / "spawn_npc.trigger")
    toggle_trigger = str(CLIENT_DIR / "kerfur_toggle.trigger")
    for t in (spawn_trigger, toggle_trigger):
        try:
            Path(t).unlink()
        except OSError:
            pass
    if kill_all() > 0:
        log("note: pre-existing VotV killed before kerfurtoggle")
    deploy_all()

    log("--- HOST LAUNCH (FRESH, spawn trigger armed) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb, trigger_file=spawn_trigger,
                           extra_env={"VOTVCOOP_FRESH": "1"})
    bound = False
    for i in range(args.boot_timeout):
        time.sleep(1)
        if host_owns_udp(host_pid, args.port):
            log(f"host bound UDP after {i+1}s"); bound = True; break
        if not any(p["PID"] == host_pid for p in list_votv()):
            log("HOST DIED before binding UDP"); tail_log(HOST_DIR / "multivoid.log", 30, "HOST"); sys.exit(1)
    if not bound:
        log("FAIL: host did not bind UDP"); kill_all(); sys.exit(1)

    log("--- CLIENT LAUNCH (FRESH, kerfur-toggle trigger armed) ---")
    client_pid = launch_peer("client", args.port, "Client", peer="127.0.0.1",
                             res_x=1280, res_y=720, peer_slot=1, monitor=2, tile_index=0,
                             memory_limit_gb=args.memory_limit_gb,
                             extra_env={"VOTVCOOP_KERFUR_TOGGLE_TRIGGER": toggle_trigger})
    slot = wait_for_client_connect(CLIENT_DIR, args.client_boot_timeout, "CLIENT", client_pid)
    if slot is None:
        log("FAIL: client never connected"); kill_all(); sys.exit(1)

    host_log = HOST_DIR / "multivoid.log"
    client_log = CLIENT_DIR / "multivoid.log"
    _wait_for_log(host_log, "npc-suppress: installed interceptor", args.install_timeout, "HOST")
    # The client poll is gated on load-tail quiescence -- it will not detect a toggle until
    # the join reconcile settles. Wait for it so a later 'no detection' is a real failure.
    _wait_for_log(client_log, "load tail quiesced", 60, "CLIENT")
    log(f"--- settling {args.settle}s, then spawning the kerfur on the host ---")
    time.sleep(args.settle)
    Path(spawn_trigger).write_text("spawn")
    _wait_for_log(host_log, "spawn_npc: spawned 'kerfurOmega_C'", 20, "HOST")
    _wait_for_log(client_log, "materialized mirror eid=", 20, "CLIENT")
    log("--- kerfur mirrored; settling 4s so the client poll baselines it ALIVE ---")
    time.sleep(4)

    log("--- CLIENT TOGGLE #1 (turn_off: NPC mirror -> prop) ---")
    Path(toggle_trigger).write_text("toggle")
    _wait_for_log(client_log, "kerfur_toggle: TEST-toggling", 15, "CLIENT")
    _wait_for_log(client_log, "POLL turn_off", 15, "CLIENT")
    _wait_for_log(host_log, "HOST executing turn_off", 15, "HOST")
    log("--- settling 6s for the prop ADOPT (Gap-I-1 fuzzy match) + ghost cleanup ---")
    time.sleep(6)

    log("--- CLIENT TOGGLE #2 (turn_on: prop mirror -> NPC) ---")
    Path(toggle_trigger).write_text("toggle")
    _wait_for_log(client_log, "POLL turn-on", 15, "CLIENT")
    _wait_for_log(host_log, "HOST executing turn-on", 15, "HOST")
    log("--- settling 6s for the NPC ADOPT (bound EXISTING) + ghost cleanup ---")
    time.sleep(6)

    shots_dir = Path(__file__).resolve().parent.parent / "research" / "npctest_shots"
    shots_dir.mkdir(parents=True, exist_ok=True)
    _capture_window(host_pid, shots_dir / "kerfurtoggle_host.png")
    _capture_window(client_pid, shots_dir / "kerfurtoggle_client.png")

    # Verdict markers (read before kill).
    c_toggled = _log_count(client_log, "kerfur_toggle: TEST-toggling")
    c_off = _log_count(client_log, "POLL turn_off")
    c_on = _log_count(client_log, "POLL turn-on")
    c_claim_prop = _log_count(client_log, "(prop turn-off)")
    c_claim_npc = _log_count(client_log, "(NPC turn-on)")
    # Prop turn-off adopt: the D2 fix (df589591) adopts the parked ghost BY EID
    # ("adopted parked turn-off ghost as PROP mirror ... by eid"), which REPLACED the
    # old Gap-I-1 fuzzy-position match. Accept either: by-eid is the primary path,
    # fuzzy is the legacy fallback. (Checking only the fuzzy marker false-FAILed the
    # whole scenario once adopt went by-eid -- 2026-06-28.)
    c_prop_adopt = _log_count(client_log, "adopted parked turn-off ghost as PROP mirror")
    c_fuzzy = _log_count(client_log, "Gap-I-1 FUZZY MATCH 'prop_kerfurOmega")
    c_adopt = _log_count(client_log, "npc-mirror[adopt]: bound EXISTING")
    c_orphan = _log_count(client_log, "orphan conversion ghost")
    c_cascade = _log_count(client_log, "BROADCAST held untracked")
    h_off = _log_count(host_log, "HOST executing turn_off")
    h_on = _log_count(host_log, "HOST executing turn-on")
    h_outofrange = _log_count(host_log, "outside the host range")
    tail_log(client_log, 28, "CLIENT")
    tail_log(host_log, 14, "HOST")
    log("--- KILLING ---"); kill_all()

    log("--- KERFURTOGGLE VERDICT ---")
    log(f"client: toggled={c_toggled} turn_off={c_off} turn_on={c_on} "
        f"claim_prop={c_claim_prop} claim_npc={c_claim_npc} "
        f"prop_adopt(eid)={c_prop_adopt} fuzzy_fallback={c_fuzzy} npc_adopt={c_adopt} orphan_destroy={c_orphan} cascade={c_cascade}")
    log(f"host: exec_off={h_off} exec_on={h_on} out_of_range_req={h_outofrange}")
    fails: list[str] = []
    if c_toggled < 2:
        fails.append(f"client toggle did not fire twice (got {c_toggled}) -- trigger/setup issue")
    if c_off < 1:
        fails.append("no client turn_off detection (poll gated off? quiescence/baseline issue)")
    if c_on < 1:
        fails.append("no client turn_on detection")
    if c_claim_prop < 1:
        fails.append("client did NOT claim+freeze the turn-off ghost prop")
    if c_claim_npc < 1:
        fails.append("client did NOT claim+park the turn-on ghost NPC")
    if c_prop_adopt < 1 and c_fuzzy < 1:
        fails.append("PROP ADOPT FAILED: no by-eid adopt ('adopted parked turn-off ghost as PROP mirror') AND no Gap-I-1 fuzzy fallback -- the frozen ghost was not adopted (host prop not broadcast? eid mismatch?)")
    if c_adopt < 1:
        fails.append("NPC ADOPT FAILED: no 'bound EXISTING' -> the turn-on fresh-spawned a duplicate beside the ghost (the dupe)")
    if c_orphan > 0:
        fails.append(f"{c_orphan} orphan-destroy: an adopt MISSED and the ghost timed out (a transient dupe before cleanup)")
    if h_outofrange > 0:
        fails.append(f"{h_outofrange} 'outside host range' kerfur request -- the client-eid cascade signature (a real dupe)")
    if c_cascade > 0:
        fails.append(f"{c_cascade} untracked-held kerfur broadcast -- grab cascade")
    if fails:
        log(f"FAIL ({len(fails)} issue(s)):")
        for f in fails:
            log(f"  - {f}")
        sys.exit(2)
    log("PASS: client toggled off+on; CLAIMED + ADOPTED both ghosts (prop via Gap-I-1 fuzzy, "
        "NPC via bound-EXISTING); no orphan-destroy, no cascade -> no dupe, no respawn pop.")
    sys.exit(0)


# --- join-churn regression markers (the 2026-06-18 R1-R4 MTA refactor: incremental per-prop
#     streaming + membership-bounded sweep REPLACED the 2026-06-17 reconcile-once latch band-aid) ---
_JC_SWEEP_FIRING = "divergence sweep FIRING"               # the divergence sweep actually running
_JC_INCREMENTAL  = "broadcasting one PropSpawn each (incremental"  # R1: bracket-free incremental delta (HOST log; THE fix proof -- replaces the deleted reconcile-once gate)
_JC_DESTROYED    = "unclaimed locals destroyed"            # a sweep that destroyed locals (the thrash, if repeated)
_JC_NOMATCH      = "no local match (key or eid)"           # the eid=0 held-clump flood


def cmd_joinchurn(args) -> None:
    """Autonomous regression test for the join-churn class of bug. Originally guarded the 2026-06-17
    reconcile-once latch band-aid; since 2026-06-18 it verifies the R1-R4 MTA refactor that REPLACED
    that latch (R1 incremental per-prop streaming + R3 membership-bounded sweep).

    Reproduces the EXACT reported bug: the HOST loads the populated s_1234 (its garbage/ambient
    spawners mint chipPiles continuously), a FRESH client joins, and the host's steady-world re-seed
    adopts new props every few seconds. The OLD bug: each adoption re-fired a FULL bracketed
    re-snapshot, re-arming the client's DESTRUCTIVE divergence sweep ~10x in one join -- thrashing the
    piles + dooming kerfur mirrors (the "piles lost sync / kerfurs come alive by themselves" report).
    R1 fixed it at the SOURCE: the re-seed now broadcasts ONE bracket-free incremental PropSpawn per
    new prop (MTA CEntityAddPacket), so no bracket re-arms the sweep. R3 made the sweep
    membership-bounded (it only adjudicates the client's OWN local Prop Elements; host-driven mirrors
    are excluded at the source), so a re-fire can no longer doom a kerfur mirror or wipe the world.

    Then it spawns a kerfur on the host -- exercising the kerfur-mirror path (it must mirror + PERSIST,
    not be doomed by the sweep -> no spurious POLL flip-flop). The host's continuous ambient chipPile
    spawns drive the R1 incremental marker.

    Screenshots are captured at FOUR situations: A=mid-join churn, B=post-reconcile, C=kerfur present,
    D=steady-state. PASS requires: both peers alive + connected; the R1 incremental PropSpawn delta
    FIRED (>=1, HOST log); the sweep fired only a few times (<= --max-sweeps, NOT the ~10 churn);
    bounded destroy-sweeps + no-local-match flood; the kerfur mirrored with no flip-flop loop."""
    shots_dir = ROOT / "research" / "joinchurn_shots"
    shots_dir.mkdir(parents=True, exist_ok=True)
    host_log = HOST_DIR / "multivoid.log"
    client_log = CLIENT_DIR / "multivoid.log"
    spawn_trigger = str(HOST_DIR / "spawn_npc.trigger")
    for t in (spawn_trigger,):
        try:
            Path(t).unlink()
        except OSError:
            pass

    if kill_all() > 0:
        log("note: pre-existing VotV killed before joinchurn")
    deploy_all()

    # --- HOST: populated s_1234 (the garbage spawners that keep minting piles = the churn trigger) ---
    log("--- HOST LAUNCH (s_1234 populated; kerfur spawn trigger armed) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb, trigger_file=spawn_trigger)
    bound = False
    for i in range(args.boot_timeout):
        time.sleep(1)
        if host_owns_udp(host_pid, args.port):
            log(f"host bound UDP after {i+1}s"); bound = True; break
        if not any(p["PID"] == host_pid for p in list_votv()):
            log("HOST DIED before UDP bind"); tail_log(host_log, 30, "HOST"); sys.exit(1)
    if not bound:
        log("FAIL: host did not bind UDP"); kill_all(); sys.exit(1)
    _wait_for_log(host_log, "==== PLAY READY ====", args.boot_timeout, "HOST")
    log(f"host-settle {args.host_settle}s (drain the boot-world's dying prop elements before connect)...")
    time.sleep(args.host_settle)

    # --- CLIENT: FRESH join (the join that drove the churn) ---
    log("--- CLIENT LAUNCH (FRESH join) ---")
    client_pid = launch_peer("client", args.port, "Client", peer="127.0.0.1",
                             res_x=1280, res_y=720, peer_slot=1, monitor=2, tile_index=0,
                             memory_limit_gb=args.memory_limit_gb)
    slot = wait_for_client_connect(CLIENT_DIR, args.client_boot_timeout, "CLIENT", client_pid)
    if slot is None:
        log("FAIL: client never connected"); tail_log(client_log, 30, "CLIENT"); kill_all(); sys.exit(1)

    def snap(tag: str) -> None:
        _capture_window(host_pid, shots_dir / f"{tag}_host.png")
        _capture_window(client_pid, shots_dir / f"{tag}_client.png")

    # --- SITUATION A: the join-churn window. Sample RSS; screenshot mid-join. ---
    log(f"--- SITUATION A: monitoring the join-churn window for {args.join_monitor}s ---")
    t0 = time.time(); shot_a = False; kill_reason = None
    while time.time() - t0 < args.join_monitor:
        time.sleep(args.sample_interval)
        peers = list_votv(); t = int(time.time() - t0)
        mx = max((p["RSS_MB"] for p in peers), default=0)
        log(f"  t={t}s peers={len(peers)} maxRSS={mx}MB  "
            f"sweep_FIRING={_log_count(client_log, _JC_SWEEP_FIRING)} "
            f"incr(HOST)={_log_count(host_log, _JC_INCREMENTAL)} "
            f"flood={_log_count(client_log, _JC_NOMATCH)}")
        if mx > args.ram_kill_mb:
            kill_reason = f"peer RSS {mx}MB > {args.ram_kill_mb}MB kill threshold"; break
        if len(peers) < 2:
            kill_reason = "a peer DIED during the join"; break
        if not shot_a and t >= args.midjoin_shot_at:
            log("  -- mid-join screenshot (SITUATION A) --"); snap("A_midjoin"); shot_a = True
    if not shot_a and not kill_reason:
        snap("A_midjoin")
    if kill_reason:
        tail_log(client_log, 40, "CLIENT"); tail_log(host_log, 12, "HOST")
        log("--- KILLING ---"); kill_all(); log(f"FAIL: {kill_reason}"); sys.exit(2)

    # --- SITUATION B: reconciliation. Wait for the client's load-tail quiescence (the ONE sweep). ---
    _wait_for_log(client_log, "load tail quiesced", 60, "CLIENT")
    log("--- SITUATION B: reconciled; post-reconcile screenshot + 4s settle ---")
    time.sleep(4)
    snap("B_reconciled")

    # --- SITUATION C: spawn a kerfur. Exercises the kerfur mirror (it must persist, not be swept ->
    #     no spurious POLL-turn-on flip-flop). The R1 incremental marker is driven by the host's
    #     continuous ambient chipPile spawns throughout the join, not by this kerfur. ---
    log("--- SITUATION C: spawn a kerfur (forces a post-reconcile re-snapshot + tests the kerfur mirror) ---")
    _wait_for_log(host_log, "npc-suppress: installed interceptor", args.install_timeout, "HOST")
    Path(spawn_trigger).write_text("spawn")
    _wait_for_log(host_log, "spawn_npc: spawned 'kerfurOmega_C'", 20, "HOST")
    _wait_for_log(client_log, "materialized mirror", 25, "CLIENT")
    log("--- kerfur mirrored; settling 12s (watch for spurious convert + the post-reconcile incremental delta) ---")
    time.sleep(12)
    snap("C_kerfur")

    # --- SITUATION D: steady state ---
    log(f"--- SITUATION D: steady-state settle {args.steady}s ---")
    time.sleep(args.steady)
    snap("D_steady")

    # --- markers (read before kill) ---
    sweeps     = _log_count(client_log, _JC_SWEEP_FIRING)
    incr       = _log_count(host_log, _JC_INCREMENTAL)
    destroyed  = _log_count(client_log, _JC_DESTROYED)
    nomatch    = _log_count(client_log, _JC_NOMATCH)
    k_mirror   = _log_count(client_log, "materialized mirror")
    k_off      = _log_count(client_log, "POLL turn_off")
    k_on       = _log_count(client_log, "POLL turn-on")
    h_outofrng = _log_count(host_log, "outside the host range")
    cmk        = parse_log_markers(client_log)
    peers_end  = list_votv()
    tail_log(client_log, 30, "CLIENT")
    tail_log(host_log, 12, "HOST")
    log("--- KILLING ---"); kill_all()

    # --- verdict ---
    log("--- JOINCHURN VERDICT ---")
    log(f"churn: divergence-sweep-FIRING(CLIENT)={sweeps}   incremental-PropSpawn(HOST)={incr}   "
        f"unclaimed-destroyed(CLIENT)={destroyed}   no-local-match-flood(CLIENT)={nomatch}")
    log(f"kerfur(CLIENT): materialized-mirror={k_mirror}  spurious turn_off={k_off} turn_on={k_on}   "
        f"host out-of-range-req={h_outofrng}")
    log(f"liveness: peers_end={len(peers_end)} assigned_slot={cmk['assigned_slot']} "
        f"puppet_slots={sorted(cmk['puppet_slots'])} malformed_drops={cmk['malformed_drops']}")
    fails: list[str] = []
    if len(peers_end) < 2:
        fails.append(f"a peer died (peers_end={len(peers_end)})")
    if cmk["assigned_slot"] is None:
        fails.append("client never reached connected (no peer-slot assignment)")
    if 0 not in cmk["puppet_slots"]:
        fails.append("client never spawned the host puppet (pose stream not flowing)")
    if incr < 1:
        fails.append("the R1 INCREMENTAL PropSpawn delta never fired (no 'broadcasting one PropSpawn each "
                     "(incremental') in the HOST log -- R1 is NOT active, OR the host's ambient/garbage "
                     "spawners are idle (the steady-world re-seed adopted nothing to express)")
    if sweeps > args.max_sweeps:
        fails.append(f"divergence sweep FIRED {sweeps}x (> {args.max_sweeps}) -- the re-snapshot/sweep CHURN is back "
                     f"(R1 should hold this to ~1-2: the world-load reconcile(s), no steady re-arm)")
    if destroyed > args.max_destroys:
        fails.append(f"{destroyed} destructive 'unclaimed locals destroyed' sweeps (> {args.max_destroys}) -- repeated reconciliation = the thrash")
    if nomatch > args.max_nomatch:
        fails.append(f"{nomatch} 'no local match' (> {args.max_nomatch}) -- the eid=0 held-clump flood is back")
    if k_mirror < 1:
        fails.append("the spawned kerfur NEVER mirrored to the client (kerfur sync broken)")
    if (k_off + k_on) > args.max_kerfur_convert:
        fails.append(f"{k_off + k_on} spurious kerfur converts (> {args.max_kerfur_convert}) -- the 'died invisibly' flip-flop loop")
    if h_outofrng > 0:
        fails.append(f"{h_outofrng} 'outside host range' kerfur request -- a client-eid cascade dupe")
    if cmk["malformed_drops"] > 0:
        fails.append(f"{cmk['malformed_drops']} malformed (senderEpoch=0) wire drop(s)")
    for tag in ("A_midjoin", "B_reconciled", "C_kerfur", "D_steady"):
        log(f"  screenshots: {tag}_host.png + {tag}_client.png  (research/joinchurn_shots/)")
    if fails:
        log(f"FAIL ({len(fails)} issue(s)):")
        for f in fails:
            log(f"  - {f}")
        sys.exit(2)
    log(f"PASS: R1 incremental PropSpawn delta fired {incr}x (HOST); divergence sweep only {sweeps}x "
        f"(no churn -- no steady re-arm); {destroyed} destroy-sweep(s); no-local-match flood {nomatch} "
        f"(bounded); kerfur mirrored ({k_mirror}) with no flip-flop ({k_off + k_on} converts); both "
        f"peers stable + connected.")
    sys.exit(0)


def cmd_spawnmenutest(args) -> None:
    """Autonomous diagnosis of the story-mode Q spawn-menu dev feature. Launches a HOST in
    STORY mode (s_1234) with the spawn-menu file trigger, fires OpenNow() once the world is up,
    and reports spawn_menu::Open's diagnostics (activeInterface state + dispatch result) + a
    screenshot -- so 'the menu doesn't appear' is root-caused without a physical Q press."""
    trigger = str(HOST_DIR / "spawnmenu.trigger")
    try:
        Path(trigger).unlink()
    except OSError:
        pass
    if kill_all() > 0:
        log("note: pre-existing VotV killed before spawnmenutest")
    deploy_all()
    log("--- HOST LAUNCH (story s_1234, spawn-menu trigger armed) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb,
                           extra_env={"VOTVCOOP_SPAWNMENU_TRIGGER": trigger})
    host_log = HOST_DIR / "multivoid.log"
    log(f"waiting up to {args.boot_timeout}s for host PLAY READY...")
    ready = False
    for i in range(args.boot_timeout):
        if "==== PLAY READY ====" in _read_text(host_log):
            log(f"host PLAY READY after {i}s"); ready = True; break
        time.sleep(1)
        if not any(p["PID"] == host_pid for p in list_votv()):
            log("HOST DIED before PLAY READY"); tail_log(host_log, 30, "HOST"); sys.exit(1)
    if not ready:
        log("FAIL: host never PLAY READY"); kill_all(); sys.exit(1)
    _wait_for_log(host_log, "spawn_menu_unlock: file-trigger ENABLED", 10, "HOST")
    log(f"--- settling {args.settle}s, then firing the spawn-menu trigger ---")
    time.sleep(args.settle)
    shots_dir0 = Path(__file__).resolve().parent.parent / "research" / "npctest_shots"
    shots_dir0.mkdir(parents=True, exist_ok=True)
    _capture_window(host_pid, shots_dir0 / "spawnmenu_host_BEFORE.png")
    Path(trigger).write_text("open")
    _wait_for_log(host_log, "spawn_menu_unlock: trigger file seen", 10, "HOST")
    log("--- waiting 3s for the open dispatch + diagnostics ---")
    time.sleep(3)
    shots_dir = Path(__file__).resolve().parent.parent / "research" / "npctest_shots"
    shots_dir.mkdir(parents=True, exist_ok=True)
    _capture_window(host_pid, shots_dir / "spawnmenu_host.png")
    text = _read_text(host_log)
    diag = [ln for ln in text.splitlines() if "spawn_menu" in ln]
    log("--- SPAWN-MENU DIAGNOSTICS (all spawn_menu log lines) ---")
    for ln in diag[-30:]:
        log(f"  {ln}")
    log("--- KILLING ---"); kill_all()
    dispatched = _log_count(host_log, "spawn menu open requested")
    iface_set = _log_count(host_log, "activeInterface is SET")
    iface_clear = _log_count(host_log, "activeInterface is null")
    no_player = _log_count(host_log, "no local mainPlayer")
    no_fn = _log_count(host_log, "could not resolve mainPlayer")
    refused = _log_count(host_log, "OpenNow REFUSED")
    log("--- SPAWNMENUTEST VERDICT (diagnostic) ---")
    log(f"dispatched={dispatched} activeInterface_SET={iface_set} activeInterface_clear={iface_clear} "
        f"no_player={no_player} no_fn={no_fn} refused={refused}")
    log(f"screenshot: {shots_dir / 'spawnmenu_host.png'}")
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
            log("HOST DIED before binding UDP"); tail_log(HOST_DIR / "multivoid.log", 30, "HOST"); sys.exit(1)
    if not bound:
        log("FAIL: host did not bind UDP"); kill_all(); sys.exit(1)

    log("--- CLIENT LAUNCH (ragdoll driver) ---")
    client_pid = launch_peer("client", args.port, "Client", peer="127.0.0.1",
                             res_x=1280, res_y=720, peer_slot=1, monitor=2,
                             tile_index=0, memory_limit_gb=args.memory_limit_gb)
    wait_for_client_connect(CLIENT_DIR, args.client_boot_timeout, "CLIENT", client_pid)

    host_log = HOST_DIR / "multivoid.log"
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

    host_log = HOST_DIR / "multivoid.log"
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

    host_log = HOST_DIR / "multivoid.log"
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


def cmd_wirewindow(args) -> None:
    """2-PEER D2 wire-window probe (islive-zeroav DESIGN 2026-08-22 section 6).

    Question: when a CLIENT exits to the menu with the coop layer LIVE (a player's
    own in-game exit), does it LEAK wire traffic about its dying world during the
    <=4 s purge-blind window before the flee's poll notices -- traffic the host
    then APPLIES to its healthy world? Zero leakage -> D2 (the world-identity
    gate) stays deferred; any leakage -> D2 re-prioritizes.

    Mechanism: host runs with VOTVCOOP_WIRE_CENSUS=1 (every inbound reliable
    logged individually + 1 Hz aggregated stream counts, each line stamped with
    GetTickCount64() -- machine-global ms, so the two logs align exactly). The
    client runs the menutravel probe in WAIT_SESSION + NO_BYPASS mode: join, dwell
    25 s to settle the join tail, then log 'WIRE-WINDOW transition NOW tick=T' and
    transition('/Game/menu') with the layer live. The verdict censuses the host's
    census lines in [T, T+10s] against a [T-5s, T) baseline.
    """
    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before wirewindow")
    deploy_all()

    log("--- HOST LAUNCH (wire census armed) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb,
                           extra_env={"VOTVCOOP_WIRE_CENSUS": "1",
                                      "VOTVCOOP_VOICE_ENABLED": "0"})
    host_log = HOST_DIR / "multivoid.log"
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

    log("--- CLIENT LAUNCH (menutravel WAIT_SESSION + NO_BYPASS) ---")
    launch_peer("client", args.port, "Client", peer="127.0.0.1",
                res_x=1280, res_y=720, monitor=2, tile_index=0,
                memory_limit_gb=args.memory_limit_gb,
                extra_env={"VOTVCOOP_RUN_MENUTRAVEL_PROBE": "1",
                           "VOTVCOOP_MENUTRAVEL_NO_BYPASS": "1",
                           "VOTVCOOP_MENUTRAVEL_WAIT_SESSION": "1",
                           "VOTVCOOP_VOICE_ENABLED": "0"})
    client_log = CLIENT_DIR / "multivoid.log"
    if not _wait_for_log(client_log, "Joined ", args.client_boot_timeout, "CLIENT"):
        log("FAIL: client never reached the world"); tail_log(client_log, 30, "CLIENT")
        kill_all(); sys.exit(1)

    # The probe dwells 25 s after the session reads live, then logs the marker.
    if not _wait_for_log(client_log, "WIRE-WINDOW transition NOW", 120, "CLIENT"):
        log("FAIL: transition marker never appeared"); tail_log(client_log, 40, "CLIENT")
        kill_all(); sys.exit(1)

    # Let the window elapse and the 1 Hz census flush land it on disk, then read
    # the logs BEFORE killing (a killed process discards its buffered INFO tail).
    time.sleep(20)
    host_text = host_log.read_text(encoding="utf-8", errors="replace")
    client_text = client_log.read_text(encoding="utf-8", errors="replace")
    kill_all()

    m = re.search(r"WIRE-WINDOW transition NOW tick=(\d+)", client_text)
    if not m:
        log("FAIL: marker line present per tail but tick unparseable"); sys.exit(1)
    t0 = int(m.group(1))
    win_end, base_start = t0 + 10000, t0 - 5000
    log(f"--- WIRE-WINDOW CENSUS: transition tick={t0}; "
        f"baseline=[-5s,0) window=[0,+10s] ---")
    base_rel, win_rel, base_str, win_str = [], [], [], []
    for line in host_text.splitlines():
        cm = re.search(r"wire_census: tick=(\d+) slot=(\d+) (RELIABLE kind|STREAM type)=(\d+)(?: n=(\d+))?", line)
        if not cm:
            continue
        tick = int(cm.group(1))
        rec = (tick - t0, int(cm.group(2)), int(cm.group(4)), int(cm.group(5) or 1))
        is_rel = cm.group(3) == "RELIABLE kind"
        if base_start <= tick < t0:
            (base_rel if is_rel else base_str).append(rec)
        elif t0 <= tick <= win_end + 1000:  # +1s: a flush at T+11s covers second T+10
            (win_rel if is_rel else win_str).append(rec)
    log(f"BASELINE reliables ({len(base_rel)}):")
    for dt, slot, kind, _ in base_rel:
        log(f"  {dt:+6d}ms slot={slot} kind={kind}")
    log(f"BASELINE streams (flushes: {len(base_str)}):")
    for dt, slot, typ, n in base_str:
        log(f"  {dt:+6d}ms slot={slot} type={typ} n={n}")
    log(f"WINDOW reliables ({len(win_rel)}):  <-- the D2 leakage question")
    for dt, slot, kind, _ in win_rel:
        log(f"  {dt:+6d}ms slot={slot} kind={kind}")
    log(f"WINDOW streams (flushes: {len(win_str)}):")
    for dt, slot, typ, n in win_str:
        log(f"  {dt:+6d}ms slot={slot} type={typ} n={n}")
    log("Decode kinds/types against include/coop/net/protocol.h. Verdict: any WINDOW "
        "reliable that is a world-mutating op (destroy/spawn/prop op) from the quitting "
        "client = leakage -> D2 re-prioritizes; handshake/leave kinds are benign.")
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

    host_log = HOST_DIR / "multivoid.log"
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


def cmd_navprobe(args) -> None:
    """SOLO Phase-0 HALT probe for the autonomous bot-director. Launches ONE host with
    VOTVCOOP_RUN_NAV_PROBE=1 (no client). The probe measures Gate A (FindPathToLocation-
    Synchronously returns a traversable path over the baked NavMesh) + Gate B (a reflected
    AddMovementInput -- resolved on the Pawn declaring class -- moves the possessed body),
    then logs 'nav_probe: VERDICT ... -> rung{0,1,2}'. Nothing of the director is built
    until this runs. No screenshot -- the verdict is entirely in the log tail."""
    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before navprobe")
    deploy_all()

    os.environ["VOTVCOOP_RUN_NAV_PROBE"] = "1"

    log("--- HOST LAUNCH (solo nav HALT probe -- director Phase-0) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb)

    host_log = HOST_DIR / "multivoid.log"
    if _wait_for_log(host_log, "nav_probe: VERDICT", args.probe_timeout, "HOST"):
        log("nav_probe VERDICT reached -- surfacing the gate lines:")
    else:
        log("WARN: never saw 'nav_probe: VERDICT' -- check the host log tail below")
    time.sleep(2)
    tail_log(host_log, 30, "HOST")
    log("--- KILLING ---")
    kill_all()
    sys.exit(0)


def _race_last_field(log_path, needle: str, field: str):
    """Return int value of `field=N` from the LAST line containing `needle` in log_path, or None."""
    try:
        txt = log_path.read_text(errors="ignore")
    except Exception:
        return None
    hit = None
    for line in txt.splitlines():
        if needle in line:
            hit = line
    if hit is None:
        return None
    m = re.search(re.escape(field) + r"=(-?\d+)", hit)
    return int(m.group(1)) if m else None


def cmd_ctakerace(args) -> None:
    """Director Phase-2 the two-peer CONTAINER concurrent-take RACE. Launches host + client, both
    running the race scenario; both walk to the SAME container (deterministic shared world-pos target)
    and take the SAME item at an orchestrator GO barrier (a future-timestamp sentinel file -> sub-ms
    simultaneity on one box). Each peer counts X locally after; THIS orchestrator SUMS across peers:
    sum 1 = correct (one owner), 2 = DUP (R11b refusal-dup CONFIRMED), 0 = X VANISHED (loss bug),
    >2 = worse. Mode 'control' (only host takes) must sum to 1 BEFORE 'race' is trusted (else sum==1 on
    a race can't tell 'correct' from 'client-walk blind')."""
    mode = getattr(args, "mode", "control")
    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before ctakerace")
    deploy_all()

    go_file = Path(os.environ.get("TEMP") or os.environ.get("TMP") or str(ROOT)) / "multivoid_race_go.txt"
    try: go_file.unlink()
    except Exception: pass

    taker = getattr(args, "taker", "host")
    os.environ["VOTVCOOP_RUN_CTAKE_RACE"] = "1"
    os.environ["VOTVCOOP_RACE_MODE"] = mode
    os.environ["VOTVCOOP_RACE_TAKER"] = taker
    os.environ["VOTVCOOP_RACE_GO_FILE"] = str(go_file)
    log(f"--- CTAKE RACE (mode={mode}{', taker='+taker if mode=='control' else ''}) GO sentinel={go_file} ---")

    log("--- HOST LAUNCH (race, role=host) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None, res_x=args.res_x, res_y=args.res_y,
                           monitor=1, center=True, memory_limit_gb=args.memory_limit_gb,
                           extra_env={"VOTVCOOP_RACE_ROLE": "host"})
    bound = False
    for i in range(args.boot_timeout):
        time.sleep(1)
        if host_owns_udp(host_pid, args.port): bound = True; break
    if not bound:
        log("FAIL: host did not bind UDP"); tail_log(HOST_DIR / "multivoid.log", 30, "HOST"); kill_all(); sys.exit(1)

    log("--- CLIENT LAUNCH (race, role=client) ---")
    launch_peer("client", args.port, "Client", peer="127.0.0.1", res_x=1280, res_y=720,
                monitor=2, tile_index=0, memory_limit_gb=args.memory_limit_gb,
                extra_env={"VOTVCOOP_RACE_ROLE": "client"})

    host_log = HOST_DIR / "multivoid.log"
    client_log = CLIENT_DIR / "multivoid.log"
    ARRIVED = "director/ctake-race: ARRIVED"
    RESULT = "director/ctake-race: RESULT"

    # Wait for BOTH peers to log ARRIVED, then drop the GO sentinel (a future timestamp ~1.5s out).
    log(f"waiting up to {args.probe_timeout}s for BOTH peers to ARRIVE...")
    both = False
    for _ in range(args.probe_timeout):
        time.sleep(1)
        h = host_log.exists() and ARRIVED in host_log.read_text(errors="ignore")
        c = client_log.exists() and ARRIVED in client_log.read_text(errors="ignore")
        if h and c: both = True; break
    if not both:
        log("FAIL: both peers did not ARRIVE (world/join/walk). Tails:")
        tail_log(host_log, 25, "HOST"); tail_log(client_log, 25, "CLIENT"); kill_all(); sys.exit(1)

    # Validate BOTH peers picked the SAME save-key (the by-construction shared target). A divergence
    # means the key is not actually stable cross-peer (or the per-peer feasibility filter split them).
    def _last_str(log_path, needle, field):
        try: txt = log_path.read_text(errors="ignore")
        except Exception: return None
        hit = None
        for ln in txt.splitlines():
            if needle in ln: hit = ln
        if hit is None: return None
        m = re.search(re.escape(field) + r"=(\S+)", hit)
        return m.group(1) if m else None
    hk = _last_str(host_log, ARRIVED, "key")
    ck = _last_str(client_log, ARRIVED, "key")
    if hk != ck:
        log(f"!! SHARED-TARGET MISMATCH: host key={hk} != client key={ck} -- the peers did NOT pick the "
            f"same container; the race would be meaningless. ABORT.")
        tail_log(host_log, 15, "HOST"); tail_log(client_log, 15, "CLIENT"); kill_all(); sys.exit(1)
    log(f"shared-target key MATCH on both peers: {hk}")

    go_ms = int(time.time() * 1000) + 1500   # GO instant ~1.5s in the future (both busy-wait to it)
    go_file.write_text(str(go_ms))
    log(f"both ARRIVED -- GO sentinel written (fire at Unix-ms {go_ms}, ~1.5s out)")

    # Wait for BOTH peers' RESULT, then SUM localCountAfter across peers.
    log("waiting for BOTH peers' RESULT...")
    hc = cc = None
    for _ in range(90):
        time.sleep(1)
        hc = _race_last_field(host_log, RESULT, "localCountAfter")
        cc = _race_last_field(client_log, RESULT, "localCountAfter")
        if hc is not None and cc is not None: break
    log("--- RESULT lines ---")
    for lg, lab in ((host_log, "HOST"), (client_log, "CLIENT")):
        try:
            for line in lg.read_text(errors="ignore").splitlines():
                if "director/ctake-race:" in line and ("RESULT" in line or "SHARED target" in line or "DUP-VERIFIER" in line):
                    log(f"  {lab}: {line.strip()[-200:]}")
        except Exception: pass

    if hc is None or cc is None:
        log(f"INCONCLUSIVE: missing RESULT (host={hc} client={cc})")
    else:
        total = hc + cc
        verdict = {0: "VANISHED (X lost -- loss bug, NOT ok)", 1: "CORRECT (exactly one owner)",
                   2: "DUP (R11b refusal-dup CONFIRMED)"}.get(total, "WORSE (>2 -- serious)")
        log(f"=== CTAKE RACE ({mode}) SUM: host={hc} + client={cc} = {total} -> {verdict} ===")
        if mode == "control" and total != 1:
            log("!! CONTROL FAILED: solo sum must be 1 -- the summation instrument is NOT race-ready "
                "(client-walk blind / double-count). FIX before trusting a real race.")
        elif mode == "control":
            log("CONTROL PASS: solo sum == 1 -- the cross-peer summation is validated; the race is trustable.")

    # PROJECTION WATCH (2026-07-24): does saveObjects refresh saveSlot.inventoryData on a peer whose
    # world-save is BLOCKED at SaveGameToSlot? The scenario samples inv/eq/hold every 20s x20 (~6.3 min)
    # so the window spans at least one autosave. Peers must stay ALIVE for it -- hence the hold.
    # HOST arm = the known-positive (its saves are not blocked); the CLIENT arm is interpretable ONLY
    # if the HOST arm's contentHash CHANGES. Absence-of-a-log-line was rejected as the instrument:
    # it fuses "never refreshed" with "refreshed but this record does not go there".
    hold = int(getattr(args, "hold_seconds", 0) or 0)
    if hold > 0:
        log(f"--- PROJECTION WATCH: holding peers {hold}s for the inventoryData sampling ---")
        deadline = time.time() + hold
        while time.time() < deadline:
            time.sleep(10)
            done_h = "director/projwatch: role=host DONE" in host_log.read_text(errors="ignore")
            done_c = "director/projwatch: role=client DONE" in client_log.read_text(errors="ignore")
            if done_h and done_c:
                log("both peers finished the projection watch"); break
        for lg, lab in ((host_log, "HOST"), (client_log, "CLIENT")):
            try:
                rows = [l.strip() for l in lg.read_text(errors="ignore").splitlines()
                        if "director/projwatch:" in l or "save_block: BLOCKED" in l]
            except Exception:
                rows = []
            log(f"--- {lab} projwatch ({len(rows)} lines) ---")
            for r in rows: log(f"  {lab}: {r[-190:]}")

    log("--- KILLING ---"); kill_all(); sys.exit(0)


def cmd_ctakeprobe(args) -> None:
    """SOLO director Phase-2 HALT gate: the CONTAINER-TAKE input probe. Launches ONE host with
    VOTVCOOP_RUN_CTAKE_PROBE=1 (no client). The bot walks to a placed non-empty world container
    and drives the FAITHFUL human take chain (openContainer -> slot pressButton -> em_take),
    MEASURING whether the take executed (the container's GObjStack item count decremented);
    extract(0) is a NON-FAITHFUL diagnostic fallback. Verdict ('director/ctake: VERDICT
    DRIVABLE-FAITHFUL / DRIVABLE-EFFECT-SEAM-ONLY / NOT-DRIVABLE') is in the log tail -- it decides
    whether the container concurrent-take race is buildable. PRECONDITION: the fresh save must have
    an item inside a world container within ~60m of spawn."""
    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before ctakeprobe")
    deploy_all()

    os.environ["VOTVCOOP_RUN_CTAKE_PROBE"] = "1"

    log("--- HOST LAUNCH (solo container-take input probe -- director Phase-2) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb)

    host_log = HOST_DIR / "multivoid.log"
    if _wait_for_log(host_log, "director/ctake: VERDICT", args.probe_timeout, "HOST"):
        log("ctake VERDICT reached -- surfacing the ladder lines:")
    else:
        log("WARN: never saw 'director/ctake: VERDICT' -- check the host log tail below")
    time.sleep(2)
    tail_log(host_log, 40, "HOST")
    log("--- KILLING ---")
    kill_all()
    sys.exit(0)


def cmd_walkgrab(args) -> None:
    """SOLO Phase-1 flagship of the autonomous bot-director: a WALKED GRAB. Launches ONE
    host with VOTVCOOP_RUN_WALKGRAB_TEST=1 (no client). The host bot picks the nearest
    chipPile, WALKS to it over the baked NavMesh (FindPath + per-tick AddMovementInput
    steering + a thin PathExecutor), SETTLES, then runs the proven InpActEvt_use grab.
    Proves the director spine end to end; the verdict ('walkgrab: VERDICT WALK=.. GRAB=..')
    is in the log tail. Cross-peer convert/mirror = a 2-peer smoke (separate step)."""
    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before walkgrab")
    deploy_all()

    os.environ["VOTVCOOP_RUN_DIRECTOR_WALKGRAB"] = "1"

    log("--- HOST LAUNCH (solo bot-director walked grab -- the brain) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb)

    host_log = HOST_DIR / "multivoid.log"
    if _wait_for_log(host_log, "director: VERDICT", args.probe_timeout, "HOST"):
        log("director VERDICT reached -- surfacing the brain/walk/grab lines:")
    else:
        log("WARN: never saw 'director: VERDICT' -- check the host log tail below")
    time.sleep(2)
    tail_log(host_log, 30, "HOST")
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

    host_log = HOST_DIR / "multivoid.log"
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


def cmd_nativeui(args) -> None:
    """SOLO MENU-TIME probe for the native server browser (docs/MULTIPLAYER_UI.md
    section 8, P1). Launches ONE instance in `menu` scenario -- VOTV's own main menu,
    no save, no session -- with `native_ui_probe` armed and, unless --no-write is
    passed, `native_ui_probe_write` too.

    It asserts on the LOG rather than on a screenshot, because the design's own rule is
    that an instrument nobody has seen fail passes by construction: every question the
    probe answers prints a line with a machine-readable prefix, and this command fails
    if a line is ABSENT as loudly as if it is wrong.

      RUNG 0  frames presented per WorldKind -- 'unknown' is the window UMG cannot draw
              in, i.e. whether the ~3,700 LOC ImGui overlay substrate is retirable.
      O1      the UMG class/function resolve census (a MISS is fatal to that feature).
      O5      is the donor brush's FSlateResourceHandle populated (does P0 arm?).
      O7      donor residency at menu time.
      O8      UButton::OnClicked layout, read-only off the game's own bound button.
      A5      the switcher child map -- which sub-screen sits at which index.
      RUNG 1  does a hand-wired UUserWidget render inside the live UWidgetSwitcher.
    """
    shots_dir = Path(__file__).resolve().parent.parent / "research" / "nativeui_shots"
    shots_dir.mkdir(parents=True, exist_ok=True)
    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before nativeui")
    deploy_all()

    env = {
        "VOTVCOOP_NATIVE_UI_PROBE": "1",
        # The content-warning screen is itself a switcher child; advance past it so the
        # probe settles on the real main menu.
        "VOTVCOOP_MENU_PROCEED": "1",
    }
    scenario = "menu"
    set_role = False
    if args.travel:
        # O4 mode. The menu-only run cannot answer "does the game present frames during a
        # LEVEL TRANSITION" because it never has one. This rides the existing menutravel
        # probe, which boots into gameplay (travel #1: the boot load) and then calls
        # AmainGamemode_C::transition("/Game/menu") (travel #2: quit-to-menu) -- two real
        # transitions in one process, which is the sample RUNG 0 needs.
        #
        # NO_BYPASS=1 is not optional here. The default menutravel arms a 300 s TRANSPARENT
        # BYPASS that puts our ProcessEvent detour dormant -- and GT::Post tasks drain
        # inside that detour, so with the bypass on, the world memo would never refresh
        # again and every frame after the transition would count as stale. The no-bypass
        # path is also the honest one: it is the shape of a player's own in-game exit.
        env["VOTVCOOP_RUN_MENUTRAVEL_PROBE"] = "1"
        env["VOTVCOOP_MENUTRAVEL_NO_BYPASS"] = "1"
        env["VOTVCOOP_MENUTRAVEL_MENU_S"] = "20"
        scenario = "play"
        args.no_write = True  # never write into the switcher during a travel run
        # SpawnEnvGatedTests -- which is what actually starts the menutravel worker -- sits
        # INSIDE harness.cpp's `if (netEnabled)` branch, so a launch with no net role never
        # spawns it. The first travel attempt lost 3.5 minutes to exactly that: two
        # WorldKind edges, zero `menutravel:` lines, and a FAIL that read as "the
        # transition hung".
        set_role = True
        log("--- O4 TRAVEL MODE: boot -> gameplay -> transition(/Game/menu), no bypass ---")
    if not args.no_write:
        env["VOTVCOOP_NATIVE_UI_PROBE_WRITE"] = "1"

    log(f"--- HOST LAUNCH (solo native-UI probe, {scenario.upper()} scenario) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb,
                           set_net_role=set_role, set_scenario=scenario, extra_env=env)
    host_log = HOST_DIR / "multivoid.log"

    t0 = time.time()
    shot: Path | None = None
    saw_stage_a = False
    while time.time() - t0 < args.duration:
        time.sleep(3)
        if not list_votv():
            log("  (no VotV process -- exited/crashed)")
            break
        try:
            text = host_log.read_text(errors="ignore")
        except Exception:
            text = ""
        if not saw_stage_a and "STAGE A done" in text:
            saw_stage_a = True
            log(f"  t+{int(time.time()-t0)}s STAGE A complete")
        if shot is None and "RUNG1 HOLD BEGIN" in text and "RUNG1 restore" not in text:
            p = shots_dir / "nativeui_rung1.png"
            if _capture_window(host_pid, p):
                shot = p
                log(f"  rung-1 shot: {p.name}")
    lines, all_lines = [], []
    try:
        all_lines = host_log.read_text(errors="ignore").splitlines()
        lines = [ln for ln in all_lines if "[native_ui_probe]" in ln]
    except Exception:
        pass
    log("--- KILLING ---")
    kill_all()

    log("--- NATIVE-UI PROBE OUTPUT ---")
    for ln in lines:
        print(ln)

    def find(needle: str) -> str | None:
        for ln in lines:
            if needle in ln:
                return ln
        return None

    def find_any(needle: str) -> str | None:
        # `lines` is pre-filtered to our own tag, so anything logged by ANOTHER subsystem
        # is structurally invisible to find(). The travel assert searched `lines` for
        # "menutravel: MENU-SHOT READY" and could never have matched -- it reported the
        # transition as hung through two runs in which the transition worked.
        for ln in all_lines:
            if needle in ln:
                return ln
        return None

    log("--- NATIVEUI VERDICT ---")
    fails = []
    if args.travel:
        # In travel mode the whole point is the EDGE lines: a run with no kind change never
        # travelled, and its RUNG0 totals would be a menu-only sample wearing an O4 label.
        edges = [ln for ln in lines if "RUNG0 EDGE" in ln]
        for ln in edges:
            log(f"EDGE: {ln.strip()}")
        if len(edges) < 2:
            fails.append(f"only {len(edges)} WorldKind edge(s) -- the run did not travel twice; "
                         "RUNG0 has no transition in its sample")
        if not find_any("menutravel: MENU-SHOT READY"):
            fails.append("menutravel never reached the menu (transition failed / hung)")
        boot = [ln for ln in edges if "Unknown -> " in ln]
        if not boot:
            fails.append("no Unknown -> * edge: the boot window was never bounded")
        summary = [ln for ln in lines if "RUNG0 periodic" in ln]
        if summary:
            log(f"FINAL: {summary[-1].strip()}")
        if not summary:
            fails.append("no RUNG0 report at all")
        for f in fails:
            log(f"FAIL: {f}")
        if not fails:
            log("ALL PASS -- two travels in the sample; read the EDGE lines above.")
        sys.exit(0 if not fails else 2)
    if not find("STAGE A done"):
        fails.append("STAGE A never ran (no ui_menu_C tick? read the tail)")
    o1 = find("O1 SUMMARY")
    if not o1:
        fails.append("no O1 SUMMARY line")
    elif ", 0 MISSING" not in o1:
        log(f"NOTE: O1 has misses -- {o1.strip()}")
    # O5 prints VERDICT or INCONCLUSIVE -- an inconclusive donor is a real answer about
    # the donor, not a missing line, and the assert must not confuse the two.
    if not (find("O5 VERDICT") or find("O5 INCONCLUSIVE")):
        fails.append("no O5 verdict line at all (donor button absent?)")
    if not find("O5 VERDICT (ui_saveSlots_C.button_back"):
        fails.append("O5 never reached the art-bearing donor (ui_saveSlots_C.button_back)")
    if not find("A5: switcher_widgets"):
        fails.append("no A5 switcher line")
    # Every donor section 8 names must be RESIDENT. A NULL donor is not a soft note: the
    # styling rule is fail-closed, so a null donor means the browser retries forever.
    nulls = [ln for ln in lines if "O7[" in ln and "NULL --" in ln]
    if nulls:
        for ln in nulls:
            log(f"NOTE: donor NULL -- {ln.strip()}")
        fails.append(f"{len(nulls)} style donor(s) NULL at menu time -- fail-closed retry would spin")
    if not find("O8 "):
        fails.append("no O8 delegate line")
    if not find("RUNG0 "):
        fails.append("no RUNG0 report -- NoteFrame never ran (overlay Present hook down?)")
    if args.no_write:
        log("rung 1 skipped (--no-write)")
    else:
        r1 = find("RUNG1 VERDICT")
        if not r1:
            fails.append("no RUNG1 VERDICT line")
        # A hold that began must have been torn down -- the probe must never leave the
        # switcher pointing at a throwaway.
        if find("RUNG1 HOLD BEGIN") and not (find("RUNG1 restore") or find("RUNG1 ABORTED")):
            fails.append("RUNG1 held but never restored -- THE SWITCHER WAS LEFT ON OUR INDEX")
        # RUNG 2 -- the two questions that GATE the browser's ~520 LOC. Both verdicts are
        # three-valued in the DLL, and this asserts the LINE EXISTS rather than that it is
        # positive: an INCONCLUSIVE hover verdict is a real answer ("do not build on it"),
        # and turning it into a runner FAIL would hide which of the two it was.
        hov = find("RUNG2 HOVER VERDICT")
        if not hov:
            fails.append("no RUNG2 HOVER VERDICT line -- the hit-test question is UNANSWERED")
        else:
            log(f"HOVER: {hov.strip()}")
            if "INCONCLUSIVE" in hov:
                log("NOTE: hover verdict INCONCLUSIVE -- foreground or an unrun phase, not a "
                    "negative. Re-run with the window focused before concluding anything.")
            elif "ALWAYS-TRUE" in hov or "NEVER TRUE" in hov:
                fails.append("RUNG2 hover says IsHovered() does not discriminate -- section 8's "
                             "row hit-test design is FALSIFIED, do not write P2")
        gc = find("RUNG2 GC VERDICT")
        if not gc:
            fails.append("no RUNG2 GC VERDICT line -- subtree survival across a purge is UNMEASURED")
        else:
            log(f"GC: {gc.strip()}")
            if "DID NOT SURVIVE" in gc:
                fails.append("RUNG2 GC says the hand-built subtree did NOT survive a forced purge "
                             "-- the pool-in-the-panel design is FALSIFIED, do not write P2")
    if shot:
        log(f"screenshot: {shot}")
    for f in fails:
        log(f"FAIL: {f}")
    if not fails:
        log("ALL PASS -- every probe question produced a line; read them above.")
    sys.exit(0 if not fails else 2)


def _start_fake_master(count: int):
    """Start tools/fake_master.py on a free port and return (proc, url).

    THE SCHEME IS NOT OPTIONAL. The master URL grammar is SCHEMELESS = SECURE
    (http_client.cpp:50-72): a bare host:port means TLS, and the fixture serves
    plaintext, so it MUST be addressed as http://127.0.0.1:<port> with the port
    spelled out -- the client refuses a URL without one.

    We wait for the fixture to actually ANSWER before returning. A game that
    starts against a socket nobody is listening on would fetch nothing, the
    list would stay empty, and T0's row wait would time out reporting "the
    content does not overflow" -- a true statement about the wrong cause.
    """
    import socket
    import urllib.request
    with socket.socket() as sk:
        sk.bind(("127.0.0.1", 0))
        port = sk.getsockname()[1]
    script = ROOT / "tools" / "fake_master.py"
    proc = subprocess.Popen([sys.executable, str(script), "--port", str(port),
                             "--count", str(count)],
                            stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            creationflags=DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP)
    url = f"http://127.0.0.1:{port}"
    for _ in range(40):
        try:
            with urllib.request.urlopen(f"{url}/v1/lobbies", timeout=1) as r:
                if r.status == 200:
                    log(f"fake_master: serving {count} lobbies at {url}")
                    return proc, url
        except Exception:
            time.sleep(0.25)
    proc.kill()
    raise SystemExit(f"fake_master did not answer on {url} -- T0 cannot run without rows")


def cmd_browser(args) -> None:
    """SOLO MENU-TIME lab run for the NATIVE server browser (docs/MULTIPLAYER_UI.md
    section 8, P2). Launches ONE instance in the `menu` scenario with [dev]
    browser_native=1 + browser_autoopen=1, waits for the screen to be shown, captures
    the window, and asserts on the LOG.

    IT ASSERTS ON LINES, and it fails on an ABSENT line as loudly as on a wrong one --
    the same rule the nativeui probe follows, for the same reason: an instrument nobody
    has seen fail passes by construction.
    """
    shots_dir = Path(__file__).resolve().parent.parent / "research" / "browser_shots"
    shots_dir.mkdir(parents=True, exist_ok=True)
    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before browser")
    deploy_all()

    env = {
        "VOTVCOOP_BROWSER_NATIVE": "1",
        "VOTVCOOP_BROWSER_AUTOOPEN": "1",
        # The content-warning screen is itself a switcher child; advance past it so the
        # browser is built against the real main menu.
        "VOTVCOOP_MENU_PROCEED": "1",
    }
    fake = None
    if args.fake_master > 0:
        fake, url = _start_fake_master(args.fake_master)
        # VOTVCOOP_MASTER_URL beats every other config layer (config.cpp:481).
        env["VOTVCOOP_MASTER_URL"] = url
    log("--- HOST LAUNCH (solo native-browser lab, MENU scenario) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb,
                           set_net_role=False, set_scenario="menu", extra_env=env)
    host_log = HOST_DIR / "multivoid.log"

    t0 = time.time()
    shot = None
    saw_shown = False
    extra_shots: dict = {}
    while time.time() - t0 < args.duration:
        time.sleep(3)
        if not list_votv():
            log("  (no VotV process -- exited/crashed)")
            break
        try:
            text = host_log.read_text(errors="ignore")
        except Exception:
            text = ""
        if not saw_shown and "server_browser_native: shown" in text:
            saw_shown = True
            log(f"  t+{int(time.time()-t0)}s browser shown -- capturing")
            time.sleep(2)   # let Slate lay the screen out and present a frame with it
            p = shots_dir / "browser_native.png"
            if _capture_window(host_pid, p):
                shot = p
                log(f"  shot: {p.name}")
        # T0 corroboration. The verdict is the LOG; these two shots exist so a human can
        # see that a list the getter calls scrolled looks scrolled. Each verdict phase
        # holds its offset for 6 s, which covers this 3 s poll.
        for needle, name, seen in (("SCROLL CONTROL ", "browser_scroll_forced.png", "ctl"),
                                   ("WHEEL VERDICT", "browser_scroll_wheel.png", "whl")):
            if needle in text and seen not in extra_shots:
                extra_shots[seen] = shots_dir / name
                if _capture_window(host_pid, extra_shots[seen]):
                    log(f"  shot: {name}")
                else:
                    extra_shots[seen] = None
    all_lines = []
    try:
        all_lines = host_log.read_text(errors="ignore").splitlines()
    except Exception:
        pass
    lines = [ln for ln in all_lines if "server_browser_native" in ln]
    log("--- KILLING ---")
    kill_all()
    if fake is not None:
        fake.kill()

    log("--- NATIVE BROWSER OUTPUT ---")
    for ln in lines:
        print(ln)

    def find(needle: str):
        for ln in lines:
            if needle in ln:
                return ln
        return None

    log("--- BROWSER VERDICT ---")
    fails = []
    if not find("screen built"):
        fails.append("the screen was never BUILT -- a donor was missing (read the O7 rule: "
                     "fail-closed means retry, and after 15 tries the user is told)")
    if find("donors still absent"):
        fails.append("donors never appeared -- boot_warning_dialog was armed")
    if not find("shown ("):
        fails.append("the screen was built but never SHOWN (autoopen intent not consumed?)")
    if find("open intent EXPIRED"):
        fails.append("the open intent expired without a main-menu tick")
    if find("the switcher moved off our index"):
        log("NOTE: the switcher moved off our index during the run (reconcile fired)")
    # T1: the chrome. A button nobody has seen work is not a way out, so this fails on an
    # ABSENT verdict exactly as loudly as on a wrong one.
    if find("SELFTEST DISARMED"):
        fails.append("the selftest DISARMED itself -- every verdict below is absent because "
                     "the probe gave up, not because the feature is missing")
    close = find("CLOSE BUTTON ")
    if not close:
        fails.append("no CLOSE BUTTON verdict -- whether the X actually closes the screen "
                     "is UNMEASURED, which is the state T1 exists to leave behind")
    else:
        log(f"CLOSE BUTTON: {close.strip()}")
        if "FAIL" in close or "SKIP" in close:
            fails.append("the X did not close the screen -- read the verdict, it "
                         "distinguishes a bad cursor estimate from a dead button")
    esc = find("hidden (ESC;")
    if not find("ESC SELFTEST"):
        fails.append("the ESC selftest never ran -- whether the screen can be CLOSED is UNMEASURED")
    elif not esc:
        fails.append("ESC did not close the screen -- with no chrome yet, that STRANDS the player "
                     "at the menu with no way out")
    else:
        log(f"ESC: {esc.strip()}")
    scrim = find("SCRIM SELFTEST")
    if not scrim:
        fails.append("no SCRIM SELFTEST verdict -- whether a stray click is absorbed is UNMEASURED")
    else:
        log(f"SCRIM: {scrim.strip()}")
        if "FAIL" in scrim:
            fails.append("the scrim does not cover the screen: a click that misses the window "
                         "reaches VOTV's own menu buttons underneath")
    # ---- T0: does the wheel scroll this widget at all -------------------------------
    # ASSERTS ON ABSENCE AS LOUDLY AS ON A WRONG ANSWER. The whole point of T0 is that
    # nobody had ever asked; a run that silently fails to ask is the same non-answer.
    if args.fake_master > 0:
        fields = find("scroll fields --")
        if fields:
            log(f"SCROLL FIELDS: {fields.strip()}")
        ctl = find("SCROLL CONTROL ")
        if not ctl:
            fails.append("no SCROLL CONTROL verdict -- the positive control never ran, so "
                         "whether this widget scrolls AT ALL is still unmeasured")
        else:
            log(f"CONTROL: {ctl.strip()}")
            if "SKIP" in ctl:
                fails.append("the scroll control SKIPPED -- with no overflow there is "
                             "nothing to scroll and T0 was not asked (is the fixture "
                             "serving enough rows?)")
            elif "FAIL" in ctl:
                fails.append("the scroll control FAILED -- either the box does not scroll "
                             "or GetScrollOffset echoes the request. Read the line: it "
                             "says which, and both invalidate the wheel result")
        whl = find("WHEEL VERDICT")
        if not whl:
            fails.append("no WHEEL VERDICT -- T0's actual question was never answered")
        else:
            log(f"WHEEL: {whl.strip()}")
            if "NO" in whl or "SKIPPED" in whl:
                fails.append("THE WHEEL DOES NOT SCROLL THIS WIDGET. Three steps of "
                             "section 8c.-1 (T2b, T4a, T6) presuppose that it does, and "
                             "NO step prices making it scroll -- read the T0 row before "
                             "continuing the lane")
        for key, label in (("ctl", "forced-offset"), ("whl", "post-wheel")):
            if extra_shots.get(key):
                log(f"screenshot ({label}): {extra_shots[key]}")
    errs = [ln for ln in all_lines if "[Error]" in ln and "server_browser_native" in ln]
    for ln in errs:
        # The T0 verdict lines are Errors BY DESIGN when they report a negative; they are
        # already assessed above, so echoing them here would double-count a known result.
        if ("SCROLL CONTROL" in ln or "WHEEL VERDICT" in ln or "CLOSE BUTTON" in ln):
            continue
        log(f"ERROR LINE: {ln.strip()}")
    if shot:
        log(f"screenshot: {shot}")
    else:
        fails.append("no window capture -- nothing to look at")
    for f in fails:
        log(f"FAIL: {f}")
    if not fails:
        log("ALL PASS -- built, shown, captured. LOOK AT THE SHOT: a log line is not a layout.")
    sys.exit(0 if not fails else 2)


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
    host_log = HOST_DIR / "multivoid.log"
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
    host_log = HOST_DIR / "multivoid.log"
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
    client_log = CLIENT_DIR / "multivoid.log"

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


def cmd_chathistory(args) -> None:
    """D-L: the LOCAL half of chat history (2 peers).

    Ten messages are typed through the real chat bar, then the run WAITS for every
    one of them to leave the live set -- which is the point: what is on screen after
    that is nothing, and what the T-reveal then shows is the RETAINED tier. The host
    holds chat open while the client keeps talking, so the two halves of the user's
    rule are exercised together: history appears, and new messages still arrive and
    move the view while it is up.

    Three things are asserted from the host's own log, and each one is a defect this
    design nearly shipped:
      H1 the reveal reports a non-empty history        (the tier exists at all)
      H2 nothing expires between the open and close markers  (the suspended TTL)
      H3 the client's mid-read message reaches the feed AFTER the reveal opened
         and is not retired inside the window          (the underflow that popped
                                                        every new message one tick
                                                        after it arrived)
      H4 PgUp pins the view and PgDn releases it       (only meaningful when the
                                                        history OVERFLOWS one
                                                        viewport -- see --messages)

    MUST-FAIL control: rerun with --inject, which sets VOTVCOOP_CHAT_NO_RETAIN=1 on
    the host so Retire destroys instead of retaining. H1 must go RED. A drill that
    has only ever been shown passing passes by construction.

    voice.enabled is forced OFF on both peers: hud::IsActive() ends in
    voice_chat::Enabled(), whose registry default is TRUE, so with voice on the
    overlay frame is unconditionally alive and the gate under test is unreachable.
    """
    shots_dir = Path(__file__).resolve().parent.parent / "research" / "chat_shots"
    shots_dir.mkdir(parents=True, exist_ok=True)
    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before chathistory")
    deploy_all()

    host_env = {"VOTVCOOP_VOICE_ENABLED": "0"}
    if getattr(args, "inject", False):
        host_env["VOTVCOOP_CHAT_NO_RETAIN"] = "1"
        log("*** INJECTION ARMED: VOTVCOOP_CHAT_NO_RETAIN=1 on the HOST -- H1 MUST go RED ***")

    log("--- HOST LAUNCH (chat-history D-L) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb, extra_env=host_env)
    host_log = HOST_DIR / "multivoid.log"
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
                             memory_limit_gb=args.memory_limit_gb,
                             extra_env={"VOTVCOOP_VOICE_ENABLED": "0"})
    client_log = CLIENT_DIR / "multivoid.log"
    # Gate on being IN-WORLD, not on having connected: `T` is swallowed while any
    # interactive surface owns input and the loading screen is one of them.
    if not _wait_for_log(client_log, "Joined ", args.client_boot_timeout, "CLIENT"):
        log("FAIL: client never reached the world"); tail_log(client_log, 30, "CLIENT")
        kill_all(); sys.exit(1)
    time.sleep(4)

    # --- N messages, alternating peers. The default is deliberately MORE than one
    # viewport of rows: at the drill's window size the reveal fits ~18, and with only
    # a dozen lines PgUp correctly does nothing -- which is indistinguishable from a
    # broken pager. H4 needs the history to overflow the viewport to mean anything.
    msgs = [f"line {n:02d} from {'host' if n % 2 else 'client'}"
            for n in range(1, args.messages + 1)]
    log(f"--- TYPING {len(msgs)} messages (alternating peers) ---")
    for n, m in enumerate(msgs):
        pid, lbl = (host_pid, "HOST") if (n % 2 == 0) else (client_pid, "CLIENT")
        log(f"  {lbl}: {m!r}")
        _type_chat(pid, m, lbl)
        time.sleep(1.0)

    # Settle: every live line must age out of the live set, so what the reveal shows
    # afterwards can only have come from the retained tier. The TTL is 11 s.
    log(f"--- SETTLING {args.settle}s (live lines must all expire) ---")
    time.sleep(args.settle)
    _capture_window(host_pid, shots_dir / "host_settled.png")
    _capture_window(client_pid, shots_dir / "client_settled.png")

    # --- open and HOLD the host's chat.
    log("--- HOST opens chat and HOLDS it ---")
    _type_chat(host_pid, "", "HOST", submit=False)
    time.sleep(1.5)                       # the 220 ms ramp, with margin
    _capture_window(host_pid, shots_dir / "host_history.png")

    # While the host READS, the client keeps talking. This is the user's rule: new
    # messages ALWAYS arrive and move the view.
    live_msg = "arrived while you were reading"
    log(f"--- CLIENT sends {live_msg!r} while the host reads ---")
    _type_chat(client_pid, live_msg, "CLIENT")
    time.sleep(2.0)
    _capture_window(host_pid, shots_dir / "host_history_live.png")

    log("--- HOST pages back (PgUp) then forward (PgDn) ---")
    VK_PRIOR, VK_NEXT = 0x21, 0x22
    _press_vk(host_pid, VK_PRIOR, "HOST"); time.sleep(0.8)
    _capture_window(host_pid, shots_dir / "host_paged_back.png")
    _press_vk(host_pid, VK_NEXT, "HOST"); time.sleep(0.8)

    # Hold well past the TTL: if the clock were still running, every retained row's
    # live sibling would have expired inside this window and H2 would catch it.
    log(f"--- HOLDING the reveal for {args.hold}s (TTL is 11 s) ---")
    time.sleep(args.hold)
    _capture_window(host_pid, shots_dir / "host_history_held.png")

    # Close with Enter on the empty field -- see _type_chat's note on why not Escape.
    log("--- HOST closes chat (Enter on an empty field) ---")
    _press_vk(host_pid, 0x0D, "HOST")
    time.sleep(2.0)
    _capture_window(host_pid, shots_dir / "host_after_close.png")

    tail_log(host_log, 25, "HOST")
    htext, herr = _read_log_strict(host_log)
    log("--- KILLING ---")
    kill_all()

    # --- verdict.
    log("--- CHAT-HISTORY (D-L) VERDICT ---")
    failures: list[str] = []
    notes: list[str] = []
    if herr:
        failures.append(f"host log is not strict UTF-8: {herr}")

    opens = [ln for ln in htext.splitlines() if "chat_view: reveal open" in ln]
    closes = [ln for ln in htext.splitlines() if "chat_view: reveal closed" in ln]
    if not opens:
        failures.append("H1: the host never logged a reveal -- chat never opened, or "
                        "the overlay frame was not being built")
    else:
        log(f"  {opens[-1].strip()}")
        m = re.search(r"history=(\d+) live=(\d+)", opens[-1])
        hist = int(m.group(1)) if m else -1
        if hist < args.min_history:
            failures.append(f"H1: reveal showed history={hist}, expected >= "
                            f"{args.min_history} retained lines")
        else:
            notes.append(f"H1: {hist} retained history lines were revealed")

    if not closes:
        failures.append("H2: no reveal-close marker -- the assertion window has no end")
    elif opens:
        lines = htext.splitlines()
        oi = max(i for i, ln in enumerate(lines) if "chat_view: reveal open" in ln)
        ci = max(i for i, ln in enumerate(lines) if "chat_view: reveal closed" in ln)
        log(f"  {closes[-1].strip()}")
        if ci < oi:
            failures.append("H2: the close marker precedes the open marker")
        else:
            window = lines[oi + 1:ci]
            expired = [ln for ln in window if "feed: retire" in ln and "via=expire" in ln]
            if expired:
                failures.append(f"H2: {len(expired)} line(s) EXPIRED while the reveal was "
                                f"up -- the TTL clock is not suspended. First: "
                                f"{expired[0].strip()}")
            else:
                notes.append(f"H2: nothing expired across the {len(window)} log line(s) "
                             f"the reveal was up for")
            got = [ln for ln in window if "feed: push via=chat" in ln and live_msg in ln]
            if not got:
                failures.append(f"H3: {live_msg!r} never reached the host's feed while "
                                f"the reveal was up -- new messages do not arrive")
            else:
                notes.append("H3: the mid-read message arrived and stayed")
            pinned = [ln for ln in window if "chat_view: PINNED" in ln]
            follow = [ln for ln in window if "chat_view: FOLLOW" in ln]
            if not pinned:
                failures.append("H4: PgUp did not pin the view -- either the pager is "
                                "dead, or the history did not overflow the viewport "
                                "(raise --messages)")
            elif not follow:
                failures.append("H4: PgDn never returned the view to FOLLOW -- a reader "
                                "who pages forward would stay stuck in history")
            else:
                notes.append("H4: PgUp pinned the view and PgDn released it")

    for n in notes:
        log(f"  OK  {n}")
    for f in failures:
        log(f"  FAIL {f}")
    log(f"shots -> {shots_dir}")
    if getattr(args, "inject", False):
        if failures:
            log("INJECTED RUN: RED as required -- the drill can see the defect it tests for")
            sys.exit(0)
        log("INJECTED RUN: GREEN -- the drill is BLIND; it would pass on a broken build")
        sys.exit(2)
    log("VERDICT: " + ("PASS" if not failures else f"FAIL ({len(failures)})"))
    sys.exit(0 if not failures else 2)


def cmd_chatseed(args) -> None:
    """D-W: the WIRE half of chat history (3 peers) -- the joiner's half.

    Host and client 1 hold a conversation. Then client 2 joins, and WHILE IT IS STILL
    LOADING the other two keep talking. That window is the whole point: it is where a
    seed and a live stream can interleave, and where a dedup rule that was a
    high-watermark instead of a range would have discarded the entire history in
    silence.

    Asserted from client 2's own log:
      W1 it applied a non-empty seed                    (the record reaches a joiner)
      W2 the applied lineSeqs are STRICTLY ASCENDING and CONTIGUOUS
                                                        (no hole, no duplicate, no
                                                         reordering -- the half a
                                                         screenshot cannot show)
      W3 the two lines said DURING its load window are present, and are the NEWEST
                                                        (principle 8: a joiner arriving
                                                         mid-conversation is not a
                                                         supported-later case)
      W4 the seeded rows landed RETAINED, so its live feed on arrival is clear
                                                        (the user's step 4)

    MUST-FAIL control: --inject sets VOTVCOOP_CHAT_SEED_SUPPRESS=1 on the host, which
    opens the joiner for live traffic but never sends it the history. W1 must go RED.
    """
    shots_dir = Path(__file__).resolve().parent.parent / "research" / "chat_shots"
    shots_dir.mkdir(parents=True, exist_ok=True)
    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before chatseed")
    deploy_all()

    host_env = {"VOTVCOOP_VOICE_ENABLED": "0"}
    if getattr(args, "inject", False):
        host_env["VOTVCOOP_CHAT_SEED_SUPPRESS"] = "1"
        log("*** INJECTION ARMED: VOTVCOOP_CHAT_SEED_SUPPRESS=1 on the HOST -- W1 MUST go RED ***")

    log("--- HOST LAUNCH (chat-seed D-W) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb, extra_env=host_env)
    host_log = HOST_DIR / "multivoid.log"
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

    log("--- CLIENT 1 LAUNCH ---")
    c1_pid = launch_peer("client", args.port, "Client", peer="127.0.0.1",
                         res_x=1280, res_y=720, peer_slot=1, monitor=2, tile_index=0,
                         memory_limit_gb=args.memory_limit_gb,
                         extra_env={"VOTVCOOP_VOICE_ENABLED": "0"})
    c1_log = CLIENT_DIR / "multivoid.log"
    if not _wait_for_log(c1_log, "Joined ", args.client_boot_timeout, "CLIENT1"):
        log("FAIL: client 1 never reached the world"); kill_all(); sys.exit(1)
    time.sleep(4)

    early = [f"before you arrived {n:02d}" for n in range(1, args.messages + 1)]
    log(f"--- {len(early)} messages BEFORE the joiner exists ---")
    for n, m in enumerate(early):
        pid, lbl = (host_pid, "HOST") if (n % 2 == 0) else (c1_pid, "CLIENT1")
        log(f"  {lbl}: {m!r}")
        _type_chat(pid, m, lbl)
        time.sleep(1.0)

    # Client 2 joins. The two lines below are said while it is STILL LOADING -- the
    # window where a seed and a live stream can interleave.
    log("--- CLIENT 2 LAUNCH (the joiner) ---")
    c2_pid = launch_peer("client", args.port, "Client2", peer="127.0.0.1",
                         res_x=1280, res_y=720, peer_slot=2, monitor=2, tile_index=1,
                         memory_limit_gb=args.memory_limit_gb,
                         extra_env={"VOTVCOOP_VOICE_ENABLED": "0"})
    c2_log = CLIENT2_DIR / "multivoid.log"
    window_msgs = ["said while you were loading A", "said while you were loading B"]
    time.sleep(args.window_delay)
    log(f"--- {len(window_msgs)} messages DURING the joiner's load window ---")
    _type_chat(host_pid, window_msgs[0], "HOST")
    time.sleep(1.0)
    _type_chat(c1_pid, window_msgs[1], "CLIENT1")

    if not _wait_for_log(c2_log, "Joined ", args.client_boot_timeout, "CLIENT2"):
        log("FAIL: client 2 never reached the world"); tail_log(c2_log, 30, "CLIENT2")
        kill_all(); sys.exit(1)
    time.sleep(6)   # let the seed land and the join lines settle

    _capture_window(c2_pid, shots_dir / "joiner_on_arrival.png")
    log("--- CLIENT 2 opens chat ---")
    _type_chat(c2_pid, "", "CLIENT2", submit=False)
    time.sleep(2.0)
    _capture_window(c2_pid, shots_dir / "joiner_history.png")
    time.sleep(args.hold)
    _press_vk(c2_pid, 0x0D, "CLIENT2")   # Enter on the empty field closes
    time.sleep(2.0)

    tail_log(c2_log, 20, "CLIENT2")
    c2text, c2err = _read_log_strict(c2_log)
    log("--- KILLING ---")
    kill_all()

    log("--- CHAT-SEED (D-W) VERDICT ---")
    failures: list[str] = []
    notes: list[str] = []
    if c2err:
        failures.append(f"client2 log is not strict UTF-8: {c2err}")

    applied = re.findall(r"chat: applied line (\d+) seeded=([01]) \"([^\"]*)\"", c2text)
    rows = [(int(a), sd == "1", t) for a, sd, t in applied]
    seqs = [r[0] for r in rows]
    texts = [r[2] for r in rows]
    seeded = [r for r in rows if r[1]]
    live = [r for r in rows if not r[1]]
    log(f"  client2 applied {len(seeded)} seeded + {len(live)} live row(s)")

    if not seeded:
        failures.append("W1: client2 applied NO seeded history -- the joiner arrived to "
                        "an empty lobby record")
    elif len(seeded) < args.messages:
        failures.append(f"W1: client2 applied only {len(seeded)} seeded row(s), "
                        f"expected >= {args.messages}")
    else:
        notes.append(f"W1: {len(seeded)} history line(s) reached the joiner")

    if len(seqs) < 2:
        failures.append("W2: too few applied rows to check ordering")
    else:
        bad = [(seqs[i], seqs[i + 1]) for i in range(len(seqs) - 1)
               if seqs[i + 1] != seqs[i] + 1]
        if bad:
            failures.append(f"W2: applied lineSeqs are not contiguous+ascending -- "
                            f"{len(bad)} break(s), first {bad[0][0]} -> {bad[0][1]}")
        else:
            notes.append(f"W2: lineSeqs {seqs[0]}..{seqs[-1]} applied contiguous and "
                         f"strictly ascending")

    missing = [m for m in window_msgs if not any(m in t for t in texts)]
    if missing:
        failures.append(f"W3: {missing} never reached the joiner -- said during its load "
                        f"window and lost (principle 8)")
    else:
        tail = texts[-len(window_msgs):]
        if not all(any(m in t for t in tail) for m in window_msgs):
            failures.append(f"W3: the load-window lines arrived but are NOT the newest -- "
                            f"tail is {tail}")
        else:
            notes.append("W3: both load-window lines arrived, and as the newest rows")

    opens = [ln for ln in c2text.splitlines() if "chat_view: reveal open" in ln]
    if not opens:
        failures.append("W4: client2 never logged a reveal -- chat did not open")
    else:
        log(f"  {opens[-1].strip()}")
        m = re.search(r"history=(\d+) live=(\d+)", opens[-1])
        hist, liveN = (int(m.group(1)), int(m.group(2))) if m else (-1, -1)
        if hist < args.messages:
            failures.append(f"W4: the joiner's reveal showed history={hist}, expected "
                            f">= {args.messages}")
        else:
            notes.append(f"W4: the joiner's reveal showed {hist} retained line(s) "
                         f"({liveN} live) -- seeded rows landed RETAINED, so its feed "
                         f"on arrival was clear of them")

    for n in notes:
        log(f"  OK  {n}")
    for f in failures:
        log(f"  FAIL {f}")
    log(f"shots -> {shots_dir}")
    if getattr(args, "inject", False):
        if failures:
            log("INJECTED RUN: RED as required -- the drill can see the defect it tests for")
            sys.exit(0)
        log("INJECTED RUN: GREEN -- the drill is BLIND; it would pass on a broken build")
        sys.exit(2)
    log("VERDICT: " + ("PASS" if not failures else f"FAIL ({len(failures)})"))
    sys.exit(0 if not failures else 2)


def cmd_puppetshot(args) -> None:
    """2-PEER PROPER nameplate shot. Launches host + client with
    VOTVCOOP_RUN_PUPPET_FRAME=1 so the HOST stands back + aims at the STANDING client
    puppet (NO ragdoll), then captures the host window -- the shot shows the ImGui
    'Client' nameplate (nick + dark-red health bar) over the puppet's head."""
    shots_dir = Path(__file__).resolve().parent.parent / "research" / "puppet_shots"
    shots_dir.mkdir(parents=True, exist_ok=True)
    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before puppetshot")
    deploy_all()
    os.environ["VOTVCOOP_RUN_PUPPET_FRAME"] = "1"

    log("--- HOST LAUNCH (puppet-frame, no ragdoll) ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=args.res_x, res_y=args.res_y, monitor=1, center=True,
                           memory_limit_gb=args.memory_limit_gb)
    host_log = HOST_DIR / "multivoid.log"
    log(f"waiting up to {args.boot_timeout}s for host to bind UDP {args.port}...")
    bound = False
    for i in range(args.boot_timeout):
        time.sleep(1)
        if host_owns_udp(host_pid, args.port):
            log(f"host bound UDP {args.port} after {i+1}s"); bound = True; break
        if not any(p["PID"] == host_pid for p in list_votv()):
            log("HOST DIED before binding UDP"); tail_log(host_log, 30, "HOST"); sys.exit(1)
    if not bound:
        log("FAIL: host did not bind UDP"); kill_all(); sys.exit(1)

    log("--- CLIENT LAUNCH ---")
    client_pid = launch_peer("client", args.port, getattr(args, "nick", None) or "Client", peer="127.0.0.1",
                             res_x=1280, res_y=720, peer_slot=1, monitor=2,
                             tile_index=0, memory_limit_gb=args.memory_limit_gb)
    wait_for_client_connect(CLIENT_DIR, args.client_boot_timeout, "CLIENT", client_pid)

    shot = shots_dir / "host_puppet_nameplate.png"
    if _wait_for_log(host_log, "PUPPET-FRAME READY", args.frame_timeout, "HOST"):
        time.sleep(2)  # let the aim + a HUD tick settle
        captured = _capture_window(host_pid, shot)
    else:
        log("WARN: never saw PUPPET-FRAME READY -- capturing anyway (may be unframed)")
        captured = _capture_window(host_pid, shot)
    tail_log(host_log, 16, "HOST")
    log("--- KILLING ---")
    kill_all()
    log("--- PUPPETSHOT VERDICT ---")
    if captured and shot.exists():
        log(f"Inspect {shot}: the white 'Client' nick + dark-red health bar should sit over the standing puppet's head.")
    sys.exit(0 if captured else 2)


# ---------------------------------------------------------------------------------------------
# GRACEFUL EXIT -- the shutdown path, made observable for the first time (2026-08-26)
# ---------------------------------------------------------------------------------------------
# WHY THIS EXISTS. `kill_all()` above is `Stop-Process -Force`, i.e. TerminateProcess. It sends no
# WM_CLOSE, our wndproc therefore never runs, and a forced kill delivers no DLL_PROCESS_DETACH
# either. So `coop::shutdown::DoShutdown()` -> `ue_wrap::hook::Shutdown()` -- our ENTIRE teardown --
# had never executed under an AUTOMATED SCENARIO, on any build. (It HAS run under a human close on
# the proxy lane: docs/piles/test-evidence/handson-s31/s32 carry the marker. What had never happened
# is a teardown with a co-resident PolyHook composed onto our relay.) Measured 2026-08-26;
# docs/UE4SS_ARC.md section 4a.
#
# That was blocking, and what it was hiding turned out to be bigger than the residual it was built
# for: `[V]` removing a MinHook hook CORRUPTS its trampoline in place -- MH_RemoveHook -> FreeBuffer
# writes a linked-list pointer over the slot's first eight bytes, i.e. over the stolen prologue a
# thread may be about to return through -- and MH_Uninitialize then unmaps it, both while the process
# is still running. Shipping a fix for that without this scenario would be a change to code no test
# runs -- unfalsifiable by construction. This makes the path observable FIRST, so the fix can be
# shown RED and then GREEN.
#
# It is deliberately ADDITIVE. `kill_all()` is untouched and every other scenario still tears down
# exactly the way it always has; nothing here changes an existing verdict.

_SC_CLOSE = 0xF060
_WM_SYSCOMMAND = 0x0112
_WM_CLOSE = 0x0010

# The teardown trail, in the order the code emits it. Cited by GREP TEXT, not by line number:
# this project has re-cited the same moved line three times, so the literal IS the citation and a
# rename breaks the scenario loudly instead of passing silently.
_TEARDOWN_TRAIL = [
    ("close-signal", "shutdown: close-signal received on HWND=", "coop/session/shutdown.cpp CoopWndProc"),
    ("begin",        "shutdown: BEGIN cleanup",                  "coop/session/shutdown.cpp DoShutdown"),
    ("minhook",      "hook: all patches lifted",                 "ue_wrap/core/hook.cpp Shutdown"),
    ("end",          "shutdown: END cleanup",                    "coop/session/shutdown.cpp DoShutdown"),
]
# Emitted from DllMain's DLL_PROCESS_DETACH arm -- present iff the process really unloaded us
# rather than being shot.
_DETACH_MARKER = "cppmod: final dispatch tally"


def _crash_dump_names() -> set:
    """Names of the engine crash-report directories written so far.

    A graceful exit must not produce one. Compared before/after rather than counted, so a
    concurrently-pruned directory cannot read as a new crash.
    """
    base = Path(os.environ.get("LOCALAPPDATA", "")) / "VotV" / "Saved" / "Crashes"
    if not base.is_dir():
        return set()
    try:
        return {p.name for p in base.iterdir()}
    except OSError:
        return set()


def _post_close(pid: int, signal: str, label: str) -> bool:
    """Post a real user-close signal to a peer's window.

    `sysclose` (the default) is what an X-click and Alt+F4 ACTUALLY generate: DefWindowProc turns
    WM_NCLBUTTONDOWN on the close box into WM_SYSCOMMAND/SC_CLOSE, and shutdown.cpp's own comment
    records (hands-on 2026-05-26) that UE4.27's FWindowsApplication::ProcessMessage acts on that
    and bypasses WM_CLOSE entirely. `wmclose` posts the canonical WM_CLOSE instead -- kept as a
    second arm so the two lanes can be TOLD APART rather than assumed equivalent.
    """
    u32 = ctypes.WinDLL("user32", use_last_error=True)
    hwnd = _peer_hwnd(pid)
    if not hwnd:
        log(f"  {label}: no visible window for pid {pid} -- cannot post a close signal")
        return False
    if signal == "wmclose":
        ok = u32.PostMessageW(hwnd, _WM_CLOSE, 0, 0)
        log(f"  {label}: posted WM_CLOSE to HWND={hwnd} (PostMessageW rc={ok})")
    else:
        ok = u32.PostMessageW(hwnd, _WM_SYSCOMMAND, _SC_CLOSE, 0)
        log(f"  {label}: posted WM_SYSCOMMAND/SC_CLOSE to HWND={hwnd} (PostMessageW rc={ok})")
    return bool(ok)


def cmd_authdrill(args) -> None:
    """NEGATIVE arm of the admission gate (v144, security A15/A2/A57).

    The happy path is proven by every smoke. This proves the other half: that the
    gate REFUSES. A gate that has never refused anything is an unproven gate, and
    the only thing that distinguishes it from `return true` is a run like this.

    THE SABOTAGE IS ENTIRELY CLIENT-SIDE (`[dev] auth_drill`). The host's gate has
    no knob, no dev branch and no env check anywhere -- so what this drills is the
    shipped refusal, not a test hook. Two arms:

      corrupt -- the client verifies the host, then sends a proof with one bit
                 flipped. The host must REFUSE and close.
      silent  -- the client verifies the host and then says nothing. The host must
                 close it on the pending deadline. Slower by design: the deadline
                 is 30 s because it must clear the slowest legitimate handshake.

    Asserted, per arm, from the HOST's own log and the CLIENT's:
      N1 the host REFUSED / swept this connection, naming why
      N2 the client NEVER took a seat        (no "host assigned us peer slot")
      N3 the host NEVER served the save      (A57's world half is what this buys)
      N4 the host's seat count stayed at zero

    CONTROL ARM (--control): the same script with the drill OFF must go GREEN on
    all four -- otherwise a drill that fails to connect for an unrelated reason
    reports a refusal it never caused.
    """
    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before authdrill")
    deploy_all()

    arm = "off" if args.control else args.arm
    log(f"--- ADMISSION DRILL: arm={arm} "
        f"({'CONTROL -- every assertion must INVERT' if args.control else 'the gate must refuse'}) ---")

    log("--- HOST LAUNCH ---")
    host_pid = launch_peer("host", args.port, "Host", peer=None,
                           res_x=1280, res_y=720, monitor=1, center=True,
                           extra_env={"VOTVCOOP_VOICE_ENABLED": "0"})
    host_log = HOST_DIR / "multivoid.log"
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

    log(f"--- CLIENT LAUNCH (auth_drill={arm}) ---")
    client_pid = launch_peer("client", args.port, "Client", peer="127.0.0.1",
                             res_x=1280, res_y=720, peer_slot=1, monitor=2, tile_index=0,
                             extra_env={"VOTVCOOP_VOICE_ENABLED": "0",
                                        "VOTVCOOP_AUTH_DRILL": arm})
    client_log = CLIENT_DIR / "multivoid.log"

    # The refusal is immediate for `corrupt`; `silent` waits out the host deadline.
    hold = args.hold if arm != "silent" else max(args.hold, 45)
    log(f"holding {hold}s for the verdict to land in both logs...")
    time.sleep(hold)

    htext, herr = _read_log_strict(host_log)
    ctext, cerr = _read_log_strict(client_log)
    log("--- KILLING ---")
    kill_all()

    if herr or cerr:
        log(f"FAIL: log did not decode as strict UTF-8 (host={herr} client={cerr})")
        sys.exit(1)

    # N1's needle differs per arm because the two refusals are different mechanisms
    # and conflating them would let the sweep's pass stand in for the verify's.
    refused = ("identity proof did not verify" in htext) if arm == "corrupt" else               ("never proved its identity" in htext)
    seated  = "host assigned us peer slot" in ctext
    # The host's own serve marker (`save_transfer.cpp:523`/`:275`) -- the whole
    # point of A57's world half is that this line must not appear for an unproved
    # peer. Matching on "streaming" catches the LIVE and the stale-fallback arms.
    served  = "save_transfer: slot 1 streaming" in htext
    admitted = "ADMITTED -> slot" in htext

    want = {"N1 host refused/swept, naming why": (not args.control) == refused,
            "N2 client took no seat":            args.control == seated,
            "N3 host served no save":            args.control == served,
            "N4 no seat was spent":              args.control == admitted}
    log("--- VERDICT ---")
    for k, ok in want.items():
        log(f"  {'PASS' if ok else 'FAIL'}  {k}")
    if all(want.values()):
        log(f"AUTH DRILL PASS (arm={arm}{', CONTROL' if args.control else ''})")
    else:
        log(f"AUTH DRILL FAIL (arm={arm}{', CONTROL' if args.control else ''})")
        tail_log(host_log, 25, "HOST")
        tail_log(client_log, 25, "CLIENT")
        sys.exit(1)


def cmd_gracefulexit(args) -> None:
    """Launch a solo host, close it the way a PLAYER closes it, and read the teardown trail.

    PASS requires all four of:
      1. the process actually exits within --exit-timeout (a hang is the failure this guards),
      2. `shutdown: BEGIN cleanup` AND `shutdown: END cleanup` both appear -- our cleanup ran to
         COMPLETION rather than faulting somewhere in the middle,
      3. no new engine crash-report directory,
      4. no [Error] line at or after BEGIN.

    Deliberately NOT gated on the individual MinHook line: which sub-steps the teardown performs
    is exactly what the leak-at-death fix changes, so gating on it would make the instrument
    contradict the fix it exists to test. The full trail is REPORTED either way.

    --control-terminate is the MUST-FAIL arm: same run, but torn down with Stop-Process -Force
    like every other scenario. It INVERTS the verdict and requires the trail to be ABSENT, which
    is what proves these markers discriminate the close path instead of appearing on every run.
    """
    if kill_all() > 0:
        log("note: pre-existing VotV instances killed before gracefulexit")

    if not args.no_deploy:
        deploy_all()

    dumps_before = _crash_dump_names()
    host_log = HOST_DIR / "multivoid.log"

    log("--- HOST LAUNCH (solo) ---")
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
        if not any(p["PID"] == host_pid for p in list_votv()):
            log(f"HOST DIED before binding UDP (PID {host_pid} gone)")
            tail_log(host_log, 30, "HOST")
            sys.exit(1)
    if not bound:
        log(f"FAIL: host did NOT bind UDP within {args.boot_timeout}s")
        tail_log(host_log, 30, "HOST")
        kill_all()
        sys.exit(1)

    # Settle. The PE detour is installed during boot, but a co-resident PolyHook composes onto our
    # relay only once UE4SS has started its own mods, and the post-init pe_diag census runs later
    # still -- so closing the instant UDP binds would measure a teardown with nothing composed on
    # it, which is the case the residual is NOT about.
    log(f"settling {args.settle}s before the close signal (lets any co-resident PE hooker compose)...")
    time.sleep(args.settle)

    pre_len = len(_read_text(host_log))

    log("--- CLOSING ---")
    t_close = time.time()
    if args.control_terminate:
        log("  CONTROL ARM: tearing down with Stop-Process -Force (the way every other scenario does)")
        run_ps("Get-Process VotV-Win64-Shipping -ErrorAction SilentlyContinue | Stop-Process -Force")
    else:
        if not _post_close(host_pid, args.signal, "HOST"):
            log("FAIL: could not post a close signal -- nothing was measured")
            kill_all()
            sys.exit(2)

    exited = False
    for i in range(args.exit_timeout):
        time.sleep(1)
        if not any(p["PID"] == host_pid for p in list_votv()):
            exited = True
            log(f"host process gone after {i+1}s")
            break
    elapsed = time.time() - t_close
    if not exited:
        log(f"host STILL ALIVE {args.exit_timeout}s after the close signal -- killing")
        kill_all()

    # Give the file a moment: the last lines are written on the dying thread.
    time.sleep(1)
    text = _read_text(host_log)
    tail = text[pre_len:]

    log("--- TEARDOWN TRAIL (log written AFTER the close signal) ---")
    seen = {}
    for key, needle, where in _TEARDOWN_TRAIL:
        hit = needle in tail
        seen[key] = hit
        log(f"  [{'X' if hit else ' '}] {key:<13} {needle!r}  ({where})")
    detached = _DETACH_MARKER in tail
    log(f"  [{'X' if detached else ' '}] {'detach':<13} {_DETACH_MARKER!r}  (bootstrap/dllmain.cpp DLL_PROCESS_DETACH)")
    log(f"  new log bytes after the signal: {len(tail)}")
    if tail.strip():
        for line in tail.strip().splitlines()[-25:]:
            log(f"    | {line}")

    dumps_new = _crash_dump_names() - dumps_before
    if dumps_new:
        log(f"  NEW crash report(s): {sorted(dumps_new)}")

    errors = [ln for ln in tail.splitlines() if "[Error]" in ln or "[ERROR]" in ln]

    log("--- GRACEFULEXIT VERDICT ---")
    if args.control_terminate:
        # Inverted: a forced kill must produce NO teardown trail. If it does, these markers are
        # not measuring the close path and every GREEN this scenario ever prints is worthless.
        if seen["begin"] or seen["end"]:
            log("CONTROL FAILED: the teardown trail appeared under Stop-Process -Force.")
            log("  The markers do not discriminate the close path -- do NOT trust this scenario.")
            sys.exit(2)
        log("CONTROL PASS: Stop-Process -Force produced no teardown trail, as it must.")
        log(f"  (exit took {elapsed:.1f}s; this arm asserts nothing about cleanliness)")
        sys.exit(0)

    probs = []
    if not exited:
        probs.append(f"process did not exit within {args.exit_timeout}s of the close signal")
    if not seen["begin"]:
        probs.append("no 'shutdown: BEGIN cleanup' -- the teardown never started")
    if not seen["end"]:
        probs.append("no 'shutdown: END cleanup' -- the teardown started and did NOT finish")
    if dumps_new:
        probs.append(f"{len(dumps_new)} new crash report(s) written during the exit")
    if errors:
        probs.append(f"{len(errors)} [Error] line(s) after the close signal")

    if probs:
        for p in probs:
            log(f"FAIL: {p}")
        sys.exit(1)

    lane = "wndproc (close-signal seen)" if seen["close-signal"] else \
           ("DLL_PROCESS_DETACH only" if detached else "unattributed")
    log(f"PASS: graceful exit in {elapsed:.1f}s, teardown ran to completion, no crash report.")
    log(f"  lane: {lane}   MinHook shutdown line: {'present' if seen['minhook'] else 'ABSENT'}")
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
    p_smoke.add_argument("--join-grace", type=int, default=90,
                         help="extra seconds to keep sampling when the client is still "
                              "mid-join (save-transfer download/load) at --duration end; "
                              "a 15s steady-state stretch follows a successful late join")
    p_smoke.add_argument("--duration", type=int, default=30,
                         help="seconds to monitor after both peers up")
    p_smoke.add_argument("--sample-interval", type=int, default=5,
                         help="seconds between RSS samples")
    p_smoke.add_argument("--boot-timeout", type=int, default=30,
                         help="seconds to wait for host UDP bind")
    p_smoke.add_argument("--ram-kill-mb", type=int, default=8000,
                         help="hard kill threshold (born from 19 GB install-loop incident)")
    p_smoke.add_argument("--host-settle", type=int, default=0,
                         help="seconds the host runs SOLO after binding UDP before the client launches "
                              "(default 0 = unchanged). Use a pre-connect window for a host-drift scenario "
                              "(VOTVCOOP_RUN_PILE_DRIFT) so the host can diverge its world before the snapshot.")
    for flag, kw in host_res: p_smoke.add_argument(flag, **kw)
    p_smoke.set_defaults(func=cmd_smoke)

    p_phys = sub.add_parser("smoke_phystele",
                            help="autonomous client world-divergence PHYSICS test (host s_1234 + fresh client; "
                                 "FAILs on divergent-prop reconcile / crash / FPS lock)")
    p_phys.add_argument("--duration", type=int, default=75,
                        help="seconds to monitor (divergent snapshot + physics settle)")
    p_phys.add_argument("--sample-interval", type=int, default=5)
    p_phys.add_argument("--boot-timeout", type=int, default=30)
    p_phys.add_argument("--host-settle", type=int, default=30,
                        help="seconds to wait after host PLAY READY (drain the boot-world's "
                             "dying prop elements) before launching the client -- avoids the "
                             "degenerate 88-live/1383-dying mid-transition snapshot")
    p_phys.add_argument("--ram-kill-mb", type=int, default=8000)
    for flag, kw in host_res: p_phys.add_argument(flag, **kw)
    p_phys.set_defaults(func=cmd_smoke_phystele)

    p_smoke4 = sub.add_parser("smoke4",
                              help="autonomous 4-PEER LAN smoke (Tier 8 cross-peer relay verdict)")
    p_smoke4.add_argument("--duration", type=int, default=45,
                          help="steady-state seconds to monitor after every client is "
                               "world-ready (the window anchors on the world-ready edge, "
                               "not on connect -- see the 2026-08-23 window-anchor note)")
    p_smoke4.add_argument("--ready-timeout", type=int, default=150,
                          help="per-client seconds to wait for its world-ready edge in the "
                               "host log before the steady-state window starts (covers the "
                               "save transfer + world load of a ~19 MB save under "
                               "4-instance contention)")
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
    p_smoke4.add_argument("--nick-all", default=None,
                          help="give EVERY peer this nickname (arc B arbitration drill)")
    p_smoke4.add_argument("--chat", default=None,
                          help="PIPE-separated per-peer chat lines host|c1|c2|c3, typed "
                               "through the real chat bar via WM_CHAR")
    p_smoke4.add_argument("--assert-i18n", action="store_true",
                          help="assert every peer saw every OTHER peer's name and message "
                               "verbatim, that no log line formatted to nothing, and that "
                               "every log is strictly well-formed UTF-8")
    p_smoke4.add_argument("--nicks", default=None,
                          help="comma-separated PER-PEER nicknames host,c1,c2,c3 (arc D2 "
                               "repertoire drill -- the gate needs DIFFERENT names per peer, "
                               "which --nick-all cannot express)")
    p_smoke4.add_argument("--scoreboard", action="store_true",
                          help="open the TAB player list for the capture")
    p_smoke4.add_argument("--memory-limit-gb", type=float, default=12.0,
                          help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_smoke4.add_argument(flag, **kw)
    p_smoke4.set_defaults(func=cmd_smoke4)

    # The standing MIXED-SCRIPT lobby (user 2026-07-28). English, Cyrillic,
    # Chinese and Japanese peers connect, are arbitrated, and TALK -- then every
    # artifact is asserted. Defaults are the whole scenario, so it is one word to
    # run and there is no way to run it half-configured.
    p_i18n = sub.add_parser("smoke_i18n",
                            help="4-peer MIXED-SCRIPT lobby (en/ru/zh/ja + emoji): connect, "
                                 "arbitrate, chat, and assert every name and message survived")
    p_i18n.add_argument("--duration", type=int, default=60)
    p_i18n.add_argument("--ready-timeout", type=int, default=150)
    p_i18n.add_argument("--sample-interval", type=int, default=5)
    p_i18n.add_argument("--boot-timeout", type=int, default=40)
    p_i18n.add_argument("--client-boot-timeout", type=int, default=75)
    p_i18n.add_argument("--settle-grace", type=int, default=25)
    p_i18n.add_argument("--ram-kill-mb", type=int, default=12000)
    p_i18n.add_argument("--memory-limit-gb", type=float, default=12.0)
    p_i18n.add_argument("--nick-all", default=None, help=argparse.SUPPRESS)
    for flag, kw in host_res: p_i18n.add_argument(flag, **kw)
    p_i18n.set_defaults(func=cmd_smoke_i18n)

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

    p_kt = sub.add_parser("kerfurtoggle",
                          help="verify the client kerfur conversion-adopt fix: host spawns a kerfur, client toggles it off+on, assert CLAIM+ADOPT (no dupe/respawn)")
    p_kt.add_argument("--boot-timeout", type=int, default=40, help="seconds to wait for host UDP bind")
    p_kt.add_argument("--client-boot-timeout", type=int, default=75, help="seconds for the client to connect")
    p_kt.add_argument("--install-timeout", type=int, default=90, help="seconds to wait for the host's NPC interceptor to install")
    p_kt.add_argument("--settle", type=int, default=6, help="seconds after quiescence before spawning the kerfur")
    p_kt.add_argument("--memory-limit-gb", type=float, default=12.0, help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_kt.add_argument(flag, **kw)
    p_kt.set_defaults(func=cmd_kerfurtoggle)

    p_jc = sub.add_parser("joinchurn",
                          help="autonomous regression test of the 2026-06-17 join-churn fix "
                               "(reconcile-once + kerfur exemption + flood skip): host s_1234 + fresh "
                               "client + kerfur spawn; 4-situation screenshots; verdicts on the "
                               "reconcile-once markers")
    p_jc.add_argument("--boot-timeout", type=int, default=40,
                      help="seconds to wait for host UDP bind / PLAY READY")
    p_jc.add_argument("--host-settle", type=int, default=25,
                      help="seconds after host PLAY READY (drain the boot-world dying props) before connect")
    p_jc.add_argument("--client-boot-timeout", type=int, default=90,
                      help="seconds for the client to reach connected (save-transfer join)")
    p_jc.add_argument("--join-monitor", type=int, default=75,
                      help="seconds to monitor the join-churn window (cover the load tail)")
    p_jc.add_argument("--midjoin-shot-at", type=int, default=20,
                      help="seconds into the join to capture the mid-join screenshot (SITUATION A)")
    p_jc.add_argument("--install-timeout", type=int, default=90,
                      help="seconds to wait for the host's NPC interceptor (gameplay loaded) before the kerfur spawn")
    p_jc.add_argument("--steady", type=int, default=15,
                      help="seconds of final steady-state settle (SITUATION D)")
    p_jc.add_argument("--sample-interval", type=int, default=5)
    p_jc.add_argument("--ram-kill-mb", type=int, default=9000)
    p_jc.add_argument("--max-sweeps", type=int, default=4,
                      help="max acceptable divergence-sweep FIRING count (pre-fix churn was ~10)")
    p_jc.add_argument("--max-destroys", type=int, default=2,
                      help="max acceptable destructive 'unclaimed locals destroyed' sweeps")
    p_jc.add_argument("--max-nomatch", type=int, default=40,
                      help="max acceptable 'no local match' held-clump flood (pre-fix was 152)")
    p_jc.add_argument("--max-kerfur-convert", type=int, default=4,
                      help="max acceptable spurious kerfur converts (the 'died invisibly' flip-flop)")
    p_jc.add_argument("--memory-limit-gb", type=float, default=12.0,
                      help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_jc.add_argument(flag, **kw)
    p_jc.set_defaults(func=cmd_joinchurn)

    p_sm = sub.add_parser("spawnmenutest",
                          help="diagnose the story-mode Q spawn-menu dev feature: host fires OpenNow + reports activeInterface/dispatch + screenshot")
    p_sm.add_argument("--boot-timeout", type=int, default=120, help="seconds to wait for host PLAY READY")
    p_sm.add_argument("--settle", type=int, default=8, help="seconds after PLAY READY before firing the trigger")
    p_sm.add_argument("--memory-limit-gb", type=float, default=12.0, help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_sm.add_argument(flag, **kw)
    p_sm.set_defaults(func=cmd_spawnmenutest)

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

    p_wirewin = sub.add_parser("wirewindow",
                               help="2-PEER D2 probe: does a client exiting to menu with the layer "
                                    "LIVE leak wire ops about its dying world? Census of the host's "
                                    "inbound in [transition, +10s] (islive-zeroav DESIGN section 6)")
    p_wirewin.add_argument("--boot-timeout", type=int, default=90, help="seconds to wait for host UDP bind")
    p_wirewin.add_argument("--client-boot-timeout", type=int, default=180,
                           help="seconds for the client to reach the world (save-transfer join)")
    p_wirewin.add_argument("--memory-limit-gb", type=float, default=12.0, help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_wirewin.add_argument(flag, **kw)
    p_wirewin.set_defaults(func=cmd_wirewindow)

    p_clumpvis = sub.add_parser("clumpvis",
                                help="SOLO probe: spawn a bare prop_garbageClump_C + report whether its StaticMesh asset is null (empty) or named (visible)")
    p_clumpvis.add_argument("--probe-timeout", type=int, default=150,
                            help="seconds to wait for CLUMPVIS READY (covers boot into gameplay)")
    p_clumpvis.add_argument("--memory-limit-gb", type=float, default=12.0,
                            help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_clumpvis.add_argument(flag, **kw)
    p_clumpvis.set_defaults(func=cmd_clumpvis)

    p_navprobe = sub.add_parser("navprobe",
                                help="SOLO Phase-0 HALT probe (bot-director): does FindPath return a path (Gate A) + does reflected AddMovementInput move the body (Gate B)")
    p_navprobe.add_argument("--probe-timeout", type=int, default=180,
                            help="seconds to wait for 'nav_probe: VERDICT' (covers boot into gameplay + the ~4s gate-B sweep)")
    p_navprobe.add_argument("--memory-limit-gb", type=float, default=12.0,
                            help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_navprobe.add_argument(flag, **kw)
    p_navprobe.set_defaults(func=cmd_navprobe)

    p_ctakeprobe = sub.add_parser("ctakeprobe",
                                  help="SOLO Phase-2 HALT gate (bot-director): walk to a placed non-empty container + drive the faithful take chain (openContainer->pressButton->em_take), measure if the take executed")
    p_ctakeprobe.add_argument("--probe-timeout", type=int, default=240,
                              help="seconds to wait for 'director/ctake: VERDICT' (boot + settle + walk + the take ladder)")
    p_ctakeprobe.add_argument("--memory-limit-gb", type=float, default=12.0,
                              help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_ctakeprobe.add_argument(flag, **kw)
    p_ctakeprobe.set_defaults(func=cmd_ctakeprobe)

    p_ctakerace = sub.add_parser("ctakerace",
                                 help="Director Phase-2 two-peer CONTAINER concurrent-take RACE (barrier + per-peer count sum: 1=correct/2=dup/0=vanished). --mode control runs the sum's positive control first")
    p_ctakerace.add_argument("--mode", choices=["control", "race"], default="control",
                             help="control = only one peer takes (solo sum MUST be 1); race = both take")
    p_ctakerace.add_argument("--taker", choices=["host", "client"], default="host",
                             help="control mode: which SINGLE peer takes. Run BOTH (host + client) so the summation is proven to see X in EITHER peer's personal store")
    p_ctakerace.add_argument("--boot-timeout", type=int, default=90, help="seconds for the host to bind UDP")
    p_ctakerace.add_argument("--probe-timeout", type=int, default=240, help="seconds to wait for BOTH peers to ARRIVE")
    p_ctakerace.add_argument("--memory-limit-gb", type=float, default=12.0, help="per-process commit cap (0=off)")
    p_ctakerace.add_argument("--hold-seconds", type=int, default=0,
                             help="after RESULT, keep both peers alive this long so the scenario's PROJECTION WATCH "
                                  "can sample saveSlot.inventoryData across an autosave (needs ~420; HOST arm is the "
                                  "known-positive, CLIENT arm is only interpretable if the HOST hash changes)")
    for flag, kw in host_res: p_ctakerace.add_argument(flag, **kw)
    p_ctakerace.set_defaults(func=cmd_ctakerace)

    p_walkgrab = sub.add_parser("walkgrab",
                                help="SOLO Phase-1 flagship (bot-director): the host bot WALKS to a chipPile over the NavMesh then runs the proven grab")
    p_walkgrab.add_argument("--probe-timeout", type=int, default=240,
                            help="seconds to wait for 'walkgrab: VERDICT' (boot + settle + the ~12s walk)")
    p_walkgrab.add_argument("--memory-limit-gb", type=float, default=12.0,
                            help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_walkgrab.add_argument(flag, **kw)
    p_walkgrab.set_defaults(func=cmd_walkgrab)

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

    p_nativeui = sub.add_parser("nativeui",
                                help="SOLO menu-time probe for the native server browser (P1): UMG "
                                     "resolve census + donor residency + switcher map + world-less "
                                     "frame count, and RUNG 1's one write")
    p_nativeui.add_argument("--duration", type=int, default=120,
                            help="seconds to hold at the menu while the probe runs. Counted from "
                                 "LAUNCH, not from the menu -- boot alone is ~50 s here, and the "
                                 "old default of 45 s killed the game mid-RUNG-2 (its verdicts and "
                                 "its restore never ran, which the runner then reported as four "
                                 "separate failures)")
    p_nativeui.add_argument("--no-write", action="store_true",
                            help="reads only -- skip RUNG 1 (the switcher write)")
    p_nativeui.add_argument("--travel", action="store_true",
                            help="O4 mode: boot -> gameplay -> transition to menu (two real level "
                                 "travels) so RUNG 0 has a transition in its sample. Implies --no-write")
    p_nativeui.add_argument("--memory-limit-gb", type=float, default=12.0,
                            help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_nativeui.add_argument(flag, **kw)
    p_nativeui.set_defaults(func=cmd_nativeui)

    p_browser = sub.add_parser("browser",
                               help="SOLO menu-time lab run for the NATIVE server browser (P2): "
                                    "builds it, shows it, screenshots it, asserts the log")
    p_browser.add_argument("--fake-master", type=int, default=0, metavar="N",
                           help="serve N synthetic lobbies from tools/fake_master.py and "
                                "point the game at it, then run the T0 scroll probe. The "
                                "live master's ~2 rows do not overflow the viewport, so "
                                "T0 is UNRUNNABLE without this. 20+ clears any plausible "
                                "viewport height; stay under kMaxRows=64 to avoid the "
                                "truncation confound.")
    p_browser.add_argument("--duration", type=int, default=140,
                           help="seconds from LAUNCH (boot alone is ~50 s here)")
    p_browser.add_argument("--memory-limit-gb", type=float, default=12.0,
                           help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_browser.add_argument(flag, **kw)
    p_browser.set_defaults(func=cmd_browser)

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

    p_chathist = sub.add_parser("chathistory",
                                help="2-PEER D-L drill: chat history retains, the T-reveal shows it, "
                                     "the TTL stops while you read, and new messages still arrive")
    p_chathist.add_argument("--boot-timeout", type=int, default=90, help="seconds to wait for host UDP bind")
    p_chathist.add_argument("--client-boot-timeout", type=int, default=120, help="seconds for the client to reach the world")
    p_chathist.add_argument("--messages", type=int, default=24,
                            help="how many messages to type; must exceed one viewport of rows "
                                 "(~18 at the drill's window size) or H4 cannot mean anything")
    p_chathist.add_argument("--settle", type=int, default=16,
                            help="seconds to let every live line expire before opening chat (TTL is 11 s)")
    p_chathist.add_argument("--hold", type=int, default=16,
                            help="seconds to hold the reveal open (must exceed the TTL to prove suspension)")
    p_chathist.add_argument("--min-history", type=int, default=6,
                            help="minimum retained lines the reveal must report")
    p_chathist.add_argument("--inject", action="store_true",
                            help="MUST-FAIL control: VOTVCOOP_CHAT_NO_RETAIN=1 on the host; the run must go RED")
    p_chathist.add_argument("--memory-limit-gb", type=float, default=12.0, help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_chathist.add_argument(flag, **kw)
    p_chathist.set_defaults(func=cmd_chathistory)

    p_chatseed = sub.add_parser("chatseed",
                                help="3-PEER D-W drill: a joiner is SEEDED with the lobby's chat record, "
                                     "in order, including lines said during its own load window")
    p_chatseed.add_argument("--boot-timeout", type=int, default=90, help="seconds to wait for host UDP bind")
    p_chatseed.add_argument("--client-boot-timeout", type=int, default=150, help="seconds for a client to reach the world")
    p_chatseed.add_argument("--messages", type=int, default=12,
                            help="messages exchanged BEFORE the joiner exists")
    p_chatseed.add_argument("--window-delay", type=int, default=25,
                            help="seconds after the joiner launches before the load-window lines are said")
    p_chatseed.add_argument("--hold", type=int, default=8, help="seconds to hold the joiner's reveal open")
    p_chatseed.add_argument("--inject", action="store_true",
                            help="MUST-FAIL control: VOTVCOOP_CHAT_SEED_SUPPRESS=1 on the host; the run must go RED")
    p_chatseed.add_argument("--memory-limit-gb", type=float, default=12.0, help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_chatseed.add_argument(flag, **kw)
    p_chatseed.set_defaults(func=cmd_chatseed)

    p_puppetshot = sub.add_parser("puppetshot",
                                  help="2-PEER PROPER nameplate shot: host frames the STANDING client puppet (no ragdoll) + captures the ImGui 'Client' nameplate over it")
    p_puppetshot.add_argument("--nick", default=None, help="client nickname to render on the plate")
    p_puppetshot.add_argument("--boot-timeout", type=int, default=40, help="seconds to wait for host UDP bind")
    p_puppetshot.add_argument("--client-boot-timeout", type=int, default=75, help="seconds for the client to connect")
    p_puppetshot.add_argument("--frame-timeout", type=int, default=70, help="seconds to wait for PUPPET-FRAME READY")
    p_puppetshot.add_argument("--memory-limit-gb", type=float, default=12.0, help="per-process commit cap in GB (0 = disabled)")
    for flag, kw in host_res: p_puppetshot.add_argument(flag, **kw)
    p_puppetshot.set_defaults(func=cmd_puppetshot)

    p_authdrill = sub.add_parser("authdrill",
                                 help="NEGATIVE arm of the admission gate: a client with a sabotaged "
                                      "identity proof must be REFUSED and must never receive the save "
                                      "(the sabotage is client-side only -- the host gate has no knob)")
    p_authdrill.add_argument("--arm", choices=["corrupt", "silent"], default="corrupt",
                             help="corrupt = flip a bit of the proof; silent = never answer the challenge")
    p_authdrill.add_argument("--control", action="store_true",
                             help="CONTROL: run with the drill OFF -- every assertion must invert, "
                                  "proving the refusal was caused by the sabotage and not by the rig")
    p_authdrill.add_argument("--port", type=int, default=DEFAULT_PORT, help="host UDP port")
    p_authdrill.add_argument("--boot-timeout", type=int, default=90, help="seconds to wait for host UDP bind")
    p_authdrill.add_argument("--hold", type=int, default=40,
                             help="seconds to hold before reading the verdict (the silent arm waits "
                                  "out the host's 30 s pending deadline regardless)")
    p_authdrill.set_defaults(func=cmd_authdrill)

    p_gexit = sub.add_parser("gracefulexit",
                             help="SOLO SHUTDOWN drill: close the host the way a PLAYER closes it "
                                  "(WM_SYSCOMMAND/SC_CLOSE) and read the teardown trail -- the path "
                                  "Stop-Process -Force has never let this rig walk")
    p_gexit.add_argument("--boot-timeout", type=int, default=90, help="seconds to wait for host UDP bind")
    p_gexit.add_argument("--settle", type=int, default=20,
                         help="seconds to hold the world open before closing (lets a co-resident "
                              "PE hooker compose onto our relay)")
    p_gexit.add_argument("--exit-timeout", type=int, default=90,
                         help="seconds to wait for the process to die after the close signal")
    p_gexit.add_argument("--signal", choices=["sysclose", "wmclose"], default="sysclose",
                         help="sysclose = what an X-click/Alt+F4 really send (default); wmclose = the "
                              "canonical WM_CLOSE, kept so the two lanes can be told apart")
    p_gexit.add_argument("--control-terminate", action="store_true",
                         help="MUST-FAIL control: tear down with Stop-Process -Force instead; the run "
                              "REQUIRES the teardown trail to be ABSENT")
    p_gexit.add_argument("--no-deploy", action="store_true",
                         help="skip deploy-all (measure the bytes already on disk)")
    for flag, kw in host_res: p_gexit.add_argument(flag, **kw)
    p_gexit.set_defaults(func=cmd_gracefulexit)

    args = ap.parse_args()

    # --- CROSS-SESSION GAME LOCK (2026-08-26) -------------------------------------------------
    # Every scenario below begins by killing EVERY VotV process on the box, so two sessions running
    # concurrently do not interleave -- they destroy each other, and the survivor reports a failure
    # that reads exactly like a bug in whatever was just changed. The lock is taken HERE, at the one
    # dispatch point every command passes through, because a protocol that has to be REMEMBERED is
    # one that gets forgotten under time pressure. See tools/game_lock.py and docs/CROSS_SESSION.md.
    import game_lock
    session = os.environ.get("MULTIVOID_SESSION") or f"pid-{os.getpid()}"
    # Derive the command from the bound handler, not from argv: every sub-parser sets
    # `func=cmd_<name>`, whereas argv[1] can be a global flag and `args.cmd` only exists if
    # the sub-parser group declared a dest. (The first version of this line read argv and had
    # a precedence bug that made it argv-dependent in two different ways.)
    cmd = getattr(args.func, "__name__", "").removeprefix("cmd_") or "?"

    if cmd == "kill":
        # `kill` is the CLEANUP path and must never be blocked -- it is what a human runs when a
        # session died holding the rig. It releases the lock instead of taking one.
        try:
            args.func(args)
        finally:
            game_lock.release(session, note_line=None)
        return

    # `host`/`client*` launch a game and RETURN, so the lock must outlive this process. `mp.py kill`
    # is what drops it.
    persistent = cmd in ("host", "client", "client2", "client3")
    if not game_lock.acquire(session, purpose=" ".join(sys.argv[1:]) or cmd,
                             persistent=persistent):
        game_lock.status()
        log("REFUSING to launch: another session holds the game rig (see above).")
        log("If that session is genuinely gone, delete ignore_folder/_GAME_LOCK.json.")
        sys.exit(2)
    try:
        args.func(args)
    finally:
        if not persistent:
            game_lock.release(session)


if __name__ == "__main__":
    main()
