# Reference: VoidTogether-Server (rival VOTV multiplayer mod) — architecture summary

**Date**: 2026-05-25
**Source**: `reference/VoidTogether-Server/` (added 2026-05-25; vendored copy REMOVED from the repo 2026-07-19 per user directive; this doc is the surviving record)
**Author of source**: GatoDeveloper (community VOTV modder)
**License**: ISC
**Status of source**: README literally says "**THIS IS UNFINISHED, AND ONLY FOR THE VOIDTOGETHER SANDBOX CONCEPT DEMO**"

This is **NOT** a code blueprint we copy from — VOTV_MP and VoidTogether target
fundamentally different problems (coop story vs sandbox demo) with different
architectures (peer-host vs dedicated server, UDP+binary vs WebSocket+JSON,
60 Hz interpolated vs 10 Hz snap-apply). This doc captures the parts we
might want to BORROW or be AWARE OF when extending VOTV_MP into adjacent
features (dedicated-server mode, moderation, dev console, public-server
support).

---

## TL;DR

- **Architecture:** Dedicated Node.js server. WebSocket transport. JSON
  messages. 10 Hz tick. Stateless relay — the server doesn't run VOTV;
  it just forwards player/prop/door state between clients.
- **Scope:** Sandbox map (`MP_Construct`) only. README admits the story
  map (`untitled_1`) is "NOT SUPPORTED FULLY YET". No save sync, no NPC
  sync, no inventory sync, no event sync.
- **Stack:** `ws` (WebSockets), `discord.js`, `chalk`, `node-yaml-config`,
  `obscenity` (chat censor), `glob` (module autoloading), `readline`
  (interactive console). Modular plugin system: each `modules/<name>/*.js`
  exports a subclass of `ServerModule` that the loader auto-discovers.
- **Codebase size:** ~1,400 lines JS, very flat (zero abstraction layers
  worth mentioning).
- **Why look at it:** moderation features (perms / bans / chat commands /
  Discord webhook) and ops features (auto-restart, console, server
  browser query) are fleshed out — useful if VOTV_MP ever ships a
  dedicated-server mode for public servers. Almost nothing of the sync
  pipeline transfers to coop story.

---

## Section 1 — Network architecture (and why it doesn't apply to us)

### Transport: WebSocket (TCP) + JSON

`modules/socket/socketModule.js` uses the `ws` library. Every message is
`JSON.stringify({ requestType: "...", ...payload })`. The client sends
five request types:

| requestType  | Direction       | Payload (server-side handler)                 |
| ------------ | --------------- | --------------------------------------------- |
| `info`       | client -> server| serves `{title, motd, version, icon, defaultMap, online, maxOnline}` then closes the socket (server-list browser query) |
| `ping`       | client -> server| server responds `{}` (RTT measure)            |
| `join`       | client -> server| `{username, machine, password?, version}` -> auth flow |
| `update`     | client -> server| `{userId, userSecret, position, rotation, crouching, scale, model, flashlight}` |
| `chat`       | client -> server| `{userId, userSecret, message}` (optionally `!command`) |
| `propUpdate` | client -> server| `{userId, userSecret, props: [{networkId, name, position, rotation, scale, velocity, isLocked, data}]}` |
| `propDelete` | client -> server| `{userId, userSecret, networkId}`             |
| `doorUpdate` | client -> server| `{userId, userSecret, doors: [{index, open}]}` |

Server-to-client message types (in `tickModule.js` broadcast loop):

| requestType    | Payload                                                   |
| -------------- | --------------------------------------------------------- |
| `auth`         | `{userId, userSecret, tickRate}` — handshake completion   |
| `update`       | `{data: [{UserId, Username, Position, Rotation, Crouching, Scale, Model, Flashlight}]}` — all other players |
| `propUpdate`   | `{data: [NetworkedProp, …]}` — dirty props this tick      |
| `propDelete`   | `{networkId}`                                             |
| `chat`         | `{name, content, senderId}`                               |
| `ambient`      | `{time: 0..1}` — day fraction                             |
| `teleportPlayer` / `resizePlayer` | admin push                             |

**Cost:** every prop position update is roughly `{"networkId":"abc12345","name":"prop_food_canMonster_C","position":{"X":1234.56,"Y":-7890.12,"Z":345.67},"rotation":{...},"scale":{...},"velocity":{...},"isLocked":false,"data":{}}` — easily **300-500 bytes JSON per prop per tick**. At 100 props × 10 Hz = ~400 KB/s/peer on the server's outbound link. Multiply by N peers.

