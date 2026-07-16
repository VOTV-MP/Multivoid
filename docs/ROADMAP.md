# ROADMAP — VOTV coop mod

**Living document.** Re-arrange as priorities shift. Phases follow
`docs/COOP_METHODOLOGY.md`, adapted for UE4.27. Each phase has a hard
gate — don't proceed until met.

Legend: ☐ not started · ◐ in progress · ☑ done.

**Quick status (updated 2026-07-03):** Phases 0-5 done. Transport is
**GameNetworkingSockets v1.5.1** (3 lanes, `kMaxPeers=4`). The protocol is at
**v95** (EventFire=84: the scheduled/story-event replay channel — host observes
settime fires via passEvents growth, clients replay per the per-row dupe-matrix
policy; COOP_SYNC_MAP `EventFire` block is canonical). Shipped surface: the MTA
Element/Registry/Mirror foundation; NPC /
WorldActor / save-snapshot-on-connect / terminals / doors+lights+keypads /
kerfur (prop⇄NPC) / events / voice / inventory / sleep / effects sync; the
host-authoritative pile/trash channel incl. client grab/carry/throw and the
pile NATIVIZATION arc — the whole pile narrative that used to be logged HERE
(2026-06-20..23 point-in-time, incl. several since-superseded claims like
"level-pile dup fix NOT built" and "proxy scale hands-on PENDING") is canonical
in the LIVING pile KB [docs/piles/](piles/) (08→12) + `research/findings/`
(RULE 2: one home); the v93 per-player SKINS system + the measured-fit offline
model converter ([COOP_CLIENT_MODEL.md](COOP_CLIENT_MODEL.md)); v94 synced
per-peer nameplate visibility + builtin kerfur skins + the join-window pose
gate (runbook 2026-07-02 take-4 — verdict pending hands-on).
**The day-to-day live state is
in the auto-memory (`MEMORY.md` index + the top `project_*` entry), NOT this
file** — this roadmap is the phase-gate structure; the memory is the running log.
For cross-cutting architecture truth see the new
[COOP_ENTITY_EXPRESSION_MAP.md](COOP_ENTITY_EXPRESSION_MAP.md) +
[COOP_DISPATCH_VISIBILITY.md](COOP_DISPATCH_VISIBILITY.md). The "Live workstreams"
section at the bottom of this file is stale (≤2026-05-29) — defer to memory.

**Foundation audit v2 (2026-05-29):** see
`research/findings/architecture-audits/votv-architecture-audit-2026-05-29.md` for the full
14-dimension audit + PR-FOUNDATION-1..5 priorities. Key gap: cross-peer
relay topology is still 2-player-shaped; 4-peer LAN smoke missing.

---

## Phase 0 — Feasibility + bootstrap ☑

**Gate met:** `docs/FEASIBILITY.md` documents 0.2-0.8; viable verdict;
skeleton committed; UE4SS verified injecting into VOTV 0.9.0-n.

## Phase 1 — Engine archaeology (reflection-driven) ☑

**Gate met:** core engine entry points documented in `research/findings/`.

- ☑ 1.1 Entity factory — `UWorld::SpawnActor` via
       `UGameplayStatics::BeginDeferredActorSpawnFromClass` /
       `FinishSpawningActor` (deferred-spawn pair).
- ☑ 1.2 Player class layout — `AmainPlayer_C : ACharacter` (camera,
       grab/hold, stats, inventory). GameMode `AmainGamemode_C`
       (world/save hub).
- ☑ 1.3 Input dispatch — stock `APlayerController`; VOTV input = pawn
       `InpActEvt_*` BP events. Full vocabulary mapped (see
       `research/findings/phase0-bootstrap/coop-phase-1-input-map-and-spawn-probe-2026-05-21.md`).
- ☑ 1.4 Tick / save / level-load / UI / Blueprint VM — covered piecewise
       across the 2026-05-22..2026-05-25 findings (autonomous harness
       skip-to-gameplay, save-load entry, UMG widget construction, BP
       UFunction invocation via ProcessEvent). The reflection substrate
       turned out to give us everything we need without per-system
       findings; outstanding spelunking lives in workstream-specific
       RE docs (terminals, doors+lights, NPCs, events).

## Phase 2 — Foundation infrastructure ☑

