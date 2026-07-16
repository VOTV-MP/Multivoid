"""SSH/SFTP helper for the OLD VPS ONLY (paramiko, password auth).

!!! 2026-07-16: the coop stack MIGRATED to the NEW Cloudzy box 172.86.94.3 !!!
This tool targets the OLD box (87.121.218.33), which hosts only unrelated services after the
client cutover to master.votv.mp. For the NEW coop box use plain OpenSSH:
    ssh -i ~/.ssh/id_rsa root@172.86.94.3
(creds + survey: reference_coop_vps_cloudzy.md, gitignored). Do NOT run coop
provisioning/deploys through this tool.

Credentials come from reference_master_server_vps.md (gitignored) -- THIS file
carries NO secrets and is safe to track. Dev/ops only (RULE 3: VPS infra never
ships with the mod).

Usage:
  python tools/vps.py recon                 # OS + installed services + firewall + ports
  python tools/vps.py run "<command>"       # run a shell command, print output, exit rc
  python tools/vps.py put <local> <remote>  # upload a file via SFTP
"""

from __future__ import annotations

import base64
import re
import sys
from pathlib import Path

import paramiko

ROOT = Path(__file__).resolve().parent.parent
CREDS = ROOT / "reference_master_server_vps.md"
KEY_PATH = ROOT / "vps_id_ed25519"      # ops private key (gitignored)
KNOWN_HOSTS = ROOT / "vps_known_hosts"  # pinned host key (gitignored)


def load_creds() -> dict:
    if not CREDS.exists():
        sys.exit(f"FATAL: creds file missing: {CREDS}")
    text = CREDS.read_text(encoding="utf-8")

    def grab(label: str) -> str | None:
        m = re.search(rf"{re.escape(label)}:\s*`([^`]+)`", text)
        return m.group(1).strip() if m else None

    ip, user, pw = grab("IP"), grab("SSH user"), grab("SSH password")
    if not (ip and user and pw):
        sys.exit("FATAL: could not parse IP / SSH user / SSH password from creds file")
    return {"ip": ip, "user": user, "password": pw}


def _new_client() -> paramiko.SSHClient:
    cli = paramiko.SSHClient()
    if KNOWN_HOSTS.exists():
        # Host-key PINNING: load the captured key + reject anything else. No
        # AutoAddPolicy -> no accept-any-key window for a MITM.
        cli.load_host_keys(str(KNOWN_HOSTS))
        cli.set_missing_host_key_policy(paramiko.RejectPolicy())
    else:
        cli.set_missing_host_key_policy(paramiko.AutoAddPolicy())  # bootstrap only
    return cli


def connect(c: dict) -> paramiko.SSHClient:
    print("[vps] NOTE: target = OLD box (no coop services there after the cutover). "
          "Coop box is 172.86.94.3 via plain ssh (reference_coop_vps_cloudzy.md).")
    # Prefer KEY auth (no password on the wire -> nothing for a MITM to steal).
    # Fall back to password so a key problem can never lock us out of the box.
    key = None
    if KEY_PATH.exists():
        try:
            key = paramiko.Ed25519Key.from_private_key_file(str(KEY_PATH))
        except Exception as e:  # noqa: BLE001
            print(f"[vps] WARN: could not load key {KEY_PATH.name}: {e}", file=sys.stderr)
    if key is not None:
        cli = _new_client()
        try:
            cli.connect(c["ip"], username=c["user"], pkey=key, timeout=20,
                        look_for_keys=False, allow_agent=False)
            return cli
        except paramiko.AuthenticationException:
            print("[vps] WARN: key auth failed -- falling back to password", file=sys.stderr)
            try:
                cli.close()
            except Exception:  # noqa: BLE001
                pass
        except Exception:  # noqa: BLE001
            # Non-auth failure (host-key mismatch under RejectPolicy, or a timeout)
            # is FATAL -- close the socket and propagate. Do NOT fall back to
            # password: the pin would reject it too, and a host-key mismatch must
            # never be silently bypassed.
            try:
                cli.close()
            except Exception:  # noqa: BLE001
                pass
            raise
    cli = _new_client()
    cli.connect(c["ip"], username=c["user"], password=c["password"], timeout=20,
                look_for_keys=False, allow_agent=False)
    return cli