**For us:** we already chose UDP+binary in Phase 3. Our `EntityPoseBatch`
design (one aggregated bitstream per tick) is ~10-20× tighter and
explicitly necessary for our 100-entity coop target
([[project-coop-scale-100-entities]]). Their design does not scale past
"a few sandbox props". Don't borrow the transport.

### Server-authoritative? Not really — "trust-on-write" relay

Look at `propModule.js:101 UpdatePropData(data)`:

```js
this.ActiveProps[PropIndex].position = data.props[i].position;
this.ActiveProps[PropIndex].rotation = data.props[i].rotation;
// ... no NaN check, no bounds check, no rate limit, no ownership check
this.ActiveProps[PropIndex].lastOwner = data.userId;
```

Whatever the client says, the server caches and re-broadcasts to peers
next tick. Same for player updates (`playerModule.js:89 UpdatePlayerData`).
The only auth gate is `userId` + `userSecret` (a 16-char random token
issued at join). After that handshake, the client is trusted to write
any field including its own scale (`SizeCommand.js`).

**For us:** [[feedback-code-with-agents-and-security]] is the relevant
standing rule — anti-spoof token, source filtering, bounded recv,
NaN/AABB validation are all designed into our session/transport layer
(`coop/net/{protocol,transport,session}`). Their relay model would fail
our audit immediately. Don't borrow.

### Interpolation: none

`playerModule.js:89 UpdatePlayerData` writes the received position/
rotation directly into the cache. `tickModule.js:42 BroadcastSyncAllPlayers`
forwards it to every other peer. On the client side
(client code not in this repo), the receiver presumably snaps to that
pose. At 10 Hz tick = ~100 ms between updates = visibly choppy without
client-side LERP. Their README admits the demo is unfinished.

**For us:** MTA-pattern receiver-side LERP buffer is shipped
([[project-remote-player-interp]]). 75 ms window, snap-on-teleport,
shortest-arc yaw. The `RemotePlayer::SetTargetPose` + `Tick()` two-call
shape mirrors MTA's `CClientPed::SetTargetPosition` + `Interpolate` —
their design and ours diverged before either started.

### Tick: 10 Hz, single setInterval

`tickModule.js:10` — `setInterval(this.TickUpdate, 1000 / 10)`. One
function runs Uptime, Time, Prop sync, Door sync, Player disconnect
sweep, Player sync in one shot. Game time of day broadcast at half-tick
(every other invocation).

**For us:** 60 Hz pump (one pump in the game loop on the host;
`NetPumpTick` per game-thread tick). Engine-thread aware. We share the
game thread with VOTV so we have to be careful — they don't, because
their server isn't a peer.

---

## Section 2 — Auth, identity, and moderation (interesting parts)

### Identity: Machine ID

Auth (`authModule.js:25`):

```js
if (!data.machine || data.machine.length != 64) { ws.close(); return; }
```

Each client sends a 64-character "machine ID" — based on README context,
a SHA-256 (or similar) hash of some stable per-install identifier
(Windows MachineGuid + CPU ID is the typical recipe). The server uses
it as:

1. **The ban-list key** (`banList.json` stores `{machine, lastUsername, reason}` rows).
2. **The permission-list key** (`permissions.json` maps machine ID -> permission groups).
3. **An anti-rejoin gate for kicks / bans** (server kicks the socket; client must reconnect; ban check rejects same machine).

**Username is decoration only.** The client picks any username they want
(censored + truncated server-side in `utilityModule.js`). The machine ID
is the durable identity.

**Why this might matter for us:** if VOTV_MP ever ships a public/
dedicated-server mode, we need a stable identity for ban/admin lists.
We could derive a "machine ID" the same way:

- Windows: `HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid` (registry).
- SHA-256 it + maybe HMAC with a server secret to prevent cross-server
  correlation.
- Send in the Join reliable message.

Anti-pattern note: machine ID is NOT a strong identity. Reinstall
Windows = new ID. A determined ban-evader can reset it. SCP:SL has the
same limitation; it's good enough for casual moderation.

### Permission system: SCP:SL / Minecraft style

`permissionsModule.js` implements dotted-permission matching with
wildcards:

- `VoidTogether.User` — base perm
- `VoidTogether.Admin.Ban` — specific subperm
- `VoidTogether.Admin.*` — wildcard match (everything under Admin)
- `*` — superuser

`CheckPermSatisfied` (line 66) splits by `.`, walks chunk-by-chunk,
returns true on the first `*` chunk. Reasonable algorithm; the bug at
line 75 (`checkLength = min(perm.length, check.length)`) means
`VoidTogether.Admin.Ban` is satisfied by a perm string `VoidTogether.Admin`
without a trailing wildcard, which is probably wrong but not security-
critical for their threat model.

