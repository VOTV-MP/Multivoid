# COOP_SCOPE — what coop replicates (and what it does not)

**Living document.** Append to sections as scope is decided; preserve the
audit trail (date + reason) when amending. **Anything not listed here is
NOT in scope and should not drive code or replication decisions.**

This is the discipline that lets coop ship (principle 5). Every "should we
replicate X?" gets a binary answer from this doc. Use the decision tree in
`docs/COOP_METHODOLOGY.md` ("should we replicate X?") to classify new
items.

> **Status**: skeleton. Scope is decided as Phase 1 reflection reveals
> what VOTV's gameplay actually consists of. Do not pre-populate from
> guesswork — fill in as each system is understood.

---

## Player count

- 2 players (host + one client), LAN-first. To be confirmed against VOTV's
  single-pawn world assumptions in Phase 2.

## In scope

- **Multiplayer menu (in VOTV's main menu)** — Host (choose save / New
  Game, load, listen) + Connect (enter IP, join) + server browser (future).
  Native UMG widget built at runtime by our C++ mod; no VOTV asset edited.
  Decided 2026-05-22 (user). Design: `docs/MULTIPLAYER_UI.md`. Build gated
  on the Phase 3 session API.

- **Nameplate ping readout** — the floating per-player nameplate shows the
  round-trip latency to that peer in parentheses after the nickname, e.g.
  `Player 2 (42 ms)`. Owner: per-machine (each side measures RTT to the peer).
  Decided 2026-05-23 (user). Reason: at-a-glance connection health while
  playing. Depends on: the Phase 3 session adding an RTT measurement (ping/pong
  or timestamped pose-ack); the nameplate already renders OUR own UMG text
  (`engine::SpawnNameplateWidget`), so this is appending to that string. Build
  after the basic pose-sync `play` path works on two machines.

- **Coop chat + session event log** — an in-game chat where players type messages
  to each other, AND a system/event feed that posts connection JOINS, DISCONNECTS,
  and ERRORS so players can see what's happening with the session. Owner: host
  relays chat between peers and is authoritative for the session events it
  broadcasts (who joined/left); each machine also surfaces its own local transport
  errors. UI: our OWN runtime UMG chat widget (the `NewObject`/`SpawnObject` path
  proven by the nameplate — no VOTV asset edited). Plain-English text.
  Decided 2026-05-23 (user). Reason: visibility of connection state + player
  communication. KEY DESIGN IMPLICATION: chat + system messages need RELIABLE,
  ORDERED delivery, which the Phase 3 pose channel deliberately is NOT (pose is
  unreliable UDP, freely dropped, newest-wins). So this adds a small reliable
  channel over the SAME session/socket — sequenced control messages with ack +
  retransmit — alongside the lossy pose stream (root-cause, RULE 1: one socket,
  two channels by reliability class; not a second transport). System events derive
  from session state transitions (Connected / Bye) + transport error paths, so the
  join/disconnect/error feed is largely free once the reliable channel exists.
  Methodology: extends Phase 3 transport (new MsgType: ChatText / SystemEvent over
  the reliable channel). Cross-link: the chat widget is the same UMG-at-runtime
  family as the multiplayer menu + nameplate.

- **Master server + opt-in public server browser** — a central master server
  that lists coop hosts who opted IN (a checkbox shown when creating a host
  game: "make my game visible in the browser"). The Connect side's server
  browser queries the master server to populate the public host list (alongside
  direct-IP connect, which stays). Owner: host opts in per-session; master
  server is external infrastructure (host-registers / heartbeats / delists).
  Decided 2026-05-23 (user). Reason: discoverability without sharing IPs.
  Methodology phase: this is a WAN concern (Phase 7+ per COOP_METHODOLOGY 3.1
  "LAN first ... WAN is a Phase 7+ concern"); the LAN direct-IP path ships
  first. Privacy: opt-in only (default OFF); no host is listed without the
  explicit checkbox. The server-browser UI is the already-in-scope multiplayer
  menu's "server browser (future)" element, now given a backing source.