def run(cli: paramiko.SSHClient, cmd: str, timeout: int = 600) -> tuple[int, str, str]:
    _in, out, err = cli.exec_command(cmd, timeout=timeout)
    o = out.read().decode("utf-8", "replace")
    e = err.read().decode("utf-8", "replace")
    rc = out.channel.recv_exit_status()
    return rc, o, e


def put_file(cli: paramiko.SSHClient, local: str, remote: str) -> int:
    """Upload via exec + base64 over stdin (does NOT use the SFTP subsystem,
    which is restricted/disabled on this box). Returns the remote byte size."""
    data = Path(local).read_bytes()
    b64 = base64.b64encode(data).decode("ascii")
    rdir = remote.rsplit("/", 1)[0] or "/"
    stdin, stdout, stderr = cli.exec_command(
        f"mkdir -p '{rdir}' && base64 -d > '{remote}'", timeout=120)
    stdin.write(b64)
    stdin.flush()
    stdin.channel.shutdown_write()
    rc = stdout.channel.recv_exit_status()
    err = stderr.read().decode("utf-8", "replace")
    if rc != 0:
        raise RuntimeError(f"put failed rc={rc}: {err}")
    rc2, o2, _ = run(cli, f"wc -c < '{remote}'")
    size = int(o2.strip() or "-1")
    if size != len(data):
        raise RuntimeError(f"put size mismatch: local={len(data)} remote={size}")
    return size


def cmd_recon(cli: paramiko.SSHClient) -> None:
    checks = [
        ("hostname", "hostname"),
        ("os", "cat /etc/os-release 2>/dev/null | head -5"),
        ("kernel", "uname -a"),
        ("cpu/mem", "nproc; free -h | head -2"),
        ("disk", "df -h / | tail -1"),
        ("public-ip", "curl -s --max-time 8 ifconfig.me || echo '(curl failed/absent)'"),
        ("coturn?", "which turnserver dpkg-query 2>/dev/null; turnserver --version 2>/dev/null | head -1 || echo 'turnserver not installed'"),
        ("python3?", "which python3 && python3 --version"),
        ("docker?", "which docker 2>/dev/null && docker --version || echo 'no docker'"),
        ("firewall ufw", "ufw status verbose 2>/dev/null || echo 'no ufw'"),
        ("firewall iptables", "iptables -S 2>/dev/null | head -20 || echo 'no iptables'"),
        ("listening ports", "ss -tulpn 2>/dev/null | head -40 || netstat -tulpn 2>/dev/null | head -40"),
        ("/opt + /root", "ls -la /opt 2>/dev/null; echo '---'; ls -la /root 2>/dev/null | head -20"),
        ("running services", "systemctl list-units --type=service --state=running --no-pager 2>/dev/null | head -25 || echo 'no systemctl'"),
    ]
    for label, c in checks:
        rc, o, e = run(cli, c)
        print(f"==== {label} (rc={rc}) ====")
        body = (o or e).rstrip()
        print(body if body else "(no output)")
        print()


def main() -> None:
    # The Windows console codepage (cp1251 here) can't encode chars apt/systemd
    # emit (e.g. U+2192 arrow); replace rather than crash on write.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except Exception:
            pass
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    creds = load_creds()
    print(f"[vps] connecting to {creds['user']}@{creds['ip']} ...", flush=True)
    cli = connect(creds)
    print("[vps] connected.", flush=True)
    try:
        action = sys.argv[1]
        if action == "recon":
            cmd_recon(cli)
        elif action == "run":
            if len(sys.argv) < 3:
                sys.exit("usage: vps.py run \"<command>\"")
            rc, o, e = run(cli, sys.argv[2])
            sys.stdout.write(o)
            if e:
                sys.stderr.write(e)
            sys.exit(rc)
        elif action == "put":
            if len(sys.argv) < 4:
                sys.exit("usage: vps.py put <local> <remote>")
            size = put_file(cli, sys.argv[2], sys.argv[3])
            print(f"[vps] uploaded {sys.argv[2]} -> {sys.argv[3]} ({size} bytes)")
        else:
            sys.exit(f"unknown action: {action}")
    finally:
        cli.close()


if __name__ == "__main__":
    main()
