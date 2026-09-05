# Joining a game

## Purpose

What happens between pressing Join and standing in the host's world, and what a peer who joins
in the middle of anything receives. This page covers admission, the save transfer, the client's
world load, the connect replay, the races inside the join window, a world change mid-session,
and leaving and rejoining. The per-system late-join answers are the "Late join" column of
[STATUS.md](STATUS.md); the rule behind them is here.

## How it works

A joiner never generates its own world. Two copies of the game started from the same save
diverge at once: random litter, first-run spawns and per-process keys differ, and nothing
reconciles them afterwards. So the joiner boots from the host's live world, and only what changed
since that capture is replayed on top. History is never replayed; current state is.

### 1. Connect and admission

The browser or a typed address produces a session config (`coop/session/session_manager`) and the
transport connects. Before a seat is spent, the two ends run the admission exchange: the client
sends a nonce, the host answers with its own nonce and a signature over both, the client signs
back, and the password proof rides inside that reply (`coop/net/peer_admission`,
`coop/net/lobby_password`). The host then assigns a slot, and that assignment is the client's
signal to begin: there is no fourth "I am done" message. Each peer's Join carries its element id,
nickname, skin, preferences and game target (`coop/session/player_handshake`); the host asserts
who occupies each slot with roster rows, re-sent as state rather than announced as events, and a
row whose player number is zero means the slot is empty. That is how a departure reaches a
client: it conforms to the current row, it never observes an absence.

The version gate runs twice here. The build number is part of every packet header, so a peer on
another build never parses a message at all. The game target rides the Join payload and is
compared byte for byte; on a mismatch the host kicks with a reason and the client shows why.

Until a client's world is up, the host sends it only a short list of engine-free kinds: the
roster family (slot, roster rows, skin, nameplate and colour preferences), the save transfer,
the per-player inventory, story-event fires (queued on the receiver until its world exists), and
the ready announce itself. Everything else waits behind the world-ready gate. A queue would
duplicate the connect replay, so lines broadcast to a not-yet-ready slot are dropped on purpose
and the lanes that care carry a seed instead (step 5).

### 2. Save transfer

The client asks for the host's world. The host captures its live world into a scratch slot
through the game's own save path (`ue_wrap/engine/save_capture`) and streams the file on the
bulk lane in chunks paced by send-buffer backpressure (`coop/save/save_transfer`). The game
writes a save in place, with no rename, so the file is trusted only when its size and timestamp
hold across two polls and two full reads agree. If the live capture cannot run, the host streams
its slot file as it is on disk (the stale fallback; the reconcile in step 6 owns the difference).
If the host has no save at all, the header says so and the joiner boots a fresh world.

Prepended to the blob, under the same checksum, is the identity sidecar: the map from the save's
array index to the host's element id for the objects the save keeps without a key (chip piles and
powered-off kerfur bodies).

The client verifies the checksum, writes the blob as `zcoop_<pid>.sav`, a prefix the game's load
menu never lists and a name two peers on one machine cannot share, and loads it through the game's
own loader with the host's game mode. Every prop is placed by the engine, at rest, with the host's
keys. The slot lives for the session (the game re-reads it for sub-level saves), is deleted at
disconnect, and a boot sweep removes stale ones older than an hour. The global progression file
is never transferred.

The loading screen (`coop/session/join_progress`, drawn by `ui/loading_screen`) shows four
stages: connecting, downloading with a byte count, loading the world, and receiving the snapshot
with a bar. Cancel stops the session and reopens the browser.

### 3. Pre-world state

The joiner's own inventory is per player and never crosses the wire as gameplay: the host stores
it per save under the peer's identity and pushes it right after the Join, before the world
exists, so the pockets are substituted in the one window before the game materialises the
world (`coop/items/player_inventory_sync`).

### 4. World load and quiescence

The game's loader destroys and respawns the world's props as it reads the save. Those destroys
are the client's own rebuild churn, and broadcasting them would destroy the host's copies by key
(measured on the rig: a bare client join drove the host from thousands of keyed props to a third
of them), so the destroy seam runs inside a world-load episode that suppresses the broadcast
(`coop/session/world_load_episode`). The same module runs the quiescence probe: five times a
second it counts the load-tail population, and the load is settled when the count holds across
consecutive scans. Two deadlines bound a pathological load, one on no progress and one absolute;
past either the client announces anyway, logged loudly as degraded.

The client announces ready once per world, when a gameplay world is up, the prop registry is
seeded for that world, no purge is running, and the probe has settled (`coop/session/net_pump`).
This is the join barrier: no authoritative state lands in a world that is still churning. On
every appearance in a co-op world, the client spawns at the base gate, never at the host's saved
position that the transferred save carries.

### 5. Connect replay