- **Physics-object pickup / drag (E-press interaction)** — sync the
  player's E-press lift/drag of physics props (the ~540 classes derived
  from `Aprop_C : AActor`). The holding player streams the held prop's
  world transform; ownership of the prop transfers to the holder for the
  duration of the grab; release/throw broadcasts terminal velocity and
  peers continue their local sim. Owner: per-grab (the player who pressed
  E owns the prop's pose stream until release); host arbitrates
  same-frame double-grab conflicts. Mass / heavy threshold is NOT on the
  wire — `propData.heavy` is loaded from the same DataTable on both
  peers (deterministic). UI/visual: puppet's arm posture differs in
  lift vs drag mode (new bool on the AnimBP, set from the GRAB packet's
  `grabMode`). Wire: 3 packets — reliable GRAB + unreliable HELD-PROP
  POSE (piggyback on PoseSnapshot, ~33 B trailing record) + reliable
  RELEASE/THROW. Sequence numbers per-prop reject stale events.
  Decided 2026-05-23 (user). Reason: a core VOTV interaction loop;
  without it coop players can't manipulate objects together (e.g.,
  carrying baggage to the satellite, dragging heavy items between
  locations). Methodology phase: ~Phase 4.x (post-pose-sync, post-input-
  sync). Deep-research converged via 3 parallel agents (IDA / SDK-
  reflection / MTA-fidelity); full plan in
  `research/findings/physics-object-pickup-coop-plan-2026-05-23.md`.
  PREREQUISITE: prop-Key cross-peer agreement (`Aprop_C.Key @ +0x02E0`)
  — spawn paths must route through host so both peers see the same Key
  for the same prop. This is a separate prerequisite scope item; the
  physics-pickup feature blocks on it.
  STATUS 2026-05-24: protocol v4 + sender + receiver SHIPPED (commit
  `8623991` and audit fixes in subsequent commits); host's autonomous
  grab propagates to client cross-process (verified numerically + visually
  via lan-test). THROW IMPULSE: also shipped same day -- AddImpulse
  observer caches impulse vector + timestamp + target component; the
  NetPumpTick release edge passes the cached impulse to SendPropRelease
  if it was applied to THIS prop's mesh within 200 ms. Receiver
  applies via UPrimitiveComponent::AddImpulse after re-enabling
  SimulatePhysics. Drop semantics remain when no recent impulse for
  the released mesh.