**Gate met:** standalone DLL injects, reflection works, game-thread
context proven, RemotePlayer puppet visible + driven on LAN.

### Standalone shipping vehicle (RULE №3) — DONE

- ☑ C++ toolchain + build: `votv-coop.dll` (CMake + VS2019, x64, static CRT).
- ☑ Standalone proxy loader: **`xinput1_3.dll`** auto-loads `votv-coop.dll`
       on game start; UE4SS absent. `tools/deploy-loader.ps1` installs it.
- ☑ Standalone reflection (no UE4SS): AOB-resolved `GUObjectArray` +
       `FName::ToString` + `ProcessEvent`. Health check on boot:
       reflection-resolves every primitive, FUNCTIONALLY validates them,
       prints PASS/FAIL with the game version + exe fingerprint.
- ☑ Generic UFunction call infrastructure (`ParamFrame` walks the live
       UFunction's FProperty chain — no hardcoded offsets per call).
- ☑ Game-thread context: `ProcessEvent` detour drains a posted-task pump
       so worker threads can dispatch onto the game thread safely.
- ☑ MinHook integrated as a static-linked C library (third-party submodule).

### Phase 2 puppet path — DONE

- ☑ 2.1 Orphan spawn: `SpawnActor<mainPlayer_C>` (deferred-spawn pair).
- ☑ 2.2a Local-player HIJACK prevention: `inertPawn=true` zeroes
       `AutoPossessPlayer` / `AutoPossessAI` / `AutoReceiveInput`
       + sets `bBlockInput` in the deferred-spawn window. No control or
       gamma stomp.
- ☑ 2.2b Remote BODY POSE: kerfurOmega AnimBP plays on the puppet's
       SkeletalMeshComponent. Anim state-machine driven by satellite
       ACharacter (BUA-pattern fix 2026-05-23). Animations + IK
       working (IK confirmed user-visible 2026-05-25).
- ☑ 2.2c Remote FACING: yaw + control rotation independent of local
       player; head-leads-body cone shipped.

## Phase 3 — Networking transport ☑

**Gate met:** both players see each other's pawn moving in real time on
LAN (two-machine + same-box-two-instance both confirmed).

- ☑ 3.1 **GameNetworkingSockets (Valve, MIT, v1.5.1)** vendored as a
       submodule + `/WHOLEARCHIVE` linked into `votv-coop.dll`.
       host-authoritative, LAN-first. `coop/net/session.{h,cpp}` drives
       GNS end-to-end (CreateListenSocketIP / ConnectByIPAddress /
       SendMessageToConnection / ReceiveMessagesOnConnection /
       SteamNetConnectionStatusChangedCallback). The hand-rolled Winsock
       UDP + Hello/HelloAck + stop-and-wait ARQ + Ping/Pong RTT layer was
       fully RETIRED in PR-2 (2026-05-28).
- ☑ 3.2 Sessions, not connections. `kMaxPeers=4` via GNS PollGroup
       (PR-4); per-slot OnDisconnect contract (PR-4.7); senderPeerSlot
       narrow-cast guarded; bounded drain; NaN/AABB validate; RFC1982
       sequence numbering.
- ☑ 3.3 Pure I/O at bottom; 3-layer split (transport ↔ serialization ↔
       application). Principle 7 applied to network.
- ☑ 3.4 Position-only pose sync at 60 Hz + receiver-side interpolation
       (50 ms LERP window, MTA-shape `SetTargetPose` on new packet +
       per-tick interp pump). See
       `research/findings/mta/mta-pose-interpolation-2026-05-23.md`.
- ☑ 3.5 Auto-spawn the remote on first packet.

### Priority lanes ☑

- ☑ **Three GNS lanes** wired via `ConfigureConnectionLanes(3, [0,1,2],
       weights=[4,2,1])` (PR-3, 2026-05-28). Lane assignment:
  - **High** — TeleportClient, RestoreVitals, ItemActivate, Join.
  - **Normal** — PoseSnapshot, PropPose, Chat, default-unspecified events.
  - **Bulk** — PropSpawn, PropDestroy, EntityDestroy, EntitySpawn,
    snapshot fan-out.
  Snapshot fan-out no longer head-of-line-blocks interactive events.
  **GNS guarantees order WITHIN a lane, not across** — receivers that
  need to compare a Bulk event against Normal state (e.g. PropSpawn vs
  Join) must gate on identity establishment, not lane order. See
  E-2 / C-1 finding in
  `research/findings/architecture-audits/votv-architecture-audit-2026-05-29.md`.

### Reliable channel ☑

- ☑ GNS reliable messages (per-lane). Carries Join / Bye / Chat /
       RestoreVitals / TeleportClient / PropSpawn / PropDestroy /
       EntityDestroy / EntitySpawn / ItemActivate / WeatherState /
       LightningStrike. The internal FIFO queue + per-feature retry
       crutches were RETIRED 2026-05-27.
- ☑ Coop chat + session event log (joins / leaves / errors) — DONE
       2026-05-23. Top-right UMG feed; reliable.

### Multiplayer menu — SHIPPED ☑

- ☑ AS-BUILT (commit 43e2a843, 2026-06-05; refined f32ed1b0): a native UMG
       "MULTIPLAYER" UButton is injected above NEW GAME in VOTV's `ui_menu_C`
       main menu (`coop/multiplayer_menu.cpp:78` via
       `engine::InjectCanvasButton`); clicking it opens an in-process ImGui
       server browser (`ui/server_browser.cpp`) with Host Game (save picker
       -> `HostWithSave`), direct-IP Connect, the master-lobby server list
       + double-click join, and nickname editing. Backends in
       `coop/session_manager.cpp` (HostWithSave / JoinLobby / ConnectDirect).
       Wired at `harness.cpp:1104`; rendered at `imgui_overlay.cpp:356`.
       Matches `docs/MULTIPLAYER_UI.md` (marked BUILT 2026-06-20). The browser
       panel itself renders in ImGui (in-process overlay), not pure UMG. The
       `mp_host_game.bat` / `mp_client_connect.bat` launchers remain only as
       the autonomous-test entry points.

## Phase 4 — Replication layers (the bulk) ◐

- ◐ 4.1 Input replication — partial. Pose stream (loc + yaw + speed +
       head-yaw) is "replay-by-state" rather than full keysync, sufficient
       for current scope. Per-action input replication (E-press, hotbar,
       drop, etc.) lands as needed per feature.
- ◐ 4.2 Equipment / held-item / tool state — **physics-prop pickup
       SHIPPED** 2026-05-24 (PropGrab / PropPose-piggyback / PropRelease
       + throw impulse). See
       `research/findings/physics-grab/physics-object-pickup-coop-plan-2026-05-23.md`
       and the Aprop lifecycle RE doc. **HOTBAR hand item SHIPPED
       2026-07-06 (v105 hand_item display axis** — player expression, out
       of the prop pipeline; **+ v105b same day**: view-anchored mirror
       (USER-APPROVED live), census hand-exclusion (dupe root), edge-instant
       stow; **+ v106 2026-07-07 (`29dfd079`)**: SEAM-DRIVEN world-prop
       coherence (K2_DestroyActor Func seam + hand-edge express +
       FinishSpawningActor Func drain — replaces the v105b forced reconcile,
       RULE 2) + pile birth-certificate/carry-termination; **+ v106b same
       day (`4a280375`)**: MIGRATION-FIRST identity (a morph husk dies
       eid-less) + BIRTH-ORPHAN express + wholesale GHOST-RETIRE (the
       E-press per-ghost retire RETIRED, RULE 2). **VERIFIED hands-on
       2026-07-07 (0ae): grab lane PASS + Q-menu spawns INSTANT on
       clients.** See COOP_SYNC_MAP.)
       Smart-ITEM behavior sync (hook/nailgun/wallbuilder/...) = RE done,
       docs/items/ pattern ratified, implementation queued.
- ◐ 4.3 Entity manifest + per-entity state — IN PROGRESS, see live
       workstreams below.
- ☐ 4.4 Cutscenes / scripted events — events RE done (~80 events,
       unified EntityEventPacket design); implementation queued.
- ◐ 4.5 Save / world-state sync — host-authoritative DECIDED 2026-05-24.
       Snapshot-on-connect (5S0) Inc1 + Inc2 shipped: world props
       enumerated + PropSpawn'd at session start; PropDestroy lifecycle
       wire-synced.
- ☐ 4.6 Inventory / progression — inventory CONTENTS are per-peer
       private (decision 2026-05-24). World ↔ inventory transitions
       (pickup / drop) replicate as world events; bag contents don't.

## Phase 5 — Validation infrastructure ☑

- ☑ 5.1 Autonomous test harness — `tools/run-test.ps1` writes
       `scenario.txt`, harness reads it from inside the DLL and drives the
       engine from a worker thread (`harness::autotest`). Scenarios:
       `play`, `netloopback`, `load:<slot>`, `none`, plus the per-feature
       grab / probe-terminals scenarios.
- ☑ 5.2 LAN test framework — `tools/lan-test.ps1` launches host + client
       same-box two-process, env-var config (`VOTVCOOP_*`), per-PID
       log capture. Found and fixed multiple real handshake bugs that
       loopback hid.
- ☑ 5.3 Live testing — `mp_host_game.bat` (host) + `mp_client_connect.bat`
       (client). Two physical game-copy directories (`Game_0.9.0n/` for
       host, `Game_0.9.0n_copy/` for client) so same-box xinput1_3 loading
       doesn't collide.
- ☑ 5.4 Multi-agent audits — every non-trivial coop change goes through
       a `feature-dev:code-reviewer` audit pass per the CLAUDE.md
       post-ship-audit rule. File-size / modularity check baked into the
       audit prompt template (RULE 2026-05-25 post-`harness.cpp`-bloat).

---

## Live workstreams (where the iteration happens)

Each item below is a feature increment series. Cross-referenced in
`memory/MEMORY.md` for session-resume context.

### 5N — NPC + entity sync ◐
- ☑ Phase 5N stream B: host-authoritative mushroom suppression
       (mushroom7_C is the growing-state class; suppress on client).
- ☑ Phase 5N1 Inc1: NPC suppressor (host-only mandatory).
- ☑ Phase 5N1 Inc2: wire layer detection-only (gated on
       `VOTVCOOP_NPC_SYNC=1`).
- ◐ Phase 5N1 Inc3: NPC client-mirror + EntitySpawn + EntityDestroy
       PARTIALLY SHIPPED. A1 receiver (NPC client mirror via
       `MirrorManager<Npc>`) + EntitySpawn / EntityDestroy reliable
       packets landed 2026-05-28 (Tier-3 MTA Element migration). Still
       OUT OF SCOPE today: continuous NPC pose stream (EntityPoseBatch
       reliable kind) and NPC vitals replication — NPC AI currently runs
       independently on every client (per-client desync). See S-1..S-5
       in the foundation audit.

### 5S — Save + world-state sync ◐
- ☑ Phase 5S0 Inc1: snapshot-on-connect, host enumerates `Aprop_C` +
       sends `PropSpawn` per prop on client join.
- ☑ Phase 5S0 Inc2: continuous spawn + PropDestroy lifecycle hook.
- ☑ Gap I-1: fuzzy de-dupe + rekey for same-position divergent-key
       spawns from natural spawners.

### 5T — Story-object terminals ◐
- ☑ RE pass complete (`research/findings/computers-devices/votv-interactable-terminals-RE-2026-05-25.md`)
       — 12 sections + 4 audit cycles + UE4SS Lua probe pass (E-12-CR1
       critical finding: direct OnClicked invoke crashes without active-use
       state). 13-increment plan.
- ☐ Inc1: terminal-key resolution + singleton fallback. Awaiting user
       direction; the user has since pivoted to simpler doors/lights work
       (5D) as the next sync target.

### 5D — Doors + light switches ☑
- ☑ RE pass complete 2026-05-25
       (`research/findings/computers-devices/votv-doors-and-lightswitches-RE-2026-05-25.md`)
       — class enumeration, hook/invoke = `doorOpen`/`doorClose` +
       `Atrigger_lightRoot_C::SetActive`, 7-increment plan, 6 open
       flags.
- ☑ Inc1: door Key resolution infrastructure — AS-BUILT. `AtriggerBase_C::Key`
       resolved via reflection (`ue_wrap/door.cpp` EnsureResolved/GetKeyString)
       and wired as the door channel's key->actor index. SHIPPED v27 2026-06-03
       (commit 43e2a843).
- ☑ Phase 5D fully SHIPPED beyond Inc1: doors + light switches + container
       lids + garage + appliances + lockers, host-authoritative, protocol
       v27->v62 (DoorState=9, LightState=10, DoorOpenRequest=26 v32,
       GarageDoorState=33 v44, ApplianceState, LockerDoorState=50 v62).
       Engine in `coop/interactable_channel.h`; adapters in
       `coop/interactable_sync.cpp`; installed at `subsystems.cpp:89`.

### Dev convenience features ◑ (one-off shipping)
- ☑ HOME freecam — flying debug camera with WASD/Space/Ctrl + wheel
       speed + MMB teleport. Ini-gated `[dev] freecam=1`. 2026-07-05
       `ee828ff7`: entering freecam FREEZES the pawn (CMC MOVE_None,
       captured mode restored verbatim on exit) so the fly keys stop
       driving the character; look stays live (the freecam aims with it).
- ☑ F2 pos+cam HUD — on-screen player position + camera rotation
       overlay. Ini-gated `[dev] posinfo=1`.
- ☑ F3 RestoreVitals — refills food/sleep/health on both peers via the
       reliable channel. Ini-gated `[dev] devkeys=1`.
       (`coffeePower` deliberately excluded — triggers a screen-shake
       BP side-effect.)
- ☑ F4 TeleportClient — host presses, client teleports to host's pose.
       Uses VOTV's own `teleportWObackrooms` UFunction (bypasses CMC
       constraints that revert `K2_TeleportTo`). Ini-gated.
