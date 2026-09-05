# Players

## Purpose

The other players as each peer sees them: the body and its motion, what it holds and toggles,
its name, colour and skin; the state that belongs to one player (vitals, damage, death, sleep,
inventory) and who owns it; the roster, and the host's moderation of it. Chat and voice have
their own page; the props a player carries are on the props page.

## How it works

### The puppet

The remote player follows the parallel class hierarchy: `RemotePlayer`
(`coop/player/remote_player`) owns the network state, the interpolation window and a pointer to
the engine actor it renders through, and the engine actor owns rendering, animation and physics.
That actor is the game's own player class, `mainPlayer_C`, spawned as an unpossessed orphan: no
controller, auto-possession and the AI controller disabled at spawn, the per-screen systems
(post-process, microphone, first-person arms) stripped, the game-mode pointer cleared, and both
the actor tick and the movement-component tick disabled, because a ticking player brain would
run the whole single-player logic and clobber global camera state. The puppet's animation
Blueprint reads its own movement component's velocity and mode, and every tick the mod writes
those fields from the streamed pose, exactly what the possessed pawn does for the local player,
so locomotion and leg IK come from the engine with real floor-trace context. Whether an actor
has a controller is the local-versus-puppet test everywhere in the code
(`coop/player/players_registry`).

### The pose stream

Each peer streams its own pose per tick as an unreliable, newest-wins message: position, yaw,
pitch, the head-to-body yaw delta, speed, a byte of state bits, and health, food and sleep as
fractions (`coop/player/local_streams`). Receivers interpolate in MTA's fixed-window linear form
(`coop/element/lerp_window`): the error is cached once when a packet arrives and applied
linearly over the window, then the target is snapped so float drift cannot accumulate.

Two things the game's first-person pawn never produces are synthesised on the receiver. Turning
in place (`coop/player/puppet_body_yaw`): standing, the body yaw holds and the head leads,
and the body turns only once the camera leads beyond the head's reach; moving, the body
rate-follows the streamed yaw. Footsteps (`coop/player/puppet_footsteps`): the puppet's own tick
is off, so the mod reproduces the game's stride accumulator and dispatches the game's own step
function, which does the ground trace, the per-material cue and the world reactions.

A pose is always stored and always relayed. Nothing at the receive boundary validates motion,
because the game itself teleports the player from ten measured sites, every one in the client's
own process where the host cannot see the trigger; a validator that refused a discontinuity would
refuse a legitimate game event. A discontinuity costs trust in the movement ledger
(`coop/player/movement_ledger`), never display, and any bound is client-scoped.

### Ragdoll and faint

A state bit says the sender is ragdolled: fainted, knocked out or dropped on purpose, not dead.
The receiver spawns the game's own plushie ragdoll body, hides the puppet's meshes for the flop,
and slaves the body's pelvis velocity to the streamed ragdoll pose so the flop tracks the sender;
the puppet stays attached as the anchor for the nameplate and the recovery hand-off
(`coop/player/remote_player_ragdoll`).

### What the player holds and toggles

The item in a player's hand is player state, not a world entity: the game destroys and respawns
the held actor on every hotbar switch. The owner polls its own holding actor and broadcasts the
class and name on change, and every peer keeps a display-only mirror, physics and collision off,
attached to the puppet's weapon component at the native hold transform
(`coop/player/hand_item`). The flashlight rides the item-activation kind: the receiver toggles
the puppet's own light and plays a positional click (`coop/player/item_activate`,
`coop/player/flashlight_click_sound`). A carried trash clump is driven to the puppet's hand on
the host each tick (`coop/player/puppet_carry_drive`); a held physics prop is on the props lane.

### Name, colour and nameplate

The host is the canonical namer. Two clients typing the same name each believe they are unique,
so the one peer that sees every name assigns the display name (a duplicate becomes the name with
a number) and every peer, the named one included, adopts what it is handed
(`coop/player/nickname_arbiter`). Arbitration keys on the roster's occupied rows, so a
reconnecting peer is never renamed for colliding with its own not-yet-reaped ghost.

The nameplate is drawn by the mod's own overlay as a screen-space projection of the puppet's
head: nickname, ping and a health bar, faded with distance (`coop/player/nameplate`, `ui/hud`).
A per-player preference hides your own plate from others, and a nickname colour is a per-player
preference that every surface (plate, chat, scoreboard) reads from one store
(`coop/player/nick_color`). The scoreboard renders a game-thread snapshot of the roster
(`coop/player/roster`, `ui/scoreboard`).