- **NPCs + interactable entities sync (proposed, AWAITING USER REVIEW)** —
  next major feature after physics-prop pickup. Architecture proposal in
  `research/findings/votv-npc-entity-coop-architecture-2026-05-24.md`,
  grounded in `research/findings/mta-npc-entity-sync-2026-05-24.md` (MTA
  patterns) and `research/findings/votv-npc-entity-survey-2026-05-24.md`
  (VOTV ~17 NPC classes + 1200+ prop subclasses + doors/triggers/keypads
  + 2 vehicle-ish + UFO/event actors). Recommended 5-phase stagging:
  5N1 NPC pose stream (kerfur/zombie/ariral, host-authoritative);
  5N2 spawn/despawn (host-routed); 5N3 interactables (doors, switches,
  triggers, keypads); 5N4 vehicles (ATV driver-authoritative, drone host);
  5N5 transient events (explosions, projectiles, one-shot sound/particle,
  AOI-culled). Universal cross-peer ID = `Key` FName (already verified
  cross-peer stable for props). Wire protocol bump v4→v5 adds NpcSpawn/
  Despawn (reliable), NpcPose (unreliable), NpcState (reliable, delta-
  encoded), EntityEvent (reliable), EffectFire (unreliable AOI-culled),
  VehicleEnter/Exit/Pose. MTA patterns to adopt directly: sync-time-
  context byte for stale-drop, flag-byte delta encoding for sparse state,
  AOI-cull for transient effects. Open decisions for user: phase ordering,
  authority model for 5N1-3, AOI radii defaults, client-AI suppression
  strategy, save-persistence in/out of scope. NO CODE until user
  reviews + approves architecture.
  USER DECISION 2026-05-24: **client inventory is unique/private**. No
  inventory contents ever cross the wire. World <-> inventory transitions
  (pickup despawns world prop on both peers; drop spawns world prop on
  both peers) ARE replicated as world events; inventory contents
  (`Aprop_inventoryContainer_player_C` actor + child items) stay
  per-peer local. Item state with world effect (e.g. flashlight on/off
  light cone) replicates as an EntityEvent with the item's Key + state.
  Implementation lands in Phase 5N3 (interactables / world events),
  not a new phase.
  USER DECISION 2026-05-24: **whole-map sync**, no AOI/radius culling.
  NPCs and objects sync across the whole map. Bandwidth gated by entity
  ACTIVITY (PhysX `IsAnyRigidBodyAwake` for props; AI-active flag for
  NPCs) rather than distance. Events (explosions, projectiles, sounds)
  broadcast unconditionally. Open decision #3 (AOI radii) closed.
  USER DECISION 2026-05-24: **enemies target BOTH host AND client**, not just
  host. Forces puppet design: each peer's representation of the OTHER peer
  must be AI-perceivable as a player. Switch puppet from `ASkeletalMeshActor`
  back to `mainPlayer_C` orphan (Phase-1 design proven viable). Block input +
  AI possession via deferred-spawn pattern (AutoPossessPlayer/AI=Disabled,
  AIControllerClass=null) so the orphan can't act as a real player but
  keeps the class identity AI relies on. Per-NPC targeting-logic RE
  (Anpc_zombie_C, AkerfurOmega_C, Akrampus_C, Afunguy_C, ariral family,
  etc.) listed as pre-Phase-5N1 task to confirm each NPC either uses
  AIPerception, `Cast to mainPlayer_C`, or `GetPlayerPawn(0)` -- the
  last hardcodes the first player only and would need per-NPC hook.

<!--
Template for an entry:
- **<system>** — replicated <how>. Owner: <host / per-machine>.
  Decided <YYYY-MM-DD>. Reason: <why>. Methodology phase: <4.x>.
-->

## Out of scope

_(empty — populate as decided, with date + reason)_

<!--
- **<system>** — NOT replicated. Reason: <re-derivable locally / breaks no
  one's experience / too costly>. Decided <YYYY-MM-DD>.
-->

## Undecided / parked

Candidate VOTV systems to classify during Phase 1 (this is a checklist of
*questions*, not a scope commitment):

- Player pose / movement (almost certainly in scope — Phase 3/4.1).
- Player input (almost certainly in scope — Phase 4.1).
- Held tool / equipment state (Phase 4.2).
- Base / facility state, machines, the satellite dish array & signals.
- Inventory, resources, progression / unlocks (host-side, Phase 4.6).
- NPCs / creatures / AI entities (Phase 4.3).
- Day/night cycle & world time (likely host-authoritative single value).
- Weather / environment events.
- Audio signals / the core signal-processing gameplay loop.
- Save / world state (host-authoritative — Phase 4.5).
- Cutscenes / scripted story events (Phase 4.4).
- UI / HUD (per-machine; not replicated, but reads shared host state).

## Architectural note — general entity/object sync (forward requirement)

2026-05-22 (user): the eventual goal is to sync **all entities and objects**
between players, not just the player pawn. This is the Phase 4.3 "entity
manifest + per-entity state" work and stays scoped/incremental (principle 5 —
each entity class is classified here before it is replicated), but the
*substrate* must be general from the start, and it is:

- **UFunction calls** on any object: DONE — `ue_wrap::ParamFrame` +
  `reflection::FunctionParams` marshal any UFunction by reading its FProperty
  offsets from the live class (no hardcoding). Drives spawn, pose, etc.
- **UProperty get/set** on any object (read/write a named field): the sibling
  capability still to build, using the SAME FProperty offset reflection
  (`Offset_Internal`/`ElementSize`). This is what state replication needs —
  read an entity's properties on the host, push, write them on the client.
- **Entity manifest/registry**: a per-object-class table (what to replicate,
  how often, host- vs per-machine-authoritative) drives the above. Populated
  per entity class as Phase 1 reflection classifies it (the parked list below).

