# Code map

One folder, one concept, named after it. `src/votv-coop/src/` holds the implementation and
`src/votv-coop/include/` the headers, in the same layout. Paths below are relative to
`src/votv-coop/src/`; a name without an extension is a `.cpp`/`.h` pair.

## The layers, top to bottom

| Layer | Folder | Owns | Never contains |
|---|---|---|---|
| gameplay and network | `coop/` | elements, sync lanes, sessions, players, the wire protocol | reflection or engine-memory access (it goes through `ue_wrap/`) |
| what the player sees | `ui/` | the menus, the server browser, the HUD, the F1 overlay | network state |
| boot glue and tests | `harness/` | the scenario runner and the autonomous test scenarios | gameplay logic |
| engine wrapper | `ue_wrap/` | reflection, signatures, hooks, the game-thread pump, one wrapper per engine or game class | network, gameplay or co-op state |
| the loader contract | `loader/`, `bootstrap/` | `start_mod()` as UE4SS calls it, `DllMain`, the refuse dialog | everything else |

## `coop/` — gameplay and network

| Folder | Concept | Key files |
|---|---|---|
| `coop/net/` | transport, sessions, admission, the wire protocol | `protocol.h` (every message kind and the build number), `session` (GameNetworkingSockets host and client, four peers, the lanes) with `session_streams` (the unreliable per-tick streams) and `session_*` send helpers, `session_start` (LAN or P2P entry), `signaling_client` + `ice_config` (NAT traversal through the master server), `lobby_client` + `lobby_announcer` (the server browser's list), `peer_identity` + `peer_admission` + `lobby_password` (the Ed25519 identity, the mutual challenge, the password), `send_backlog` (a reliable send is delivered or the connection dies), `blob_chunks` (large transfers), `http_client` |
| `coop/dispatch/` | routing of reliable messages | `event_feed` drains and dispatches; `event_dispatch_entity`, `_state`, `_signal`, `_intent`, `_world` are the five family routers, each owning its message kinds |
| `coop/session/` | the per-tick orchestrator and the join | `net_pump` (connect and disconnect edges, announcements), `session_manager`, `player_handshake` with `_version` (the game+build gate), `_nick`, `_prefs`, `_roster` (the Join and PlayerJoined messages), `join_seed`, `join_progress`, `world_load_episode`, `host_mode`, `pause_guard` (no pause while connected), `shutdown`, `teleport_client`, `subsystems` |
| `coop/element/` | entity identity and lifecycle: the one owner of eid to actor | `registry`, `element`, `mirror_manager` + `mirror_managers`, `identity_create` (create-or-adopt a mirror), `identity_destroy` (retire one), `element_deleter` (deferred destroy), `portable_identity` (a name both peers compute for an actor the save does not key), `intent_authority`, `object_scan_hub` (the one sliced object-array pass every index rides), `quiescence_drain`, `mirror_defer`, `lerp_window`, and the element kinds `prop`, `npc`, `player`, `world_actor` |
| `coop/player/` | remote players and per-player state | `remote_player` + `remote_player_ragdoll`, `puppet_drive`, `puppet_carry_drive`, `puppet_body_yaw`, `puppet_footsteps` (driving the engine pawn), `players_registry`, `roster` + `roster_ledger` (who is in each slot), `nickname_arbiter`, `nick_color`, `nameplate`, `hand_item`, `local_body`, `local_streams`, `client_model` + `skin_registry` + `skin_preview` + `skin_effects` (skins), `movement_ledger`, `player_damage`, `death_revive`, `ragdoll_gate`, `sleep_sync`, `item_activate`, `flashlight_click_sound` |
| `coop/props/` | physics props, piles and trash | `prop_lifecycle` (the destroy seam), `prop_snapshot` + `snapshot_census` (the connect snapshot), `remote_prop` with `_spawn`, `_convert`, `_physics`, `_destroy` (the receivers), `host_spawn_watcher`, `prop_key_index`, `prop_synth_key`, `save_identity_bind` + `save_identity_map` (bind save-loaded actors to host eids in the join window), `pile_spawn_bind`, `native_pile_mirror`, `trash_channel`, `trash_proxy`, `trash_grab_intent`, `trash_collect_sync`, `trash_pile_sync`, `trash_use_intercept`, `trash_clump_pose_stream`, `prop_drop_intent`, `prop_stick_sync`, `container_contents_sync`, `grab_observer`, `registry_reaper`, `join_membership_sweep`, `unresolved_pose_ledger`, `prop_echo_suppress`, `prop_wire_parity`, `prop_census`, `prop_container_extract`, `prop_destroy_seam`, `prop_fresh_spawn`, `prop_element_tracker`, `prop_sound`, `active_drive` |
| `coop/creatures/` | NPCs and creatures | `npc_sync` + `npc_sync_install` (the spawn seam), `npc_mirror`, `npc_adoption`, `npc_pose_host`, `npc_pose_drive`, `npc_world_enum`, `kerfur_convert` with `_host` and `_client`, `kerfur_entity`, `kerfur_reconcile`, `kerfur_prop_adoption`, `kerfur_command`, `kerfur_menu_input`, `kerfur_form_assembler` (the prop-to-NPC conversion cycle), `owner_entity_sync` (creatures whose AI reads the local player: owned per peer, visible to all), `piramid_sync`, `roach_sync`, `wisp_attack_sync`, `wisp_grab_hold`, `wisp_tear_mirror` |
| `coop/interactables/` | keyed device state | doors, lights, light groups, containers, garage, appliances, lockers: `interactable_sync` + `interactable_channel`; `keypad_sync`, `power_sync`, `window_sync`, `grime_sync`, `turbine_sync`, `drone_sync`, `device_occupancy`, `garbage_sync`. The signal workstation: `signal_sync`, `signal_catch_sync`, `signal_wire`, `dish_sync`, `desk_input_sync`, `desk_sim_sync`, `desk_cursor_sync`, `desk_snd_fx`, `deck_play_sync`, `comp_sync`, `console_state_sync`, `drive_sync`, `drive_rack_sync`, `tape_caddy_sync`, `physmods_sync`, `serverbox_sync`, `laptop_sync`, `laptop_buffer_sync`, `floppybox_sync`, `meadow_db_sync`. The ATV: `atv_sync`, `atv_corrector`, `atv_hit_guard`, `atv_condition_sync` |
| `coop/world/` | global and ambient state | `time_sync`, `sky_sync`, `weather_sync` with `weather_rain`, `weather_fog`, `weather_lightning`, `weather_redsky`, `weather_event_births`, `firefly_sync`, `spawn_authority` (shared-world spawners run on the host only), `event_fire_sync`, `event_active_sync`, `event_cue_sync`, `alarm_sync`, `balance_sync`, `email_sync`, `daily_task_sync`, `world_actor_sync` + `world_actor_mirror` |
| `coop/items/` | inventory and the economy | `player_inventory_sync` + `inventory_wire`, `inventory_pickup_sync`, `order_sync` (shop orders: the client names the row, the host performs and prices it), `coingun_sync` + `coingun_arbiter` + `coingun_collect` (the sell gun and its coins), `save_record_wire` |
| `coop/comms/` | text chat | `chat_sync`, `chat_feed`, `chat_bubbles`, `chat_log`, `chat_nick_color`, `peer_action_feed` |
| `coop/voice/` | voice chat | `voice_capture`, `voice_chat`, `voice_playback` (Opus, 3D positional) |
| `coop/moderation/` | kick and ban | `moderation`, `ban_list`, `seen_players` |
| `coop/save/` | the host owns the save | `save_transfer` (streams the host's world to a joiner), `save_guard`, `save_block`, `save_button_disable`, `save_indicator_suppress` |
| `coop/text/` | text encoding and names | `utf8_codec` (the one owner of decoding), `repertoire` + `repertoire_ranges` (what this build can draw), `case_fold`, `mark_ranges`, `ignorable_ranges`, `exclude_ranges`, `novelty_ledger` |
| `coop/config/` | the ini | `config`, `config_registry` + `config_registry_rows` (every key is a registered row with a description), `config_ini_write`, `config_review`, `config_selftest` |
| `coop/input/` | who owns the keyboard | `input_owner` (the game, a text field, or the overlay) |
| `coop/dev/` | developer features and probes, ini-gated, off by default | `freecam`, `pos_hud`, `set_clock`, `spawn_npc`, `force_weather`, `event_force`, `event_trigger`, the `*_probe` and `*_selftest` instruments, `perf_probe`; `dev/director/` is the autonomous director that drives a player through a test scenario |

## `ui/` — what the player sees

| Concept | Files |
|---|---|
| the Multiplayer menu | `multiplayer_menu`, `host_window_native`, `host_save_picker`, `host_session_settings`, `host_session_choices`, `browser_input_screens`, `connect_failed_dialog`, `boot_warning_dialog`, `menu_sfx` |
| the server browser | `server_browser_native` with `server_browser_rows`, `server_browser_actions`, `server_browser_panels`, `server_browser_surface` (which browser is open) and `server_browser_selftest`; `server_browser` is the ImGui fallback |
| the native UI kit | `native_screen`, `native_text_field`, `style`, `scale`, `link_format` |
| in-game surfaces | `hud` (nameplates, chat, the event feed), `chat_input`, `chat_view`, `scoreboard`, `loading_screen`, `join_curtain`, `voice_icons` |
| the F1 overlay | `imgui_overlay`, `overlay_backend` with `overlay_backend_dx11`, `overlay_backend_dx12`, `overlay_backend_dx12_capture`, `overlay_cursor`, `overlay_diag`, `overlay_test_arm`, `fonts`, `atlas_watch`, `input_focus` |
| overlay panels | `dev_menu`, `admin_panel`, `skins_panel`, `voice_panel`, `world_rules_panel`, `config_review_panel`, `net_stats_panel`, `console` |

## `harness/` — boot glue and autonomous tests

`harness` (the scenario runner), `session_runtime`, `autotest` + `autotest_dispatch` and `harness/autotest/`
(one file per scenario: join churn, grab, ragdoll, weather, events, death, damage, pause guard, save UI,
scan parity and more), `sdk_check`, `screenshot`, `mod_environment`, `harness_diag`. `tools/mp.py` drives
these from outside the game.

## `ue_wrap/` — the engine wrapper

| Folder | Concept | Key files |
|---|---|---|
| `ue_wrap/core/` | the substrate | `sig_scan` (AOB resolution of `GUObjectArray`, `GNames`, `ProcessEvent`), `reflection` + `reflection_props` + `reflected_offset` (classes, properties, functions), `call` (calling a reflected function), `hook` + `pe_detour` + `ufunction_hook` (the MinHook `ProcessEvent` detour and per-function interceptors), `vm_dispatch` (bytecode-level interception of Blueprint-internal calls), `game_thread` (the posted-task pump; engine calls happen only there), `cached_obj_ref` + `gc_pin` (object lifetime across worlds), `sdk_profile` + `sdk_profile_names` (the per-game-version offsets and names), `fname_utils`, `fstring_utils`, `ftext_utils`, `field_io`, `engine_heap`, `asset_load`, `hot_path_guard`, `walk_timer`, `log`, `trace`, `paths`, `types`, `hook_drill`, `pe_diag` |
| `ue_wrap/engine/` | engine objects | `engine` (world, actors, spawning), `engine_pawn`, `engine_component`, `engine_widget` + `umg_build` (UMG), `engine_bones`, `engine_attach`, `engine_audio`, `engine_nav`, `engine_save` + `save_capture` + `save_browser` + `gvas_meta` (save files), `engine_mainplayer`, `engine_playerragdoll`, `level_travel`, `spawn_gate`, `spawn_menu`, `scs_rig`, `world_identity` (which world is current) |
| `ue_wrap/actors/` | the game's actor classes | `puppet` + `puppet_spawn`, `prop`, `kerfur`, `wisp`, `inventory`, `vitals`, `sleep`, `swinger`, `save_record`, `begin_equipment` |
| `ue_wrap/devices/` | keyed devices | `door`, `door_box`, `lightswitch`, `passwordlock`, `power_control`, `garage`, `appliance`, `base_window`, `grime`, `windturbine`, `drone`, `atv` + `atv_condition`, `laptop`, `portable_pc`, `floppybox` |
| `ue_wrap/desk/` | the signal workstation | `dish`, `console_desk`, `coords_panel`, `comp_pane`, `device_screen`, `drive_chain`, `tape_caddy`, `phys_mods`, `saved_signals`, `meadow_store`, `signal_dynamic`, `space_renderer`, `desk_audio`, `daily_task` |
| `ue_wrap/world/` | world singletons | `daynightcycle`, `skysphere`, `directionalwind`, `game_rules`, `economy`, `order_economy`, `store_catalog`, `email`, `votv_lib` |

## `loader/` and `bootstrap/`

`loader/cppmod_entry` exports `start_mod()` and `uninstall_mod()`, the two functions the UE4SS mod scan
calls, and refuses to start beside a leftover pre-mod-folder install; `cppmod_stubs.asm` carries the
stubs. `bootstrap/dllmain`, `boot` and `refuse_dialog` are the process entry and the one dialog the mod
can show before the game's UI exists.

## Adding a sync lane

1. Decide who owns the state (see [ARCHITECTURE.md](ARCHITECTURE.md)): the host owns it and
   clients mirror; a client names an intent and the host performs it; or each peer owns its own
   copy and the rest see it.
2. Add the message kind to `coop/net/protocol.h` and bump the build number: the wire changed.
3. Route it in the family router that owns its kind in `coop/dispatch/`.
4. Own it in one file, in the folder of its concept. The late-join answer (what a peer who joins
   mid-way receives) lives in the same file.
5. Add its row to the subsystem's doc and a scenario under `harness/autotest/`.
