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

## Project phases — the long-term arc (RESTRUCTURED 2026-07-20, user-approved)

The MTA/gmod trajectory. Everything below "Phase 0 — Feasibility" in this
document is the DETAIL of project phase 1. Each later phase gets its own
phase-gate breakdown when it opens.

> **TWO numbering systems exist in this tree — do not conflate them.**
> **Project phases** = the arc in THIS section (1-8: coop, arbiter, sandbox,
> Lua, resources, dedicated, resource infra). **Methodology phases** = the
> work phases in `docs/COOP_METHODOLOGY.md` (0-5: feasibility, engine
> archaeology, foundation, transport, replication, validation), which are
> also the "Phase 0/1/2..." headings further down THIS file — all of them
> detail of project phase 1.
>
> **Never write a numbered FORWARD reference to a project phase** ("deferred
> to Phase 7+"). The arc renumbers — it did on 2026-07-20 — and every such
> reference silently shifts by one. Name the phase instead ("deferred to the
> public-server phase"). Eight such references were repaired on 2026-07-20;
> see `[[lesson-cite-sections-not-lines-in-files-you-also-edit]]`.

**Restructured 2026-07-20 (user directive).** The prior 8-phase arc (fixed
2026-07-19) put the authority work last, as a "native standalone server"
endgame. Two measurements dissolved that shape — see
`docs/COOP_SERVER_MODEL.md`:

- the arbiter is a **child process from day one** (measured MTA `CServer.cpp`
  precedent), so there is nothing to "extract later" and no inversion EVENT;
- the arbiter's authority **grows monotonically** as each sync lane moves its
  canon into it, so the endgame is a gradient, not a milestone.

So the old phase 8 is **gone as a phase**, and the authority work moves to
**phase 2** — it is simultaneously the MTA architecture, the fix for the
authority-shaped security findings (A3/A4/A5), and what the dedicated server
needed all along. The user's ordering stands: MTA architecture first,
security findings after.

**The chain is not strictly linear.** Phase 2 gates phase 7 (dedicated) and
the authority-shaped security work. Phases 3-6 (sandbox, Lua, resources) are
independent of it and may interleave.

1. **votv-coop** — the current work: functional coop on the standalone
   substrate (this whole document).
2. **The arbiter** — **NEW, pulled forward from the old phase 8.** Move
   per-element authority into an arbiter that runs as a child process and
   never reads the engine (`docs/COOP_SYNCER_MODEL.md` = who may write what;
   `docs/COOP_SERVER_MODEL.md` = deployment + state). The host's game becomes
   an ordinary client of it and loses its privileged in-process authority
   path. Absorbs the A3/A4/A5 security findings, which are the ABSENCE of
   this architecture rather than bugs to patch.
   - **The measured work:** 32 canon-derivation read sites in 9 lanes must be
     inverted to write-only — today e.g. `meadow_db_sync` builds the
     "canonical" DB by reading the engine's widget arrays instead of
     remembering it (`COOP_SERVER_MODEL.md` §7; the figure is an order of
     magnitude from a name-shape grep, not a verified list).
   - **What is NOT work:** intent production (reading the local player) stays;
     handle validation disappears (the arbiter holds ids, not pointers);
     outcome capture stays (the engine's machines decide, the arbiter records).
   - **THE RULE:** the arbiter holds values and anchors; the engine holds only
     what has a world-dependent rate.
   - Gate: the host's game is a client of the arbiter, and the 9 lanes' canon
     lives in the arbiter's record rather than in an engine read.
3. **Sandbox mode** — support VOTV's sandbox game mode with its own rules
   (coop currently targets the story/normal mode). Produces the "rules of a
   mode" as an explicit, portable layer — the thing phase 5 will port.
4. **LuaJIT embedding** — vendor LuaJIT + bindings over the `ue_wrap`/`coop`
   APIs. The scripting SUBSTRATE only; nothing moves to Lua yet.
5. **Lua API** — the C++ core STAYS (transport, sync, identity,
   interpolation — MTA keeps its core native for a reason); the coop and
   sandbox mode RULES move to Lua as the first two reference resources.
   Not a rewrite — an API layer plus two ported rule sets.
6. **Resource system** — custom modes AND plugins as ONE mechanism (the MTA
   shape: manifest, server+client scripts, events, start/stop). A
   gamemode-resource and a utility-resource live in the same system — no
   separate "plugin API".
7. **Dedicated server** — 24/7 hosting with zero players (the gmod shape:
   host from in-game OR run dedicated). **Much smaller than it was**, because
   phase 2 delivers the binary: hosting from in-game already spawns the same
   arbiter as a child process, so "dedicated" is that binary launched by hand
   instead. What remains:
   - **Ghost host** — the architecture assumes the host is a live player with
     a pawn; a zero-player host needs its local pawn parked/excluded (not a
     slot, not an event target). THE design question of this phase.
   - **A Linux build of the arbiter** — it is engine-free C++, so this is
     ordinary cross-compilation. Launch-of-child must sit behind an
     abstraction: MTA's job objects + `CreatePipe` are Windows-only.
   - **No Wine carrier for a zero-player server** (2026-07-20, superseding
     the 2026-07-19 commitment): the empty world FREEZES, and the two
     mechanisms in play are each freeze-compatible on their own — time-linear
     accumulators are recomputed at unfreeze, gated random rolls simply do
     not fire. Wine returns ONLY if a world-rate-dependent progression turns
     up that no anchor can express (`COOP_WORLD_PROP_DIVERGENCE.md` owns that
     measurement; rule-of-three is NOT met — exactly one accumulator is
     confirmed).
   - **No redistribution** — the server package = our DLL + launcher + config
     dropped onto the OPERATOR'S OWN game copy (itch.io). No SteamCMD analog;
     we never ship game files.
   - **Native game server binary = only via the devs** (UE4.27 LinuxServer
     target needs the game project). If that ever lands, our DLL needs its own
     port (ELF loading, new AOB signatures).
   - Wine spike parameters, if ever needed: `-nullrhi`, Xvfb if required,
     `WINEDLLOVERRIDES="xinput1_3=n,b"`; expectation NOT measured (0-10% CPU
     overhead on a CPU-bound headless load with fsync/ntsync).
8. **Resource infrastructure** — client-side resource download from the
   server (no manual mod-pack installs), Lua sandboxing/security (clients
   execute untrusted server code — mandatory layer), and the server browser
   (the VPS signaling service grows into the master list).
   Trust note (2026-07-19): once servers are public, client-side cheating
   returns as a threat — MTA's answer is a client AC + server-side validation
   hooks, NOT authority architecture (MTA is client-simulated with per-element
   syncers; verified in the vendored source,
   `CUnoccupiedVehicleSync::FindPlayerCloseToVehicle` assigns the nearest
   player as an element's simulator). In our current model only the HOST is
   trusted (host == admin, acceptable); a public-server future needs its own
   AC layer decision here.

### RETIRED as a phase: "Native standalone server" (the old phase 8)

**Not a milestone — a gradient.** The old entry described an authority
INVERSION performed after phases 5-7: the server would come to hold state +
Lua rules + arbitration while clients simulate. That destination is unchanged
and still correct; what was wrong was modelling it as an EVENT.

The arbiter binary exists from phase 2, and the boundary between "the
arbiter's canon" and "the opaque save blob shipped at bootstrap" is exactly
what crosses the wire today. **Every new sync lane extends the server's
authority**, so the MTA shape arrives by accumulation. There is no rewrite
moment to schedule.

Two things from the old entry survive as standing notes:

- **NOT "rewrite VOTV in C++".** Server-side authoritative physics without the
  engine is a decade-class trap. The inversion is about rules and state
  machines, which phases 5-6 already produce as Lua resources. The accumulated
  RE corpus (`docs/events/`, `docs/signals/`, `docs/items/`,
  `COOP_RNG_AUTHORITY`, the entity-expression map) IS the rules specification.
- **Research branch:** executing the game's own cooked BP bytecode in our VM
  (we already parse it; native-call stubs are the wall).

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

- ☑ C++ toolchain + build: the mod DLL (CMake + VS2019, x64, static CRT);
       since 2026-07-19 (b122) the artifact is the Paper-pair-versioned
       `multivoid-<game>-<build>.dll` (was `votv-coop.dll`).
- ☑ Standalone proxy loader: **`xinput1_3.dll`** scans + auto-loads the
       highest-build `multivoid-*.dll` on game start (duplicate installs ->
       in-game popup); UE4SS absent. `tools/deploy-loader.ps1` installs it.
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
       `play`, `load:<slot>`, `none`, plus the per-feature
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
         rebooted, docker/WG removed, re-verified outside). Domain: `multivoid.dev` LIVE
         2026-07-19 (root proxied; `master.multivoid.dev` unproxied/grey-cloud -> the box;
         the never-delegated `votv.mp` zone retired).
         See `research/findings/network/votv-master-server-RE-and-rust-port-scope-2026-07-16.md`.
       - 2026-07-20: **Tier B TLS arcs 1-2 BUILT + LIVE.** A Let's Encrypt cert on
         `master.multivoid.dev` terminated by `tokio-rustls` inside our own bins on new
         ports 10443/10442, beside the plaintext pair for the cutover (`7aff6b73`); the
         client's master traffic moved to TLS, grammar **schemeless = secure** (`87e66bce`).
         Renewal is hardened (deploy hook restarts the services, since `LoadCredential` is a
         start-time snapshot) with the staleness alarm OFF the box (`tools/cert_check.py`).
         Arcs 1-2 stay shipped. Design of record (as-built record only, no longer the plan):
         `research/findings/network/votv-tls-tier-b-c-DESIGN-2026-07-20.md`.
       - 2026-07-20 (later, same day): **the remaining TLS arcs are ON HOLD — a threat model was
         finally written and it reordered the work.** `docs/security/` is now the source of truth
         (`README.md` = model, `TRACKER.md` = findings; commit `d95683cc`). The measurement that
         changed it: GNS is encrypted (AES-256-GCM) but **peer-UNAUTHENTICATED** — the opensource
         build defaults `IP_AllowWithoutAuth = 2` ("don't attempt authentication") and we never
         override it. So passive eavesdropping already fails, an ACTIVE attacker at the rendezvous
         does not, and the control plane is the only place peer identity can be established.
         `signalingToken` is a static shared secret every mod user holds, so **Tier C dissolves into
         peer certificates** (GNS ships a CA: certstore + certtool + `SetCertificate`; Ed25519
         sign/verify already links into our process). Arcs 3 / 3b / 4 / 5 are HELD pending the CA
         spike; the `net.master.insecure` flag was designed and then dropped — do not build it.
         Two read-only audits produced **20 OPEN findings** (all `[A]`, none personally re-verified,
         nothing fixed): among them "Locked" lobbies are not locked, signaling identity is
         self-asserted so a stranger can evict a host, `PropDestroy` trusts any client and is
         relayed before validation, and three save-transfer lanes let a hostile host kill a joining
         client with one packet. See `docs/security/TRACKER.md` for the ranked list + fix order.

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