So no entity-specific code is hardcoded into the transport: it walks the
manifest, reads/writes properties + calls UFunctions generically. The
RemotePlayer pose path (Phase 3) is the first concrete instance of this shape.

### Game events (100+) — host-authoritative, triggered by space signals

2026-05-22 (user): VOTV has 100+ scripted world events (alien ship landing,
base earthquake, ...), MOST triggered by the player receiving certain data
**signals from space** (the core gameplay loop). These must fire consistently
for both players. Methodology phase 4.4 (scripted events) + 4.3.

Design implications (do NOT build yet; record so the architecture serves it):
- **Host-authoritative triggering, single source of truth.** The host decides
  which signal arrives when (many events have randomness/timing — clients must
  NOT independently roll, or they desync). The host drives the event; the client
  is told.
- **Two layers to sync, prefer the upstream one.** (a) Sync the *signal
  reception / world state* that triggers events, so each machine's event logic
  fires naturally from replicated state (fewer messages, robust). (b) Where an
  event is pure presentation or not state-derivable, replicate the *event
  trigger itself* as an RPC: the host invokes the event's Blueprint UFunction,
  then the client invokes the same UFunction via our generic CallFunction
  substrate (the 100+ events become a manifest of {event id -> BP function +
  params}). Same generic-marshaling foundation as everything else.
- **Skip/idempotency semantics** (per methodology cutscene handling): an event
  already playing on the host shouldn't double-fire on a late-joining client.
- Cross-link: signals/events are also the satellite-dish gameplay loop in the
  parked list below; classify each event as it is understood (principle 5).

### Shared economy + world reactions to the 2nd player

2026-05-22 (user):
- **Money / points balance** — host-authoritative SHARED balance (one wallet for
  the session, not per-player), replicated to the client; the client's UI reads
  the shared value and spends route through the host. Phase 4.6
  (inventory/progression). Classify as In scope when 4.6 starts.
- **Proximity-reactive world (auto-open doors etc.)** — anything that reacts to a
  player being near (auto doors, triggers, sensors) must also react to the SECOND
  player, and its resulting state (door open/closed) must be synced. NOTE: because
  our remote player is a REAL 2nd `mainPlayer_C` pawn in the world (not a fake
  proxy), overlap/proximity volumes should detect it automatically -- a strong
  point for the real-pawn approach (principle 3). What still needs syncing is the
  resulting STATE so both machines agree (host-authoritative door/trigger state).
  This is the entity-state-sync work (4.3) applied to interactables.

## Amendment log

- 2026-05-21 — Created skeleton at project bootstrap.
- 2026-05-22 — Added multiplayer menu (host/connect/server-browser) to In
  scope per user; design in `docs/MULTIPLAYER_UI.md`.
- 2026-05-22 — Recorded general entity/object-sync intent (user) + the generic
  reflection/marshaling substrate that serves it (see architectural note).
- 2026-05-22 — Recorded game-events sync (100+ events, signal-triggered; user):
  host-authoritative, sync upstream signal/state where possible else replicate
  the event-trigger UFunction (see architectural note). Phase 4.4.
- 2026-05-22 — Recorded shared money/points balance (host-authoritative shared
  wallet, 4.6) and proximity-reactive world incl. auto-doors reacting to the 2nd
  player + synced state (4.3); user (see architectural note).
- 2026-05-23 — Added nameplate ping readout (`Player 2 (42 ms)`, per-machine RTT)
  to In scope; user. Depends on a session RTT measurement.
- 2026-05-23 — Added master server + opt-in public server browser to In scope;
  user. Opt-in only (default OFF), WAN/Phase 7+ (LAN direct-IP ships first);
  backs the multiplayer menu's "server browser" element.
- 2026-05-23 — Added coop chat + session event log (joins/disconnects/errors) to
  In scope; user. Needs a RELIABLE ordered channel (ack+retransmit) over the
  Phase 3 session, distinct from the lossy pose stream; UMG-at-runtime chat widget.
