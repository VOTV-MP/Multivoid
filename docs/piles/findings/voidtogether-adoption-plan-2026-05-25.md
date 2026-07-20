# VoidTogether-Server -> VOTV_MP adoption plan

**Date:** 2026-05-25
**Source:** `reference/VoidTogether-Server/` (rival sandbox VOTV multiplayer mod, Node.js/WebSocket; vendored copy REMOVED from the repo 2026-07-19 per user directive -- this doc + the RE doc are the surviving record)
**Prior RE doc:** `research/findings/network/voidtogether-server-RE-2026-05-25.md` (architecture summary + per-file map)
**User directive:** "from that voidtogether mod we can take CHAT, PERMISSIONS other stuff (ask agents)"

> **Phase-number note (added 2026-07-20).** This doc is a POINT-IN-TIME record and its 13 "Phase 7+"
> references are from the pre-2026-07-20 project arc, which has since been renumbered (the arbiter
> became phase 2; the old phase 8 was retired as a phase). Read every "Phase 7+" here as **"the
> public-server phase"** — the numbers are off by one against the current `docs/ROADMAP.md`. Left
> unrenumbered on purpose: this is a dated record, not a live plan.

This doc takes each candidate feature from VoidTogether's ops/moderation
layer, scores it against VOTV_MP's coop story scope, lists the concrete
build cost (files + function-level outline + estimated LOC + deps), and
ranks the shortlist by value/effort.

Architectural difference is the constant backdrop: VoidTogether is a
**dedicated relay server with N untrusted clients**; VOTV_MP is a
**peer-to-peer LAN coop with 2 trusted-ish peers, host-authoritative**.
Anything that needs a dedicated-server model (rich moderation, Discord,
server browser metadata) is naturally weaker fit until VOTV_MP grows a
dedicated-server target in Phase 7+. Anything that fits a 2-peer
host-authoritative model (commands, chat, time-of-day) ports cleanly.

Cross-refs: [[project-coop-chat-feed]] (existing reliable channel +
on-screen feed), [[project-phase3-udp-transport]] (our transport),
[[project-coop-save-host-authoritative]] (host owns world truth),
[[feedback-check-mta-and-document]] (MTA precedent required),
[[feedback-code-with-agents-and-security]] (security designed in).

---

## Current VOTV_MP state (integration starting point)

The pieces relevant to chat/commands/moderation that already exist:

- `src/votv-coop/include/coop/net/protocol.h` -- wire format, with a
  `ReliableKind` enum (Join, PropRelease, PropSpawn, PropDestroy). New
  reliable kinds drop in here as new enum values.
- `src/votv-coop/include/coop/net/reliable_channel.h` -- stop-and-wait
  ARQ over the same UDP socket; `Session::SendReliable` + `TryGetReliable`
  is how all reliable traffic moves.
- `src/votv-coop/src/coop/event_feed.cpp` -- game-thread pump that
  drains the reliable inbox and routes by `ReliableKind`. New kinds
  add a `case` here.
- `src/votv-coop/src/ue_wrap/hud_feed.cpp` + `include/ue_wrap/hud_feed.h`
  -- screen-space UMG overlay (top-right multi-line text). `Push(line)`
  is the only API.
- `src/votv-coop/src/coop/net/session.cpp` + `session.h` -- handshake,
  peer-lock, session token, RTT. Knows the local nickname via
  `event_feed::SetLocalNickname`.
- **Typed chat input is NOT yet shipped.** Per
  [[project-coop-chat-feed]] the user deferred typed input; v1 was
  the reliable channel + system event feed (join/leave). Anything
  that needs the player to type text crosses that build first.

What does NOT exist: chat text channel, command dispatch, permissions
table, ban list, machine ID, server console, Discord, dedicated-server
binary, server browser metadata, auto-restart, time-of-day sync.

---

## Feature-by-feature assessment

### 1. Chat text channel + command dispatch

**Feature:** Player types `hello` -> broadcast to all peers. Player
types `!ban Bob spam` -> dispatch to the `ban` command handler.
VoidTogether (`chatModule.js:62`) detects the prefix character
(configurable, default `!`) on the first byte and routes to
`CommandModule.HandleCommandMessage`; otherwise broadcasts.

**Fit:** HIGH (chat text), HIGH (command dispatch shape).

**Why borrow:** Typed chat is already on our roadmap
([[project-coop-chat-feed]] "NEXT"). Command dispatch is the natural
follow-on -- once a player can type, `!something` is one more `if`
on the first char. The pattern (prefix detect -> registry lookup ->
perm check -> handler) is well-trodden (Source/Minecraft/SCP:SL/MTA
all do it), simple, and gives us a place to hang every admin feature
below.