- ☑ F12 screenshot — via VOTV's own console `HighResShot`. Hands-on play
       SHOULD NOT use it (the saving-screenshot toast is distracting);
       autonomous scenarios only.

### Shipped since (moved out of Open / future)
- ☑ Phase 5D Inc1+ — doors + light switches sync SHIPPED (proto v27,
       2026-06-03; commit 43e2a843). See the 5D section above (extended to
       container lids, garage v44, lockers/console v62).
- ☑ Multiplayer menu in VOTV's main menu — native MULTIPLAYER button
       injected above NEW GAME into `ui_menu_C` (`coop/multiplayer_menu.cpp`)
       opening an ImGui server browser (`ui/server_browser.cpp`) with Host
       Game (save picker), direct-IP Connect, master-lobby list + double-click
       join, and nickname editing; backends in `coop/session_manager.cpp`.
       Wired at `harness.cpp:1104`, rendered at `imgui_overlay.cpp:356`.
       Shipped 43e2a843 (2026-06-05). See the "Multiplayer menu — SHIPPED"
       section above. The .bat launchers remain a dev/test convenience only.
- ☑ v66 Voice chat — proximity positional (SetListener + SVC REDUCED-mode
       pan + distance attenuation) + push-to-talk (default key 'G',
       `voice.ptt_key` overridable) + activation mode; Opus over the existing
       coop session (`MsgType::VoiceFrame`), miniaudio capture/playback, jitter
       buffer, whisper, per-player volume, mute, voice icons on nameplate /
       scoreboard / HUD. Simple-Voice-Chat port
       (`research/findings/network/votv-voice-chat-port-design-2026-06-12.md`). Code:
       `src/votv-coop/src/coop/voice/*`, `session_voice.cpp`; wired
       `subsystems.cpp:108`/`:332`. Shipped f32ed1b0, UI refined 9ed8789a.