### Skins

Every player carries a body-skin name, kept in the ini, announced in the Join and on change, and
applied through one path to both body slots of a pawn (`coop/player/client_model`). A skin is a
cooked skeletal mesh on the game's own anthro skeleton, so the animation Blueprint drives any
skin unchanged; only the mesh and one texture parameter swap. Three sources
(`coop/player/skin_registry`): the stock body; the game's own kerfur-robot bodies, loaded by
asset path, with the variant's effect rig rebuilt on the player body, the face screen, the glow
and the particles (`coop/player/skin_effects`); and converter paks under the game's mod pak
folder, where a single-skin pak's file stem is the skin's name. The starter bundle carries four
scientists, and a new identity rolls one of them at random. The local first-person body wears the
same skin as the puppet and is re-applied across pawn generations (`coop/player/local_body`).
A peer without a pak sees the stock body, by design; nothing is downloaded from another peer.
The F1 skins panel previews a skin on a live mannequin (`coop/player/skin_preview`,
`ui/skins_panel`). Making a skin from a GoldSrc model, without the editor, is
`tools/client_model/`; the starter bundle is `assets/paks/README.md`.

### Vitals, damage and death

Vitals are per peer: each machine computes its own health, food and sleep and streams them as
fractions for the nameplate's bar. Damage is victim-authoritative, MTA's shape. A physical
impact (a vehicle, a thrown prop, a coin) dispatches the game's impact events on the body that
was hit, and the game's health is a per-machine singleton, so an impact resolving against a
puppet in the host's world would drain the host; an interceptor cancels the three impact entries
on any body that is not the local possessed player, and the victim's own machine computes the
same contact natively (`coop/player/player_damage`). Enemy hits are the one relay: the host runs
the enemies, and when one hits a peer's puppet the host sends that peer a reliable damage event,
which the peer applies on its own pawn, armour and inventory mitigation included, so its health
stream and hurt flash follow.

Death is per peer and runs natively to its end: the sound, the dead flag, the black screen. The
cut is the last stage, the level travel that would unload the world. A detour on the engine's
level-open call refuses it when the local player is dead inside a co-op session, and the mod
writes a revive in its place: the position at the base gate, health, the dead flag, the ragdoll,
and the screen artifacts the travel used to dispose of, the damage quadrants and the blood-loss
effect (`coop/player/death_revive`, `ue_wrap/engine/level_travel`). A host that dies is revived
the same way, so a host death no longer ends the session; a revive that fails returns to the
menu, and on a host that ends the lobby. Other peers see the death through the ragdoll bit.

### Sleep

Sleeping in the game is a per-process time dilation, so a lone sleeper would run twenty times
faster than a shared clock. The gate is Minecraft's (`coop/player/sleep_sync`): a peer entering
a bed keeps the cosmetic half while the dilation is undone every tick; each peer's in-bed edge is
reported, the host tallies and announces how many are sleeping, and when everyone is in bed the
host broadcasts the acceleration. Any wake ends the night for everyone; only a natural end, the
host sleeping it out, grants everyone a full night. Nightmares roll on the host only.

### Inventory

Per player and private: the contents never cross the wire as gameplay. The host stores each
peer's inventory per save under the identity the peer proved at admission and hands it back at
the next join, before the world exists (`coop/items/player_inventory_sync`). The world-side
effect of a pickup or a drop is on the props lane.

### Roster and moderation

Slots are recycled, so who is in a slot is a token, never a boolean (`coop/player/roster_ledger`).
A row carries a session-unique player number, zero when the slot is empty, and everything true of
the person while they are there: nickname, skin, colour, and how they are connected, which the
host measures from the connection and publishes so every board shows the same value. A departure
is a row transition, which is how a lost disconnect and a fast replacement heal the same way.

