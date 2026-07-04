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
| `prop_snapshot.cpp`, `snapshot_census.cpp`, `join_progress.cpp`, `join_curtain.cpp`, `mirror_defer.cpp` | Connect-snapshot drain + the join-window reconcile gating (quiescence, claim sweep, curtain). **`prop_snapshot` is THE prop deliver-missing owner, two arms of ONE invariant ("every host entity reaches every peer, idempotently"): (1) at-join full-state = the bracketed snapshot drain; (2) late-registration delta = `DeliverLateRegisteredProps` (bracket-FREE, fed by the net_pump re-seed). A kerfur off-prop the convert missed (join-window race) rides arm (2) via `ExpressIncrementalKerfurOffProp` -> deferKerfur -> `kerfur_prop_adoption::Arm` (eid-dedup). OWNER BOUNDARY: join-edge only; steady-state stays KerfurConvert-primary (runtime-self-enforced WARN + autotest). See COOP_MIRROR_IDENTITY_WINDOW_RACE.md (delivery-ownership facet).** `[V]` |

## L1 — Entity identity & lifecycle = THE `coop/element/` module
The eid↔actor owner. Everything here goes through `Registry` / `MirrorManager<T>` / `Element` /
`ElementDeleter`, and binds via `CreateOrAdopt`.

