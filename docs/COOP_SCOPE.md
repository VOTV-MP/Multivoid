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