- ☑ Master server + opt-in public server browser — SHIPPED. ImGui browser
       (`ui/server_browser.cpp`) over a master lobby service
       (`coop/net/lobby_client.cpp` GET /v1/lobbies + `lobby_announcer.cpp`
       /v1/host /heartbeat /leave /visibility); built-in official VPS endpoint
       (`config.cpp` kBuiltinMasterUrl), opt-in "Show in server browser"
       toggle (`scoreboard.cpp` -> /v1/visibility), reference master server at
       `tools/coop_master_server.py`. Landed in commit 43e2a843.
       - 2026-07-16: the VPS master + signaling were **rewritten in Rust**
         (`tools/coop-server-rs/`, static musl) and **DEPLOYED LIVE** (wire-compatible;
         Python retired on the box). A 4-agent security audit + Tier A hardening
         followed (server `249a22b0`, client `7e8b1d2c`).
       - 2026-07-16 (evening): stack **MIGRATED to the new coop VPS `172.86.94.3`**
         (Rust-native provision, verified; old box keeps only unrelated services, coop wiped per RULE 2;
         compiled endpoints flipped `cd6faf81`; box upgraded +
         rebooted, docker/WG removed, re-verified outside). Domain `votv.mp` on Cloudflare DNS
         (NS pending). NEXT: Tier B TLS (`master.votv.mp` + rustls + client https
         cutover), then Tier C per-session tokens. Control plane cleartext until then.
         See `research/findings/network/votv-master-server-RE-and-rust-port-scope-2026-07-16.md`.

