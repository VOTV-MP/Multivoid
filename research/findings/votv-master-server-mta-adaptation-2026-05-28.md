# PR-5 master server: adapt MTA's server browser (2026-05-28)

User direction at HEAD=`b529e0e`: "we can research mta's server browser
and adapt that to our game".

The MTA codebase already vendored at `reference/mtasa-blue/` is the
conceptual precedent for our coop substrate (per CLAUDE.md). For PR-5
(lobby browser), the same precedent applies: MTA's server browser is
shipped, battle-tested at multi-thousand-server scale for >15 years, and
its source is directly readable. Adapt rather than reinvent.

This doc is a starting-point research map -- not a final design. PR-5
will land its own dedicated design + implementation doc once we've
walked through these files.

---

## What's already in our reference tree

`reference/mtasa-blue/Client/core/ServerBrowser/`:

- **CServerBrowser.{cpp,h}** -- the top-level UI controller (CEGUI-based
  in MTA; we'll port the IDEA to ImGui per our existing UI direction).
  Owns the tabbed browser (Internet / LAN / Favourites / Recent /
  Personal History) and bridges UI events to the list/master-server
  managers below. Read this first to understand the user-facing shape.

- **CServerList.{cpp,h}** -- the in-memory list of known servers + their
  per-server query state machine. Each entry tracks last-seen, ping,
  player count, name, mode, locked-by-password, version, etc. The async
  ping driver (UDP query to each server in parallel, with backoff) is the
  reusable mechanic. Same shape we need: GNS query packet, NAT-traversal-
  aware return path.

- **CServerBrowser.MasterServerManager.{cpp,h}** -- coordinates polling
  the master server endpoints (MTA runs multiple master servers in
  parallel for redundancy). Reads/writes a local cache + falls back to
  cached results when the master is unreachable. The redundancy + cache
  pattern is exactly what we want for resilience.

- **CServerBrowser.RemoteMasterServer.{cpp,h}** -- the HTTP client that
  fetches the server list from a single master endpoint. MTA's wire
  format is JSON (with versioned schemas). We'll likely use the same
  shape (versioned JSON over HTTPS) so we get TLS + cache-headers for
  free.

- **CServerCache.{cpp,h}** -- on-disk cache of last-known server list so
  the browser opens populated even before the master responds. Critical
  for UX -- otherwise the user stares at a spinner for 1-3 seconds every
  launch.

- **CServerInfo.{cpp,h}** -- the detail-pane data model when a user
  clicks a single server. Includes the full player-list query (separate
  packet from the summary in the main list). We'll likely defer the
  detail pane to PR-5b -- v0 just needs name/players/ping/mode.

Additional context elsewhere in the tree:

- `reference/mtasa-blue/Server/mods/deathmatch/logic/CMasterServerAnnouncer.cpp`
  -- the SERVER-SIDE announce loop. Servers periodically POST their
  current metadata (player count, name, mode) to the master so the
  master's list stays fresh. This is the host-side of the MTA pattern
  we'd mirror in our coop host.

---

## What to adapt vs what to NOT copy

### Adapt the shape

- **Tabbed UI** (Internet / LAN / Favourites / Recent / Personal
  History). For VOTV coop v0 we probably only ship Internet + LAN tabs;
  Favourites + Recent are v0.1.
- **Two-stage query**: cheap summary in the list-view, full detail
  on-click. Saves bandwidth at scale.
- **Master server is a JSON aggregator, not a routing relay.** The
  master's only job is to know who's hosting. Once the client picks a
  host from the list, it connects directly (LAN-direct in v0; ICE-
  signaled P2P via GNS later in PR-6). The master never sees gameplay
  packets.
- **On-disk cache** so the browser opens populated.
- **Multiple master URLs in parallel** for redundancy (we can start with
  one and add the second when there's actual traffic to justify it).
- **Server-side announce loop**: the host POSTs its metadata every
  ~30-60s so the master never has stale entries longer than that.
- **Heartbeat-with-leave**: server explicitly POSTs "going offline" on
  Stop() so the master can drop it immediately rather than waiting for
  the next heartbeat to time out.

### Do NOT copy

- MTA's CEGUI dependency -- we use ImGui (per CLAUDE.md / mod menu
  direction). Port the IDEA, write new render code.
- MTA's "ASE protocol" UDP query format -- predates GNS. We'll use GNS
  connectionless query messages instead (it has built-in support for
  this).
- MTA's anti-cheat / serial-number reporting in the announce payload --
  not relevant to a coop mod for a single-player game.
- The full anti-spam / IP-rate-limit machinery in
  RemoteMasterServer.cpp -- v0 doesn't need it; revisit if abuse
  appears.

---

## Concrete v0 scope (proposal -- to be confirmed during PR-5 design)

**Master server** (new -- not in this repo; separate hosting):
- One HTTPS endpoint, `GET /v0/servers` returning a JSON array of
  `{name, host_ip, port, version, players_current, players_max,
   world, mode, ping_target, last_seen_unix}`.
- One HTTPS endpoint, `POST /v0/announce` accepting the same shape from
  hosts. Validates the host IP matches `host_ip` in the body (anti-
  spoofing) and stamps `last_seen_unix = now`.
- One HTTPS endpoint, `POST /v0/leave` to clear an entry early.
- Stateless aggregator: stores in-memory only; clears entries with
  `last_seen_unix > 5 minutes`. Restart loses the list (hosts re-
  announce in <=60s anyway).
- Deploy: single small container (Cloud Run / Fly.io / a $5/mo VPS),
  TLS terminated by the platform.

**Client side** (new code under `src/votv-coop/src/coop/net/`):
- `lobby_client.{cpp,h}`: HTTPS client (libcurl, vendored if not
  already), JSON parser (likely picojson or nlohmann::json, single-
  header), background fetch thread + cache.
- `lobby_announcer.{cpp,h}`: host-side periodic POST loop. Started by
  Session::Start() when `cfg_.role == Host`.

**UI side** (new code under `src/votv-coop/src/ui/`):
- `lobby_browser.{cpp,h}`: ImGui-rendered table -- per-row: server name,
  ping, players, version, world, "Connect" button.
- Triggered from a main-menu entry (need to find the right UMG override
  point in VOTV's main menu -- separate RE task).

---

## Open questions / next-session research

1. **Where in VOTV's main menu do we inject the "Browse Servers" entry?**
   VOTV has its own UMG main-menu UClass; we need to either (a) patch in
   a new button (asset edit -- forbidden by RULE 1) or (b) hook the menu
   tick + render an ImGui overlay button on top, or (c) intercept the
   keyboard input + show our own full-screen ImGui menu when the user
   presses a hotkey from the main menu. Option (c) is the standalone-
   safest per RULE 3.

2. **Master server hosting choice.** We'll likely want it in the user's
   own control (their domain), not Anthropic's. Document the deploy
   procedure as part of PR-5.

3. **Versioning.** When the game updates (new VOTV release) the
   protocol version bumps. Old clients shouldn't see new-version
   servers in their list (or should see them grayed-out with a
   "version mismatch" tag). Master can filter by version on the GET, or
   send everything + client filters.

4. **NAT traversal.** v0 lobby browser assumes LAN-direct connect
   (manual port forward on the host's router). PR-6 adds GNS ICE-
   signaling so the host can be behind NAT. Until then, "Internet"
   servers in the browser are restricted to publicly-routable hosts.

5. **Anti-griefing.** MTA's master has IP rate-limiting on POSTs to
   prevent a single client flooding the list with phantom servers. v0
   skip; revisit at scale.

---

## Cross-refs

- Existing master-server design seed:
  `research/findings/votv-gns-p2p-masterserver-plan-2026-05-28.md`
- MTA precedent (vendored, read-only):
  `reference/mtasa-blue/Client/core/ServerBrowser/`
- Audit report that prioritized PR-5 as next-major-arc:
  `research/findings/votv-coop-audit-post-pr4-7-2026-05-28.md`
- COOP methodology (Principle 7 + MTA-aligned coop substrate):
  `docs/COOP_METHODOLOGY.md`
