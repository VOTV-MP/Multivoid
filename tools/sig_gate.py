#!/usr/bin/env python3
"""sig_gate -- prove that a signaling relay REFUSES a name its caller cannot sign for.

This is one instrument with two jobs, deliberately not two:

  * as a DRILL, run against a locally built relay (the default). A gate that has
    never been watched refusing is indistinguishable from one that returns
    `Ok(())`, and the refusal arms below are stageable nowhere else -- no LAN
    scenario can make a peer register under someone else's key, because the mod
    has no code that would try.

  * as a RELEASE GATE, run against the DEPLOYED url (`--remote host:port`). Since
    b145 the mod's signaling client FAILS CLOSED on a relay that never challenges
    it, so publishing that build before the deployed relay speaks the challenge
    would take P2P down for everyone at once. `docs/RELEASE.md` names this run as
    the precondition; the two must land as one step.

WHAT IT ASSERTS (security A59). A registered identity IS an Ed25519 public key,
so the relay must ask its holder to prove it:

  P1  an honest holder registers, and can relay a line to itself
  N1  a `gen:` identity with NO proof is refused
  N2  a proof signed by a DIFFERENT key -- the squat, i.e. A59 itself -- is refused
  N3  a proof over a DIFFERENT nonce (a recording) is refused
  N4  one flipped bit is refused
  N5  a legacy `h<16hex>` name is refused (the b<=133 cohort is retired; this arm
      is what stops that deprecation from silently un-happening)
  C   the relay CHALLENGES at all -- the arm the release gate exists for

WHAT IT CANNOT ASSERT, and why A59 is MITIGATED rather than CLOSED: the mod's
signaling leg is plaintext, and on a plaintext channel a challenge-response is
relayable -- an on-path attacker proxies the nonce and harvests a live proof.
Nothing here can see that; only TLS on that leg (Tier B arc 3) closes it.

Usage:
  python tools/sig_gate.py                      # build + run a local relay, drill it
  python tools/sig_gate.py --remote host:10442  # gate the deployed relay (TLS)
  python tools/sig_gate.py --remote host:10000 --plaintext
"""

from __future__ import annotations

import argparse
import os
import secrets
import socket
import ssl
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mp  # noqa: E402  -- the shared rig (builds/locates the production relay)

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey  # noqa: E402

# MUST equal REGISTER_TAG in tools/coop-server-rs/src/bin/signaling.rs and
# kRegisterTag in src/votv-coop/src/coop/net/signaling_client.cpp. Three
# constants in three languages; this script is what keeps them honest, because a
# disagreement makes P1 below fail against a real relay.
TAG = b"multivoid-signaling-register-v1"

LOG = mp.ROOT / "build" / "sig-gate-relay.log"
LOCAL_PORT = 18101
LOCAL_TOKEN = "siggate-local"

_fails: list[str] = []
_passes = 0


def check(ok: bool, what: str) -> None:
    global _passes
    if ok:
        _passes += 1
        mp.log(f"  PASS  {what}")
    else:
        _fails.append(what)
        mp.log(f"  FAIL  {what}")


class Peer:
    """A keypair and the `gen:` identity it renders to, as GNS renders one."""

    def __init__(self) -> None:
        self.priv = Ed25519PrivateKey.generate()
        raw = self.priv.public_key().public_bytes_raw()
        self.ident = "gen:" + raw.hex()

    def sign(self, ident: str, nonce: str) -> str:
        return self.priv.sign(TAG + ident.encode() + nonce.encode()).hex()


class Conn:
    """One relay connection, with just enough line protocol to drill it."""

    def __init__(self, host: str, port: int, tls: bool) -> None:
        raw = socket.create_connection((host, port), timeout=10)
        if tls:
            ctx = ssl.create_default_context()
            self.sock = ctx.wrap_socket(raw, server_hostname=host)
        else:
            self.sock = raw
        self.sock.settimeout(10)
        self.buf = b""

    def send(self, line: str) -> None:
        self.sock.sendall(line.encode() + b"\n")

    def line(self) -> str | None:
        """Next line, or None if the peer closed (which is how a refusal reads)."""
        while b"\n" not in self.buf:
            try:
                chunk = self.sock.recv(4096)
            except (socket.timeout, TimeoutError, ConnectionResetError, ssl.SSLError):
                return None
            if not chunk:
                return None
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return line.decode("utf-8", "replace").rstrip("\r")

    def closed(self) -> bool:
        """True when the relay dropped us -- a refusal is a close, not a message."""
        return self.line() is None

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def greet(host: str, port: int, tls: bool, token: str, ident: str) -> tuple[Conn, str | None]:
    """Greet and return (conn, nonce). `nonce` is None when no challenge came."""
    c = Conn(host, port, tls)
    c.send(f"{token} {ident}")
    first = c.line()
    if first is None or not first.startswith("nonce "):
        return c, None
    return c, first[len("nonce "):]