**What we'd need to build:**
- `coop/net/protocol.h` -- new `ReliableKind::ChatText` with payload
  `{uint8 nickLen, char[nickLen] nick, uint16 textLen, char[textLen]
  text}`. ~10 lines.
- `coop/chat_input.cpp` + `.h` (NEW) -- engine input hook on Enter
  to focus/blur a UMG editable text field; submit -> `Session::Send
  Reliable(ChatText, payload)`. Pattern reused from `ue_wrap/hud_feed`
  (NewObject UMG; AddToViewport with HitTestInvisible flipped to
  Visible while focused so it captures keys; restore on blur). ~150
  lines + UMG widget build via reflection.
- `coop/event_feed.cpp` -- new `case ChatText:` in the drain loop:
  call a helper that splits into command vs broadcast and either
  dispatches to `coop::commands::Execute(text, sender)` or calls
  `hud_feed::Push(L"<" + nick + L"> " + text)`. ~30 lines.
- `coop/commands/registry.cpp` + `.h` (NEW) -- a static
  `std::unordered_map<std::wstring, CommandDef>` of `{name, aliases,
  required_perm, handler_fn_ptr}`. No glob loading; static
  registration via a `REGISTER_COMMAND(name, perm, fn)` macro in each
  command's TU. ~80 lines.
- One `commands/<group>/help.cpp` per command (see feature 2 onward).

**Total:** ~280 LOC (excluding individual command bodies); needs the
UMG focus+input dance, which is the biggest unknown.

**MTA precedent:** `Server/mods/deathmatch/logic/CCommands*` runs
the registered command list per chat line; `CGame::HandleChat`
detects the `/` prefix. Client-side `CClientChat` owns the input
widget + the chat history; commands route over RPC. Same shape as
VoidTogether minus the colour-tag string format (which we won't
borrow -- see "decline" list below).

**Scope fit:** Phase 6 (post-coop-stability ops layer). Useful even
in LAN coop for quick host->client communication that doesn't need
voice. Voice chat is a separate future feature
([[project-voice-chat-future]]).

---

### 2. Permissions system (dotted-perm wildcards)

**Feature:** Each user maps to a list of permission strings like
`["VoidTogether.User", "VoidTogether.Admin.*"]`. Commands declare
required perms (`["VoidTogether.User", "VoidTogether.Admin.Ban"]`).
A check walks each user perm chunk-by-chunk, returning true on `*`.
JSON-backed file (`permissions.json`); machine ID is the key, with
a `"default"` fallback.

**Fit:** MEDIUM. The PATTERN ports cleanly; the SCOPE is overkill
for a 2-peer LAN coop where the host is always admin and the client
is always not. The pattern earns its keep only when (a) we add a
dedicated-server mode with N untrusted players, OR (b) the host
wants to grant the client SOME admin-ish powers (e.g. `!tphere`
host but not `!kick` host).

**Why borrow (the pattern, not the JSON file):** It's the right
shape if we ever add more roles than `{host, client}`. The
algorithm is ~40 lines of C++; no point inventing a different one.
The JSON file machinery is the part we'd skip until dedicated mode.

**What we'd need to build (when needed):**
- `coop/permissions/perms.cpp` + `.h` (NEW) -- `bool Check(const
  std::vector<std::wstring>& userPerms, const std::wstring& required)`
  doing the chunk walk. ~40 lines + tests.
- Two hardcoded role tables for LAN coop: `Host` -> `[*]`, `Client`
  -> `[VoidTogether.User]` (or whatever namespace we pick -- I'd
  use `"votv.coop.*"` to avoid identity confusion with the rival
  mod). ~10 lines.
- (Dedicated mode only): permissions JSON I/O via nlohmann/json or
  the system already used for `votv-coop.ini`. Keyed by machine ID.
  ~50 lines.

**Total:** ~50 LOC for the LAN-coop case; another ~50 if we later
add dedicated-server JSON storage.

**MTA precedent:** `CAccessControlList*` (8 files in
`reference/mtasa-blue/Server/mods/deathmatch/logic/`) -- richer than
VoidTogether (ACL groups, accept/deny ordering, resource-specific
ACLs) but the same conceptual shape: each user -> set of rights;
commands check rights at dispatch. MTA uses dot-separated rights
like `command.kick` matched literally, no `*` wildcards in the
default ACLs (groups give you the multi-permission grouping
instead). Two ways to skin the same cat; VoidTogether's wildcards
are simpler and we won't have the resource-pluralisation MTA does.

**Scope fit:** Pattern shipped with feature 1 (commands need a
gate); JSON-file machinery deferred to Phase 7+ (dedicated server
mode).

**Watch out:** the bug noted in the RE doc -- `checkLength = min(
perm.length, check.length)` makes `VoidTogether.Admin` satisfy
`VoidTogether.Admin.Ban` even without a trailing `.*`. Easy enough
to require the comparison to consume ALL of the requested perm
before considering it satisfied. Fix when porting.