On the ready announce the host runs the replay for that slot, in this order
(`coop/session/subsystems`):

1. **Explicit deletes.** One destroy per keyed prop the joiner's blob had and the host's live
   world no longer has (grabbed, eaten, converted during the download). The joiner drops exactly
   those instead of inferring them.
2. **The snapshot bracket.** A begin marker, one spawn per live keyed prop, an end marker. On the
   client an existing actor is adopted by key, a missing one is created, and a transform that
   differs converges to the host's.
3. **Position corrections** for the save-authoritative objects the host moved during the window.
4. **Every state lane's current state**, each in its own kind: held items, weather, doors, lights
   and containers, keypads, the clock, the sky, power, the ATV, the drone, the turbine, device
   occupancy, the console and desk, physical modules, drives and racks, the laptop and its
   buffer, the floppy box, container contents, the dish, sleep, the decode pane, voice states,
   windows, grime, trash piles, NPCs and world actors, the balance, hand items, in-flight events,
   the alarm, server boxes, roaches, the pyramid gather, event cues, and the chat record, which
   lands retained rather than replayed across the screen.
5. **Seeds** for the lanes whose lines were broadcast during the load window: a multiset of the
   lane's content is captured at the blob instant, and at the ready edge the delta against the
   live state is sent as appends and hash-keyed deletes (`coop/session/join_seed` for saved
   signals and emails; the meadow database has its own).

### 6. Reconcile and reveal

The begin marker arms a claim bracket on the client: every actor a spawn binds is claimed. The
end marker arms the divergence sweep, deferred to quiescence: the locals the host never claimed
are the client's save-loaded world minus the host's, and they go
(`coop/props/join_membership_sweep`). Two bounds keep a racing bracket from wiping a world: a
per-class completeness floor keeps a class when the host expressed fewer of it than the joiner's
census counted, and a valve aborts a sweep that would remove more than half of what it sees.

Visually the joiner never sees the reconcile. A full-screen curtain rises at connect and fades
out when the primary world is assembled, at the end marker plus the spawn drain, seconds before
quiescence (`ui/join_curtain`). Every host mirror spawns hidden and without collision
(`coop/element/mirror_defer`); the confirmed ones are revealed as the curtain lifts, the ones
still pending adjudication are revealed at quiescence, after the sweep has run.

### The join window

Between the blob capture and the client's quiescence the host keeps playing, and every change
it makes has an owner: deletes by the blob diff, adds and moves by the bracket and the
corrections, lines by the seeds, roster changes by the pre-world set. Identity across the two
channels is the hard part. A keyed prop is the same object on both sides by its key, because
both loaded the same bytes. An object the save keeps without a key (a chip pile, a kerfur body)
is tied to its host id by the exact position it had at the blob instant, carried on the spawn
that delivers it and matched at quiescence (`coop/props/pile_spawn_bind`,
`coop/element/quiescence_drain`, `coop/creatures/kerfur_reconcile`); a pile the host grabs
inside the window has its id minted at the grab edge so the identity survives the pile-to-clump
change of actor. The index-to-id sidecar of step 2 is the intended replacement for the position
match; see the limits.

### A world change mid-session

Cave and level travel replace the engine world. The client's registry re-seeds, and when the
current world differs from the one it announced against, the client re-announces after a fresh
probe session settles the new world's load tail; the host runs the connect replay again. The
replay is idempotent, and the join-once placement is not repeated.

### Leaving and rejoining

A slot teardown is a roster-row transition: the leaver's mirrors, proxies, seed brackets and
half-assembled transfers are dropped, its send gate closes, and the slot is recycled with no
observable absence. A rejoin runs the whole pipeline again; after death a client returns to the
menu and may rejoin. The host owns the save throughout. A client cannot write the world save: a
native detour at the engine's one write chokepoint cancels the world-save container on a client
(`coop/save/save_block`), the pause-menu Save button is greyed out to say so
(`coop/save/save_button_disable`), and the host's save directory is backed up before a session
because the game's in-place writes leave no other recovery (`coop/save/save_guard`).

## Who owns what