| File(s) | Entity | ReliableKind(s) |
|---|---|---|
| `coop/element/identity.h`, `identity_create.cpp` (`CreateOrAdopt{Prop,Npc,WorldActor}Mirror` -- the ONE wire-mirror bind, sealed to `coop::element`), `identity_destroy.cpp` (`RetireMirror` -- the ONE type-dispatched destroy), `quiescence_drain.cpp` (the join-window ORDER owner -- the drain-edge reconcile SEQUENCE + the deferred queues; was `identity_reconcile.cpp` + `pile_reconcile.cpp` groups B+C, consolidated 2026-06-30) | THE keystone: bind / adopt / morph / reconcile / retire, for all 3 streamed kinds (Prop/Npc/WorldActor). `[V]` | — |
| `element/registry.*`, `mirror_manager.h`, `element.*`, `element_deleter.*`, `prop.h`, `npc.h`, `player.h` | The identity owner + the deferred-destroy funnel. `[V]` | — |
| `remote_prop.cpp` (drive/release/convert receiver), `remote_prop_destroy.cpp` (the prop-destroy path, split 2026-06-30), `remote_prop_spawn.cpp` (the OnSpawn receiver), `join_membership_sweep.cpp` (the join-window membership claim + divergence sweep -- the claim set + RunDivergenceSweep_ + the load-tail quiescence gate `HasLoadTailQuiesced`; split out of remote_prop_spawn 2026-06-30), `prop_element_tracker.cpp`, `prop_lifecycle.cpp`, `prop_echo_suppress.cpp`, `prop_stick_sync.cpp`, `prop_synth_key.cpp` | Props (Aprop_C, chipPile, etc.) | PropSpawn/Destroy/Convert/Release/SnapPos/StickState `[V]` |
| `pile_spawn_bind.cpp` (the pile SPAWN-time native-bind index: TryDestroyTwin / adopt / census; was `pile_reconcile.cpp` group A, split 2026-06-30), `trash_proxy.cpp`, `trash_channel.cpp`, `trash_collect_sync.cpp`, `trash_pile_sync.cpp`, `trash_clump_pose_stream.cpp` | chipPiles / trash clumps (mirror proxies + carry) | PropConvert, TrashPileState, GrabIntent/ThrowIntent/PileResyncRequest `[V]` |
| `npc_sync.cpp`, `npc_mirror.cpp`, `npc_world_enum.cpp`, `npc_adoption.cpp`, `npc_pose_drive.cpp`, `npc_pose_host.cpp` | NPCs. 2026-07-03: + the **wisp_C event-swarm lane** — EX_CallMath spawns caught by the `ufunction_hook` Func-thunk in npc_world_enum (SOURCE-GATED to trigger_wispSwarm_C; ambient ticker wisp_C stays per-peer), PE-invisible self-despawns caught by the pose-walk dead-retire (`SyncDestroyedNpcByEid`), >31-NPC batches fair-share-rotated, mirror fade-in via the lane-driven landing edge (`ue_wrap/wisp::DriveWispLanding`). `[V smoke 32/32 x 4 legs]` | EntitySpawn/EntityDestroy `[V]` |
| `kerfur_convert.cpp`, `kerfur_entity.cpp`, `kerfur_reconcile.cpp`, `kerfur_prop_adoption.cpp`, `kerfur_command.cpp`, `kerfur_menu_input.cpp` | Kerfur (NPC⇄prop form convert) | KerfurConvert/Request/Command `[V]` |
| `world_actor_sync.cpp`, `host_spawn_watcher.cpp` | Non-Character event actors (B3b). 2026-07-04 late: +HostEnrollExSpawn (EX_CallMath spawns via npc_world_enum's source-gated Func-thunk — piramidSpawner_C proven PE-invisible) + pose-walk DEAD-RETIRE in TickPoseStream (event-end SELF K2_DestroyActor is PE-invisible; closed a latent mirror leak for all 17 WA classes) | WorldActorSpawn/Destroy `[V; ex-enroll+dead-retire AS-BUILT, e2e 23:19]` |
| `remote_player.cpp`, `remote_player_ragdoll.cpp` (the ragdoll DISPLAY lifecycle, extracted 2026-07-02 `488b801b`; 2026-07-03 `f36798ab` display = VISIBLE plushie body [user-V "amazing"], kel meshes hidden during the flop; master-pose probe REFUTED 4/6 + deleted), `nameplate.cpp`, `local_streams.cpp`, `puppet_carry_drive.cpp` | Remote-player puppets + the local pose/held streams. JOIN-JUMP sender gate (`614cade8`): the client streams only past ClientWorldReady + load-tail QUIESCENCE (flips after loadObjects' spawn flux — incl. the player teleport — ends; per-world reset); host = worldUp. Hooking gm loadObjects itself is IMPOSSIBLE (script-local calls are Func-patch-blind — DISPATCH_VISIBILITY `[V]`) | (pose lane) + PlayerJoined/Damage/RagdollPose/Wisp* `[V; gate hands-on 20:2x "работает"]` |
| `client_model.cpp` (name→asset + THE ApplySkinToBody), `skin_registry.cpp` (pak-folder catalog + the 25-entry census-verified BUILTIN kerfur-body table `805ae0f8` + the v95 random-starter roll `4570180e`), `local_body.cpp` (LOCAL first-person body + the ini-persisted choice), `player_handshake.cpp` (per-slot skin state + the wire fields), `ui/skins_panel.cpp` (the F1 browser), `skin_effects.cpp` + `ue_wrap/scs_rig.cpp` (2026-07-03 take-3: the builtin skins' NATIVE EFFECT RIG — kerfusFace_C RT face on the 4 omega bodies, mynet floor-grid decals + electricity + step FX, keljoy squeak; template flags honored BIT-EXACTLY via reflection::FindBoolProperty — dormant sentient light/sparks OFF (take-1 force-enable = pink blast; take-2 byte-XOR guess = the violet-on-every-skin regression), mynet bAbsoluteRotation decals + bStartWithTickEnabled=false emitters + att_small zapps copied; step modes REPLACE (mynet: default muted vol 0 + boltrix@1) / ADDITIVE (keljoy squeak scaled/4, /2+1 pitch), own-body REPLACE sound skipped (unmutable native local step); receiver-local, nothing on the wire; RE: findings/votv-kerfur-variant-effects-RE-2026-07-03.md) | v93 player SKINS (docs/COOP_CLIENT_MODEL.md §3); NEW identity rolls a random starter from the curated 6-list ∩ present paks | SkinChange (82) + skin fields inside Join/PlayerJoined `[AS-BUILT 2026-07-02; effects rig AS-BUILT take-3 2026-07-03, smoke PASS, hands-on pending]` |
| `nameplate.cpp` (per-slot VISIBILITY store + the local pref; Update skips hidden slots), `player_handshake.cpp` (v94 prefs flags byte in Join/PlayerJoined + NameplateChange announce/handler), `ui/dev_menu.cpp` (Cosmetics>Nameplate checkbox) | v94 per-peer nameplate visibility (synced pref; ini `nameplate=`; resets to visible on slot disconnect) | NameplateChange (83) + the prefs flags byte inside Join/PlayerJoined `[AS-BUILT 2026-07-02]` |
| `save_identity_bind.cpp`, `save_identity_map.cpp` | The stable-ID sidecar: bind save-loaded natives to host eids in the join window | (rides PropSpawn match key) `[V]` |

> The **authority contract** for this layer is written down in `coop/element/identity.h`
> (host-auth lifecycle+convert / client-relay grab-throw / peer-symmetric pose). The one
> deferred behavior change (D2 host→client corrective-pose) lives there too.

## L2 — Keyed object/device state (apply to a named native, no identity)
Each keys off the actor's save Key. Pattern: an index + a poll + a connect-snapshot rebroadcast +
an `OnReliable` apply. Template siblings: `keypad_sync` / `power_sync`.
**Index-scan discipline (AS-BUILT 2026-07-04 `497b38e0`): every GUObjectArray index here (and
grime/turbine/atv/trash_pile) rebuilds via `coop/scan/settled_object_scan.h` — full-walk while the
live count changes, tail-scan once settled, 60s staggered backstop. A raw tail-scan goes permanently
EMPTY at the host's session-start world reload (the 18:41 keypad-0-sync root; recycled slots below
the cursor) — any new index MUST use the shared component, never raw `NextRange`.**

| File | What | ReliableKind(s) |
|---|---|---|
| `interactable_sync.cpp` | Doors / lights / containers / garage / appliance / locker lids | DoorState/LightState/ContainerState/GarageDoorState/ApplianceState/LockerDoorState/DoorOpenRequest `[V]` |
| `keypad_sync.cpp` | Password keypads: digit buffer = bidirectional input mirror; `active` (power/LED) host-auth for ev=None STATE packets, but Accept/Deny press EVENTS are INPUT-replicated — every peer incl. the host replays CallOpen natively (2026-07-04 `f8185847`, the "client red button rewrote to green" fix; press detected via the isAcc/isDeny lookAt hover flags) | KeypadState `[V]` (press replay `[AS-BUILT 2026-07-04]`, verdict = runbook 0n-b) |
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
| `time_sync.cpp` | Game clock (v96 2026-07-03: payload += the NAMED clock triple timeZ hour/min/dayZ -- a TimeScale=0 client never runs its minute pulse, so its HUD clock/day were frozen; dev set-clock is now instant full-state: totalTime+accumulator+timeZ in one write, sun re-derives same tick) | TimeSync `[AS-BUILT v96; pre-v96 [V]]` |
| `sky_sync.cpp` | Sky rotation + moonPhase | SkyState `[V]` |
| `weather_sync.cpp`, `weather_redsky.cpp`, `weather_lightning.cpp`, `weather_fog.cpp` | Weather scalar / red-sky / lightning / fog. Wind (v50: windTarget stream + client changeWindOrigin PRE-cancel) re-verified statically 2026-07-04 after a live desync report; the live gap is INSTRUMENTED, not diagnosed -- weather_probe=1 logs [probe wind] + roll counters on both peers (`6398ff53`; capture = runbook 0g) | WeatherState/RedSky/LightningStrike `[V; wind desync under live probe]` |
| `firefly_sync.cpp` | Ambient fireflies (peer-symmetric) | FireflySpawn `[V]` |
| `event_cue_sync.cpp` | Host-auth cosmetic cues (starfall etc.) | EventCue `[V]` |
| `event_active_sync.cpp` | Join-during-event Phase 1 (v98, 2026-07-05; COOP_EVENT_JOIN.md): host 1 Hz mirror of the NATIVE activeEvents registry (gamemode.activeEvents_senders membership diff -> BEGIN/END edges) + at a joiner's world-ready edge one EventSnapshot per in-flight entry (class + kClassRowMap row + elapsedSec); client receiver hands mapped replay-safe rows to event_fire_sync's ACTIVE-OVERRIDE replay (bypasses the passEvents "history" dedupe). Unmapped classes WARN LOUD = the Phase 2 census-fill signal. Per-event scenario docs = docs/events/ | EventSnapshot `[AS-BUILT; autonomous mid-join e2e PASS 2026-07-05 00:06 -- forced obelisk pre-join: snapshot -> override replay dispatched; alarm unmapped-skip; hands-on pending]` |
| `piramid_sync.cpp` (creatures/) | Walking-pyramid event choreography lane (v97, 2026-07-04 late; docs/events/piramid.md): client mirror brain suppression (3 PRE-cancelled timer handlers, tick kept alive for beams/look-at) + host gather-commit relay + client native checkIfReached re-dispatch + (v98) join-edge in-flight gather re-send ToSlot (the lane's late-join answer, COOP_EVENT_JOIN 3.4). Pose/spawn/despawn ride world_actor_sync (piramid2_C allowlisted; EX-catch enroll + pose-walk dead-retire added there); wisps ride the npc lane; 'piramid' verdict FLIPPED to no-replay | PyramidGather `[AS-BUILT; autonomous e2e PASS 23:19 -- relay + replay dist=9495 + destroy propagation; hands-on visual pending; the v98 gather-at-join re-send is code-verified only]` |
| `balance_sync.cpp` | Shared Points balance | BalanceSync/BalanceDelta `[V]` |

## L4 — Player-scoped / social
Per-player or broadcast; not world entities.

| File | What | ReliableKind |
|---|---|---|
| `player_inventory_sync.cpp`, `inventory_wire.cpp` | Per-player inventory blob | PlayerInventoryBlob `[V]` |
| `inventory_pickup_sync.cpp` | World item pickup | InventoryPickup `[V]` |
| `item_activate.cpp`, `flashlight_click_sound.cpp`, `prop_sound.cpp` | Held-item effects (flashlight etc.) | ItemActivate `[V]` |
| `chat_sync.cpp`, `chat_feed.cpp` (+ `ui/fonts.cpp`, `ui/hud.cpp` DrawChat, `ui/chat_input.cpp`) | Text chat + the on-screen feed. 2026-07-04 `684f6670`: UTF-8 END-TO-END (the ASCII '?'-squash retired; Cyrillic renders -- embedded-Roboto overlay font w/ Cyrillic ranges), fade-in + per-slot colored nick + word-wrap + Up/Down send-history. Wire UNCHANGED (text[203] always carried raw bytes). `[wire V; UTF-8/UI layer AS-BUILT -- hands-on = runbook 0j]` | ChatMessage `[V]` |
| `email_sync.cpp` | In-game email / saved signals | EmailAppend/Delete `[V]` |
| `player_damage.cpp`, `wisp_tear_mirror.cpp`, `wisp_attack_sync.cpp`, `wisp_grab_hold.cpp` | Combat: relayed damage + killer-wisp fatality. v2 `769d02f7` (2026-07-04): host-authoritative AGGRO SELECTOR (uniform random + stickiness + canReach LOS over host+puppets in 5000u; raw Target re-assert per tick), two-stage close (arm 550u+LOS, relay at 200u contact / 2.5 s LOS-gated timeout), grab CHOREOGRAPHY on every peer (victim replays the native Capture template on its LOCAL mirror; host+third peers snap the victim puppet to the 'playerGrab' socket AFTER pose apply — net_pump ordering; host lifts the wisp 150 cm/s for the window), canRagdoll=false belt over the false-grab window. NO wire change. `[kill CHAIN V hands-on 2026-07-04 eve — client died to kwisp AND host died with clients watching (runbook 0b partial); OPEN: kill pacing too fast vs SP + the tendril/"РУКИ" beam VFX not mirrored]` | PlayerDamage/WispGrab/WispTear `[V]` |
| (voice) `VoiceState` is dispatched in L2's state family; voice audio rides its own Opus path. `[?]` | Voice chat | VoiceState `[?]` |

## L5 — Host control / moderation / dev (not gameplay sync)
| File | Role |
|---|---|
| `moderation.cpp`, `ban_list.cpp`, `roster.cpp` | Kick/ban, peer admin. `[?]` |
| `save_guard.cpp`, `save_block.cpp`, `save_button_disable.cpp` | Client save suppression (host-authoritative world). save_block = the SaveGameToSlot disk write-block `[V]` + PART 3 (2026-07-04 `99eb4566`): the native save CYCLE off — the client holds `gamemode.disableSave=true`, the head-gate of `saveSlot_C::save` `[V bytecode]`, so autosave/sleep/menu saves never even GATHER; the disk hook stays as the belt for the ungated `savePlayerOnly`/direct-trigger writes. `[AS-BUILT; hands-on runbook 0e = verdict]` |
| `save_indicator_suppress.cpp` | Host-side join-save "SAVED" toast suppression — **NOT WORKING, hypothesis REFUTED live** (user 2026-07-04: "never worked"): PHASE 1 (detect-log, read-only BY DESIGN) ran and caught NOTHING — host log 2026-07-04 12:37 `in-window saveAnim=0 addHint=0`, no deferred hits — so the RE guess (saveAnim/addHint paints it) is wrong or the UFunction::Func patch missed; PHASE 2 was never built. Next: widen the hunt for the actual painter. `[REFUTED-PROBE]` |
| `shutdown.cpp`, `ambient_spawner_suppress.cpp`, `garbage_sync.cpp` | Lifecycle/teardown + targeted client-side crash fixes (e.g. garbage_sync = the open-container pickup AV fix). `[V]` |
| `pause_guard.cpp` | Coop NO-PAUSE invariant (2026-07-04 `769d02f7`): while connected, a paused world (client/host ESC, console `pause`) is un-paused every gameplay tick via GameplayStatics::SetGamePaused(false) — STATE-level, one owner; the ESC pause is EX_CallMath = PE-invisible so call-site interception is impossible. ESC menu stays usable; solo pause untouched. `[AS-BUILT; e2e autotest VOTVCOOP_RUN_PAUSE_TEST queued; hands-on ESC = verdict]` |
| `multiplayer_menu.cpp`, `ini_config.cpp`, `grab_observer.cpp` | Menu UI, config, the grab input observer. `[V]` |

---

## EventFire — scheduled/story event replay `[AS-BUILT v95 2026-07-03]`

`coop/world/event_fire_sync.{h,cpp}` + `ReliableKind::EventFire=84` (32 B: u8 dispatch + name[31]).
One owner of the whole scheduled-event authority axis — the mechanics DIVERGED from the original
design sketch after bytecode verification (better seams found; do not re-derive):
- **Host observation** = `saveSlot.passEvents` GROWTH poll, 1 Hz (settime `Array_Add`s each fired
  row — settime is the ONLY appender; `runEvent` itself never touches the array).
- **Dev fires** (F1 menu) dispatch THROUGH `event_fire_sync::HostFire` = native fire + broadcast at
  dispatch (a direct runEvent never appends passEvents, so the poll can't see menu fires).
- **Client suppression** = `saveSlot.allEvents.Num = 0` (settime's walk SOURCE; one int write; the
  boot ubergraph rebuilds allEvents from the DataTable unconditionally every load → self-healing,
  no save poisoning) — NOT the sketched passEvents pre-mark (needed an engine allocator we don't
  bind). Restored on disconnect. Closes the sleep-accelerate hole (client settime RUNS at
  TimeScale=1 during v71 accelerated nights and would natively fire 00:00 rows).
- **Client replay** = reflected `runEvent(name, None)` / `runSpecialEvent(name)` gated by the
  PER-ROW policy table in the .cpp (the dupe matrix from votv-event-system-RE-2026-06-13.md §10):
  replay ONLY level/save/cosmetic flips no lane carries (treehouse_0..5 — the campfire, breaks,
  obelisk, forceObjects signal rows, solar/call0, scare arms, arirGraff_*; `piramid` FLIPPED
  to lane-owned 2026-07-04 — piramid mirror lane, docs/events/piramid.md); lane-covered
  rows (prop/npc/atv/sleep/wisp/event_cue/device) log + skip. `ariralPrank` never crosses the wire
  (host-local RNG). One-shot rows dedupe vs the client's own passEvents + a session replayed-set;
  specials are repeatable. Join window: replays queue until the eventer resolves (cap 64, loud).

## DESIGNED, NOT BUILT (the queued wire feature; do not re-derive)

- **Screens/panels gap list** (the rest of "sync every screen and panel"): reactor rods ->
  generator/transformer OUTCOME -> SAT console LogText release-snapshot -> TV keyed state -> laptop
  tasks roll -> serverBox RE. Shapes + boundaries: `research/findings/votv-screens-panels-sync-DESIGN-2026-07-03.md`
  (~70%% of the ask already shipped v63-71 — occupancy, desk, dish, signals, refiner, emails, orders).

## The boundary, restated
`coop/element/` is the **L1 identity engine** — not "everything that sends a packet." L2/L3/L4
replicators route through the **same SyncRouter dispatch** and the same session/transport, but
they have no identity race to consolidate, so they stay as their own small single-responsibility
files (RULE 2: don't churn working, well-factored code into a different folder for a name). If we
ever deliberately rescope `coop/element/` to mean "all wire replication," that's a conscious, separate
decision — record it here and update the boundary above.