---

### 3. Ban list (machine-ID-keyed JSON)

**Feature:** `{machine, lastUsername, reason}[]` rows in
`banList.json`. Checked at the join handshake; banned machines get
the socket closed.

**Fit:** LOW for LAN coop, MEDIUM-HIGH for dedicated mode.

**Why not borrow now:** In a 2-peer LAN coop the host already
chose who to play with by typing their IP. A ban list adds zero
value -- the host just doesn't share the IP next time. The
machine-ID-keyed ban list earns its keep only when there's a public
server with strangers showing up.

**What we'd need to build (when needed):**
- Machine ID derivation (feature 9 below) -- prerequisite.
- `coop/bans/banlist.cpp` + `.h` (NEW) -- `{string machine, string
  lastUsername, string reason}` rows in `bans.json` (under the mod
  data dir, same place as `votv-coop.ini`). Load on session start,
  save on `Ban()`/`Unban()`. ~80 lines + JSON dep.
- `coop/net/session.cpp` -- in the Hello handler (after
  `peerLocked_`), look up the joining peer's machine ID in the ban
  list; if hit, send a `Bye` with a reason code (new
  `ReliableKind::Bye` payload extension) and `peerLocked_ = false`.
  ~25 lines.

**Total:** ~100 LOC + JSON lib.

**MTA precedent:** `CBan` + `CBanManager`
(`reference/mtasa-blue/Server/mods/deathmatch/logic/CBan.cpp`) --
keys on IP / IP-range / serial / username (multi-key, more
flexible). The "serial" is MTA's machine-fingerprint hash, same
concept as VoidTogether's `machine`. Per-row metadata: banner name,
ban reason, ban time, unban time, optionally an expiry. Take MTA's
multi-key + expiry shape over VoidTogether's machine-only shape if
we ever ship this -- the IP and username variants are useful
fallbacks.

**Scope fit:** Phase 7+ (dedicated-server / public-server). Don't
build before then.

---

### 4. Server browser protocol (info request)

**Feature:** Client opens a temp socket, sends `{requestType:
"info"}`, server sends back `{title, motd, version, icon (base64),
defaultMap, online, maxOnline}` and closes the socket. The client
library uses this to populate a server-list UI without committing
to a full join.

**Fit:** MEDIUM. Per `docs/COOP_SCOPE.md`, the public-server
browser is in scope but flagged as Phase 7+ (WAN concern). The
info-request shape is exactly what our master server needs to
expose for each opted-in host.

**Why borrow (the shape, not the JSON+WebSocket):** It's the
correct protocol minimalism: one request, one reply, close. We
won't use WebSocket -- one UDP request/reply over a known port
(or a single TCP connect on a dedicated info port) carries the
same info more cheaply. The METADATA SCHEMA (title, motd, version,
icon, current/max players, map) is the right set.

**What we'd need to build:**
- `coop/net/protocol.h` -- new `MsgType::InfoQuery` (no payload)
  and `MsgType::InfoReply` with payload `{u8 titleLen, char[],
  u8 motdLen, char[], u16 version, u32 iconLen, u8[iconLen]
  PNG bytes, u8 mapLen, char[], u8 online, u8 maxOnline}`. ~30 lines.
- `coop/net/session.cpp` -- the host responds to `InfoQuery` (no
  session token required; sender's address learned from `recvfrom`
  -> reply to that address; do NOT lock the peer; do NOT advance
  any session state). One-shot, stateless. Rate-limited to N
  per-IP-per-second to avoid amplification. ~50 lines.
- `coop/info_query.cpp` + `.h` (NEW, client-side helper) -- send
  one InfoQuery, wait up to 1 s for InfoReply, return the parsed
  metadata or timeout. For driving the server-browser UI. ~80
  lines.

**Total:** ~160 LOC.

**MTA precedent:** MTA has the ASE (All-Seeing Eye) browser
protocol -- one UDP packet, server replies with all metadata, the
master server aggregator hits each registered server periodically.
Same shape as VoidTogether's info, multiplied. Our master server
can lift this directly. `CMasterServerAnnouncer.h` is the
heartbeat side.

**Scope fit:** Phase 7+. Builds with the master-server work. Not
before LAN+direct-IP ships solidly.

**Security note:** UDP info-query is an **amplification vector** if
the reply is much bigger than the request (and ours would be --
server icon PNG ~10 KB vs 20-byte query). Mitigation: drop server
icon from the info-reply (or limit it to ~1 KB), require token-
in-query for any larger payloads, hard rate-limit per source IP.
[[feedback-code-with-agents-and-security]] applies.

---

### 5. Discord webhook logging

