#!/usr/bin/env python3
"""game_lock.py -- one game, one session at a time.

WHY THIS EXISTS (2026-08-26). Two Claude Code sessions were working the same tree, and every
`mp.py` scenario begins by killing EVERY VotV process on the box. So two concurrent runs do not
merely interleave -- they destroy each other, and the survivor reports a plausible-looking failure
("expected 2 peers, got 1") that reads exactly like a bug in whatever was just changed. That is the
expensive part: not the lost run, but the false attribution. On 2026-08-26 it cost both sessions
several runs and produced a confident, wrong "your build is a boot-killer" claim that took an
mtime census to retract.

WHY A LOCKFILE AND NOT JUST THE LOG. `ignore_folder/_FRIENDLY_SESSION.txt` is the append-only
CONVERSATION between sessions, and it is genuinely valuable -- cross-lane findings, corrections,
who-owns-which-bytes. But it answers "what happened", not "is the game free RIGHT NOW". Deriving
state from an append-only log means pairing TAKING/DONE lines by eye, which is ambiguous the moment
a session dies without writing DONE. Two files, two jobs: the log is history, the lock is state.

WHY IT IS ENFORCED IN `mp.py` RATHER THAN WRITTEN DOWN. A protocol both sessions must REMEMBER is a
protocol that gets forgotten under time pressure -- this project's own lesson about a convention
wearing a ratchet's hat. `mp.py` is the single entry point for every game scenario, so acquiring
there makes the lock impossible to skip rather than impolite to skip.

STALENESS DEPENDS ON WHICH KIND OF LOCK IT IS, and conflating the two made the first version of
this file useless -- a drill caught it handing a held lock straight to a second session.

  * TRANSIENT (`mp.py smoke`, every scenario): the holder runs the whole scenario in-process, so a
    dead PID PROVES the lock is stale. A deadline would only ever be a guess -- too short breaks a
    live run, too long deadlocks after a crash -- so it is a backstop against PID reuse, nothing
    more, and an alive-but-overdue lock is NEVER broken.
  * PERSISTENT (`mp.py host`, `mp.py client`): the launcher starts a game and EXITS ON PURPOSE, so
    its PID is dead by design. Judging these by PID would mark every one stale the instant it was
    written. They are stale only when the deadline has passed AND no VotV process is running.

Run `python tools/game_lock.py status` to see which kind holds the rig and why.
"""

from __future__ import annotations

import argparse
import ctypes
import datetime as _dt
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCK_PATH = ROOT / "ignore_folder" / "_GAME_LOCK.json"
LOG_PATH = ROOT / "ignore_folder" / "_FRIENDLY_SESSION.txt"

# Backstop only -- see the module docstring. PID liveness is the real test.
DEFAULT_MAX_MINUTES = 45


def _now() -> _dt.datetime:
    return _dt.datetime.now()


def _stamp(t: _dt.datetime) -> str:
    return t.strftime("%Y-%m-%d %H:%M:%S")


def _pid_alive(pid: int) -> bool:
    """True if `pid` names a live process.

    On Windows `os.kill(pid, 0)` is not a liveness probe (it maps to TerminateProcess for real
    signals and raises for 0), so ask the OS directly. SYNCHRONIZE is the cheapest right that any
    live process will grant; ERROR_ACCESS_DENIED means the process EXISTS but is not ours, which is
    still alive for our purposes.
    """
    if pid <= 0:
        return False
    if os.name != "nt":
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
    SYNCHRONIZE = 0x00100000
    ERROR_ACCESS_DENIED = 5
    k32 = ctypes.windll.kernel32
    h = k32.OpenProcess(SYNCHRONIZE, False, pid)
    if h:
        k32.CloseHandle(h)
        return True
    return k32.GetLastError() == ERROR_ACCESS_DENIED


def _read() -> dict | None:
    try:
        return json.loads(LOCK_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def note(line: str) -> None:
    """Append one line to the shared conversation log. Never raises: a failure to journal must not
    take down a game run."""
    try:
        LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
        with LOG_PATH.open("a", encoding="utf-8") as fh:
            fh.write(line.rstrip() + "\n")
    except OSError:
        pass


def _votv_running() -> bool:
    """Is a game process actually up? Used only to decide whether a PERSISTENT lock is stale."""
    if os.name != "nt":
        return False
    try:
        import subprocess
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq VotV*"],
                             capture_output=True, text=True, timeout=15).stdout
    except Exception:
        return True    # cannot tell -> assume occupied; refusing to launch is the safe error
    return "VotV" in out


