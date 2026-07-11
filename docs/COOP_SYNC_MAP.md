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

**NOT wire-synced (ride the save, not a packet):** the **world rules** (`Fstruct_gameRules` — fall
damage / difficulty / funny / custom content / seasons / minigames / decay...) are host-authoritative
**for free** — a joining client boots from the host's live-captured save, so the host's rules populate
the client's per-peer `mainGameInstance.gameRules`. No lane, no ReliableKind. Read-only display is
`ui/world_rules_panel` (F1 > World > Rules, everyone) via `ue_wrap/game_rules`. RE + G1-confirmed:
`research/findings/votv-gamerules-settings-RE-2026-07-09.md`; class: `COOP_WORLD_PROP_DIVERGENCE.md`.
General **user settings** (graphics/audio/sensitivity) are a separate object, per-client, never synced.

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
> - `coop/session/` -- handshake/net-pump/subsystems/session-manager/shutdown/pause-guard/teleport (lifecycle
>   + join only, post-2026-07-10 reorg). Split out that day: `coop/dispatch/` (event_feed + the 4 per-family
>   event_dispatch_* TUs = the ReliableKind wire router), `coop/save/` (save_block/button/guard/indicator/
>   transfer -- the coop-save discipline), `coop/moderation/` (moderation/ban_list/seen_players),
>   `coop/config/` (env+ini readers; ONE config home -- harness/config + ini_config merged, RULE 2),
>   `ui/input_focus` (the keyboard-arbitration predicates), `ui/multiplayer_menu`, `element/mirror_defer`,
>   `world/world_actor_*` (from creatures/), `items/inventory_pickup_sync` (from player/).
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
| `coop/element/identity.h`, `identity_create.cpp` (`CreateOrAdopt{Prop,Npc,WorldActor}Mirror` -- the ONE wire-mirror bind, sealed to `coop::element`), `identity_destroy.cpp` (`RetireMirror` -- the ONE type-dispatched destroy), `quiescence_drain.cpp` (the join-window ORDER owner -- the drain-edge reconcile SEQUENCE + the deferred queues; was `identity_reconcile.cpp` + `pile_reconcile.cpp` groups B+C, consolidated 2026-06-30; **+ the SPAWN-REVALIDATION queue 2026-07-11 `8a2b04d0`: every in-episode wire prop expression is captured and, at the drain (step 4, pre-destroys), re-expressed iff its row is still dead/absent -- the churn-victim-with-no-save-recreate class, see COOP_ENTITY_EXPRESSION_MAP "Join-window PROVISIONALITY"**) | THE keystone: bind / adopt / morph / reconcile / retire, for all 3 streamed kinds (Prop/Npc/WorldActor). `[V; spawn-revalidation AS-BUILT, take-3 hands-on pending]` | — |
| `element/registry.*`, `mirror_manager.h`, `element.*`, `element_deleter.*`, `prop.h`, `npc.h`, `player.h` | The identity owner + the deferred-destroy funnel. `[V]` | — |
| `remote_prop.cpp` (drive/release/convert receiver), `remote_prop_destroy.cpp` (the prop-destroy path, split 2026-06-30), `remote_prop_spawn.cpp` (the OnSpawn receiver), `join_membership_sweep.cpp` (the join-window membership claim + divergence sweep -- the claim set + RunDivergenceSweep_ + the load-tail quiescence gate `HasLoadTailQuiesced`; split out of remote_prop_spawn 2026-06-30), `prop_element_tracker.cpp` (+ `prop_census.cpp` seed walk + `prop_key_index.cpp` key->live-actor index, soft-cap splits 2026-07-10), `prop_lifecycle.cpp` (+ `prop_destroy_seam.cpp` K2 seam + `prop_container_extract.cpp` takeObj pair, soft-cap splits 2026-07-10), `prop_echo_suppress.cpp`, `prop_stick_sync.cpp`, `prop_synth_key.cpp` | Props (Aprop_C, chipPile, etc.) | PropSpawn/Destroy/Convert/Release/SnapPos/StickState `[V]` |
| `pile_spawn_bind.cpp` (the pile SPAWN-time native-bind index: TryDestroyTwin / adopt / census; was `pile_reconcile.cpp` group A, split 2026-06-30), `trash_proxy.cpp`, `trash_channel.cpp` (+ `trash_grab_intent.cpp` -- the client grab/throw intent lane + HELD_BY, soft-cap split 2026-07-10), `trash_collect_sync.cpp`, `trash_pile_sync.cpp`, `trash_clump_pose_stream.cpp` | chipPiles / trash clumps (mirror proxies + carry) | PropConvert, TrashPileState, GrabIntent/ThrowIntent/PileResyncRequest `[V]` |
| `npc_sync.cpp`, `npc_mirror.cpp`, `npc_world_enum.cpp`, `npc_adoption.cpp`, `npc_pose_drive.cpp`, `npc_pose_host.cpp` | NPCs. 2026-07-03: + the **wisp_C event-swarm lane** — EX_CallMath spawns caught by the `ufunction_hook` Func-thunk in npc_world_enum (SOURCE-GATED; sources now trigger_wispSwarm_C + piramidSpawner_C + ticker_wispSpawner_C -- the ambient sky wisps went host-auth 2026-07-10 eve `08560687`, 8 color variants allowlisted), PE-invisible self-despawns caught by the pose-walk dead-retire (`SyncDestroyedNpcByEid`), >31-NPC batches fair-share-rotated, mirror fade-in via the lane-driven landing edge (`ue_wrap/wisp::DriveWispLanding`). `[V smoke 32/32 x 4 legs]` 2026-07-05 `ff338d87`: enroll/EX-drain/dead-retire HOSTING-gated (alone-spawned event creatures now track; joiner gets them via the connect snapshot — the 0s failure class); kerfur-family deaths EXCLUDED from the dead-retire (the conversion poll owns that edge) | EntitySpawn/EntityDestroy `[V]` |
| `kerfur_convert.cpp`, `kerfur_entity.cpp`, `kerfur_reconcile.cpp`, `kerfur_prop_adoption.cpp`, `kerfur_command.cpp`, `kerfur_menu_input.cpp` | Kerfur (NPC⇄prop form convert) | KerfurConvert/Request/Command `[V]` |
| `world_actor_sync.cpp`, `host_spawn_watcher.cpp` | Non-Character event actors (B3b). 2026-07-04 late: +HostEnrollExSpawn (EX_CallMath spawns via npc_world_enum's source-gated Func-thunk — piramidSpawner_C proven PE-invisible) + pose-walk DEAD-RETIRE in TickPoseStream (PE-invisible SELF-destroys; all 17 WA classes). 2026-07-05 `ff338d87`: interceptor + ex-enroll + dead-retire HOSTING-gated (alone-spawn tracking — the 0s failure) — **[V live 11:25: joiner materialized the pyramid]**. `c98c6543`: PERMANENT `[WA-TRACE]` 1 Hz telemetry at all 5 pose hops incl. the K2_SetActorLocation/Rotation RETURNS (the old "first batch" log proved ARRIVAL only; the trace then proved the pose chain alive end-to-end live — host-vs-client delta ~100 units). `419e3894` **v99**: EntitySpawnPayload +Scale3D (104->116), filled at all 6 senders (WA interceptor/ex-enroll/connect-snapshot + NPC interceptor/ex-spawn/connect-snapshot), applied at both receivers + SanitizeWireScaleAxis (piramid2_C spawns at scale 2 via the deferred transform; the unit-scale mirror rendered half-size + floated at the host's scale-2 hover Z = «далеко и маленькая») — **[V live: «пирамида идёт»]**. `75e5ab10` **v100**: WorldActorPoseSnapshot +auxYaw (28->32) — the class-specific VISIBLE heading (piramid: movementVector world yaw; others: ==yaw), interp'd in the element, consumed post-apply by piramid_sync. `a255b70f` **v102**: +auxX/Y/Z (32->44) — class-specific aux TARGET vector (piramid: relLook, the head/searchlight idle look target; latest-wins, NO interp — the mirror's native ease consumes it). **v104** (2026-07-05 night): +auxTargetEid (44->48, batch cap 31->28) — the wispTarget IDENTITY streams per tick (0y residue root: the host's walk-to-wisp phase runs the CHASE look-at branch while the mirror idled on ignored relLook wander; the client sets/clears the mirror's wispTarget via the npc table — `piramid_sync::ApplyMirrorWispTarget`, gathering-owned windows untouched). 2026-07-05: client half extracted to `world_actor_mirror.cpp` (`bdbab84c`, detail seam world_actor_detail.h) | WorldActorSpawn/Destroy + WorldActorPose `[V live: spawn delivery + walk at true scale + full-event position track; v100 facing user-verdicted «хороший результат»; suck [V by user]; v102+v104 head AS-BUILT, runbook 0y re-verdict pending]` |
| `remote_player.cpp`, `remote_player_ragdoll.cpp` (the ragdoll DISPLAY lifecycle, extracted 2026-07-02 `488b801b`; 2026-07-03 `f36798ab` display = VISIBLE plushie body [user-V "amazing"], kel meshes hidden during the flop; master-pose probe REFUTED 4/6 + deleted), `nameplate.cpp`, `local_streams.cpp`, `puppet_carry_drive.cpp` | Remote-player puppets + the local pose/held streams. JOIN-JUMP sender gate (`614cade8`): the client streams only past ClientWorldReady + load-tail QUIESCENCE (flips after loadObjects' spawn flux — incl. the player teleport — ends; per-world reset); host = worldUp. Hooking gm loadObjects itself is IMPOSSIBLE (script-local calls are Func-patch-blind — DISPATCH_VISIBILITY `[V]`) | (pose lane) + PlayerJoined/Damage/RagdollPose/Wisp* `[V; gate hands-on 20:2x "работает"]` |
| `client_model.cpp` (name→asset + THE ApplySkinToBody), `skin_registry.cpp` (pak-folder catalog + the 25-entry census-verified BUILTIN kerfur-body table `805ae0f8` + the v95 random-starter roll `4570180e`), `local_body.cpp` (LOCAL first-person body + the ini-persisted choice), `player_handshake.cpp` (per-slot skin state + the wire fields), `ui/skins_panel.cpp` (the F1 browser), `skin_effects.cpp` + `ue_wrap/scs_rig.cpp` (2026-07-03 take-3: the builtin skins' NATIVE EFFECT RIG — kerfusFace_C RT face on the 4 omega bodies, mynet floor-grid decals + electricity + step FX, keljoy squeak; template flags honored BIT-EXACTLY via reflection::FindBoolProperty — dormant sentient light/sparks OFF (take-1 force-enable = pink blast; take-2 byte-XOR guess = the violet-on-every-skin regression), mynet bAbsoluteRotation decals + bStartWithTickEnabled=false emitters + att_small zapps copied; step modes REPLACE (mynet: default muted vol 0 + boltrix@1) / ADDITIVE (keljoy squeak scaled/4, /2+1 pitch), own-body REPLACE sound skipped (unmutable native local step); receiver-local, nothing on the wire; RE: findings/votv-kerfur-variant-effects-RE-2026-07-03.md) | v93 player SKINS (docs/COOP_CLIENT_MODEL.md §3); NEW identity rolls a random starter from the curated 6-list ∩ present paks | SkinChange (82) + skin fields inside Join/PlayerJoined `[AS-BUILT 2026-07-02; effects rig AS-BUILT take-3 2026-07-03, smoke PASS, hands-on pending]` |
| `nameplate.cpp` (per-slot VISIBILITY store + the local pref; Update skips hidden slots), `player_handshake.cpp` (v94 prefs flags byte in Join/PlayerJoined), `player_handshake_prefs.cpp` (the live-change announce/handle family, extracted 2026-07-05), `ui/dev_menu.cpp` (Cosmetics>Nameplate checkbox) | v94 per-peer nameplate visibility (synced pref; ini `nameplate=`; resets to visible on slot disconnect) | NameplateChange (83) + the prefs flags byte inside Join/PlayerJoined `[AS-BUILT 2026-07-02]` |
| `nick_color.cpp` (the color axis' ONE owner: atomic per-slot packed store, 0 = surface default; local-slot mirror for render-thread self-resolution), `player_handshake.cpp` (v103 `[has][r][g][b]` field in Join/PlayerJoined after the prefs byte), `player_handshake_prefs.cpp` (NickColorChange announce/handle), consumers `ui/hud.cpp` (nameplate nick default white + chat nick default slot palette), `ui/scoreboard.cpp` (row, default role gold/white), `ui/dev_menu.cpp` (Cosmetics>Nameplate hue-wheel picker) | v103 (12f) per-peer nickname COLOR (synced pref; ini `nick_color=RRGGBB`; resets on slot disconnect; flash/occluded signal colors keep priority) | NickColorChange (88) + the color field inside Join/PlayerJoined `[AS-BUILT 2026-07-05 `76ce8c58` + fixes `8d6a3708` (picker commit DEBOUNCED ~0.35 s — composite-ColorPicker3 deactivate-after-edit never fired, user live report; NEW identity defaults to custom WHITE); deployed, smoke PASS (join on the v103+ payloads proven live); hands-on 0z re-check]` |
| `hand_item.cpp` (per-slot state store + owner change-poll + the VIEW-ANCHORED display mirrors + `LocalHandActor()` the census boundary), `local_streams.cpp` (the owner poll site: an Aprop_C in `holding_actor` routes HERE, not to the prop pipeline), `event_feed.cpp` (router case), `subsystems.cpp` (connect replay), `prop_element_tracker.cpp` (SeedWalk_ skips the local hand actor), the v106 seams (see the row below) | **v105/v105b hotbar HAND-ITEM display axis** (RULE-1 root fixes 2026-07-06): the item in a player's quick-slot hand is PLAYER EXPRESSION (MTA current-weapon shape) — owner announces `{class,name}` on change (~1 reliable msg/switch, stow announce EDGE-INSTANT — bytecode-proven: a switch is ONE synchronous updateHold call, so a polled null IS the stow; the first-cut 250 ms debounce guarded a nonexistent flicker and read as lag, user 2026-07-06); peers keep a display-only mirror (physics/collision off, spawn/destroy echo-suppressed) **re-driven per tick at the puppet's VIEW-space hold point** (GetHeadPosition + GetSyncedAimDirection basis × offsets, rotation = look yaw+pitch — the puppet_carry_drive shape; NO attach: natively updateHold welds the item into the FP-arms VIEWMODEL chain ('weapon' <- arms <- viewmodel, camera-relative) which is bHiddenInGame + never ticks on a puppet — a ref-pose socket attach landed the mirror on the BACK, hands-on 14:2x, and the puppet Camera never pitches. Placement 2026-07-11 `3cc7d5db`/`a3c55529` (v109): MEASURED, NOT TUNED — the owner measures its natively-welded held actor in its own view space (head bone + 33 anchor, control yaw/pitch roll-0 basis = the pose stream's definitions) and the mirror re-composes it; hand-tuned offsets RETIRED (RULE 2 — the 07-10 fwd45/right10/up−40 constants remain only as the pre-measure fallback; user 2026-07-11 live: keljoy face pointed away under constants). MOTION streams per tick via MsgType::HandPose=35 (RagdollPose sibling, sendHz while holding, host-relayed; the reliable announce carries identity + the pre-stream frame; the 0.5 s reliable drift refresh which rendered swings at ~1 fps was retired same-day, user live report)). The census NEVER adopts the local hand actor (v105b dupe root, hands-on 13:44:00 eid=5377: recycled-slot respawn → safety census → PropSpawn → frozen dupe at the puppet). Trash clump/pile carry (non-Aprop_C holding_actor) unchanged | HandItem (89) + HandPose (MsgType 35, unreliable) + connect replay `[AS-BUILT 2026-07-06 v105b, smoke PASS per iteration on the v105b DLL (harness R-picks a rock — announce+mirror exercised in-smoke); mirror placement USER-APPROVED live 2026-07-06 ("it's good"); 0ae hands-on 2026-07-07 general PASS («вроде всё норм»), no dupe reported (immediacy owned by the v106 seam row below)]` |
| `prop_lifecycle.cpp` (**K2_DestroyActor Func seam** — UFunction::Func patch, fires for EVERY dispatch route incl. the EX_CallMath pickup destroy putObjectInventory2@719; bidirectional host+client, echo-suppressed), `host_spawn_watcher.cpp` (**FinishSpawningActor Func seam** — enqueue + `DrainPendingSpawns` +1 tick: takeObj drops/Q-menu/stack-drop adopt with the restored key; excludes mirrors via `PeekIncomingSpawn` + the local hand actor), `hand_item.cpp` (**hand-edge express**: an ex-hand actor that SURVIVES the edge was RELEASED to the world — `EnsureHeldItemBroadcast` right there), `subsystems.cpp` (drain call site) | **v106 SEAM-DRIVEN world-prop coherence** (RULE-1 root fix 2026-07-07, «no patches»; REPLACES the v105b forced-reconcile per RULE 2 — that mechanism ran the census BEFORE the world changed and taxed the useful pass with a 750 ms cooldown = the user-felt ~0.5 s). Vanish/appear now broadcast AT the destroy/release/spawn moment; the 4 s reap + ~20 s safety census remain background safety only. The K2 seam is bidirectional → the formerly-OPEN client-pickup destroy lane closes by construction | (no new wire kind — the existing per-delta PropDestroy / PropSpawn lanes) `[VERIFIED hands-on 2026-07-07 0ae (DLL 340E5573E87DF5AB): Q-menu spawns INSTANT on clients + grab lane PASS. v106b `4a280375` same day: MIGRATION-FIRST (identity rebinds onto the born clump AT the thunk — a morph husk dies eid-less, else the widened destroy seam kills its own entity = the 11:43 regression) + BIRTH-ORPHAN express + wholesale GHOST-RETIRE (save_identity_bind tail + quiescence_drain::ArmGhostSweep; the v106 one-ghost-per-E-press retire RETIRED, RULE 2)]` |
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
| `firefly_sync.cpp` | Ambient fireflies (peer-symmetric; the shipped OWNER-EFFECT precedent — [[feedback-owner-effect-rule]]). 2026-07-10 eve: second OWNER-EFFECT member = pinecone forest drops (props, not particles → they ride host_spawn_watcher's now peer-symmetric ambient PropSpawn broadcaster, not this module) | FireflySpawn `[V]` |
| `spawn_authority.cpp` | T1 Inc-1 (2026-07-10 `e6c1371b`, docs/COOP_RNG_AUTHORITY.md T1 STRUCTURAL DESIGN): client shared-world spawner suppression — t1 tick-park (insomniac+fossilhound spawners + cockroachMaster+ticker_roachSummoner since v108; 1 Hz first-instance hunt then 15 s reconcile; restore on session end) + t3 cancels (mushroomMaster/mushroomSpawner/yellowWisp + ticker_wispSpawner sky wisps + the 4 roach timer/summon entries; **pinecone row REMOVED 2026-07-10 eve** — dump-measured player-camera anchor → OWNER-EFFECT, each peer rolls its own) + shipping tripwire via npc_sync pass-through | none (suppression, no wire) `[Inc-1 AS-BUILT + census-verified; v108 rows AS-BUILT, smoked]` |
| `creatures/roach_sync.cpp` | Roach infestation (v108, 2026-07-10 eve): roaches = COMPONENTS on the one world-anchored cockroachMaster (bytecode RE: nests near food, growth by eating, calc() mutates SHARED food props) → host-auth. HOST 1 Hz population poll → PAGED full snapshot (12/datagram, ≤128) on change/drift/keepalive + join snapshot; CLIENT ordinal apply (count equal → drive component loc/scale; differs → rebuild via the game's own addRoach(bypassCheck)/deleteRoach) + local eat/stomp detection by tracked-component liveness → RoachConsumed intent, host deletes nearest-within-200u. Client sim parked in spawn_authority. Known gap: the acting peer's prop_deadRoach_C husk is local-only (tryCrush's inner spawn is EX-invisible) | RoachState=92 / RoachConsumed=93 `[AS-BUILT 2026-07-10 eve; smoke pending]` |
| `creatures/owner_entity_sync.cpp` | OWNER-ENTITY tier (2026-07-10 eve, user rule): stalker creatures whose AI reads the LOCAL player (eyer_C first) are per-peer OWNED + cross-peer VISIBLE. Owner: BeginDeferred POST detect (ScopedMirrorSpawn-excluded) -> announce + 4 Hz pose on movement + 10 s keepalive (= late-join delivery) + IsLiveByIndex death-watch. Receivers: brain-parked (DisableCharacterTicks) collision-OFF (killsphere can never hurt a viewer) display mirror keyed (slot,seq); leaver's mirrors destroyed in DisconnectSlot; ALL mirrors destroyed on session end. F1 Content > Entities "Spawn Eyer" = lane test | OwnerEntitySpawn/Pose/Destroy=94-96 (client-relayable) `[AS-BUILT 2026-07-10 eve; smoke pending]` |
| `event_cue_sync.cpp` | Host-auth cosmetic cues (starfall etc.). Phase 2a join re-send (2026-07-05, COOP_EVENT_JOIN 3.4): world-ready edge re-sends live ALREADY-BROADCAST cues ToSlot (a newer-than-last-poll cue is left to the next Tick broadcast -- no double); emitter phase not synced (joiner replays t=0) | EventCue `[V; join re-send AS-BUILT, autonomous mid-shower e2e PASS 2026-07-05 09:36 -- exactly one client replay]` |
| `event_active_sync.cpp` | Join-during-event Phase 1 (v98, 2026-07-05; COOP_EVENT_JOIN.md): host 1 Hz mirror of the NATIVE activeEvents registry (gamemode.activeEvents_senders membership diff -> BEGIN/END edges) + at a joiner's world-ready edge one EventSnapshot per in-flight entry (class + kClassRowMap row + elapsedSec); client receiver hands mapped replay-safe rows to event_fire_sync's ACTIVE-OVERRIDE replay (bypasses the passEvents "history" dedupe). Unmapped classes WARN LOUD = the Phase 2 census-fill signal. Per-event scenario docs = docs/events/ | EventSnapshot `[AS-BUILT; autonomous mid-join e2e PASS 2026-07-05 00:06 -- forced obelisk pre-join: snapshot -> override replay dispatched; alarm unmapped-skip; hands-on pending]` |
| `piramid_sync.cpp` (creatures/) | Walking-pyramid event choreography lane (v97, 2026-07-04 late; docs/events/piramid.md): client mirror brain suppression (FOUR PRE-cancelled timer handlers — seeWisps/checkIfReached/randLoc, + changeLook since v102; tick kept alive for beams/look-at) + host gather-commit relay + client native checkIfReached re-dispatch + (v98) join-edge in-flight gather re-send ToSlot (COOP_EVENT_JOIN 3.4). 2026-07-05 **v100 FACING** (`75e5ab10`): visible heading = movementVector/Arrow components' WORLD rotation (actor root never yaws), streamed as auxYaw; delta-derivation LIVE-REFUTED + removed (RULE 2). **SUCK fix `7ec1f666`**: the gathered wisp's rise is the WISP's OWN tick — a MESH move (root pose stream blind by design) on an npc_mirror-parked tick-off mirror; the gather replay now re-enables that wisp's actor tick (safe: hunt AI unreachable under gathered [bytecode gate], CMC MOVE_None). **v102 HEAD `a255b70f`**: lookat eases toward wispTarget (chase — converges natively, same mirrored wisp) OR relLook, re-rolled 1 Hz by changeLook's RandomFloatInRange per instance -> host relLook streams as auxVec, mirror changeLook suppressed, native ease plays out | PyramidGather `[AS-BUILT; SUCK **[V by user 2026-07-05, full event run: «засасывание зеркально 100%»]**; facing v100 user-verdicted «хороший результат»; v102 head 0y re-verdict pending; TRUE mid-join = the still-open acceptance case; gather-at-join re-send code-verified only]` |
| `alarm_sync.cpp` | BASE emergency alarm -- fire-alarm siren + red alarmLamp beacons (v101, 2026-07-05; docs/events/alarm.md; NOT the per-terminal radar beep, which stays local per-viewer): trigger_alarm_C.active as a shared-world toggle. ON sources (exhaustive census): the computer's radar scan on an important comp_radarPoint (= how events light it indirectly; obelisk never calls runTrigger itself) + the snuskLoaf prank; OFF = panel_radar "b/Stop alarm". runTrigger is EX_VirtualFunction (PE-INVISIBLE, both callers) -> 1 Hz active-bit POLL on BOTH peers (FindBoolProperty real-bit, IsLiveByIndex-cached instance); host broadcasts observed transitions, client forwards LOCAL ones (its scan/stop-press) to the host; apply = reflected runTrigger(nullptr, active) = the FULL native fanout (lamps+klaxon+grate+ceiling solar+setEvent), natively idempotent -> echo-proof. Join answer: unconditional connect snapshot; trigger_alarm_C = first kLaneOwnedClasses EventSnapshot skip | AlarmState `[AS-BUILT; autonomous e2e PASS 2026-07-05 13:50 -- BOTH the mid-alarm-join delivery (connect snapshot after a gated pre-world broadcast) and the live OFF transition, same-second client applies, 0 ERROR; hands-on pending for siren/lamps + client-originated stop]` |
| `balance_sync.cpp` | Shared Points balance | BalanceSync/BalanceDelta `[V]` |

## L4 — Player-scoped / social
Per-player or broadcast; not world entities.

| File | What | ReliableKind |
|---|---|---|
| `player_inventory_sync.cpp`, `inventory_wire.cpp` | Per-player inventory blob | PlayerInventoryBlob `[V]` |
| `inventory_pickup_sync.cpp` | World item pickup | InventoryPickup `[V]` |
| `item_activate.cpp`, `flashlight_click_sound.cpp`, `prop_sound.cpp` | Held-item effects (flashlight etc.) | ItemActivate `[V]` |
| `chat_sync.cpp`, `chat_feed.cpp`, `chat_bubbles.cpp` (+ `ui/fonts.cpp`, `ui/hud.cpp` DrawChat + the bubble draw, `ui/chat_input.cpp`) | Text chat + the on-screen feed. 2026-07-04 `684f6670`: UTF-8 END-TO-END (the ASCII '?'-squash retired; Cyrillic renders -- embedded-Roboto overlay font w/ Cyrillic ranges), fade-in + per-slot colored nick + word-wrap + Up/Down send-history. Wire UNCHANGED (text[203] always carried raw bytes). 2026-07-05 (12g): **overhead chat BUBBLES** (MTA/SAMP shape) -- chat_bubbles stores the last message per slot (8 s hold + 0.7 s fade), nameplate::Update snapshots it into the Plate, hud draws it word-wrapped above the nick (rides the plate anchor: distance/occlusion fade applies, a v94-hidden plate hides the bubble; display-only, NOTHING on the wire). `[wire V; UTF-8/UI layer AS-BUILT runbook 0j; bubbles AS-BUILT runbook 0aa]` | ChatMessage `[V]` |
| `email_sync.cpp` | In-game email. **HOST-ONLY append authority since `e5718fc6` (2026-07-09)**: only the host broadcasts EmailAppend (role-gated send; clients author zero); DELETE stays peer-symmetric (shared delete, content-hash + tombstones). Hardened `9becc5e3`/`606fda3b` (2026-07-10): EmailAppend REMOVED from the relay whitelist + host-side authority drop (serverbox parity shape); DELETE-VERB CENSUS closed — pak-wide the ONLY remover of saveSlot.emails is the player's `ui_laptop.delEmail`, so the "You deleted" attribution is correct by construction; a >32-row removed batch in one poll = shadow desync → silent re-baseline (no announce/broadcast; symmetric with kBulkAppendThreshold). | EmailAppend (host-only) / EmailDelete (symmetric) `[AS-BUILT, smoked; NOT hands-on]` |
| `interactables/serverbox_sync.cpp` | Signal-server sim state (v107 Inc-1, `96012938`): HOST 1 Hz polls brokenServers + per-box IsBroken mask -> broadcasts on change + join snapshot; CLIENT raw-writes IsBroken + notify-free `check()` re-skin, kills its `ticker_serverBreaker` (tick off; RESTORED on disconnect since `db6ecd0b` -- **restore VERIFIED by real log 2026-07-10 14:15:00**: the census client's death teardown printed `restored 1 ticker_serverBreaker on session end`). Kills the client's false "server down". Minigame VARIANT roll = T1-3 OPEN (INTENT shape). | ServerState=91 (host->client only) `[restore V-by-real-log; mirror AS-BUILT, smoked; NOT hands-on]` |
| `comms/peer_action_feed.cpp` | Peer-action chat announces (`4a085d82`): email delete -> "<nick> deleted an email: <subj>", rendered LOCALLY from the existing EmailDelete wire event (no new packet); F1 toggle `ui.chat.peer_actions` (default ON). The extensible home for future peer-action notices. | (none — rides EmailDelete) `[AS-BUILT; NOT hands-on]` |
| `player_damage.cpp`, `wisp_tear_mirror.cpp`, `wisp_attack_sync.cpp`, `wisp_grab_hold.cpp` | Combat: relayed damage + killer-wisp fatality. v2 `769d02f7` (2026-07-04): host-authoritative AGGRO SELECTOR (uniform random + stickiness + canReach LOS over host+puppets in 5000u; raw Target re-assert per tick), two-stage close (arm 550u+LOS, relay at 200u contact / 2.5 s LOS-gated timeout), grab CHOREOGRAPHY on every peer (victim replays the native Capture template on its LOCAL mirror; host+third peers snap the victim puppet to the 'playerGrab' socket AFTER pose apply — net_pump ordering; host lifts the wisp 150 cm/s for the window), canRagdoll=false belt over the false-grab window. NO wire change. `[kill CHAIN V hands-on 2026-07-04 eve — client died to kwisp AND host died with clients watching (runbook 0b partial); OPEN: kill pacing too fast vs SP + the tendril/"РУКИ" beam VFX not mirrored]` | PlayerDamage/WispGrab/WispTear `[V]` |
| (voice) `VoiceState` is dispatched in L2's state family; voice audio rides its own Opus path. `[?]` | Voice chat | VoiceState `[?]` |

## L5 — Host control / moderation / dev (not gameplay sync)
| File | Role |
|---|---|
| `moderation.cpp`, `ban_list.cpp`, `roster.cpp` | Kick/ban, peer admin. `[?]` |
| `save_guard.cpp`, `save_block.cpp`, `save_button_disable.cpp` | Client save suppression (host-authoritative world). save_block = the SaveGameToSlot disk write-block `[V]` + PART 3 (2026-07-04 `99eb4566`): the native save CYCLE off — the client holds `gamemode.disableSave=true`, the head-gate of `saveSlot_C::save` `[V bytecode]`, so autosave/sleep/menu saves never even GATHER; the disk hook stays as the belt for the ungated `savePlayerOnly`/direct-trigger writes. `[AS-BUILT; hands-on runbook 0e = verdict]` |
| `save_indicator_suppress.cpp` | Host-side join-save "SAVED" toast suppression — **NOT WORKING, hypothesis REFUTED live** (user 2026-07-04: "never worked"): PHASE 1 (detect-log, read-only BY DESIGN) ran and caught NOTHING — host log 2026-07-04 12:37 `in-window saveAnim=0 addHint=0`, no deferred hits — so the RE guess (saveAnim/addHint paints it) is wrong or the UFunction::Func patch missed; PHASE 2 was never built. Next: widen the hunt for the actual painter. `[REFUTED-PROBE]` |
| `shutdown.cpp`, `garbage_sync.cpp` | Lifecycle/teardown + targeted client-side crash fixes (e.g. garbage_sync = the open-container pickup AV fix). (ambient_spawner_suppress dissolved 2026-07-10 into world/spawn_authority.) `[V]` |
| `pause_guard.cpp` | Coop NO-PAUSE invariant (2026-07-04 `769d02f7`): while connected, a paused world (client/host ESC, console `pause`) is un-paused every gameplay tick via GameplayStatics::SetGamePaused(false) — STATE-level, one owner; the ESC pause is EX_CallMath = PE-invisible so call-site interception is impossible. ESC menu stays usable; solo pause untouched. `[AS-BUILT; e2e autotest VOTVCOOP_RUN_PAUSE_TEST queued; hands-on ESC = verdict]` |
| `ui/multiplayer_menu.cpp`, `coop/config/config.cpp`, `ui/input_focus.cpp`, `grab_observer.cpp` | Menu UI; the ONE config home (2026-07-10: harness/config + session/ini_config merged, `coop::config`); keyboard-focus arbitration (`ui::input_focus` -- extracted from ini_config); the grab input observer. `[V]` |

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