### Open / future
- ☐ Phase 5N1 Inc3 cont. — EntityPoseBatch stream for NPC pose
       replication (currently NPC AI runs per-client; combat / horror
       loop incoherent without it). See S-1 in
       `research/findings/architecture-audits/votv-architecture-audit-2026-05-29.md`.
- ☐ Phase 5T Inc1+ — terminals interactive sync.
- ☐ P2P + ICE / NAT punch (WAN). Design seed:
       `research/findings/network/votv-gns-p2p-masterserver-plan-2026-05-28.md`.
       `ENABLE_ICE=OFF` today.
- ☐ Ragdoll sync (non-trivial — VOTV ragdoll renders on a SEPARATE
       invisible body; we'd be inventing visible ragdoll on the puppet).
- ☐ LuaJIT resource system, MTA-shape (designed-not-scheduled; user ask
       2026-07-05: "MTA/SAMP have scripts support... custom gamemodes,
       filterscripts, their own admin systems"). Post-sync-parity (R5+).
       Shape: LuaJIT vendored INTO votv-coop.dll (RULE №3 — like MinHook/
       ImGui; UE4SS-Lua stays dev-only), `resources/<name>/` folders with
       a meta-manifest (MTA meta.xml analog), one sandboxed lua_State per
       resource (no io/os by default), start/stop at runtime. API spine =
       what already exists as C++ subsystems: coop/element registry
       (element-tree functions), ProcessEvent-visible seams
       (addEventHandler analog), one new ReliableKind ScriptEvent
       namespaced per resource (triggerServerEvent/triggerClientEvent
       analog), chat/nameplate/moderation/ImGui as stdlib. Client-script
       distribution at join = the one MTA piece currently in the
       explicitly-skipped column (asset-replication lite, hash-verified)
       — revisit then. Phasing: embed + read-only API -> mutations via
       existing lanes -> client-side scripts + ACL-like permission model.
       Nothing built now is throwaway: every principle-2 subsystem API
       becomes a binding.

### PR-FOUNDATION queue (foundation audit v2, 2026-05-29)
Strategic priorities derived from the 14-dimension audit. Read
`research/findings/architecture-audits/votv-architecture-audit-2026-05-29.md` for the full
TL;DR + tier-list.
- ☐ PR-FOUNDATION-1 — Identity epoch + range enforcement
       (`IsAllowedSenderEid` helper across 7+ wire sites; closes E-1 +
       3 PropSpawn/Destroy/Join gaps + host-migration eid collision).
- ☐ PR-FOUNDATION-2 — Save-game safety contract (PreSaveScrub /
       PostSaveRestore hooks, atomic-rename, backup).
- ☐ PR-FOUNDATION-3 — Manager pattern collapse (one canonical mirror
       table; `ScopedElementRef<T>` RAII handles). Do BEFORE NPC 5N
       expansion or Door/Vehicle/Switch.
- ◐ PR-FOUNDATION-4 — Host policy layer. Kick/ban SHIPPED (moderation.cpp +
       ban_list.cpp + the scoreboard actions; reasons + unban + the persistent
       seen-players registry + the host-only F1 Administration UI landed
       2026-07-05 `f66d2c7f`). Remaining: rate-limiting.
- ☐ PR-FOUNDATION-5 — Per-peer observability HUD + structured event
       stream.

---

## Timeline note

Per the methodology's final note: coop is a 6-month-to-2-year effort.
UE4SS + reflection compressed Phase 1 (no blind RE) and de-risked Phase 2
(engine natively supports multiple pawns). Phase 4 (replication) remains
the bulk regardless of engine — it's where 2026-05-22 to today has been
spent and where the upcoming months will continue.