def stale_reason(rec: dict) -> str | None:
    """Why this lock may be broken, or None if it is live and must be respected.

    THE TWO KINDS ARE JUDGED DIFFERENTLY, and conflating them made the first version of this file
    useless: a TRANSIENT lock is held by a process that stays alive for the whole run, so a dead PID
    proves it is stale. A PERSISTENT lock is taken by a launcher that EXITS ON PURPOSE
    (`mp.py host` starts a game and returns), so its PID is dead by design and PID liveness would
    declare every such lock stale the instant it was written -- which a drill caught doing exactly
    that, handing a held lock straight to a second session.
    """
    if not rec.get("persistent"):
        pid = int(rec.get("pid", 0) or 0)
        if not _pid_alive(pid):
            return f"holder pid {pid} is gone"
        # PID alive but past the deadline: do NOT break. A long run is far likelier than PID reuse,
        # and breaking a live run is the exact harm this file exists to prevent.
        return None

    # Persistent: the rig is occupied for as long as a game is up, deadline or not.
    if _votv_running():
        return None
    try:
        deadline = _dt.datetime.strptime(rec["deadline"], "%Y-%m-%d %H:%M:%S")
    except (KeyError, ValueError):
        return "persistent lock has no readable deadline and no game is running"
    if _now() > deadline:
        return "persistent lock expired and no VotV process is running"
    return None


def acquire(session: str, purpose: str, minutes: int = DEFAULT_MAX_MINUTES,
            persistent: bool = False) -> bool:
    """Take the lock. Returns True on success, False if someone live holds it."""
    LOCK_PATH.parent.mkdir(parents=True, exist_ok=True)
    start = _now()
    rec = {
        "session": session,
        "purpose": purpose,
        "pid": os.getpid(),
        "started": _stamp(start),
        "deadline": _stamp(start + _dt.timedelta(minutes=minutes)),
        # A persistent lock outlives the process that took it (`mp.py host` launches a game and
        # returns). `mp.py kill` is what releases those.
        "persistent": bool(persistent),
    }
    payload = json.dumps(rec, indent=2)
    try:
        with LOCK_PATH.open("x", encoding="utf-8") as fh:   # O_EXCL: atomic, no TOCTOU window
            fh.write(payload)
        note(f"{_stamp(start)}  {session}  TAKING   {purpose}")
        return True
    except FileExistsError:
        pass

    held = _read()
    if held is None:
        # Unreadable/corrupt lock file: treat as stale rather than deadlocking on garbage.
        note(f"{_stamp(_now())}  {session}  BREAK    unreadable lock file -- taking it")
        LOCK_PATH.write_text(payload, encoding="utf-8")
        return True

    # NB: do NOT special-case a persistent lock with a dead PID here. An earlier version returned
    # False at this point, which was correct for the common case (the launcher exits on purpose and
    # the game is still up) but made the expired-persistent escape hatch in `stale_reason`
    # UNREACHABLE -- a crashed launcher would have deadlocked the rig forever. One owner decides
    # staleness, and it is `stale_reason`.
    reason = stale_reason(held)
    if reason is None:
        return False

    note(f"{_stamp(_now())}  {session}  BREAK    stale lock from "
         f"{held.get('session', '?')} ({reason}) -- taking it")
    LOCK_PATH.write_text(payload, encoding="utf-8")
    return True


def release(session: str, note_line: str | None = None) -> bool:
    """Drop the lock if we hold it. Releasing someone else's lock is refused."""
    held = _read()
    if held is None:
        return False
    if held.get("session") != session and _pid_alive(int(held.get("pid", 0) or 0)):
        return False
    try:
        LOCK_PATH.unlink()
    except OSError:
        return False
    note(note_line or f"{_stamp(_now())}  {session}  DONE     game is FREE")
    return True


def status() -> int:
    held = _read()
    if held is None:
        print("FREE -- no session holds the game")
        return 0
    reason = stale_reason(held)
    tag = f" (STALE: {reason})" if reason else ""
    print(f"HELD by {held.get('session','?')}{tag}\n"
          f"  purpose : {held.get('purpose','?')}\n"
          f"  pid     : {held.get('pid','?')}"
          f"{' (persistent launcher)' if held.get('persistent') else ''}\n"
          f"  started : {held.get('started','?')}\n"
          f"  deadline: {held.get('deadline','?')}")
    return 0 if reason else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="cross-session lock for the VotV game rig")
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("acquire")
    a.add_argument("--session", required=True)
    a.add_argument("--purpose", default="unspecified")
    a.add_argument("--minutes", type=int, default=DEFAULT_MAX_MINUTES)
    # A CLI acquire is PERSISTENT by default, and that is not a convenience: this process exits
    # immediately, so a transient lock taken here would be stale the moment it was written. Pass
    # --transient only from a caller that stays alive for the whole run (mp.py does).
    a.add_argument("--transient", action="store_true",
                   help="hold only while THIS process lives (for in-process callers)")
    r = sub.add_parser("release")
    r.add_argument("--session", required=True)
    sub.add_parser("status")
    args = ap.parse_args()

    if args.cmd == "status":
        return status()
    if args.cmd == "acquire":
        ok = acquire(args.session, args.purpose, args.minutes,
                     persistent=not args.transient)
        if not ok:
            held = _read() or {}
            print(f"BUSY -- {held.get('session','?')} is running: {held.get('purpose','?')} "
                  f"(since {held.get('started','?')})", file=sys.stderr)
            return 1
        print("ACQUIRED")
        return 0
    return 0 if release(args.session) else 1


if __name__ == "__main__":
    raise SystemExit(main())