**Feature:** Each join/leave/chat/ban posts an embed to a Discord
webhook URL. No interactive bot needed (just outbound HTTP).
VoidTogether's `discordModule.SendWebsocket(color, author, title,
description)` builds an embed via `discord.js` `EmbedBuilder`.

**Fit:** LOW for LAN coop, MEDIUM for dedicated mode.

**Why not borrow now:** A 2-peer LAN coop doesn't need an audit
log in someone's Discord. A dedicated server hosting strangers
absolutely does (so the admin can see who joined/left/spam'd while
they were away).

**What we'd need to build (when needed):**
- C++ HTTP client. Options:
  - **WinHTTP** (Windows API, already a system DLL) -- minimal,
    ~50 lines for "POST JSON to URL". Adds no dep.
  - **libcurl** -- more capable; adds a build dep but is the
    standard. Probably overkill for one POST.
  - The standalone build is Windows-only ([[reference-standalone-build]])
    so WinHTTP is the obvious choice.
- `coop/discord/webhook.cpp` + `.h` (NEW) -- `PostEmbed(const
  std::wstring& webhookUrl, int color, const std::wstring& author,
  const std::wstring& title, const std::wstring& description)`,
  building a minimal embed JSON by hand (no JSON lib needed for
  outbound). Fire-and-forget on a background thread (no callback
  needed). ~120 lines.
- Hook calls from `event_feed.cpp` (on Join/Bye drains), from
  `chat_input` (on broadcast), from `coop/bans` (on Ban) -- each
  is a single `if (webhookUrl) PostEmbed(...)`. ~20 lines.

**Total:** ~150 LOC + WinHTTP usage. Optional config:
`net.discord_webhook=https://...` in `votv-coop.ini`.

**MTA precedent:** MTA's `logger` resource (Lua) writes to file
and optionally posts to IRC. The Discord embedding is community
add-on; not in the core. So no direct C++ precedent in MTA's tree
-- mirror VoidTogether's payload shape.

**Scope fit:** Phase 7+ (dedicated mode). Skip the Discord BOT
(rich presence, slash commands -- their `client.login` path); the
webhook alone is 80% of the value at 20% of the complexity.

---

### 6. Discord bot rich presence

**Feature:** Logs into Discord as a bot, sets presence "X/Y
Players" every 15s.

**Fit:** LOW.

**Why not borrow:** Rich presence is community polish, not
substantive ops. A server admin who wants player counts can get
them from the webhook log or the server browser. The Discord bot
also requires gateway connection management (websocket keepalive,
reconnect, scope/permissions), which in C++ means writing or
linking a Discord gateway lib -- way out of proportion to the
payoff.

**Recommendation:** Decline. Not borrowed.

**Scope fit:** Not in scope.

---

### 7. Auto-restart (announce-then-restart on timer)

**Feature:** After N minutes (`autoRestartTimer`), warn at T-1m
("Server restarts in 1 minute"), restart at T. Restart =
`process.exit(69420)`; the parent cluster master forks a fresh
worker on that exit code.

**Fit:** LOW for LAN coop, MEDIUM for dedicated mode.

**Why not borrow now:** Our coop session is the host's
single-player game with one extra peer. Restarting it is
restarting VOTV itself -- nothing the mod can do alone, and the
host's session has a save the host doesn't want to lose. The
auto-restart pattern only makes sense for a dedicated-server
binary that has no human at the keyboard.

**What we'd need (dedicated mode only):**
- A dedicated-server build target (a separate exe + .pak setup, or
  a `--dedicated` flag on VOTV that runs the server-side mod
  without the client side). This is itself an architecture lift
  that VoidTogether's existence doesn't change.
- `coop/uptime/uptime.cpp` + `.h` (NEW) -- start-time + per-tick
  check + warn-and-exit path. ~40 lines.
- Process supervisor (Windows service or a small batch loop) to
  re-launch on exit. Out-of-process; not C++.

**Total:** ~40 LOC of mod code + a separate dedicated-server
build effort that dwarfs this feature.

**MTA precedent:** MTA's `mta-server.exe` does NOT auto-restart
itself; admins use systemd / service supervisors / `restarter`
resources. Same conclusion: out-of-process concern.

**Scope fit:** Phase 7+ and only after dedicated mode ships.

---

### 8. Time-of-day sync (dayFraction broadcast)

**Feature:** Server tracks day length (configurable
`dayLength` minutes); broadcasts a normalised `time: 0..1`
fraction every other tick. Server-authoritative; clients apply
the fraction to their day-night system. Admin command `!time
night|day|dawn|dusk|<0..1>` jumps the time.

**Fit:** HIGH. Time-of-day is a state the host already owns
([[project-coop-save-host-authoritative]]) and which directly
affects gameplay (NPCs that spawn at night, the satellite dish
gameplay loop). Without host->client sync, clients see a
different time-of-day than the host and the world doesn't agree.

**Why borrow:** It's a simple, host-authoritative, low-rate
broadcast -- exactly the shape we want. Drop-in.

