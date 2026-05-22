# Two-instance LAN test framework + the handshake bugs it caught

Snapshot 2026-05-23. Built per RULE 1 (the single-process loopback isn't real coop;
a self-send can't exercise cross-process handshake/auth). The framework launches TWO
real VotV processes (host + client) and verifies they discover + authenticate each
other and exchange state over real UDP. PASS on first clean run after two fixes.

## The framework

`tools/lan-test.ps1` -- launches host + client, configures each by ENVIRONMENT
VARIABLES (both load the same DLL from the same dir, so per-file scenario.txt/ini/log
can't differ), polls their separate logs for cross-process proof, captures both
windows, prints PASS/FAIL, tears down.

Enablers:
- Harness reads env first, then ini: `VOTVCOOP_SCENARIO`, `VOTVCOOP_NET_ROLE`,
  `VOTVCOOP_NET_PEER`, `VOTVCOOP_NET_PORT`, `VOTVCOOP_NET_NICK` (harness.cpp
  ReadEnv/ReadNetConfig/ReadNickname).
- Per-instance log file via `VOTVCOOP_LOG` (log.cpp LogPath) -- else both clobber
  votv-coop.log.
- `tools/capture-window.ps1 -ProcessId <pid>` -- capture each instance by PID (two
  windows share the title "Voices of the Void").
- Periodic `net stats: state=.. sent=.. recv=.. puppet=..` log line (play-net branch).

PASS criteria (the deterministic proof): host log "CONNECTED (host..." +
"Client joined the game"; client log "CONNECTED (client..." + "Host joined the game";
both net stats recv>0 + puppet=1. The DISTINCT nicknames crossing (host sees the
client's nick, client sees the host's) prove real cross-process exchange -- a
self-loopback can't produce it. Two VotV instances run fine on one machine (no
single-instance lock).

## Bugs the framework caught (loopback could NOT, and DID not)

1. **Client never locked its known peer.** Session::Start set `peerLocked_ =
   (role == Host)`, so a real CLIENT had peer_ resolved but peerLocked_=false ->
   `fromPeer` false -> it rejected the host's Hello and never adopted the token ->
   stuck Handshaking despite packets flowing. Loopback hid it: its host had
   peerLocked_=true via `initiate`. FIX: lock the peer whenever it's known up front
   (client or loopback host).

2. **Host never reached Connected.** The Connected transition fired only on a Hello
   echoing the host's token. But the client, the instant it adopts the token, flips
   to Connected and STOPS sending Hellos (handshake send is gated on !Connected) --
   it then sends only poses. So the host authenticated the client's poses but never
   saw a token'd Hello -> stuck Handshaking (recv huge, state=1, puppet=0). FIX: any
   valid-token message from the locked peer also confirms Connected (a correct token
   proves the peer received our Hello -- same trust as the Hello path, no new hole).

(A third issue was in the TEST checker only: after fix #2 the host's connect line
became "CONNECTED (host, via token'd msg)", which no longer matched the literal
"CONNECTED (host)" SimpleMatch -> false-negative FAIL while the coop actually worked.
Fixed to a prefix match.)

## Result

PASS: host + client (separate processes) connect over UDP, exchange the Join nickname
both ways ("Client joined" on host, "Host joined" on client), drive each other's
puppet, balanced bidirectional pose flow (~30 Hz), both state=Connected, puppet=1.
Screenshots show the remote puppet rendering in each window (both at the same story
spawn, so the puppet fills the view).

Methodology 5.2 (LAN soak) is now real. NEXT: distinct spawn / a host test-drive so
the puppet is visibly beside the player (cleaner visual); receiver-side interpolation;
true two-MACHINE run (this is two processes on one box -- loopback IP, real sockets).
See [[project-phase3-udp-transport]], [[project-coop-chat-feed]].
