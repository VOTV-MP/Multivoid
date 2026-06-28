# Sync-tree reorganization — ready-to-execute spec (2026-06-28)

STATUS: **PLANNED, GATED.** Execute ONLY after the sync module is hands-on-verified + PUSHED (clean
baseline) AND the user green-lights. A ~40-file move mid-refactor would wreck the pending hands-on +
bloat the unpushed diff + ruin bisect. This doc is the prep so the moment the gate opens it's mechanical.

Companion to `docs/COOP_SYNC_MAP.md` (the layer taxonomy). The map is the *logical* overlay; this is the
*physical* move that makes the filesystem mirror it — PARTIALLY (peripheral cohesive clusters only), not a
strict 6-layer split (RULE 2: don't force fuzzy-edge files into arbitrary homes).

## Scope decision (pragmatic, NOT strict)
- **MOVE** the 4 loosely-coupled peripheral clusters into new subdirs: `devices/` `world/` `social/` `host/`.
- **LEAVE** the tightly-coupled identity core (props/npc/kerfur/trash/world-actor/streams/save-identity)
  + transport/dispatch/session at the `coop/` root, adjacent to the existing `element/` `sync/` `net/`.
  Folding the ~35-file entity core is the risky part with little gain (it already lives next to element/sync)
  — a SEPARATE later decision, not pass 1.
- Cuts the flat-file count roughly in half; every moved cluster is a clean, self-contained concern.

## Mechanics (per the verified repo shape)
- Headers: `include/coop/X.h`  Sources: `src/coop/X.cpp`. Subdir precedent exists (`element/ sync/ net/ dev/`).
- CMake uses an **explicit** source list (no glob) -> every move needs a `CMakeLists.txt` path edit.
- Includes are rooted `"coop/X.h"` -> moving to `coop/<sub>/X.h` requires a **repo-wide** rewrite of
  `#include "coop/X.h"` -> `#include "coop/<sub>/X.h"` (exact-quote match to avoid prefix collisions).

### Per-cluster procedure (ONE commit per cluster, build-verify each -> bisectable)
For each file X in the cluster, target subdir `<sub>`:
1. `git mv src/coop/X.cpp src/coop/<sub>/X.cpp`
2. `git mv include/coop/X.h  include/coop/<sub>/X.h`
3. `CMakeLists.txt`: `src/coop/X.cpp` -> `src/coop/<sub>/X.cpp`
4. Repo-wide: `grep -rl '"coop/X.h"' src include` -> rewrite each hit to `"coop/<sub>/X.h"` (incl. X.cpp's
   own self-include). Exact `"coop/X.h"` quote match.
After the WHOLE cluster: `cmake --build ... --config Release` GREEN -> hash-verify unchanged-behavior is moot
(no code change) -> commit `[sync-reorg move] <cluster> -> coop/<sub>/`. Then the next cluster.

## The MOVE sets (explicit -- .h + .cpp pair each)

### `coop/devices/` -- L2 keyed device/object state (16)
interactable_sync, interactable_channel, keypad_sync, power_sync, window_sync, grime_sync, turbine_sync,
device_occupancy, atv_sync, drone_sync, order_sync, sleep_sync, comp_sync, console_state_sync, signal_sync,
signal_catch_sync, signal_wire
> NOTE: `interactable_channel.h` is currently in the HELD/uncommitted L5-instrumentation set. Resolve that
> (commit or revert) BEFORE the reorg so the move isn't tangled with unrelated edits.

### `coop/world/` -- L3 global/ambient scalars (9)
time_sync, sky_sync, weather_sync, weather_redsky, weather_lightning, weather_fog, firefly_sync,
event_cue_sync, balance_sync

### `coop/social/` -- L4 player-scoped / social (11)
player_inventory_sync, inventory_pickup_sync, inventory_wire, item_activate, chat_sync, chat_feed,
email_sync, player_damage, wisp_tear_mirror, wisp_attack_sync, flashlight_click_sound

### `coop/host/` -- L5 host control / moderation / suppression (10)
moderation, ban_list, save_guard, save_block, save_button_disable, save_indicator_suppress, shutdown,
ambient_spawner_suppress, multiplayer_menu, garbage_sync

## STAY at `coop/` root (rationale)
- **Identity core** (it IS the sync module's entity layer, deeply coupled to element/ + sync/):
  remote_prop, remote_prop_spawn, prop_element_tracker, prop_lifecycle, prop_echo_suppress, prop_stick_sync,
  prop_synth_key, prop_snapshot, prop_sound, pile_reconcile, trash_proxy, trash_channel, trash_collect_sync,
  trash_pile_sync, trash_clump_pose_stream, local_streams, puppet_carry_drive, npc_sync, npc_mirror,
  npc_world_enum, npc_adoption, npc_pose_drive, npc_pose_host, kerfur_convert, kerfur_entity,
  kerfur_reconcile, kerfur_prop_adoption, kerfur_command, kerfur_menu_input, world_actor_sync,
  host_spawn_watcher, save_identity_bind, save_identity_map, remote_player, nameplate.
- **Transport / dispatch / session**: net_pump, event_feed, event_dispatch_{entity,state,world},
  player_handshake, players_registry, roster, session_manager, subsystems, save_transfer, blob_chunks,
  snapshot_census, join_progress, join_curtain, mirror_defer.
- **Infra / ambiguous (default STAY, least-surprising)**: ini_config (foundational config), grab_observer
  (grab input -> eid, identity-adjacent). Revisit only if a clear home emerges.

## Execution gate (do NOT skip)
1. Sync module hands-on PASSES + pushed (the etalon baseline is on origin/main).
2. Held L5 files resolved (esp. interactable_channel.h).
3. User green-lights the reorg.
THEN: 4 cluster commits (devices -> world -> social -> host), build-verify each, on a branch, one push.
A no-behavior-change reorg still gets a quick smoke (it can only break via a botched include) before merge.
