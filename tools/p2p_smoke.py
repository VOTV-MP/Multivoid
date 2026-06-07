"""Autonomous P2P (ICE) 2-peer smoke -- validation of the zero-open-ports transport.

Defaults to a LOCAL signaling server (loopback). Point it at the VPS with
--signaling/--stun/--turn-* to validate the real-internet path.

What it does:
  1. Kill stragglers; deploy via deploy-all.ps1 (build/votv-coop is the
     manifest+webrtc build now, so this ships the webrtc DLL -- no hand-staging).
  2. If signaling is local (127.0.0.1), start the local async signaling server.
     If remote (the VPS), use it as-is.
  3. Launch host then client in P2P topology (all config via env, parsed by
     harness/config.cpp::ReadNetConfig -> Session::StartP2P).
  4. Monitor until BOTH transport-connected markers appear, or timeout.
  5. Dump signaling log (local) + both peer logs + P2P marker counts; verdict.

Success = the TRANSPORT: client "host assigned us peer slot" AND host "accepted
client at slot" = bidirectional reliable app traffic over an ICE connection with
ZERO open ports on either peer. (puppet=0 is a pre-existing orthogonal bug, not a
gate -- "verify via CONNECTED+pkts, not puppet".)

Examples:
  python tools/p2p_smoke.py                                   # local loopback
  python tools/p2p_smoke.py --signaling <vps-ip>:10000 \\
      --stun <vps-ip>:3478 --turn-url turn:<vps-ip>:3478 \\
      --turn-user <user> --turn-pass <pass>                     # via the VPS
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

import mp  # reuse the launcher rig

# The production async, token-authed signaling server (replaces the trivial one).
SIG_SERVER = mp.ROOT / "tools" / "coop_signaling_server.py"
SIG_LOG = mp.ROOT / "build" / "p2p-signaling-server.log"

HOST_BOOT_S = 22
MONITOR_S = 55


def is_local(sig_url: str) -> bool:
    host = sig_url.rsplit(":", 1)[0].strip("[]")
    return host in ("127.0.0.1", "localhost", "::1")


def grep(path: Path, needle: str) -> int:
    try:
        return path.read_text(encoding="utf-8", errors="replace").count(needle)
    except OSError:
        return 0


def parse_args():
    ap = argparse.ArgumentParser(description="P2P/ICE 2-peer transport smoke")
    ap.add_argument("--signaling", default="127.0.0.1:10000",
                    help="signaling server host:port (remote -> no local server started)")
    ap.add_argument("--stun", default="stun.l.google.com:19302", help="STUN host:port")
    ap.add_argument("--turn-url", default="", help="TURN url, e.g. turn:host:3478")
    ap.add_argument("--turn-user", default="")
    ap.add_argument("--turn-pass", default="")
    ap.add_argument("--ice", default="",
                    help="ICE policy: '' (all) / relay (force TURN) / disable / default")
    ap.add_argument("--signaling-token", default="localtest",
                    help="shared bearer token the signaling server requires (VPS: its real token)")
    return ap.parse_args()


def main() -> None:
    args = parse_args()
    code = 1
    local_sig = is_local(args.signaling)
    sig_port = args.signaling.rsplit(":", 1)[1] if ":" in args.signaling else "10000"

    if mp.kill_all() > 0:
        mp.log("note: pre-existing VotV killed before p2p smoke")

    # deploy-all ships the webrtc DLL now (build/votv-coop is manifest+webrtc).
    mp.deploy_all()

    sig = None
    sig_out = None
    if local_sig:
        mp.log(f"starting LOCAL signaling server on :{sig_port} ({SIG_SERVER.name})")
        sig_out = open(SIG_LOG, "w", encoding="utf-8")
        sig_env = dict(os.environ, COOP_SIGNALING_PORT=str(sig_port),
                       COOP_SIGNALING_TOKEN=args.signaling_token)
        sig = subprocess.Popen([sys.executable, str(SIG_SERVER)], env=sig_env,
                               stdout=sig_out, stderr=subprocess.STDOUT, text=True)
        time.sleep(1.5)
        if sig.poll() is not None:
            sig_out.close()
            mp.log("FATAL: local signaling server exited immediately:")
            mp.log(SIG_LOG.read_text(errors="replace"))
            sys.exit(1)
    else:
        mp.log(f"using REMOTE signaling at {args.signaling} (no local server)")

    host_log = mp.HOST_DIR / "votv-coop.log"
    client_log = mp.CLIENT_DIR / "votv-coop.log"
    connected = False

    try:
        common = {"VOTVCOOP_NET_TOPOLOGY": "p2p",
                  "VOTVCOOP_NET_SIGNALING": args.signaling,
                  "VOTVCOOP_NET_SIGNALING_TOKEN": args.signaling_token,
                  "VOTVCOOP_NET_STUN": args.stun}
        if args.turn_url:
            common["VOTVCOOP_NET_TURN"] = args.turn_url
            common["VOTVCOOP_NET_TURN_USER"] = args.turn_user
            common["VOTVCOOP_NET_TURN_PASS"] = args.turn_pass
        if args.ice:
            common["VOTVCOOP_NET_ICE"] = args.ice
        host_env = dict(common, VOTVCOOP_NET_IDENTITY="votvhost")
        client_env = dict(common, VOTVCOOP_NET_IDENTITY="votvclient",
                          VOTVCOOP_NET_HOST_IDENTITY="votvhost")

        mp.log(f"--- HOST LAUNCH (P2P, signaling={args.signaling} stun={args.stun} "
               f"turn={'yes' if args.turn_url else 'no'}) ---")
        host_pid = mp.launch_peer("host", mp.DEFAULT_PORT, "Host", peer=None,
                                  res_x=1280, res_y=720, monitor=1, center=True,
                                  extra_env=host_env)
        mp.log(f"waiting {HOST_BOOT_S}s for host to boot + register identity...")
        time.sleep(HOST_BOOT_S)
        if not any(p["PID"] == host_pid for p in mp.list_votv()):
            mp.log("HOST DIED during boot (see log below)")

        mp.log("--- CLIENT LAUNCH (P2P) ---")
        client_pid = mp.launch_peer("client", mp.DEFAULT_PORT, "Client", peer="127.0.0.1",
                                    res_x=1280, res_y=720, monitor=2, tile_index=0,
                                    extra_env=client_env)

        mp.log(f"--- MONITORING {MONITOR_S}s for ICE connect ---")
        t0 = time.time()
        while time.time() - t0 < MONITOR_S:
            time.sleep(3)
            peers = mp.list_votv()
            assigned = grep(client_log, "host assigned us peer slot")
            accepted = grep(host_log, "host accepted client at slot")
            t = int(time.time() - t0)
            mp.log(f"  t={t}s peers={len(peers)} client_assigned={assigned} host_accepted={accepted}")
            if assigned > 0 and accepted > 0:
                connected = True
                mp.log(f"  *** ICE CONNECTED after ~{t}s ***")
                break
            if not any(p["PID"] == client_pid for p in mp.list_votv()):
                mp.log("  client process died -- stopping monitor")
                break

        code = 0 if connected else 2
    finally:
        time.sleep(1)
        if sig is not None:
            if sig.poll() is None:
                sig.terminate()
                try:
                    sig.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    sig.kill()
            if sig_out:
                sig_out.close()
            mp.log("--- LOCAL SIGNALING SERVER LOG ---")
            for line in SIG_LOG.read_text(errors="replace").splitlines():
                mp.log(f"  sig: {line}")

        mp.tail_log(host_log, 30, "HOST")
        mp.tail_log(client_log, 30, "CLIENT")

        for label, lp in (("HOST", host_log), ("CLIENT", client_log)):
            mp.log(f"--- {label} P2P markers ---")
            for needle in ("P2P identity set", "ice: applied", "signaling: resolved",
                           "signaling: connecting", "P2P host listening", "P2P client dialing",
                           "signaling: creating signaling session",
                           "host accepted client at slot", "host assigned us peer slot",
                           "latched senderEpoch",
                           "will reconnect", "getaddrinfo", "WSAStartup failed",
                           "ConnectP2PCustomSignaling", "CreateListenSocketP2P failed",
                           "FAULT"):
                c = grep(lp, needle)
                if c:
                    mp.log(f"  {label}: '{needle}' x{c}")

        mp.log("--- KILLING ---")
        mp.kill_all()

    mp.log("--- VERDICT ---")
    if connected:
        mp.log(f"PASS: P2P ICE transport CONNECTED via signaling {args.signaling} "
               "-- client got a peer slot AND the host accepted the client over a "
               "connection established with zero open ports.")
    else:
        mp.log("FAIL: P2P transport did NOT reach connected in the window. Localize via "
               "the markers above (registration? signal routing? ICE candidate exchange?).")
    sys.exit(code)


if __name__ == "__main__":
    main()
