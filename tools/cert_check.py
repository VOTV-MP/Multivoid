"""Off-box TLS certificate freshness check for the coop control plane.

WHY IT LIVES HERE AND NOT ON THE SERVER: an alarm that logs on the box nobody
watches has exactly the failure mode it was built to prevent. This check runs on
the DEV machine -- off the box by construction -- so it also covers the cases an
on-box timer cannot: the box being down, or the timer never having fired at all
(silence is not health).

WHAT ONE HANDSHAKE PROVES: it reads the certificate the LISTENER IS ACTUALLY
SERVING, so a fresh cert here means, in one observation, that renewal ran AND the
deploy hook ran AND the restart succeeded AND the systemd LoadCredential snapshot
in the running process matches what is on disk. Reading the file on the server
would prove only the first of those (the process holds a start-time snapshot).

Exit codes: 0 = healthy, 1 = below the warning threshold or unreachable.
Usage: python tools/cert_check.py [--host master.multivoid.dev] [--warn-days 21]
"""
from __future__ import annotations

import argparse
import datetime as dt
import socket
import ssl
import sys

# certbot renews at 30 days remaining and retries twice daily, so a threshold
# below that gives ~9 days of failing renewals before this starts complaining --
# loud enough to be real, quiet enough not to cry during normal operation.
DEFAULT_WARN_DAYS = 21
DEFAULT_PORTS = (10443, 10442)  # master TLS, signaling TLS


def served_cert_expiry(host: str, port: int, timeout: float = 10.0) -> dt.datetime:
    """Return the notAfter of the cert this listener serves (full verification ON)."""
    ctx = ssl.create_default_context()
    try:  # prefer certifi: some Windows Python builds reject LE's ECDSA chain
        import certifi

        ctx.load_verify_locations(cafile=certifi.where())
    except ImportError:
        pass
    with socket.create_connection((host, port), timeout=timeout) as raw:
        with ctx.wrap_socket(raw, server_hostname=host) as tls:
            cert = tls.getpeercert()
    return dt.datetime.strptime(cert["notAfter"], "%b %d %H:%M:%S %Y %Z").replace(
        tzinfo=dt.timezone.utc
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="master.multivoid.dev")
    ap.add_argument("--ports", type=int, nargs="+", default=list(DEFAULT_PORTS))
    ap.add_argument("--warn-days", type=int, default=DEFAULT_WARN_DAYS)
    args = ap.parse_args()

    now = dt.datetime.now(dt.timezone.utc)
    worst = None
    failed = False

    for port in args.ports:
        try:
            exp = served_cert_expiry(args.host, port)
        except Exception as e:  # noqa: BLE001 -- any failure here is a real alarm
            print(f"[cert] {args.host}:{port} UNREACHABLE/INVALID -- {e}")
            failed = True
            continue
        days = (exp - now).total_seconds() / 86400.0
        worst = days if worst is None else min(worst, days)
        state = "OK" if days >= args.warn_days else "STALE"
        print(f"[cert] {args.host}:{port} serves a cert valid until {exp:%Y-%m-%d} "
              f"({days:.1f} days left) -- {state}")

    if failed:
        print("[cert] RESULT: FAIL -- a TLS endpoint did not present a valid certificate.")
        return 1
    if worst is not None and worst < args.warn_days:
        print(f"[cert] RESULT: FAIL -- {worst:.1f} days left is below the {args.warn_days}-day "
              "threshold. Renewal has probably been failing; check `certbot renew` and the "
              "deploy hook on the box (a renewed file on disk does NOT reach the running "
              "process -- the services must restart).")
        return 1
    print("[cert] RESULT: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