Kick and ban are host-only actions on the player list (`coop/moderation/`). A ban records the
address, the nickname and the reason in a file next to the mod, is applied at the connection's
accept filter before a seat is spent, and survives host restarts; the host also keeps a
seen-players file for the administration panel. Bans are keyed by address, not by identity.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| pose, vitals, ragdoll | that peer | a sender-authored stream, never gated on receive |
| the display name | the host | assigned for uniqueness; every peer adopts it |
| nameplate visibility, colour, skin | that peer | a preference, relayed to all |
| the hand item, the flashlight | that peer | on change, mirrored for display |
| damage from contact | the victim's machine | other bodies' impact entries are cancelled |
| damage from enemies | the host detects, the victim applies | a reliable relay to the hit peer |
| death and revive | that peer | the native chain, the travel refused |
| sleep | each peer reports; the host tallies, accelerates and ends | |
| inventory | that peer; the host stores it | never on the wire as gameplay |
| roster, kick, ban | the host | rows asserted as state; a ban at the accept filter |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `PoseSnapshot` (stream) | each peer | position, yaw, pitch, head delta, speed, state bits, three vitals |
| `RagdollPose` (stream) | each peer, while ragdolled | pelvis transform and velocity |
| `Join`, `RosterRow` | each peer; the host | identity, nickname, skin, preferences; who occupies each slot |
| `SkinChange`, `NameplateChange`, `NickColorChange` | each peer, relayed | the three preferences |
| `HandItem`, `ItemActivate` | each peer, relayed | the held item's class and name; the flashlight state |
| `PlayerDamage` | host to the hit peer | an enemy hit to apply |
| `SleepState` | each peer to the host; the host to all | in bed or not; the tally; accelerate; end |
| `PlayerInventoryBlob` | both | the per-player inventory, pre-world |
| `TeleportClient` | host to one peer | a placement; the administration action, not the join |

## Late join

The puppet spawns on the first pose that arrives. The roster rows, skin, nameplate and colour
preferences are in the set of messages a host may send before the joiner's world exists, so a
choice made during the load is not lost. Hand items and item states are replayed to the joiner
at its ready edge. A joiner arrives awake: a running acceleration is ended and the tally re-run.
The inventory is seeded before the world. A peer's death state is not seeded; a joiner sees a
dead peer through the ragdoll bit of its next pose.

## Known limits

| Limit | Evidence |
|---|---|
| What another peer sees of a dead player has never been observed in a two-peer run | `[?]` the ragdoll bit is the mechanism; the observation is owed |
| A death drops the held item on every machine; whether that drop rides the drop-intent lane is unmeasured | `[?]` both acceptance runs held nothing |
| No chat line announces a death | `[V]` chat is host-authored; the announce is a wire kind not yet built |
| After a revive the player drifts horizontally from the base gate, metres over seconds, cause unmeasured | `[V]` measured by the death scenario |
| A joining client's spawn at the base gate is overridden late in the load on some runs | `[?]` deferred; the two candidate mechanisms are named in the code |
| Damage a puppet takes on another machine is dropped by design; only the victim's own contacts count | `[V]` the interceptor; the field report it answers |
| A third-party bundle pak cannot be discovered: bundle membership is a fixed table | `[V]` the registry |
| Bans are by address, so a banned player with a new address is a new player | `[V]` the ban file's key |

## Code map

| Concept | Files |
|---|---|
| the remote player and its engine actor | `coop/player/remote_player`, `coop/player/remote_player_ragdoll`, `ue_wrap/actors/puppet` |
| the per-slot puppet lifecycle and drive | `coop/player/puppet_drive`, `coop/player/puppet_body_yaw`, `coop/player/puppet_footsteps`, `coop/player/puppet_carry_drive` |
| the local player's outbound streams | `coop/player/local_streams`, `coop/player/movement_ledger` |
| identity and the roster | `coop/player/players_registry`, `coop/player/roster_ledger`, `coop/player/roster`, `coop/player/nickname_arbiter` |
| name, colour, nameplate | `coop/player/nameplate`, `coop/player/nick_color`, `ui/hud`, `ui/scoreboard` |
| skins | `coop/player/client_model`, `coop/player/skin_registry`, `coop/player/skin_effects`, `coop/player/local_body`, `coop/player/skin_preview`, `ui/skins_panel`, `tools/client_model/`, `assets/paks/` |
| held and toggled items | `coop/player/hand_item`, `coop/player/item_activate`, `coop/player/flashlight_click_sound` |
| damage and death | `coop/player/player_damage`, `coop/player/death_revive`, `coop/player/ragdoll_gate`, `ue_wrap/engine/level_travel` |
| sleep | `coop/player/sleep_sync` |
| inventory | `coop/items/player_inventory_sync` |
| moderation | `coop/moderation/moderation`, `coop/moderation/ban_list`, `coop/moderation/seen_players` |
| tests | `harness/autotest/autotest_death.cpp`, `autotest_damage.cpp`, `autotest_playerdmg.cpp`, `autotest_ragdoll.cpp`, `autotest_puppetframe.cpp`; `python tools/mp.py death` |