**What we'd need to build:**
- Reverse-engineer VOTV's time-of-day system first. RE candidates:
  the lighting `ASkyLight` actor, `ADirectionalLight` (sun
  rotation), or whatever `gameInstance` field carries the day
  fraction. Likely a `float` field on the GameInstance or the
  GameMode. ~half-day RE work; possibly a UFunction like
  `setTimeOfDay(float)` that we can call generically via our
  `ue_wrap::ParamFrame` reflection. (Per [[feedback-re-related-functions]]
  do this first.)
- `coop/net/protocol.h` -- new `MsgType::TimeOfDay` with payload
  `{float dayFraction, u16 dayNumber}` (12 bytes total). Sent
  unreliable, ~1 Hz from the host. Client applies on receive.
  ~15 lines.
- `coop/net/session.cpp` -- in the net thread's send tick, when
  host AND connected, send TimeOfDay once per second.
  Receiver-side: store latest in a slot like `remotePose_`.
  ~30 lines.
- `coop/time_sync.cpp` + `.h` (NEW) -- game-thread pump: drain
  the slot, write to the engine field via reflection. ~50 lines.
- (Optional) `!time` chat command (depends on feature 1).

**Total:** ~100 LOC + half-day RE.

**MTA precedent:** MTA has `setTime(hour, minute)` server-side
that propagates via the SyncManager. Same shape: host owns the
time, broadcasts to clients on change.

**Scope fit:** Phase 5S0 / 5S1 (host save authority cluster). Ships
with the save bootstrap -- the save already carries time-of-day in
it, so the snapshot-on-connect already syncs the INITIAL time;
the continuous broadcast handles the day-running-onward case.

---

### 9. Chat censor + name simplification

**Feature:** Two functions in `utilityModule.js`:
- `SimplifyName(name)` -- regex-strip non-alphanumerics, truncate
  to 20 chars. Prevents Zalgo / control-character / extra-long
  usernames.
- `CensorSwears(msg)` -- the `obscenity` library matches
  profanity; matches replaced with `X` repeated.

**Fit:** MEDIUM (truncate + strip), LOW (profanity censor).

**Why borrow truncate+strip:** Anything we put on the wire as
a string ought to be length-bounded and character-filtered at the
trust boundary, regardless of content policy. Our `Join` payload
already truncates to 200 bytes UTF-8 (see `event_feed.cpp:65`)
but doesn't strip control chars / unicode-direction-overrides /
multi-byte trickery that could break the HUD widget rendering or
log-line parsing. Easy hardening.

**Why NOT borrow profanity censor:** Coop story is a 2-peer
opt-in session; profanity-filtering is paternalistic at that
scale. If we later add public servers we revisit. The `obscenity`
library is JS-only anyway; in C++ we'd need a different
implementation. Not worth the build.

**What we'd need:**
- `coop/util/string_sanitize.cpp` + `.h` (NEW) -- two functions:
  - `std::wstring SanitizeNickname(const std::wstring& in)` --
    keep `[A-Za-z0-9_-]`, drop everything else, truncate to 20.
  - `std::wstring SanitizeChat(const std::wstring& in)` -- keep
    printable Unicode (>= U+0020) excluding control chars and
    BiDi overrides (U+202A..U+202E, U+2066..U+2069), truncate to
    200. Allow normal punctuation.
  ~50 lines.
- Call sites: `event_feed::SetLocalNickname` (sanitize on input),
  the Join packet receive (sanitize before storing), the chat-
  text drain (sanitize before HUD push).
  ~10 lines of integration.

**Total:** ~60 LOC.

**MTA precedent:** MTA's `CClient::CheckNickProvided` enforces
3..MAX_NICK_LENGTH chars and excludes control codes / colour
codes; no profanity filter in the core (community resources do
it via Lua). Same conclusion: truncate+strip is core hygiene,
profanity filter is community add-on.

**Scope fit:** Ships with feature 1 (chat text). Trivially small.

---

### 10. Machine-ID identity

**Feature:** A 64-char hex hash of `HKLM\SOFTWARE\Microsoft\
Cryptography\MachineGuid` (or similar stable per-install
identifier). Used as ban key, permission key, anti-rejoin gate.

**Fit:** LOW for LAN coop, REQUIRED for dedicated mode.

**Why not borrow now:** In LAN coop we lock the peer by socket
address after the first Hello -- a re-connecting peer just gets
a new socket from the same IP. The IP is good enough identity
for "host trusts this peer". Machine ID earns its keep only
when we ship features 3 (bans) or 5 (Discord audit log) where
the persistent-across-restart identity matters.

