# COOP_SYNC_MAP — where every piece of wire sync lives (discoverability map)

Born 2026-06-28 (after the sync-consolidation refactor). Answers "where is the code for X
sync, and which layer does it belong to?" without grepping 95 files. The organizing
principle is **what each thing replicates**, NOT the word "sync":

- **Identity / mirror-entity lifecycle** — an eid↔actor mapping with create / adopt /
  morph / destroy. This is the only thing the `coop/element/` module consolidates (it was
  built to kill the mirror-identity-race class, D1/D2). MTA analogue: `CElementIDs` +
  the `CClient*Manager` family + `CClientEntity`.
- **Keyed state** — apply a value to a NAMED native actor (keyed by its save Key). No
  mirror, no eid, no lifecycle race.
- **Global / ambient state** — a singleton scalar (clock, sky, weather). No key, no eid.
- **Player-scoped / social** — per-player or broadcast (inventory, chat, voice, damage).
- **Transport / dispatch / session** — the substrate everything rides; not a feature.

Confidence: `[V]` verified this session, `[RD]` RE-derived, `[?]` inferred from name/kind.

> **PHYSICAL LAYOUT (STRICT reorg REDONE by behaviour 2026-06-29, commit `33945284`;
> `research/findings/kerfur-identity-authority-and-module-refactor-DESIGN-2026-06-29.md` Part 3).** The
> 2026-06-28 `coop/{world,social,host,devices}` layout was filename-guessed and WRONG; it was dissolved and
> re-placed by what each file DOES. The coop/ root is now FLAT into mechanic-named modules:
> - `coop/element/` -- THE entity-identity layer (Registry / MirrorManager / the 3 managers / ElementDeleter
>   / the typed Elements + the CreateOrAdopt bind + RetireMirror destroy + reconcile authority). The former
>   `coop/sync/` was DISSOLVED into here 2026-06-29 (folder + namespace `coop::sync`→`coop::element`) -- one
>   entity concept, one folder. `coop/net/` -- transport. `coop/voice/` `coop/dev/`.
> - `coop/creatures/` -- NPCs + kerfur (all forms) + wisps + WorldActor event entities.
> - `coop/props/` -- grabbable physics props: chipPile/trash/grab-throw/proxy/pile-reconcile + save-identity.
> - `coop/interactables/` -- the E-press objects: doors/keypad/power/window/grime/turbine/ATV/drone/
>   console/signal/comp + `interactable_channel` (was held L5 WIP, now placed) + `device_occupancy`.
> - `coop/world/` -- time/sky/weather/firefly/event-cue/balance + `email_sync` (story-bot mail, NOT social).
> - `coop/player/` -- remote-player puppets, inventory-pickup/flashlight/damage, sleep, nameplate, roster.
> - `coop/items/` -- inventory persistence + wire + delivery `order_sync`. `coop/comms/` -- chat sync + feed.
> - `coop/session/` -- handshake/save-transfer/join-curtain/event-feed+dispatch/net-pump/subsystems +
>   the former `host/` (ban/moderation/save-guard/shutdown/multiplayer-menu -- "host" was a role, not a mechanic).
>
> `social/`, `host/`, and `sync/` are FULLY DISSOLVED (no such folders). Tables below list bare names; find the
> file's module by its behaviour above (or `git ls-files`). NPC + WorldActor wire binds now ALSO funnel through
> `coop::element` (CreateOrAdoptNpc/WorldActorMirror); the destroy funnel is `coop::element::RetireMirror`; and
> `MirrorManager::Install` is SEALED to `coop::element` (a feature-file wire-mirror bind is a compile error). [Inc A/B/C]

> **Where does a NEW sync feature go?** If it has an eid and can be created/destroyed/
> morphed at runtime as a mirror → it's an entity, use the `coop/element/` identity layer
> (`CreateOrAdopt` / `MirrorManager` / `ElementDeleter`). If it's "set field K on the
> named actor" → it's keyed state, make a small `*_sync.cpp` next to `keypad_sync` /
> `power_sync`. If it's one global value → an even smaller one next to `time_sync`. All
> of them wire the same way: a ReliableKind in the enum + a case in the right family
> switch (`event_dispatch_{entity,state,world}.cpp`) — that's the 2-place wiring the
> SyncRouter consolidation left us with.

---

## L0 — Transport / dispatch / session (the substrate)
Not "sync features" — the pipes. Read these to understand HOW a packet flows, not WHAT syncs.

