# ROADMAP — VOTV coop mod

**Living document.** Re-arrange as priorities shift. Phases follow
`docs/COOP_METHODOLOGY.md`, adapted for UE4.27. Each phase has a hard
gate — don't proceed until met.

Legend: ☐ not started · ◐ in progress · ☑ done.

**Quick status (2026-05-25):** Phases 0-3 done. Phase 4 partial (4.1
input replication is per-pawn pose stream; 4.2-4.6 ongoing). Phase 5
done (autonomous harness + LAN test framework). Recent work is in the
5N* / 5S* / 5T / 5D series (NPC sync, save snapshot-on-connect, terminals,
doors+lights). See the bottom of this file for the "Live workstreams"
section — that's where iteration actually happens.

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
       `research/findings/coop-phase-1-input-map-and-spawn-probe-2026-05-21.md`).
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

- ☑ 3.1 UDP, host-authoritative, LAN-first. `coop/net/transport.cpp`.
- ☑ 3.2 Sessions, not connections. Session-token + peer-lock + bounded
       drain + NaN/AABB validate + RFC1982 sequence numbering.
- ☑ 3.3 Pure I/O at bottom; 3-layer split (transport ↔ serialization ↔
       application). Principle 7 applied to network.
- ☑ 3.4 Position-only pose sync at 60 Hz + receiver-side interpolation
       (50 ms LERP window, MTA-shape `SetTargetPose` on new packet +
       per-tick interp pump). See
       `research/findings/mta-pose-interpolation-2026-05-23.md`.
- ☑ 3.5 Auto-spawn the remote on first packet.

### Reliable channel ☑

- ☑ Stop-and-wait ARQ over the same socket, distinct sequence space,
       250 ms RTO. Carries Join / Bye / Chat / RestoreVitals /
       TeleportClient / PropSpawn / PropDestroy / EntityDestroy.
       See `coop/net/reliable_channel.cpp`.
- ☑ Coop chat + session event log (joins / leaves / errors) — DONE
       2026-05-23. Top-right UMG feed; reliable.

### Multiplayer menu — DEFERRED

- ☐ Native UMG menu (Host / Connect / browser) integrated into VOTV's
       main menu. Currently we use `mp_host_game.bat` / `mp_client_connect.bat`
       env-var-driven launchers (`tools/`). Design preserved in
       `docs/MULTIPLAYER_UI.md`; build deferred until the per-feature
       sync work stabilises.

## Phase 4 — Replication layers (the bulk) ◐

- ◐ 4.1 Input replication — partial. Pose stream (loc + yaw + speed +
       head-yaw) is "replay-by-state" rather than full keysync, sufficient
       for current scope. Per-action input replication (E-press, hotbar,
       drop, etc.) lands as needed per feature.
- ◐ 4.2 Equipment / held-item / tool state — **physics-prop pickup
       SHIPPED** 2026-05-24 (PropGrab / PropPose-piggyback / PropRelease
       + throw impulse). See
       `research/findings/physics-object-pickup-coop-plan-2026-05-23.md`
       and the Aprop lifecycle RE doc.
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
- ☐ Phase 5N1 Inc3: NPC client-mirror + EntityDestroy + EntityPoseBatch.

### 5S — Save + world-state sync ◐
- ☑ Phase 5S0 Inc1: snapshot-on-connect, host enumerates `Aprop_C` +
       sends `PropSpawn` per prop on client join.
- ☑ Phase 5S0 Inc2: continuous spawn + PropDestroy lifecycle hook.
- ☑ Gap I-1: fuzzy de-dupe + rekey for same-position divergent-key
       spawns from natural spawners.

### 5T — Story-object terminals ◐
- ☑ RE pass complete (`research/findings/votv-interactable-terminals-RE-2026-05-25.md`)
       — 12 sections + 4 audit cycles + UE4SS Lua probe pass (E-12-CR1
       critical finding: direct OnClicked invoke crashes without active-use
       state). 13-increment plan.
- ☐ Inc1: terminal-key resolution + singleton fallback. Awaiting user
       direction; the user has since pivoted to simpler doors/lights work
       (5D) as the next sync target.

### 5D — Doors + light switches ◐
- ☑ RE pass complete 2026-05-25
       (`research/findings/votv-doors-and-lightswitches-RE-2026-05-25.md`)
       — class enumeration, hook/invoke = `doorOpen`/`doorClose` +
       `Atrigger_lightRoot_C::SetActive`, 7-increment plan, 6 open
       flags.
- ☐ Inc1: door Key resolution infrastructure. Queued.

### Dev convenience features ◑ (one-off shipping)
- ☑ HOME freecam — flying debug camera with WASD/Space/Ctrl + wheel
       speed + MMB teleport. Ini-gated `[dev] freecam=1`.
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

### Open / future
- ☐ Phase 5N1 Inc3 — NPC client-mirror + EntityPoseBatch stream.
- ☐ Phase 5D Inc1+ — doors + light switches sync implementation.
- ☐ Phase 5T Inc1+ — terminals interactive sync.
- ☐ Multiplayer menu in VOTV's main menu (currently env-var .bat
       launchers; design ready in `docs/MULTIPLAYER_UI.md`).
- ☐ Voice chat (proximity positional + push-to-talk, Plasmo-Voice-style).
       Out of current scope; mic capture component (`UAudioCaptureComponent`)
       already on the puppet to destroy.
- ☐ Master server + opt-in public server browser (WAN concern, Phase 7+).
- ☐ Ragdoll sync (non-trivial — VOTV ragdoll renders on a SEPARATE
       invisible body; we'd be inventing visible ragdoll on the puppet).

---

## Timeline note

Per the methodology's final note: coop is a 6-month-to-2-year effort.
UE4SS + reflection compressed Phase 1 (no blind RE) and de-risked Phase 2
(engine natively supports multiple pawns). Phase 4 (replication) remains
the bulk regardless of engine — it's where 2026-05-22 to today has been
spent and where the upcoming months will continue.
