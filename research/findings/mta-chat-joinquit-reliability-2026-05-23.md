# MTA precedent — chat, player join/quit, and per-packet reliability

Snapshot 2026-05-23. Source: `reference/mtasa-blue/` (Multi Theft Auto, the
conceptual precedent per CLAUDE.md). Captured to inform the VOTV coop chat +
session-event feed (the reliable channel over our custom UDP layer). Findings are a
point-in-time snapshot; the MTA source is authority if this drifts.

## 1. Chat

- Packet `PACKET_ID_CHAT_ECHO` (`Shared/sdk/net/Packets.h`). Server tx
  `CChatEchoPacket` (`Server/.../packets/`), client rx
  `CPacketHandler::Packet_ChatEcho()`, UI `CChat` (`Client/core/`).
- Reliability: `PACKET_RELIABLE | PACKET_SEQUENCED`, medium priority, on a
  **dedicated ordering channel `PACKET_ORDERING_CHAT`** -> RakNet
  `RELIABLE_ORDERED`. So chat is reliable + ordered, isolated from sync traffic so
  low-priority pose can't starve it.
- **Server is the exclusive authoritative relay**: a client sends its line to the
  server (`PACKET_ID_COMMAND`, reliable-ordered on the chat channel); the server
  echoes `CChatEchoPacket` (RGB colour + sender ElementID + type + text) to all.
  Clients never display their own chat until the server echoes it back -> anti-spoof
  + globally consistent ordering.

## 2. Player join / quit

- Join: `PACKET_ID_PLAYER_JOIN`/`JOINDATA`/`SERVER_JOINEDGAME`; the server
  broadcasts a `CPlayerListPacket` with `SetShowInChat(true)` to already-joined
  players. Quit: `PACKET_ID_PLAYER_QUIT` carrying the player id + a **reason enum**.
- Both are `PACKET_HIGH_PRIORITY | PACKET_RELIABLE | PACKET_SEQUENCED` ->
  `RELIABLE_ORDERED` (critical state changes must arrive).
- **Quit reason enum** (`eQuitReasons`): `QUIT_QUIT`, `QUIT_KICK`, `QUIT_BAN`,
  `QUIT_CONNECTION_DESYNC`, `QUIT_TIMEOUT`. The client maps the enum to the display
  string locally ("Quit" / "Kicked" / "Banned" / "Bad Connection" / "Timed Out"),
  fires `onClientPlayerQuit`, then removes the player. The quit *message text is
  generated client-side from the enum*, not sent as a string (saves bandwidth).

## 3. Per-packet reliability (the core principle)

`NetServerPacketReliability`: UNRELIABLE / UNRELIABLE_SEQUENCED / RELIABLE /
RELIABLE_ORDERED / RELIABLE_SEQUENCED. Per-packet flags `PACKET_RELIABLE`,
`PACKET_SEQUENCED`, priority bits; ordering channels `PACKET_ORDERING_{DEFAULT,
CHAT,DATA_TRANSFER,VOICE}`.

| Packet | Reliability | Why |
|---|---|---|
| Player/Vehicle Puresync, Keysync | UNRELIABLE_SEQUENCED | pose/input: latency-tolerant, drop OK, newest wins |
| ChatEcho | RELIABLE_ORDERED (CHAT channel) | conversation: loss/reorder unacceptable |
| PlayerJoin / PlayerQuit | RELIABLE_ORDERED (high prio) | critical state change, must be seen |
| ExplosionSync etc. | RELIABLE_ORDERED (high prio) | game-critical event |
| VoiceData | UNRELIABLE_SEQUENCED (VOICE channel) | drop OK for latency |

## What we mirror in VOTV coop (custom UDP, not RakNet)

- **Two reliability classes on one socket** (RULE 2: not two transports):
  - pose = unreliable, newest-wins via seq (= MTA puresync/keysync). DONE (Phase 3).
  - chat + system events (join/quit/error) = our `ReliableChannel` (reliable,
    ordered, ack+retransmit) = MTA's RELIABLE_ORDERED. DONE.
- **Quit/disconnect reason as an enum, message text generated locally** (MTA's
  client-side-from-enum). Our feed maps a local disconnect cause -> "left the game"
  / "timed out" / "connection error" rather than sending a string.
- **Host-authoritative chat relay** (MTA server-relay): when typed chat lands
  (future), the client sends to the host, the host echoes to all -> anti-spoof +
  consistent ordering. (v1 has no typed chat yet; the relay shape is reserved.)
- Deferred at our 2-peer LAN scale: MTA's *separate ordering channels* (chat vs
  sync) matter to stop priority inversion with many players; with 2 peers and a
  handful of control messages, one reliable stream + the unreliable pose stream is
  enough. Revisit if/when player count or reliable-traffic volume grows.

See `docs/COOP_SCOPE.md` (coop chat entry) and the Phase 3 net layer
(`src/votv-coop/.../coop/net/`).