| File | Role |
|---|---|
| `net/session.*`, `net_pump.cpp` | UDP lanes (unreliable pose + reliable ARQ), the per-tick pump, the reaper/reseed lifecycle. `[V]` |
| `event_feed.cpp` | The reliable-drain switch: inline special cases + the **SyncRouter** default that chains the three family routers. `[V]` |
| `event_dispatch_{entity,state,world}.cpp` (+ `event_dispatch.h`) | The three family routers. Each `Handle*Event` returns true iff it owns the kind (the single membership declaration). `[V]` |
| `player_handshake.cpp`, `players_registry.cpp`, `roster.cpp`, `session_manager.cpp`, `subsystems.cpp` | Join/assign-slot, peer roster, session lifecycle, install wiring. `[V]` |
| `save_transfer.cpp`, `blob_chunks.cpp` | Menu-mode world-save streaming to a joiner. `[V]` |
| `prop_snapshot.cpp`, `snapshot_census.cpp`, `join_progress.cpp`, `join_curtain.cpp`, `mirror_defer.cpp` | Connect-snapshot drain + the join-window reconcile gating (quiescence, claim sweep, curtain). `[V]` |

## L1 — Entity identity & lifecycle = THE `coop/element/` module
The eid↔actor owner. Everything here goes through `Registry` / `MirrorManager<T>` / `Element` /
`ElementDeleter`, and binds via `CreateOrAdopt`.

| File(s) | Entity | ReliableKind(s) |
|---|---|---|
| `coop/element/identity.h`, `identity_create.cpp` (`CreateOrAdopt{Prop,Npc,WorldActor}Mirror` -- the ONE wire-mirror bind, sealed to `coop::element`), `identity_destroy.cpp` (`RetireMirror` -- the ONE type-dispatched destroy), `identity_reconcile.cpp` | THE keystone: bind / adopt / morph / reconcile / retire, for all 3 streamed kinds (Prop/Npc/WorldActor). `[V]` | — |
| `element/registry.*`, `mirror_manager.h`, `element.*`, `element_deleter.*`, `prop.h`, `npc.h`, `player.h` | The identity owner + the deferred-destroy funnel. `[V]` | — |
| `remote_prop.cpp`, `remote_prop_spawn.cpp`, `prop_element_tracker.cpp`, `prop_lifecycle.cpp`, `prop_echo_suppress.cpp`, `prop_stick_sync.cpp`, `prop_synth_key.cpp` | Props (Aprop_C, chipPile, etc.) | PropSpawn/Destroy/Convert/Release/SnapPos/StickState `[V]` |
| `pile_reconcile.cpp`, `trash_proxy.cpp`, `trash_channel.cpp`, `trash_collect_sync.cpp`, `trash_pile_sync.cpp`, `trash_clump_pose_stream.cpp` | chipPiles / trash clumps (mirror proxies + carry) | PropConvert, TrashPileState, GrabIntent/ThrowIntent/PileResyncRequest `[V]` |
| `npc_sync.cpp`, `npc_mirror.cpp`, `npc_world_enum.cpp`, `npc_adoption.cpp`, `npc_pose_drive.cpp`, `npc_pose_host.cpp` | NPCs | EntitySpawn/EntityDestroy `[V]` |
| `kerfur_convert.cpp`, `kerfur_entity.cpp`, `kerfur_reconcile.cpp`, `kerfur_prop_adoption.cpp`, `kerfur_command.cpp`, `kerfur_menu_input.cpp` | Kerfur (NPC⇄prop form convert) | KerfurConvert/Request/Command `[V]` |
| `world_actor_sync.cpp`, `host_spawn_watcher.cpp` | Non-Character event actors (B3b) | WorldActorSpawn/Destroy `[V]` |
| `remote_player.cpp`, `nameplate.cpp`, `local_streams.cpp`, `puppet_carry_drive.cpp` | Remote-player puppets + the local pose/held streams | (pose lane) + PlayerJoined/Damage/Wisp* `[V]` |
| `save_identity_bind.cpp`, `save_identity_map.cpp` | The stable-ID sidecar: bind save-loaded natives to host eids in the join window | (rides PropSpawn match key) `[V]` |

> The **authority contract** for this layer is written down in `coop/element/identity.h`
> (host-auth lifecycle+convert / client-relay grab-throw / peer-symmetric pose). The one
> deferred behavior change (D2 host→client corrective-pose) lives there too.

## L2 — Keyed object/device state (apply to a named native, no identity)
Each keys off the actor's save Key. Pattern: an index + a poll + a connect-snapshot rebroadcast +
an `OnReliable` apply. Template siblings: `keypad_sync` / `power_sync`.

