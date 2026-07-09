# COOP_SCOPE — what coop replicates (and what it does not)

**Living document.** Append to sections as scope is decided; preserve the
audit trail (date + reason) when amending. **Anything not listed here is
NOT in scope and should not drive code or replication decisions.**

This is the discipline that lets coop ship (principle 5). Every "should we
replicate X?" gets a binary answer from this doc. Use the decision tree in
`docs/COOP_METHODOLOGY.md` ("should we replicate X?") to classify new
items.

> **Status**: actively maintained (no longer skeleton — most major
> categories have at least one decision logged). New entries are added
> as user makes scope decisions during iteration; each entry has a date
> + reason. Cross-references to live workstreams in `docs/ROADMAP.md`
> and the `memory/` topic files.

---

## Player count

- **4 players** (host + 3 clients), LAN-first. `kMaxPeers = 4` in
  `coop::players::Registry` and `coop::net::Session` (PollGroup, 3 lanes,
  per-slot OnDisconnect contract). The transport is multi-peer end-to-end
  (PR-4 SHIPPED 2026-05-28); gameplay subsystems must follow suit (state
  push to all puppets, snapshot fan-out, host-relay of client-originated
  events for 3+ peers — see PR-FOUNDATION-1..3 in
  `research/findings/votv-architecture-audit-2026-05-29.md`).
  Decided 2026-05-26 (user verbatim: "It should handle at least 4 players").
  STATUS 2026-05-29: wire layer 4-peer-ready; some gameplay paths still
  2-peer-shaped (no host-relay between clients yet); 4-peer autonomous
  smoke missing — Tier 8 work in the foundation audit.

## In scope

- **Multiplayer menu (in VOTV's main menu)** — Host (choose save / New
  Game, load, listen) + Connect (enter IP, join) + server browser (future).
  Native UMG widget built at runtime by our C++ mod; no VOTV asset edited.
  Decided 2026-05-22 (user). Design: `docs/MULTIPLAYER_UI.md`.
  STATUS 2026-07-02: **SHIPPED** (docs/MULTIPLAYER_UI.md "BUILT"; the menu
  shell + host save picker + server browser live in `src/votv-coop/src/ui/` +
  `coop/session/multiplayer_menu.cpp`). The "build gated on Phase 3" note is
  history — Phase 3 shipped long ago.

- **Nameplate ping readout** — the floating per-player nameplate shows the
  round-trip latency to that peer in parentheses after the nickname, e.g.
  `Player 2 (42 ms)`. Owner: per-machine (each side measures RTT to the peer).
  Decided 2026-05-23 (user). Reason: at-a-glance connection health while
  playing. STATUS 2026-07-02: **SHIPPED** — `coop/player/nameplate.cpp`
  Plate.ping (`GetPing()`, "<1ms" LAN form) rendered by `ui/hud.cpp`; the
  nameplate itself moved from the old UMG widget to our ImGui screen-space
  projection. The "build after pose-sync works" note is history.

- **Player cosmetics: body SKINS + per-peer nameplate visibility** — every
  player carries a persisted body-skin choice (converter paks + builtin
  kerfur bodies + native dr_kel; F1 > Cosmetics > Skins browser) and a
  persisted "show my nameplate" pref; BOTH are replicated per-player state
  (Join/PlayerJoined identity fields + SkinChange(82)/NameplateChange(83)
  live changes) so every peer — including late joiners — agrees. Decided
  2026-07-02 (user: local body must wear the own skin; skins are a system
  with an F1 browser like gmod; "любой пир может выключить свой nameplate"
  synced). STATUS 2026-07-02: **AS-BUILT** (v93 skins `bfee8f62`, v94
  nameplate pref `23f2ca51`, builtin kerfur bodies `98678bf1`; docs/
  COOP_CLIENT_MODEL.md §3 + COOP_SYNC_MAP rows) — hands-on verdict pending
  (runbook 2026-07-02 take-4). NOT in scope: asset replication (a peer
  missing a skin pak sees kel; by design), ragdoll-body reskin (documented
  gap).

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
  STATUS 2026-07-02: **SHIPPED** — T-chat (`coop/comms/chat_sync.cpp` +
  `ui/chat_input`, v60) + the system feed (`coop/comms/chat_feed.cpp`:
  connect/join/leave/skin/nameplate lines); the reliable channel became the
  session's 3-lane reliable transport, exactly the shape this entry called for.