| State | Owner | Shape |
|---|---|---|
| the world at join | the host | its live world, captured and streamed whole |
| what changed since the capture | the host | explicit deletes, the snapshot bracket, position corrections, per-lane seeds |
| the ready signal | the client | announced at quiescence; the host never guesses when a world is loaded |
| the joiner's placement | the client | the base gate on every appearance |
| the joiner's inventory | the joiner; the host stores it | pushed before the world exists |
| the world save | the host only | clients are blocked at the write chokepoint |
| the reconcile verdict | the client, bounded | the membership sweep with a floor and a valve |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `AuthHello`, `AuthChallenge`, `AuthProof` | both | the admission exchange, before any slot exists |
| `AssignPeerSlot` | host to client | the slot and the host's element id; means admitted |
| `Join` | each peer | element id, nickname, skin, preferences, game target |
| `RosterRow` | host to all | who occupies a slot; zero means empty; re-sent as state |
| `SaveTransferRequest`, `SaveTransferBegin`, `SaveTransferChunk` | client, then host | the request; total bytes, sidecar bytes, checksum, game mode; the chunks |
| `PlayerInventoryBlob` | both | the per-player inventory, pre-world |
| `ClientWorldReady` | client to host | once per world, at quiescence |
| `SnapshotBegin`, `PropSpawn`, `PropSnapPos`, `PropDestroy`, `SnapshotComplete` | host to one client | the bracket |
| `EventSnapshot` | host to one client | one per in-flight event |
| `ChatLine` | host to one client | the retained chat record, one line each |

Every state lane adds its own kind for step 5.4; the family routers in `coop/dispatch/` own them.

## Late join

A peer joining mid-event, mid-download, mid-drive or mid-anything converges to the host's current
state, and a lane is not done until its answer is written and built. The answers take five
shapes: a **snapshot** re-sends the current state; a **seed** sends the delta since the blob
instant; a **park** disables the joiner's own copy until authority arrives; a **replay** re-runs
an in-flight event on the joiner from the start, with an override that bypasses the "already
happened" record the transferred save carries; an **unlatch** releases a hold the joiner arrived
under. "Don't join during X" is not an answer.

The rule the answers stand on: the host's tracking seams gate on hosting, never on "a peer is
connected". An event actor spawned while the host is alone must still be tracked, or the snapshot
has nothing to send when someone joins. Two residuals are accepted by design: one-shot effects
(a click, a running ping's stage visuals) are missed, and a lane-owned event on the joiner does
not raise the game's own active-event counter, whose save and pause blocks the mod holds anyway.

## Known limits

| Limit | Evidence |
|---|---|
| The transfer runs at the transport's minimum send rate, about one megabyte per second, with no bandwidth estimation, so a large world is a long download | `[V]` measured on an internet link; two field joiners quit inside the first seconds before the byte counter existed |
| The world load takes thirty to sixty seconds and is capped by the probe's deadlines; a load past them announces into a world that may still be churning | `[V]` the probe logs the deadline mode |
| Keyless save-loaded objects are matched by position; the index-to-id sidecar is built, transferred and bound in the rig, but the bind is behind a developer flag until it is verified by hand | `[V]` rig runs bind every entry; not hands-on |
| A host change inside the window that post-dates the snapshot (a kerfur turned off) materialises at quiescence, after the curtain has lifted, as a visible pop-in | `[V]` measured; delivery is correct, the order is cosmetic |
| A local save-loaded actor repositioned after the curtain lifts is visible; the short curtain is chosen over a blank screen | `[?]` rare, not quantified |
| The trash-pile mirror stands in a bare proxy for the engine's own actor instead of driving it; it is queued for a rebuild | `[V]` [ROADMAP.md](ROADMAP.md) |
| The stale fallback streams the on-disk slot, which may be older than the live world | `[V]` logged when it happens |
| The true mid-event join of the walking pyramid is still the open hands-on acceptance test | `[?]` autonomous packet flow only |

## Code map

| Concept | Files |
|---|---|
| the browser to a session config | `coop/session/session_manager`, `coop/session/host_mode` |
| admission and identity | `coop/net/peer_identity`, `coop/net/peer_admission`, `coop/net/lobby_password` |
| the handshake | `coop/session/player_handshake` with its version, nickname, preferences and roster halves |
| the client join state machine and its screen | `coop/session/join_progress`, `ui/loading_screen`, `ui/join_curtain` |
| the save transfer | `coop/save/save_transfer`, `ue_wrap/engine/save_capture`, `coop/props/save_identity_map`, `coop/props/save_identity_bind` |
| the host owns the save | `coop/save/save_guard`, `coop/save/save_block`, `coop/save/save_button_disable`, `coop/save/save_indicator_suppress` |
| the world-load episode and the probe | `coop/session/world_load_episode` |
| the announce, the placement, the edges | `coop/session/net_pump`, `coop/session/teleport_client` |
| the connect replay | `coop/session/subsystems`, `coop/props/prop_snapshot`, `coop/props/snapshot_census`, `coop/session/join_seed` |
| the reconcile | `coop/props/join_membership_sweep`, `coop/props/pile_spawn_bind`, `coop/element/quiescence_drain`, `coop/creatures/kerfur_reconcile`, `coop/element/mirror_defer` |
| the pre-world set and the lanes | `coop/net/session_lanes.h` |
| tests | `python tools/mp.py smoke` (the two-peer join), `harness/autotest/autotest_reloadchurn.cpp` (world-change churn) |