The permissions file shape:

```json
{
  "users": {
    "default": ["VoidTogether.User"],
    "<machine-id-hex>": ["VoidTogether.User", "VoidTogether.Admin.*"]
  }
}
```

`default` is the fallback if the machine ID has no entry.

**Possibly useful for us:** if we add a "host's friends list" or "admin
the host can hand out", this dotted-perm pattern is a cheap, well-known
shape. We wouldn't need the JSON file; could be a simple in-memory map
the host configures via the mod menu.

### Ban list

`banModule.js` — `{machine, lastUsername, reason}[]` in `banList.json`.
Checked at join (`authModule.js:53`); banned machines get the socket
closed immediately.

Trivial design; the only nuance is storing `lastUsername` so the admin
can recognise who's in the list later.

### Chat commands + console commands

Commands are individual files under `modules/commands/<category>/*.js`.
Each exports an object with `{Name, Aliases, Description, RequiredPermissions, AllowConsole, AllowClient, Callback}`.

`commandModule.js:27` glob-loads them at startup. `chatModule.js:62` —
if a message starts with the configurable prefix (default `!`),
dispatch to the command handler. Console (`consoleModule.js`) calls
the same handler with `isConsole=true`.

Bundled commands:

- `admin/` — Ban, Kick, ClearProps, Restart, Stop
- `ambient/` — SetTime
- `fun/` — Spawn-prop, Size, SizeAll, TpAll, TpHere
- `general/` — Help

This is a clean, simple chat-command pattern (resembles SourceMod /
SCP:SL plugin shape). If we add admin chat commands to VOTV_MP, this
is roughly the right shape — though we'd skip the per-file glob and
just use a static command registry (no need to support runtime
addition).

### Discord integration

`discordModule.js` — two integrations:

1. **Bot** (presence): logs in, sets rich presence "X/Y Players" every 15s.
2. **Webhook** (logging): every join/leave/chat/ban posts an embed to a
   Discord channel.

Out of scope for us short-term, but a nice pattern if we ever do
self-hosted public servers and want server-admin tooling.

### Auto-restart

`uptimeModule.js:24` — at config-defined `autoRestartTimer` minutes,
warn at T-1m, restart at T. The restart mechanism (`server.js:29
RestartServer`) is `process.exit(69420)`; the cluster master forks a
fresh worker when it sees that exit code.

**Useful pattern:** a graceful announce-then-restart, if we ever ship
dedicated.

### Server browser support

`infoModule.js:17 SendServerInfo`:

```js
ws.send(JSON.stringify({
    title, motd, version, icon: base64(servericon.png),
    defaultMap, online, maxOnline
}));
ws.close();
```

The client opens a temp socket, sends `{requestType: "info"}`, receives
the metadata, displays in a server-list UI. Closing the socket after
the response is a nice touch — no idle connections for browse-only
clients.

**Useful pattern:** if we add a public-server browser to VOTV_MP, this
is the protocol shape. (Server icon as base64 is cute but the actual
bandwidth at scale would be better-served by a CDN.)

---

## Section 3 — Gameplay sync features (and why most don't transfer)

### Player sync (227 lines)

Per-player fields broadcast each tick:
`{userId, username, position, rotation, crouching, scale, model, flashlight}`

`username` is censored at join (`obscenity` library) and stripped of
non-alphanumerics. `scale` and `model` are sandbox features (size
manipulation; player model selection for the demo's puppet system).
`flashlight` is a boolean state.

**For us:** our `PoseSnapshot` carries position + actor yaw + head yaw
delta + pitch + speed + (planned: crouch bit + flashlight bit + a few
more state bits). Same set of dimensions, more compact wire format,
already shipped. No code-borrow.

### Prop sync (213 lines)

`SpawnProp(name, pos, rot, scale, velocity, data, ownerId)` — server
allocates an 8-char `networkId`, stores in `ActiveProps[]`, broadcasts
on next tick. Special case: `cq_0` (the sandbox CQ vehicle) gets
default `{battery: 100, health: 100, light: false, brake: false}` data.

`UpdatePropData` is the per-tick prop sync from client. The pattern is
clever: if a client sends `networkId: "*"`, the server treats it as a
spawn request — `propModule.js:114`:

```js
if (data.props[i].networkId == "*")
    this.SpawnProp(...);
```

This lets a client spawn a prop and let the server assign the ID, then
the next propUpdate from peers receives the assigned ID.

**Comparison to our design:**

- They use a random 8-char `networkId` issued by the server.
- We use VOTV's own `Aprop_C.Key @ 0x02E0` — a UUID baked into the save
  file, **cross-peer stable** (proven via lan-test:
  `Xym5dmnBEzreWaHSkQ9Tew` for the suitcase across two independent
  processes). See [[project-physics-object-pickup]].
- Ours requires no allocation handshake — both peers already know the
  Key. Theirs does (`*` -> server assigns -> echo back).
- Ours can't drift (Key is content-addressable). Theirs can if a
  packet is lost.

**For us:** save-UUID-as-key is strictly better for VOTV-spawned
content. Their `*`-spawn pattern WOULD be useful for **transient
runtime-spawned props that aren't in any save** (e.g., admin
`!spawnprop` command, debug spawns). If we add such a feature we can
borrow the shape.