- **Master server + opt-in public server browser** — a central master server
  that lists coop hosts who opted IN (a checkbox shown when creating a host
  game: "make my game visible in the browser"). The Connect side's server
  browser queries the master server to populate the public host list (alongside
  direct-IP connect, which stays). Owner: host opts in per-session; master
  server is external infrastructure (host-registers / heartbeats / delists).
  Decided 2026-05-23 (user). Reason: discoverability without sharing IPs.
  STATUS 2026-07-02: **SHIPPED** — `ui/server_browser.cpp` (live lobby feed =
  master `GET /v1/lobbies` via `lobby_client`, heartbeat age, direct-connect
  row that works with the master down) + the master-up flow overriding the
  signaling URL/token per session (`harness/config.cpp`; env/ini overrides =
  the dev framework).
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
  ADDED 2026-05-30 (user): this explicitly covers **scripted pursuit /
  chase EVENTS** -- e.g. the **zombie-kerfur** event where the enemy
  actively pursues a player. The pursuer must be able to chase ANY peer
  (and switch targets between peers -- nearest / most-recently-seen /
  aggro, whatever the BP uses), not lock onto the host. Host-authoritative:
  the host runs the pursuit AI, selects the target among ALL peers
  (host's local `mainPlayer_C` + every client puppet), and replicates the
  chosen target + NPC pose so each client sees the enemy chase the correct
  peer. Same puppet-as-`mainPlayer_C` requirement (so the pursuer perceives
  / casts the client puppet as a valid player); the per-NPC targeting RE
  above must, for each pursuer, confirm it enumerates all players rather
  than `GetPlayerPawn(0)`. FUTURE scope (lands with the NPC AI-targeting
  work, Phase 5N1); not built yet.

- **Player health / vitals / death sync** — peers SEE each other's health
  drop, take damage, ragdoll, and die. Authority: **PER-PEER-AUTHORITATIVE
  vitals** — each peer computes its OWN health (host-run enemy hits are
  delivered as a reliable `PlayerDamage` EVENT to the hit peer, which feeds it
  to its LOCAL SP `Add Player Damage` BP so its own armor/FX/health-decrement
  run) and STREAMS the resulting health for others' puppet display. Death is a
  reliable, self-attested event from the dying peer (the only peer that
  authoritatively knows it died). Self-damage (fall/fire/radiation) is computed
  locally and shows up in the streamed health with no event. Owner: per-peer
  for the health number + death; host for enemy hit DELIVERY (host runs the
  AI/hit per enemies-target-both above, then tells the hit peer). Wire:
  continuous health/food/sleep piggybacked on `PoseSnapshot` (8-bit, ZERO size
  change — spends the existing `_pad[3]`; health streamed as the FRACTION
  `health/maxHealth` since `maxHealth` is per-peer, not shared) + the `stateBits`
  ragdoll DISPLAY bit (bit1 `kStateBitRagdoll`, see below) + reliable
  `PlayerDamage` (host enemy hit -> owner, host-only-send/not-relayable, Inc3).
  **Ragdoll/faint (Inc2b) = a CONTINUOUS per-peer DISPLAY bit, NOT a reliable
  event.** The sender sets `stateBits` bit1 from its own `isRagdoll && !dead`;
  the receiver EDGE-DETECTS the wire bit + dispatches the puppet flop/recover once
  per transition. One bit covers EVERY ragdoll cause — manual **C-key**, exhaustion
  `faint()`, KO — since all flip `isRagdoll`; there is no readable `passOut`/cause
  to distinguish them, and faint is a transient lossy-tolerant display state (MTA's
  `CPlayerPuresyncPacket` flag shape), so it rides the proven per-peer pose relay
  rather than a discrete reliable event. **The puppet flop is on the puppet's OWN
  mesh, not VOTV's ragdoll:** the orphan puppet is tickless/controllerless, so
  VOTV's `ragdollMode` (which spawns a separate `playerRagdoll_C` physics actor)
  leaks +5 GB on it (the actor goes PendingKill but never GC-reaps and keeps
  simulating). Instead we borrow the player ragdoll PhysicsAsset
  (`kerfurOmegaV1_PhysicsAsset`), `SetPhysicsAsset` it on `mesh_playerVisible`,
  enable collision, and `SetAllBodiesSimulatePhysics(true)` — leak-free, we own the
  teardown (probe-confirmed `IsAnyRigidBodyAwake=1` during, host RSS stable). The
  LOCAL player keeps using VOTV's real ragdoll (works — possessed). **Death is
  EXCLUDED from the bit** (the `!dead` gate): death uses VOTV's native SP menu flow
  and ends/leaves the session, never a synced recoverable faint.
  **Death lifecycle (user decision 2026-05-30): permadeath, rejoinable; host
  death ends the session.** Both host and peer death use VOTV's NATIVE SP
  death->menu flow (no death->menu intercept). Host death = session ends for all
  (== host exit; no migration); peer death = that peer disconnects to menu (host
  runs existing puppet/mirror teardown; client save already blocked) and may
  REJOIN the host's ongoing world via the existing late-join snapshot. So
  **respawn/revive (`PlayerRevive`) is CUT (RULE 2)** — no respawn-in-place, no
  revive. Protocol bump v18->v19. Display: health bar on the existing nameplate;
  ragdoll via the puppet's own `ragdollMode`/`ragdollActor` (narrowed to the
  NON-death faint/sleep/KO states where the player STAYS in the world).
  RULE-3 (no anti-cheat): a peer is authoritative over its own death — a
  dishonest peer can refuse to die / under-report health (4 trusted LAN peers;
  MTA trusts the same at the C++ layer). MTA shape: `CPlayerPuresyncPacket`
  health stream + reliable `CPlayerWastedPacket` death + `CPlayerSpawnPacket`
  respawn. Decided 2026-05-30 (user; foundation-audit critical realization #2:
  vitals UNREPLICATED -> combat/horror loop incoherent across peers).
  Methodology phase: ~5N1-adjacent (enemy damage delivery depends on the host
  hitting client puppets). Cross-link: enemies-target-both above (those puppets
  are what enemies hit; THIS replicates the resulting health/death). RE +
  design (incl. adversarial-verify must-fixes):
  `research/findings/votv-player-vitals-death-RE-2026-05-30.md`. STATUS: Inc0 (P7
  extraction) + Inc1 (continuous health/food/sleep on PoseSnapshot + nameplate
  health bar) SHIPPED (6f5949c); Inc2a (#8 PASS, 4d52d40) + **Inc2b ragdoll/faint
  DISPLAY-bit sync SHIPPED 2026-05-31** (protocol v20, `kStateBitRagdoll`,
  continuous-bit pivot — see the wire note above); **Inc3-visual (puppet damage
  HURT-FLASH: nameplate flashes red on a streamed health drop, no new wire)
  SHIPPED 2026-05-31**, plus **Inc3-BODY-PULSE SHIPPED 2026-05-31** (Minecraft-style
  whole-body red flash by material SWAP to the EXISTING skeletal gore mat
  `inst_goregibs_organsSK` on both visible body meshes — no asset edit/Principle 1;
  generic static-mesh mats fall back to grey on a skinned mesh, so a skeletal mat is
  required); Inc3-wire (host-auth enemy-hit -> owner damage delivery, needs
  MUST-VERIFY #6) pending; respawn/revive CUT (see above).

<!--
Template for an entry:
- **<system>** — replicated <how>. Owner: <host / per-machine>.
  Decided <YYYY-MM-DD>. Reason: <why>. Methodology phase: <4.x>.
-->

## Out of scope

- **Inventory CONTENTS (slot order, stack counts, items in pockets)** —
  NOT replicated. Per-peer private. World ↔ inventory transitions
  (pickup despawn, drop spawn, flashlight on/off) DO replicate as world
  events. Reason: keeps player UI responsive (no wait for host) and
  per-player progression / equipment unique to each peer.
  Decided 2026-05-24 (user). Implementation lands in Phase 5N3
  (interactables / world events).

- **Engine built-in replication** (UE4 `Replicated` UPROPERTYs and RPC
  attributes). NOT used for gameplay. VOTV blueprints are SP-authored;
  enabling replication would require editing assets (anti-pattern A6).
  We drive the engine via reflected UFunction calls + direct state
  push on our custom UDP layer instead.
  Decided 2026-05-21 (Phase 0 / feasibility).

- **Outdoor lamp posts (`AlampPost_C`)** — NOT synced. Driven by the
  game's day/night cycle (mainGamemode tracks `allLampPosts`); both
  peers run the same local cycle and lamp posts toggle in lockstep
  without wire traffic. Confirmed via the Phase 5D doors+lights RE
  pass 2026-05-25.

- **Pak-mounted custom content** — NOT in scope for the coop sync work.
  All shipping behaviour rides through the standalone DLL; no asset
  edits, no pak overlays. Revisit Phase 7+ ONLY for the multiplayer
  menu polish if needed. See the 3 architecture-decision findings docs
  from 2026-05-25 (votv-mp-pak-mount-feasibility).
  Decided 2026-05-25 (user, after hybrid-pak survey).

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
  STATUS 2026-05-26: Phase 5W Inc1+Inc2 **SHIPPED**. Host-authoritative
  weather state sync (rain, snow, fog enables, wind via cycle propagation,
  lightning strikes) lands via two new reliable kinds: `WeatherState` (20 B
  continuous push on host scheduler state-change + connect-edge replay)
  and `LightningStrike` (16 B discrete event at strike loc).
  Authority: `AdaynightCycle_C` singleton. Host POST-observes 5 scheduler
  UFunctions (`timerRain`/`timerLightning`/`fogEvent`/`superFogEvent`/
  `permaRain_timer`), FNV1a-dedups state, broadcasts. Client PRE-cancels
  the same 5 via new multi-slot interceptor in `ue_wrap::game_thread` so
  the client never rolls weather locally; receives state via wire +
  applies through cycle mutator UFunctions (`causeRain` /
  `setRainProperties` / `setWindParameters` / `intComs_triggerSnow`).
  Lightning: host POST observer on `BeginDeferredActorSpawnFromClass`
  filtered on `lightningStrike_C`; receiver spawns at received loc via
  BeginDeferred+FinishSpawning. RE docs:
  `research/findings/votv-weather-RE-{mainGamemode,effect-actors,scheduler}-2026-05-26.md`,
  synthesis `votv-weather-DESIGN-2026-05-26.md`. **OUT OF Phase 5W**
  (deferred to a future `EntityEventPacket` phase): story-event weather
  spawns (`spawnBlackFog` / `spawnRedSky` / `Spawn Bad Sun` /
  `event_fleshRain` / `skyFallingEvent`) — sparse one-shots with bespoke
  assets that warrant per-event packets, not the continuous-state packet.
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
- **UProperty get/set** on any object (read/write a named field): DONE (AS-BUILT)
  — `reflection::FindPropertyOffset` (reflection.h:202; prefix variant :211)
  resolves a named instance field's `Offset_Internal` via the live FProperty
  chain; the `reflected_offset.{cpp,h}` layer caches per (class, field). Concrete
  host-read / client-write instance: `ue_wrap/windturbine.cpp`
  ResolveOff/ReadState/WriteState (lines 34/81/92), wired into the network in
  `coop/turbine_sync.cpp` (ReadState host:195,241 / WriteState client:140). Same
  shape in prop.cpp, save_browser.cpp, puppet.cpp, kerfur.cpp. Landed e8c0faa5
  (reflection primitive, 2026-05-25) + f32ed1b0 (turbine state-sync wiring,
  2026-06-15). This is what state replication needs — read an entity's
  properties on the host, push, write them on the client.
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
- 2026-05-23 — Added physics-object pickup / drag (E-press) to In scope; user.
  3-packet design (GRAB reliable + HELD-PROP-POSE piggyback + RELEASE reliable
  with throw impulse). SHIPPED 2026-05-24.
- 2026-05-24 — Added NPC + interactable entity sync to In scope; user. Phase
  5N1 (NPC pose) → 5N2 (spawn/despawn) → 5N3 (interactables / doors / world
  events) → 5N4 (vehicles) → 5N5 (transient events).
- 2026-05-24 — **client inventory is unique/private** (Out of scope; see
  above). World ↔ inventory transitions DO replicate as world events.
- 2026-05-24 — **whole-map sync, no AOI culling**. NPCs + objects sync
  across the whole map. Bandwidth gated by entity ACTIVITY
  (`IsAnyRigidBodyAwake` for props; AI-active flag for NPCs), not distance.
- 2026-05-24 — **enemies target BOTH host AND client**, not just host.
  Forces puppet design: each peer's representation of the OTHER peer must
  be AI-perceivable as a player (mainPlayer_C orphan with AutoPossess /
  AIControllerClass disabled).
- 2026-05-24 — **scale 100 entities active simultaneously**; aggregated
  `EntityPoseBatch` bitstream (1 packet/tick × N entries), activity-rate
  gating (5/15/30/60 Hz by AI state), WireKey → 4-byte session-id
  mapping after spawn.
- 2026-05-24 — **save host-authoritative** (REVERSED prior "out of
  scope"); client receives full host snapshot on connect; per-peer
  inventory exception preserved.
- 2026-05-25 — **Polish v1** added to In scope: nameplate text outline +
  drop shadow + sanitization (alnum + space + `[-_.]` allowlist + 20
  char cap, applied symmetrically inbound + outbound at the nameplate
  surface).
- 2026-05-25 LATE — **F3 RestoreVitals + F4 TeleportClient dev features**
  added (host fires for both peers; client F4 hotkey suppressed;
  `[dev] devkeys=1` ini-gated). F3 refills food/sleep/health
  (coffeePower deliberately excluded — triggers screen-shake BP
  side-effect). F4 uses VOTV's `teleportWObackrooms` UFunction
  (bypasses CMC constraints).
- 2026-05-25 LATE — **Story-object terminals (computers/servers/radar/
  signal-catching) concurrently interactable + analog controls synced**
  added to In scope; user. Widget atlas (`Uui_consolesAtlas_C`) renders
  to RT — state sync = visual sync free. 4 wire packet additions
  planned. Delivery drone (Adrone_C) body pose + FX/state sync SHIPPED
  (proto v48 2026-06-08, extended v69 dust anchor). Host-authoritative
  singleton transform stream (~20 Hz while Active); client suppresses the
  drone ReceiveTick and mirrors via LerpWindow interp; connect-snapshot
  adopts a joiner to the live pose. Cargo rides the existing prop pipeline.
  Code: `coop/drone_sync.{cpp,h}` + `ue_wrap/drone.{cpp,h}`;
  ReliableKind::DroneState=38 / DroneStatePayload (40 B). Commits 77225106
  (add) + f32ed1b0 (v69 FX).
- 2026-05-25 NIGHT — **Doors (E-press + NPC auto-open) + light switches**
  added to In scope; user. Replaces terminals as the immediate sync
  target (simpler protocol surface). Hook/invoke =
  `Adoor_C::doorOpen/doorClose` + `Atrigger_lightRoot_C::SetActive`.
  3 new ReliableKind packets (DoorState + LightState + LockState).
  Symmetric for player-triggered; host-authoritative for
  NPC-sensor-triggered (NPCs are host-only). RE pass complete
  (`research/findings/votv-doors-and-lightswitches-RE-2026-05-25.md`);
  7-increment implementation queued.
- 2026-05-26 — **Player count raised to 4** (host + 3 clients); user
  verbatim "It should handle at least 4 players". Replaces the prior
  2-player target. `kMaxPeers=4` shipped in `Registry` + `Session`
  (PR-4 of the GNS migration 2026-05-28).
- 2026-05-29 — **Foundation audit v2** identified that the network
  topology is still fundamentally 2-player today (no host-relay between
  clients for PoseSnapshot/PropPose/ItemActivate; NPC AI per-client).
  Doc: `research/findings/votv-architecture-audit-2026-05-29.md`.
  PR-FOUNDATION-1..5 work-list queued.
- 2026-05-25 — **Voice chat** parked as future (mention only; out of
  current scope). Plasmo-Voice-style proximity positional + push-to-
  talk; would reuse the local player's `UAudioCaptureComponent` for
  capture and a new VoiceFrame unreliable packet.
- 2026-05-30 — **Scripted pursuit/chase events target ALL peers** (user):
  e.g. the zombie-kerfur event must pursue any peer, not just the host
  (and may switch targets between peers). Host-authoritative target
  selection over all players + replicated chosen-target/pose. Concretizes
  the 2026-05-24 enemies-target-both decision; FUTURE (Phase 5N1 NPC AI
  targeting).
- 2026-05-30 — **Player health / vitals / death sync** added to In scope;
  user (foundation-audit critical realization #2). Per-peer-authoritative
  health (owner computes, others display) + host-delivered enemy hits
  (reliable `PlayerDamage`, host-only/not-relayable) + self-attested reliable
  `PlayerRagdollState` (carries death vs passOut/faint — `ragdollMode`/
  `isRagdoll` is SHARED with sleep, so sleeping shows a sleeping puppet).
  Continuous health(as fraction)/food/sleep piggybacked on `PoseSnapshot`
  (8-bit, zero size change via the existing `_pad[3]`). Reliable event is sole
  DEAD<->ALIVE authority; unreliable stateBits display-only. Protocol v18->v19.
  Revive/respawn deferred pending respawn-mechanism RE. MTA shape:
  `CPlayerPuresyncPacket` stream + `CPlayerWastedPacket` death +
  `CPlayerSpawnPacket` respawn. Design + adversarial-verify must-fixes:
  `research/findings/votv-player-vitals-death-RE-2026-05-30.md`. Concretizes
  the enemies-target-both decision (puppets are what enemies hit; this
  replicates the resulting health/death). SHIPPED across Inc1/Inc2b/Inc3 —
  see the 2026-05-30/05-31 entries below (commits 6f5949c7 Inc1 health/food/
  sleep stream + nameplate bar; fee45289 death policy permadeath-rejoinable,
  respawn/revive CUT; 848c45a9 Inc2b ragdoll/faint stateBit; d66d0022 ragdoll
  physics v22; d57350da/5acd36b9/1369b8aa Inc3 PlayerDamage relay + hurt-flash).
  The only deferred sub-part is the real enemy->puppet damage DETECTION hook
  (player_damage.cpp drives the relay via DebugForceHitPuppet for now).
- 2026-05-30 — **Death lifecycle policy decided + vitals Inc1 SHIPPED.** User
  decision: **permadeath, rejoinable; host death ends the session.** Both host
  and peer death use VOTV's native SP death->menu flow — NO death->menu intercept
  (principle 6: native behavior is acceptable for coop). Host death = session ends
  for all (== host exit, no migration); peer death = disconnect to menu (existing
  teardown; client save already blocked) + REJOIN via existing late-join snapshot.
  CONSEQUENCE: **respawn/revive CUT (RULE 2)**; Inc2 ragdoll narrows to the
  non-death faint/sleep/KO states; Inc3 PlayerDamage (combat loop) unchanged.
  Vitals **Inc1 SHIPPED 6f5949c** (Pelmentor): continuous health/food/sleep on
  PoseSnapshot `_pad[3]` (zero size change, v18->v19) + display-only nameplate
  health bar; pre-deploy checklist PASS, sender resolution proven on both peers.
- 2026-05-31 — **Vitals Inc2b ragdoll/faint sync SHIPPED + DESIGN PIVOT.** The
  banked Inc2 design was a reliable `PlayerRagdollState` event; Inc2b instead ships
  a CONTINUOUS per-peer DISPLAY bit (`PoseSnapshot.stateBits` bit1 `kStateBitRagdoll`,
  protocol v19->v20, zero size change). Driven by RE that landed AFTER the plan:
  (a) user confirmed the player ragdolls on the **C key** (manual
  `InpActEvt_ragdoll_..._25`) in addition to exhaustion faint + KO — all flip the
  SAME `isRagdoll` bit; (b) mainPlayer_C has NO readable `passOut` field (only a
  `ragdollMode` param), so the reliable payload's passOut/death/cause were
  unpopulatable; (c) faint is transient + lossy-tolerant + death-excluded =
  a pure display state (MTA `CPlayerPuresyncPacket` flag shape). Sender sets the
  bit = `isRagdoll && !dead`; receiver reconciles the puppet against the puppet's
  ACTUAL isRagdoll (`ragdollMode(1,1,0)` rising / `forceGetUp()` falling), self-
  healing + idempotent. The bit rides the proven per-peer pose relay, so the
  reliable design's machinery (new ReliableKind, relay-whitelist, lane, defer/latch,
  connect-replay, per-slot disconnect) is ALL unnecessary (those existed only to
  survive a dropped-transition permanent desync the self-healing bit can't have).
  P7 split clean (ue_wrap engine accessors + reflected_offset; coop = 1 sender line
  + 1 edge-latch reconcile). RECEIVER VISUAL: VOTV's real `ragdollMode` spawns a
  separate `playerRagdoll_C` physics actor that LEAKS +5 GB on the tickless orphan
  puppet (PendingKill but never GC-reaped, keeps simulating) -- root-caused via 6
  teardown attempts + 2-agent IDA/SDK RE + probes; USER chose to ragdoll the
  puppet's OWN mesh instead (borrow `kerfurOmegaV1_PhysicsAsset`, SetPhysicsAsset +
  enable collision + SetAllBodiesSimulatePhysics) -- leak-free, probe-confirmed
  flopping (`IsAnyRigidBodyAwake=1`, host RSS stable 3.5 GB). e2e wire autotest:
  client drives local ragdoll, host confirms slot-1 puppet flips isRagdoll + the
  mesh physically flops. Death stays native SP flow (excluded). Inc3 PlayerDamage
  still pending.
- 2026-06-10 — **Ambient flora/forage spawners host-authoritative-by-suppression
  on connected clients; dirthole mounds per-peer LOCAL** (Fork C of the snapshot-
  adoption session). A connected CLIENT PRE-cancels its own
  `mushroomMaster_C::spawn`, `mushroomSpawner_C::spawn` and
  `pineconeSpawner_C::ReceiveTick` (new `coop/ambient_spawner_suppress`,
  runtime-gated on running()+role==Client — SP and post-session solo play
  unaffected); the HOST's spawn results stream over the existing prop pipeline
  with v54 identity. Consequence: a connected client loses its local "a shroom
  grew" hint toasts (never synced anyway). REVERSAL: the
  `undergroundGarbageSpawner_C::Timer` suppression (Inc 3, 2026-05-27) is
  DELETED — bytecode-falsified (it mints only `dirthole_item_C` buried-item
  mounds, which are OUTSIDE the snapshot/broadcast universe; suppressing it
  deleted the client's loot mounds with no host replacement). Dirthole mounds +
  their dug loot are now per-peer LOCAL while connected (dug loot becomes shared
  the moment it is grabbed — normal client-owned prop flow). Same session also
  widened the adoption sweep to the full expressible universe + keyless chipPile
  eid expression (protocol v55); RE + verdicts:
  `research/findings/votv-snapshot-adoption-root-causes-2026-06-10.md`.
- 2026-07-09 — **Signal-SERVER simulation state + its notifications** added to In
  scope; user ("Build A. Per rule 1."). DISTINCT from and DEEPER than the
  2026-05-25 "terminals interactable + analog controls synced" entry (that was the
  console WIDGET visual+analog; this is the underlying server SIM). Root problem
  (RE, `docs/notifications/` + `research/findings/votv-notifications-suppress-mirror-DESIGN-2026-07-09.md`):
  VOTV's signal-server state — `mainGamemode.{servers, brokenServers,
  serverEfficiency_calc/downl}` + per-`serverBox_C.{isBroken,damaged,upgrades,health}`,
  broken/fixed by the host-only `ticker_serverBreaker` — is NOT UE-replicated, so a
  coop CLIENT self-computes DIVERGED server state and self-authors FALSE notices
  (the `SERVER "X" is down` EMAIL + console `writeToLog` line + `serverDown` alarm;
  it is NOT a HUD toast). FIX SHAPE (`/qf`-converged ROOT, RULE-1): the HOST owns
  server state; clients MIRROR + render and do NOT run the server sim (neutralize
  the client `ticker_serverBreaker`) — so the client never self-authors and no
  per-channel notice suppressor is needed (that was the NARROW crutch). Likely two
  wire lanes (server-STATE poll = alarm_sync shape; break/damage EDGE = event_fire
  shape). Gated behind an authoring census (measure-don't-infer) before build.
  In-scope notices to mirror: the server down/damaged EMAIL + console line + alarm,
  the SAT-console `sv.*/tw.*/cr.*/tr.*` query answers, and the `addGloss`
  signal-glossary hints. OUT of this lane (stay LOCAL per-player): the ~280 generic
  `addHint` interaction/inventory/sanity toasts + first-load tutorial tips.
  NOTE: this necessarily begins mirroring the host EMAIL inbox (`addEmail` →
  `laptop`) for server alerts — a first toe into email sync (broader email sync
  stays out of scope until separately amended).
