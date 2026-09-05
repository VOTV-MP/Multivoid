# Scope

What Multivoid synchronises between peers, and what it deliberately does not. This is principle 5
in practice: anything not listed here is not synced, and a new item is classified against the rules
at the bottom before a line of code is written for it. When a decision changes, its row changes, in
the commit that ships the change.

## Synced

| System | What is synced | Who owns it |
|---|---|---|
| Players | position and pose, the ragdoll and faint display state, health, food and sleep as fractions, the held item, skins, nameplate visibility, nickname colour; enemy hits delivered to the hit peer; death runs natively and the level travel is refused so the world survives | each peer computes its own vitals and death; the host delivers enemy hits |
| Chat and the event feed | text chat; join, leave and error lines | the host relays chat; each machine surfaces its own transport errors |
| Voice | 3D positional voice | each peer |
| Master server and browser | an opt-in listing of the host's game, the update check, signaling for NAT traversal | the host opts in per session; the master is external infrastructure |
| Physics props | pickup, drag, throw and drop; per-grab authority; an identity that survives saves and rejoins; world-to-inventory and inventory-to-world transitions | the holder while held; the host owns everything at rest |
| Piles and trash | the whole collection loop: piles, clumps, carrying, depositing | the host |
| NPCs and creatures | pose, spawn and despawn, the kerfur prop-to-NPC cycle; enemies target every peer and a pursuit can switch targets between peers; creatures whose AI reads the local player are owned per peer and visible to all | the host, except the per-peer owned creatures |
| Doors, lights and devices | doors, light switches and light groups, keypads, power panels, the garage, appliances, locker lids, windows, grime, the wind turbine, containers and their contents, the delivery drone, shop orders, the coin gun and the shared balance | the host holds the state; a client sends an intent that the host performs |
| The signal workstation | the dish, pinging, signal catch, downloads, decoding, the playback deck, drives and racks, tapes, the physical modules, the laptop and its floppies, the meadow signal database, the signal-server simulation and the notices it produces | one authority per axis: the pressing peer authors inputs, the host runs every simulation |
| World state | the clock, the sky, weather and lightning, fireflies, ambient spawners, the story and scheduled events, the base alarm, emails, the daily task, world props that change over time | the host; clients mirror and never roll their own randomness |
| Saves | the joiner boots from the host's world; the host is the only peer that saves | the host |
| Vehicles | the ATV: a peer that is not driving runs the vehicle's physics natively and is corrected toward the driver's pose; its condition travels; vitals, configuration and the act-as-host intents are in scope and not all built | the driver authors the pose; the host the rest |
| Sleep, damage and hazards | sleep state, hazard damage, the resulting vitals | the host for shared effects; each peer for its own vitals |

## Not synced

| What | Why |
|---|---|
| Inventory contents: slot order, stack counts, what is in a pocket | per-peer and private; the UI stays responsive and each player's kit is their own. The world-side effect of a pickup or drop is synced |
| Unreal's built-in replication | the game's Blueprints are single-player; enabling it means editing assets, which the mod never does |
| Outdoor lamp posts | driven by the shared day and night cycle on every machine; they toggle in lockstep without traffic |
| Custom content paks and asset replication | a peer without a skin pak sees the default body, by design; nothing is downloaded from another peer |
| Anti-cheat | the host is trusted and is the admin; every bound and sanity check applies to clients only |
| Hint toasts, tutorial tips, the per-viewer HUD | local to the machine; the HUD reads shared state, it is not itself replicated |

## Rules for classifying a new item

1. **Replicate authoritative state; re-derive the rest.** Animation, physics and rendering are
   computed by each machine's own engine from the streamed state.
2. **The whole map syncs.** There is no area-of-interest culling; bandwidth is gated by whether an
   entity is active, never by distance.
3. **The host rolls shared-world randomness.** A client never rolls a shared outcome.
4. **A client acts by intent.** A discrete, persistent, shared-world change made by a client is an
   intent the host performs, and the intent names what, never what it costs.
5. **Every lane answers the late join.** A peer that joins mid-event, mid-download or mid-drive
   sees the current state; a lane is not done until that answer exists.
6. **Per-peer things stay per-peer.** Own vitals, own inventory, own view.
7. **Four players, about a hundred active entities** is the scale everything is sized for.