`isLocked` bit on props is interesting — looks like a "this player is
holding this; nobody else can grab it" lock. We use a richer
`PropOwnership` model (grab observers, holder transfer events) so the
bit-level encoding doesn't transfer directly, but the CONCEPT (one
authoritative holder at a time) is the same as our grab-release path.

### Door sync (103 lines)

`doorsModule.js` — minimal. Doors are identified by an `index` (an int,
presumably an array index into the level's doors list). State is a
single `open: bool`. Broadcast each tick if `needUpdate` set.

**For us:** VOTV doors are `Adoor_*_C` props caught by the prop
lifecycle observer. We treat them as state on the prop, not as a
separate channel. Their model is simpler because their sandbox map has
fewer prop types overall.

There's a bug in `doorsModule.js:17 FindIndex`:

```js
for(let i=0;i<this.ActiveDoors;i++) {  // should be this.ActiveDoors.length
    if (this.ActiveDoors[i].index == SearchIndex)
        return i;
}
```

`i<this.ActiveDoors` (without `.length`) compares int to array, the
loop never runs, `FindIndex` always returns -1, every door packet
creates a new entry. Door sync is effectively broken in this codebase
(unless this is fixed in a downstream branch we don't have).

### Time of day sync

`timeModule.js` — configurable `dayLength` in minutes; `dayStartingPercent`
sets where day 1 starts. Broadcast as a normalised `time: 0..1`
fraction every other tick.

The time scaling is server-authoritative — clients receive the
fraction and apply it to their own day-night system locally. No client
RNG involvement (the demo doesn't have weather / events to RNG).

**For us:** we need this for coop story too. The host owns the save
([[project-coop-save-host-authoritative]]), so the host owns the time.
A simple `ServerTimePacket` with `dayFraction + dayNumber` is roughly
the right shape. We could borrow this pattern verbatim.

### Other features they have, we don't (yet)

- `!size` / `!sizeall` — admin scale change. Sandbox toy; we won't
  build this for coop.
- `!tpall` / `!tphere` — admin teleport. Useful for coop if a peer
  desyncs; could be a future feature.
- `!cleanprops` — wipe all server-tracked props. Sandbox toy; the
  coop equivalent is "host force-resyncs world" which is
  [[project-coop-mushroom-desync-and-remedy]] (reconnect-based).

---

## Section 4 — Patterns worth borrowing (concrete checklist)

If we ever extend VOTV_MP toward dedicated-server / public-server
support, these are the cherry-pick candidates from VoidTogether:

1. **Server-list browser protocol** (`info` request -> metadata blob ->
   close). The shape is straightforward; we'd use UDP for it (single
   packet round-trip) instead of WebSocket. ETA: 1 day if we ever
   decide to build a public-server mode.

2. **Machine ID identity** for ban / admin / persistent identity.
   Derived from `HKLM\…\MachineGuid` + HMAC. **Not in scope** until
   we add a server-list browser; we can rely on IP filtering for LAN.

3. **Dotted-permission system with wildcards** for admin chat
   commands. Pattern is well-known (SCP:SL / Minecraft) and we don't
   need anything fancier.

4. **Chat command dispatch** with prefix detection +
   permission check + per-command handler. We already have a chat
   channel ([[project-coop-chat-feed]]); the command-prefix split
   would be ~50 lines on top.

5. **Time-of-day sync** as a simple `dayFraction` broadcast. Will
   ship with Phase 5S0 (host save authority) regardless of whether
   we look at VoidTogether.

6. **Auto-restart with announce-then-restart** for any future
   dedicated-server mode.

7. **`*`-spawn-allocates-ID pattern** for runtime-spawned props with
   no save UUID. Only relevant if we add admin commands that spawn
   props or any other purely-runtime spawned entities.

---

## Section 5 — Patterns to AVOID (concrete anti-patterns)

1. **WebSocket + JSON** for high-frequency game state. We're already on
   UDP+binary, our choice was correct, do not regress.

2. **No interpolation on the client side** — already addressed; our
   MTA-pattern LERP is shipped.

3. **Trust-on-write** prop / player updates. The server caches whatever
   the client claims. Our session layer already validates everything.

4. **10 Hz tick** for pose. Player movement is visibly choppy at 10 Hz
   without aggressive client extrapolation; their demo doesn't have
   the latter.

5. **Single global TickUpdate** mixing time + props + doors + sync. Fine
   at their scale (sandbox demo); breaks down past a few peers.

6. **Glob-load command files per startup with reflection**
   (`globSync` -> dynamic import). It's clean but in a compiled C++ mod
   we don't need it; a static registry is simpler and gives us
   compile-time safety.

7. **`FindIndex` bug pattern** (forgetting `.length` on the array)
   exists in `doorsModule.js:17`. Cautionary: the JS `for` loop is
   easy to typo. C++ for-loops + range-based-for largely avoid this.

---

## Section 6 — Source file map (for future drilldowns)

| File                                                       | LOC | What it does                                       |
| ---------------------------------------------------------- | ---:| -------------------------------------------------- |
| `server.js`                                                |  53 | Entry; cluster fork for auto-restart; module bootstrap |
| `serverconf.yml`                                           |  39 | Config: name, motd, port, max players, tick rate, etc. |
| `modules/moduleHandler.js`                                 | 117 | `ServerModule` base class + glob auto-discovery     |
| `modules/socket/socketModule.js`                           |  61 | `ws` WebSocket server + requestType dispatch        |
| `modules/auth/authModule.js`                               |  80 | Join handshake: pw, machine, version, ban, slot     |
| `modules/player/playerModule.js`                           | 227 | `PlayerObject` + per-tick sync + teleport / scale   |
| `modules/props/propModule.js`                              | 213 | `NetworkedProp` + per-tick sync + `*`-spawn pattern |
| `modules/doors/doorsModule.js`                             | 103 | Door open/close sync (has a bug: see Section 3)     |
| `modules/chat/chatModule.js`                               |  73 | Chat broadcast + command prefix detection           |
| `modules/commands/commandModule.js`                        | 122 | Command auto-discovery + dispatch + perm check      |
| `modules/commands/admin/{Ban,Kick,ClearProps,Restart,Stop}.js` |  ~ | Admin commands                                      |
| `modules/commands/ambient/SetTimeCommand.js`               |   ~ | Admin: set day fraction                            |
| `modules/commands/fun/{SizeAll,Size,SpawnProp,TpAll,TpHere}.js` |   ~ | Sandbox toy commands                                |
| `modules/commands/general/HelpCommand.js`                  |   ~ | List commands                                       |
| `modules/permissions/permissionsModule.js`                 |  99 | Dotted-permission matcher + wildcard support        |
| `modules/bans/banModule.js`                                |  76 | `banList.json` storage + check                      |
| `modules/discord/discordModule.js`                         |  81 | discord.js bot + webhook                            |
| `modules/info/infoModule.js`                               |  28 | Server-list browser query response                  |
| `modules/uptime/uptimeModule.js`                           |  40 | Auto-restart timer with warning                     |
| `modules/ticks/tickModule.js`                              |  43 | Global TickUpdate driver (10 Hz)                    |
| `modules/ticks/timeModule.js`                              |  40 | Day-fraction tracking + broadcast                   |
| `modules/console/consoleModule.js`                         |  53 | Interactive server console for admins               |
| `modules/utility/utilityModule.js`                         |  48 | Truncate, clamp, regex strip, swear censor          |

---

## Bottom line

VoidTogether is a **sandbox demo with a real moderation layer**. Their
sync pipeline is the part that aged out of relevance for us before they
finished writing it — JSON over WebSocket at 10 Hz can't carry coop
story content (NPCs, saves, events, 100-entity scale). Their ops layer
(perms, bans, commands, Discord, console, server browser, auto-restart)
is the part to look at when VOTV_MP grows in that direction.

Cross-refs:
- [[project-votv-coop-foundation]] — our starting point.
- [[project-coop-scale-100-entities]] — why their 10 Hz JSON design
  can't scale to where we need to go.
- [[project-coop-save-host-authoritative]] — our save model; their
  sandbox doesn't need this.
- [[project-coop-inventory-private]] — we explicitly DO NOT sync
  inventory; their model has no inventory at all.
- [[project-phase3-udp-transport]] — our transport; their transport.
- [[project-coop-chat-feed]] — our chat; their chat + commands.
- [[project-remote-player-interp]] — our LERP; they have none.