def run(host: str, port: int, tls: bool, token: str) -> None:
    where = f"{host}:{port} ({'TLS' if tls else 'plaintext'})"
    mp.log(f"--- sig_gate against {where} ---")

    # C -- does this relay challenge AT ALL? Every arm below depends on it, and it
    # is the one the release gate exists to answer: a b145 client fails closed
    # against a relay that does not.
    victim = Peer()
    conn, nonce = greet(host, port, tls, token, victim.ident)
    check(nonce is not None and len(nonce) == 64,
          "C  the relay challenges a key identity with a 64-hex nonce")
    if nonce is None:
        conn.close()
        mp.log("FATAL: no challenge -- every other arm would be vacuous")
        _fails.append("C (fatal)")
        return

    # P1 -- the positive control. Registration is not observable directly, so we
    # prove it the way it MATTERS: a registered peer can be routed to. Sending to
    # ourselves round-trips through the relay's map, which is exactly the state a
    # refusal denies.
    conn.send(f"auth {victim.sign(victim.ident, nonce)}")
    conn.send(f"{victim.ident} deadbeef")
    echoed = conn.line()
    check(echoed is not None and echoed.startswith(victim.ident + " ") and echoed.endswith("deadbeef"),
          "P1 an honest holder registers and is routable")
    conn.close()

    # N1 -- no proof at all. The relay must not fall back to registering us; this
    # is the arm that fails if the challenge is ever made advisory.
    c1, n1 = greet(host, port, tls, token, victim.ident)
    check(n1 is not None, "N1 (setup) challenged")
    c1.send(f"{victim.ident} deadbeef")   # skip `auth` entirely
    check(c1.closed(), "N1 a key identity with NO proof is refused")
    c1.close()

    # N2 -- THE SQUAT, i.e. A59 itself: an attacker signs with its own key while
    # registering as the victim. Note it signs a blob NAMING THE VICTIM, which is
    # the strongest form -- a blob naming itself would never be offered.
    attacker = Peer()
    c2, n2 = greet(host, port, tls, token, victim.ident)
    check(n2 is not None, "N2 (setup) challenged")
    c2.send(f"auth {attacker.sign(victim.ident, n2)}")
    check(c2.closed(), "N2 a proof by the WRONG key is refused (the A59 squat)")
    c2.close()

    # N3 -- a recording. The nonce is fresh per connection, so a proof captured
    # from an earlier one must not answer this one.
    c3, n3 = greet(host, port, tls, token, victim.ident)
    check(n3 is not None, "N3 (setup) challenged")
    stale = "0" * 64
    check(n3 != stale, "N3 (setup) the live nonce differs from the replayed one")
    c3.send(f"auth {victim.sign(victim.ident, stale)}")
    check(c3.closed(), "N3 a proof over a DIFFERENT nonce is refused")
    c3.close()

    # N4 -- one flipped bit. Fails if verification is ever reduced to a length check.
    c4, n4 = greet(host, port, tls, token, victim.ident)
    check(n4 is not None, "N4 (setup) challenged")
    sig = victim.sign(victim.ident, n4)
    flipped = sig[:-1] + ("1" if sig[-1] == "0" else "0")
    check(flipped != sig, "N4 (setup) the flip actually changed the signature")
    c4.send(f"auth {flipped}")
    check(c4.closed(), "N4 a one-bit-corrupt proof is refused")
    c4.close()

    # N5 -- the retired cohort. A `h<16hex>` name is one the master used to mint;
    # nobody can sign for it, so the relay must refuse it outright rather than
    # register it unproved beside the proved ones.
    c5 = Conn(host, port, tls)
    c5.send(f"{token} h{secrets.token_hex(8)}")
    check(c5.closed(), "N5 a legacy non-key identity is refused (b<=133 retired)")
    c5.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--remote", default="",
                    help="host:port of a DEPLOYED relay to gate (default: build + run one locally)")
    ap.add_argument("--plaintext", action="store_true",
                    help="with --remote: speak plaintext instead of TLS")
    ap.add_argument("--token", default="",
                    help="with --remote: the relay's COOP_SIGNALING_TOKEN")
    args = ap.parse_args()

    relay = None
    out = None
    try:
        if args.remote:
            host, _, port = args.remote.rpartition(":")
            if not host or not port.isdigit():
                mp.log("FATAL: --remote wants host:port")
                return 2
            if not args.token:
                mp.log("FATAL: --remote needs --token (the relay's shared bearer)")
                return 2
            run(host, int(port), not args.plaintext, args.token)
        else:
            exe = mp.signaling_exe()
            LOG.parent.mkdir(parents=True, exist_ok=True)
            out = open(LOG, "w", encoding="utf-8")
            env = dict(os.environ, COOP_SIGNALING_PORT=str(LOCAL_PORT),
                       COOP_SIGNALING_TOKEN=LOCAL_TOKEN)
            relay = subprocess.Popen([str(exe)], env=env, stdout=out,
                                     stderr=subprocess.STDOUT, text=True)
            time.sleep(1.0)
            if relay.poll() is not None:
                mp.log("FATAL: the local relay exited immediately:")
                mp.log(LOG.read_text(errors="replace"))
                return 2
            run("127.0.0.1", LOCAL_PORT, False, LOCAL_TOKEN)
    finally:
        if relay is not None:
            relay.terminate()
            try:
                relay.wait(timeout=5)
            except subprocess.TimeoutExpired:
                relay.kill()
        if out is not None:
            out.close()
            mp.log("--- RELAY LOG ---")
            for line in LOG.read_text(errors="replace").splitlines():
                mp.log(f"  relay: {line}")

    mp.log("--- VERDICT ---")
    if _fails:
        mp.log(f"FAIL: {len(_fails)} of {_passes + len(_fails)} checks failed:")
        for f in _fails:
            mp.log(f"  - {f}")
        return 1
    mp.log(f"PASS: {_passes}/{_passes} -- the relay refuses every name its caller "
           f"cannot sign for, and routes the one it can.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