**What we'd need (when needed):**
- `coop/identity/machine_id.cpp` + `.h` (NEW) --
  - On Windows: read `HKLM\SOFTWARE\Microsoft\Cryptography\
    MachineGuid` via `RegGetValueW`. Fallback to a per-install
    GUID written under our config dir if the registry read fails
    (e.g. on Wine or a hardened machine).
  - HMAC-SHA-256 it with a fixed mod-wide salt so we don't echo
    the raw MachineGuid (privacy hardening).
  - Lazy-init at first call; cache.
  ~80 lines + a SHA-256 impl (could lift from BoringSSL/mbedTLS or
  hand-roll SHA-256 from RFC 6234 -- the latter is ~100 lines).
- `coop/net/session.cpp` -- send machine ID in the Hello payload.
  The Hello payload struct grows by 32 bytes (raw HMAC output). ~10
  lines.

**Total:** ~180 LOC.

**Privacy note:** Per the RE doc and SCP:SL/MTA experience, the
machine ID is NOT a strong identity (reinstall = new ID, can be
spoofed by a determined adversary). HMAC with a per-mod salt
prevents cross-mod correlation of the ID. We should NEVER ship
the raw MachineGuid on the wire.

**Scope fit:** Phase 7+. Bundled with bans (feature 3).

---

### 11. Server console (interactive readline)

**Feature:** Server admin types commands locally into the
running process; same dispatch as chat commands with `isConsole
= true` flag (which bypasses the perm check). Useful for
remote SSH / screen sessions.

**Fit:** LOW.

**Why not borrow:** Our mod runs inside VOTV.exe; the user
already has the game window and the chat input (feature 1).
There's no "headless terminal" to type into until we ship
dedicated mode -- and then the dedicated binary needs a console
loop. So this naturally rides on top of feature 1's command
dispatch with an `isConsole` flag.

**What we'd need (dedicated mode only):**
- A console thread on the dedicated build: `std::getline(std::
  cin, line)` loop calling `coop::commands::Execute(line, /*isConsole=*/true)`.
  ~30 lines.

**Total:** ~30 LOC, but contingent on dedicated mode existing.

**MTA precedent:** `CConsole` + `CConsoleCommand`
(`reference/mtasa-blue/Server/mods/deathmatch/logic/CConsole.cpp`)
-- yes, MTA's dedicated server has an interactive console that
takes the same commands the client chat does. Same `isConsole`
bool concept. Mirror this exactly when shipping dedicated.

**Scope fit:** Phase 7+. Free once commands+dedicated exist.

---

### 12. Server icon + MOTD (server-browser metadata)

**Feature:** Per-server `title`, `motd`, `icon` (base64 PNG)
configured in `serverconf.yml`. Exposed via the info request
(feature 4). The client's server-browser UI displays them.

**Fit:** MEDIUM (string fields), LOW (PNG icon).

**Why borrow strings, not icon:** title + motd are zero-cost
text fields that make a server-list listing legible. The PNG
icon is the amplification vector (feature 4's security note);
skip until we have a CDN or a known-safe-size cap (e.g. 8x8 ico,
~256 bytes).

**What we'd need:** part of feature 4 (server browser
protocol). The metadata reads from `votv-coop.ini`:
```
[server]
title=Pelmen Coop
motd=Story playthrough
max_players=2
```

**Total:** ~10 LOC (config read), ~30 LOC (info-reply
construction).

**MTA precedent:** MTA's `mtaserver.conf` has the same
`servername` + `password` + `description` shape, plus the icon
is at a known path on the server filesystem. Same shape.

**Scope fit:** Phase 7+, with feature 4.

---

## Additional candidates surfaced during the survey

### 13. `*`-spawn-allocates-ID pattern (for runtime-spawned props)

**Feature:** `propModule.js:114` -- if a client sends a propUpdate
with `networkId: "*"`, the server treats it as a spawn request,
allocates a real network ID, and broadcasts back. Lets the client
spawn an entity without a round-trip handshake first.

**Fit:** LOW.

**Why not borrow:** Our `Aprop_C.Key` is content-addressed (the
save UUID, cross-peer stable -- per
[[project-physics-object-pickup]] `Xym5dmnBEzreWaHSkQ9Tew`-style
strings). We don't need server-allocated IDs for save-resident
props; both peers know the Key. For TRANSIENT runtime spawns (an
admin `!spawnprop` command), we generate a UUID-like Key on the
sender and ship it in the `PropSpawnPayload` -- this is what
[[project-coop-aprop-lifecycle-RE-inc3-scope]] already designed.
No `*`-handshake needed.

**Recommendation:** Decline. Our Key model is strictly better.

---

### 14. Sandbox toy commands (`!size`, `!sizeall`, `!tpall`, `!spawnprop`)

**Feature:** Admin can resize / teleport all players, spawn props.
Pure sandbox affordances.

**Fit:** LOW for coop story; only `!tphere` / `!tpto` survives.