| File | What | ReliableKind(s) |
|---|---|---|
| `interactable_sync.cpp` | Doors / lights / containers / garage / appliance / locker lids | DoorState/LightState/ContainerState/GarageDoorState/ApplianceState/LockerDoorState/DoorOpenRequest `[V]` |
| `keypad_sync.cpp` | Password keypads | KeypadState `[V]` |
| `power_sync.cpp` | Breaker / power panels | PowerControlState `[V]` |
| `window_sync.cpp` | Window-clean state | WindowCleanState `[V]` |
| `grime_sync.cpp` | Surface grime/dirt | GrimeState `[V]` |
| `turbine_sync.cpp` | Wind turbine | TurbineState `[V]` |
| `device_occupancy.cpp` | "who's using this device" claim | DeviceClaim `[V]` |
| `atv_sync.cpp` | ATV/quadbike body pose (keyed, occupant-auth) | AtvState/Release/Spawn/Destroy `[V]` |
| `drone_sync.cpp` | Delivery drone state | DroneState `[V]` |
| `order_sync.cpp` | Shop orders | OrderRequest `[V]` |
| `sleep_sync.cpp` | Sleep/bed state | SleepState `[V]` |
| `comp_sync.cpp`, `console_state_sync.cpp` | Computers / the in-game console + desk | CompState/CompData/DeskState/DeskLogLine `[?]` |
| `signal_sync.cpp`, `signal_catch_sync.cpp`, `signal_wire.cpp` | Sky-signal hardware + catch + dish aim | SkySignalState/SkySignalCatch/DishAimState/SavedSignalAppend/Delete `[?]` |

## L3 — Global / ambient world state (singletons, no key)
One value, host-authoritative, peers apply. Template sibling: `time_sync`.

| File | What | ReliableKind |
|---|---|---|
| `time_sync.cpp` | Game clock | TimeSync `[V]` |
| `sky_sync.cpp` | Sky rotation + moonPhase | SkyState `[V]` |
| `weather_sync.cpp`, `weather_redsky.cpp`, `weather_lightning.cpp`, `weather_fog.cpp` | Weather scalar / red-sky / lightning / fog | WeatherState/RedSky/LightningStrike `[V]` |
| `firefly_sync.cpp` | Ambient fireflies (peer-symmetric) | FireflySpawn `[V]` |
| `event_cue_sync.cpp` | Host-auth cosmetic cues (starfall etc.) | EventCue `[V]` |
| `balance_sync.cpp` | Shared Points balance | BalanceSync/BalanceDelta `[V]` |

## L4 — Player-scoped / social
Per-player or broadcast; not world entities.

| File | What | ReliableKind |
|---|---|---|
| `player_inventory_sync.cpp`, `inventory_wire.cpp` | Per-player inventory blob | PlayerInventoryBlob `[V]` |
| `inventory_pickup_sync.cpp` | World item pickup | InventoryPickup `[V]` |
| `item_activate.cpp`, `flashlight_click_sound.cpp`, `prop_sound.cpp` | Held-item effects (flashlight etc.) | ItemActivate `[V]` |
| `chat_sync.cpp`, `chat_feed.cpp` | Text chat + the on-screen feed | ChatMessage `[V]` |
| `email_sync.cpp` | In-game email / saved signals | EmailAppend/Delete `[V]` |
| `player_damage.cpp`, `wisp_tear_mirror.cpp`, `wisp_attack_sync.cpp` | Combat: relayed damage + killer-wisp fatality | PlayerDamage/WispGrab/WispTear `[V]` |
| (voice) `VoiceState` is dispatched in L2's state family; voice audio rides its own Opus path. `[?]` | Voice chat | VoiceState `[?]` |

## L5 — Host control / moderation / dev (not gameplay sync)
| File | Role |
|---|---|
| `moderation.cpp`, `ban_list.cpp`, `roster.cpp` | Kick/ban, peer admin. `[?]` |
| `save_guard.cpp`, `save_block.cpp`, `save_button_disable.cpp`, `save_indicator_suppress.cpp` | Client save suppression (host-authoritative world). `[?]` |
| `shutdown.cpp`, `ambient_spawner_suppress.cpp`, `garbage_sync.cpp` | Lifecycle/teardown + targeted client-side crash fixes (e.g. garbage_sync = the open-container pickup AV fix). `[V]` |
| `multiplayer_menu.cpp`, `ini_config.cpp`, `grab_observer.cpp` | Menu UI, config, the grab input observer. `[V]` |

---

## The boundary, restated
`coop/element/` is the **L1 identity engine** — not "everything that sends a packet." L2/L3/L4
replicators route through the **same SyncRouter dispatch** and the same session/transport, but
they have no identity race to consolidate, so they stay as their own small single-responsibility
files (RULE 2: don't churn working, well-factored code into a different folder for a name). If we
ever deliberately rescope `coop/element/` to mean "all wire replication," that's a conscious, separate
decision — record it here and update the boundary above.