**Why borrow only the teleport pair:** Coop story has a known
desync remedy: reconnect ([[project-coop-mushroom-desync-and-remedy]]).
But a host-initiated "teleport peer to me" is occasionally useful
(stuck in geometry, wants to show the client a thing). `!tphere`
(teleport peer to my position) and `!tpto` (teleport me to peer's
position) are cheap; everything else is sandbox-flavour we
explicitly don't want.

**What we'd need (with commands):**
- `coop/commands/admin/tphere.cpp` (~30 lines) -- writes a new
  `ReliableKind::TeleportTo` reliable msg to the peer with target
  XYZ; peer's drain calls `engine::SetPlayerLocation`. Uses the
  same reflection we already have for `RemotePlayer` writes.

**Recommendation:** Ship only `!tphere` / `!tpto` if/when feature 1
lands. Decline the sandbox toys.

---

### 15. Coloured chat tag syntax (`<red>foo</>`)

**Feature:** VoidTogether's chat messages embed simple HTML-ish
tags (`<red>[Server]</>`, `<green>...</>`). The client renders them.

**Fit:** LOW.

**Why not borrow:** Our HUD feed is a single UTextBlock with a
single colour ([[project-coop-chat-feed]] rebuilds the whole
string on each push). Multi-colour requires either per-segment
UTextBlock children or RichTextBlock support, both of which need
asset-data-table edits (A6 anti-pattern -- our prohibition) or a
RichTextBlock built at runtime with a runtime style-set, which is
substantially more UMG plumbing than the value justifies. A
PREFIX convention (`[Server] ...` for system, `<Bob>: ...` for
chat) gives 80% of the legibility with no plumbing.

**Recommendation:** Decline. Use a plain text format with prefix
conventions.

---

## Recommended cherry-pick shortlist (rank-ordered by value/effort)

Each row: rank, feature, justification (impact -- effort), scope
fit.

| # | Feature                                  | Why now                                                                                                                                                                                                                          | Effort | Scope          |
|--:|------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-------:|----------------|
| 1 | **Time-of-day sync (feature 8)**         | Largest IMPACT on coop-story gameplay correctness (NPCs spawn / quests gate on time). Already-host-authoritative model fits like a glove. Doesn't depend on commands or any other ops feature. Half-day RE + ~100 LOC.           | S      | Phase 5S0/5S1  |
| 2 | **Chat text + command dispatch (feature 1)** | Unblocks every other ops feature on this list (perms, tphere, time-set-via-command, future kicks). Already partly built ([[project-coop-chat-feed]] has the reliable channel; "NEXT" is typed input). ~280 LOC + UMG focus dance. | M      | Phase 6        |
| 3 | **String sanitization (feature 9)**      | Cheap hardening of the trust boundary. Plugs Zalgo / BiDi / control-char attacks on the HUD before we have any of them. Ships with feature 2. ~60 LOC.                                                                           | XS     | Phase 6 (with #2) |
| 4 | **Permissions pattern (feature 2)**      | Pattern (not JSON file) gates feature 2's commands cleanly between host and client. ~50 LOC of dotted-perm matcher; two hardcoded role tables. JSON storage deferred to Phase 7+.                                                  | XS     | Phase 6 (with #2) |
| 5 | **`!tphere` / `!tpto` admin commands (feature 14)** | Concrete user value (unstick clients, point at things) for ~30 LOC each once feature 2 ships. Skips the rest of the sandbox toys.                                                                                                | XS     | Phase 6 (with #2) |

**Rationale for ordering:**

- **#1 (time-of-day) is first because it's independent.** No chat,
  no commands, no perms required. It blocks no other shortlist
  item and is blocked by no other shortlist item. It also has the
  most direct gameplay impact -- coop story without synchronised
  day/night is visibly wrong the moment a player checks the sky.
  This is the rare case where a VoidTogether feature transfers
  with almost zero adaptation.

- **#2 (chat + commands) is second because it's the keystone.**
  Once typed chat lands, items 3/4/5 each cost 30-60 LOC and we
  pick up moderation in increments. Without it, every ops
  feature is independently more expensive (no input channel, no
  dispatcher).

- **#3 (sanitization) is third only because it depends on #2
  having a chat-text payload to sanitize.** If it could ship
  standalone (sanitize nicknames in the existing Join) we'd
  ship it now -- ~30 LOC just for nicknames.

- **#4 (perms) is fourth because items 5 onward NEED a gate.**
  Without perms, a client can run `!tphere host` and shove the
  host around the map. The dotted-perm matcher is the simplest
  thing that works.

- **#5 (`!tphere`/`!tpto`) is fifth because it has measurable
  user value at low cost.** It's the one VoidTogether
  sandbox-toy command that's actually useful in coop story (stuck
  player, point-of-interest sharing).

Everything below the line (bans, machine ID, server browser,
Discord, auto-restart, console, server icon) is **Phase 7+
dedicated-mode work** and not in the shortlist. The pattern (4)
and protocol shape (server browser) are documented in this file
so we don't redesign them when we get there.

---

## Anti-shortlist (decline outright)

- **Glob-loaded commands** -- C++ should use static registration,
  not runtime file globbing. RULE 1: pick the proper design (no
  runtime reflection where compile-time will do).
- **Coloured chat tag markup** -- needs RichTextBlock or
  per-segment widgets; not worth the UMG plumbing.
- **Profanity censor** -- not appropriate for opt-in 2-peer
  sessions; JS-only lib anyway.
- **Discord bot rich presence** -- gateway-keepalive overkill for
  the value; webhook (feature 5) covers the audit log half.
- **`*`-spawn-allocates-ID** -- our content-addressed Key is
  strictly better.
- **Sandbox `!size` / `!sizeall` / `!spawnprop` etc.** -- not
  coop-story affordances.

---

## File-level summary (when the shortlist ships)

If we ship shortlist items 1-5 in order, the new/touched files are:

| File                                         | Status   | Purpose                                       |
|----------------------------------------------|----------|-----------------------------------------------|
| `src/votv-coop/include/coop/net/protocol.h`  | TOUCH    | New `MsgType::TimeOfDay`; new `ReliableKind::ChatText` |
| `src/votv-coop/src/coop/net/session.cpp`     | TOUCH    | Host emits TimeOfDay tick; routes ChatText    |
| `src/votv-coop/src/coop/time_sync.cpp`+`.h`  | NEW      | Apply remote time-of-day to engine field      |
| `src/votv-coop/src/coop/chat_input.cpp`+`.h` | NEW      | UMG editable text field + Enter capture       |
| `src/votv-coop/src/coop/event_feed.cpp`      | TOUCH    | New `case ChatText:` -> route command vs broadcast |
| `src/votv-coop/src/coop/commands/registry.cpp`+`.h` | NEW | Static command registry + dispatch + perm check |
| `src/votv-coop/src/coop/commands/admin/tphere.cpp`  | NEW | `!tphere` impl                                |
| `src/votv-coop/src/coop/commands/admin/tpto.cpp`    | NEW | `!tpto` impl                                  |
| `src/votv-coop/src/coop/commands/admin/time.cpp`    | NEW | `!time night|day|dawn|dusk|0..1`              |
| `src/votv-coop/src/coop/commands/general/help.cpp`  | NEW | `!help` -- list visible commands              |
| `src/votv-coop/src/coop/permissions/perms.cpp`+`.h` | NEW | Dotted-perm matcher + two role tables         |
| `src/votv-coop/src/coop/util/string_sanitize.cpp`+`.h` | NEW | Nickname + chat sanitizers                |

Estimated total: ~700 LOC of mod code + half a day of RE for the
time-of-day field. All Windows-only (no new third-party deps until
Phase 7+ pulls in JSON + WinHTTP for the dedicated-mode shortlist).

---

## Cross-references

- [[project-votv-coop-foundation]] -- foundation; nothing here
  changes that.
- [[project-coop-chat-feed]] -- the reliable channel and event feed
  every shortlist item builds on.
- [[project-phase3-udp-transport]] -- the transport layer; new
  `MsgType` and `ReliableKind` values slot in here.
- [[project-coop-save-host-authoritative]] -- the host owns world
  truth; this is why time-of-day sync is one-way host -> client.
- [[project-physics-object-pickup]] -- the prop pickup design
  already gave us content-addressed Keys; that's why we decline
  the `*`-spawn-allocates-ID pattern.
- [[feedback-check-mta-and-document]] -- MTA precedent rows in
  each feature section.
- [[feedback-code-with-agents-and-security]] -- the trust-boundary
  hardening in features 4 (info amplification) and 9
  (sanitization).
- [[feedback-re-related-functions]] -- the RE step on the
  time-of-day field BEFORE writing the sync code.

---

## Bottom line

VoidTogether's ops layer is the source of three immediately
useful things for VOTV_MP:

1. **A time-of-day broadcast** that we can lift conceptually and
   build in ~100 LOC + half a day's RE. Independent of every other
   ops feature.
2. **A chat + command dispatch shape** that aligns with MTA and
   SCP:SL/Source/Minecraft conventions; ours would be smaller
   (static registry, no JSON, no glob, no colour tags) but the
   prefix-detect / perm-check / handler shape ports cleanly.
3. **A protocol+config shape for the future dedicated/public-
   server mode** (info-request + metadata + machine-ID + bans +
   webhook) that we now have documented before we need it -- so
   Phase 7+ work can lift it directly instead of re-deriving.

Everything else (sandbox toys, colour tags, glob loading, Discord
bot, profanity filter) is decline-worthy on coop-story scope.
