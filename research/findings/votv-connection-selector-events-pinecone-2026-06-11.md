# Session 7 (2026-06-11) — connection selector + sound fixes + pinecone + events catalog

Authoritative record for the 2026-06-11 work. Everything UNCOMMITTED (since commit
7722510 "[coop v43-v57]"). Post-compact READ-FIRST order: MEMORY.md top line ->
memory/project_connection_events_2026-06-11.md -> THIS doc.

================================================================
## A. CONNECTION-TYPE SELECTOR + DIRECT LOBBIES — SHIPPED + DEPLOYED
================================================================
User: host-game should let the user pick the connection type; technical users set
DIRECT + a "show in browser" flag; relay users hide via the IN-GAME scoreboard (not
at host time -- a hidden relay game is unjoinable since the master is its only
rendezvous). 3 connection types: AUTO (P2P/ICE, master-brokered, direct-or-TURN
automatic -- TURN is NOT a user choice, it's ICE's fallback), DIRECT (LanDirect UDP
listen on a forwarded port, master-LISTED), LAN (.bat/manual Direct Connect).

MASTER (tools/coop_master_server.py) -- DEPLOYED TO THE VPS (see section E):
- Lobby +conn/+direct_port slots; h_host parses conn="direct"+port, REJECTS
  out-of-range port with 400 (audit R1: no silent p2p downgrade that hands joiners
  ICE creds a LanDirect host can't speak) + REFLECTS accepted conn back; direct
  hosts get NO ICE/TURN block; h_join direct -> {conn,addr=src-ip:port}; rows +conn.
- **resolve_client_ip CRITICAL fix (audit C-1)**: took the LEFTMOST X-Forwarded-For
  (client-FORGEABLE). Behind the production xray proxy (peer=loopback -> XFF
  trusted) an attacker could forge XFF:<victim> -> master advertises a direct
  lobby's addr / [if portcheck shipped] UDP-probes the victim. FIX: take the
  RIGHTMOST XFF entry (what OUR trusted proxy appended = the real observed peer);
  X-Real-IP (proxy-overwritten) still preferred. The 1st audit PASSED this (checked
  the no-proxy model); the 2nd caught it (proxy model = production).
- **/v1/portcheck REMOVED** (both audits HIGH): a master emitting outbound UDP on
  request = reflection/amplification surface its provider can flag; per-IP RL can't
  bound aggregate UDP. Pulled this round (no UI wired). SAFE design noted for when
  the DIRECT port-check UI lands: FOLD the probe into the RL_CREATE-capped, real-
  session-tied /v1/host direct announce + a nonce the host echoes -- no standalone
  reflector.
- master_smoke.py +8 direct-lobby checks, ALL PASS (incl. R1 reject 400).

CLIENT (DLL, deployed 4 folders):
- lobby_announcer.Host(+directPort); lobby_client LobbyRow.direct + JoinInfo
  {direct,addr} (parse conn=="direct").
- session_manager HostWithSave(+directConnection,+hideFromBrowser): ONE worker
  announces P2P or DIRECT, listed=ok && !(direct&&hide); JoinLobby direct branch ->
  ParseHostPort -> LanDirect dial; g_listedState/ListedState()/HostListenPort().
- **4 audit fixes (loading-cover/inflight strands)**: F1/F2 JoinLobby shutdown-race
  -> Fail() on every exit (not bare return); F3 HostWithSave throw -> AbortHost +
  join_progress::Reset; F4 RefreshAsync inFlight_ latched-true on a parse bad_alloc
  -> try/catch always clears it.
- PICKER (host_save_picker.cpp): AUTO checkbox GONE (always listed); DIRECT "Hide
  from server browser" checkbox; plain-English hints; NO port-check button this round.
- SCOREBOARD (scoreboard.cpp): host-only "Show in server browser" checkbox ->
  session_manager::SetListed (THE place relay hosts hide once friends are in) +
  the new "Link" column (connection type per row: LAN HOST/P2P HOST on own row;
  LAN/P2P/P2P RELAY for owned links; VIA HOST for peer clients -- the GNS
  connection description names the active ICE path).
SMOKE: master_smoke green; LAN smoke PASS (0 errors). HANDS-ON PENDING: the direct-
lobby browser round-trip (host DIRECT -> appears in browser -> friend joins by the
master-handed addr) -- the autonomous smoke uses the LanDirect env path, not the
browser->master->direct flow.

================================================================
## B. SOUND FIXES
================================================================
- **PUPPET FOOTSTEPS — FIXED (smoke-verified, audio hands-on-pending)**: the native
  footstep accumulator lives in mainPlayer's BP TICK (block @70172: |v|>10, 150cm
  stride -> lib_C::step), which the puppet SUPPRESSES (puppet.cpp
  SetActorTickEnabled(false)). Kerfur is audible via anim-notify -> int_anim_events
  .step -> the SAME lib_C::step (mainPlayer_C lacks that interface; A6 to add). FIX:
  NEW coop/puppet_footsteps.h (header-only Stride: native constants, teleport guard,
  idle re-prime) ticked from RemotePlayer::ApplyToEngine after the CMC drive + NEW
  ue_wrap/votv_lib.{h,cpp} (latched lib_C CDO + step thunk; args mirror @70973). All
  sound selection/spatialization/steppedOn are the game's own. Side effects flagged:
  observer-side stats.steps++ (cosmetic), steppedOn fires on observer (correct).
- **CLUMP THROW WHOOSH — NOT A BUG** (RE-falsified): every hands-on release was an
  E-key DROP (no impulse -> the LOCAL clump also fell at the feet, silent in SP too).
  The real throw is LMB (traceThrow->throwShit ~1500cm/s, velocity applied BEFORE
  grabbing_actor clears -> our edge sampler reads it). HANDS-ON: throw a clump with
  LMB; if it's silent remotely there's a 3-tick deferred-sample contingency design,
  but the evidence says it won't be needed (RULE 1: don't fix SP-parity behavior).
- **DAY DISPLAY FIX**: the host-game save picker showed "Day 3566" -- it read the
  float Day property RAW (= the elapsed-hours accumulator). The game's own load
  screen formula (uicomp_saveSlot::upd bytecode) is `savedtime.Z + 1` (FIntVector
  {h,m,day}, Z=day at +8). save_browser.cpp now reads savedtime.Z+1.
- **DEFAULT mask**: the shipped ini + connect console no longer print the raw VPS IP
  -- net.master=DEFAULT (sentinel -> kOfficialMasterUrl in protocol.h), and
  session_manager DisplayMaster + config maskOfficial print "DEFAULT" for any
  official-host endpoint (a custom master prints verbatim). Presentation-level
  privacy only (Wireshark still sees it on connect) -- the casual surface is masked.

================================================================
## C. PINECONE SCARE — ROOT CAUSE PINNED (real fix folded into the events catalog)
================================================================
User: the RNG pinecone scare (a pinecone spawns, bounces/rolls to startle) must be
host-mirrored. The client SUPPRESSES its own pineconeSpawner (Fork C) -> sees none.

RE agent FIRST said "already syncs via the generic Aprop_C Init-POST -> PropSpawn
pipeline" (premise: the IsDescendantOfProp->return at prop_lifecycle:191 is a
CLIENT-side branch; the host falls through and broadcasts). RULE-1 "verify, don't
guess" -> built coop/dev/pinecone_probe.{h,cpp} (ini pinecone_probe, host force-
spawns prop_food_pinecone_C exactly like the spawner). EMPIRICAL VERDICT REFUTED THE
RE:
- Run 1 (+5s, DURING the connect snapshot): pinecone reached the client 30s LATE +
  at REST (physFlags=0x00) via the SNAPSHOT drain -- NOT a live broadcast; no host
  "broadcasting SPAWN" line. Scare (drop+bounce) lost.
- Run 2 (+45s, AFTER the snapshot, clean window): STILL no host broadcast, no client
  mirror. The Init-POST observer NEVER FIRED for the force-spawned pinecone.
- Diagnosis: prop_food_pinecone_C owns 0 BP functions -> dispatches Aprop_food_C::Init
  (its base OWNS a `void Init()` override) -> the one-shot subclass Init-scan MISSED
  it (prop_food_C loads late). ADDED prop_food_C to RegisterExtraKeyedInitObservers
  (prop_lifecycle.cpp -- the established late-load catch). Re-smoke: prop_food_C::Init
  now REGISTERED, but the force-spawn STILL produced no Init-POST.
- **TRUE ROOT CAUSE**: the spawner's Init is dispatched BP-INTERNALLY (the UCS calls
  Init on `self` via EX_LocalVirtualFunction -> ProcessInternal, BYPASSING our
  ProcessEvent detour -- the firefly/clump trap). So a PLAYER-DROPPED prop broadcasts
  (cross-object ProcessEvent Init dispatch) but a SPAWNER-spawned one does NOT, at
  ANY registration. The pinecone (and EVERY spawner output) is uncatchable at Init.
- The prop_food_C::Init registration is KEPT (it genuinely fixes player-DROPPED food
  props -- a separate latent late-load gap). The pinecone's REAL fix = a dedicated
  host-side spawn DETECTOR (firefly_sync shape, but for physics props) -- folded into
  the events catalog (section D) so we build ONE shared detector for the whole
  spawner family, not a bespoke pinecone patch. pinecone_probe ini -> 0 (code kept).

================================================================
## D. FULL EVENTS + TRIGGERS CATALOG — IN FLIGHT (user req)
================================================================
User: "I want FULL events, triggers catalog." VOTV has a 50+ class event system
(CXX dump): scripted event_* (arir events, fleshRain, fossilBoarWar, lightsTurnoffer,
trashPiles, vaccine, redSky, skyFalling, bedEvent...), the scheduler layer
(struct_event, trigger_eventer, grayEventController, ariralRepEventHandler,
eventSpawnerPivot, ui_eventRun, the "ticker" system), ambient+creature+ticker
spawners (pinecone/mushroom/autumnLeaf/birch/batch + ticker_*Spawner x12 + creature
spawners x16), and level trigger_* volumes (~35: sound/spawn/teleport/alarm/box/
agrav/jamDoor/breakDish...).

4 PARALLEL RE AGENTS LAUNCHED (writing section files that survive compaction):
- A -> research/findings/_events_catalog_A_scripted.md (event_* effects + output type
  + observability + sync shape + priority)
- B -> research/findings/_events_catalog_B_scheduler.md (THE KEYSTONE: does VOTV roll
  events CENTRALLY? if yes, syncing the one roll = leverage for the whole system;
  central-roll-sync vs per-event-detector verdict)
- C -> research/findings/_events_catalog_C_spawners.md (the spawner family + THE
  SHARED HOST-DETECTION MECHANISM, given Init is unobservable -- the pinecone's real
  fix)
- D -> research/findings/_events_catalog_D_triggers.md (trigger_* volumes split:
  shared-world-NEEDS-SYNC | covered-transitively | per-player-no-sync)
NEXT (post-compact): when all 4 land, SYNTHESIZE into
research/findings/votv-events-triggers-catalog-2026-06-11.md (master roadmap + a
priority/status table + the recommended shared sync architecture), then plan the
implementation (the shared host-roll/host-detect -> broadcast -> mirror detector).
The recurring pattern across ALL of it (user's principle): host rolls RNG ->
broadcasts the RESULT -> peers mirror (global rand() can't be seed-synced).

### LANDED-AGENT KEYSTONES (A scripted + C spawners done; B/D still running at compact)
**THE ARCHITECTURE (agent C, the most important finding -- REVISES the pinecone fix):**
the "spawner Init is BP-internal/unobservable" finding is true of the spawned actor's
INIT, but the SPAWN CALL itself -- `BeginDeferredActorSpawnFromClass` -- is a
GameplayStatics UFunction dispatched CROSS-OBJECT through ProcessEvent = OBSERVABLE.
**ONE POST observer on BeginDeferredActorSpawnFromClass sees EVERY spawner's output**
(reads ActorClass + ReturnValue[the actor] + SpawnTransform in one fire, like
npc_sync.cpp:575-589), routed by a class-set lookup -- no per-spawner hooks, no
per-frame GUObjectArray walk. **npc_sync ALREADY IS this detector for creatures**
(its NpcSpawn_POST hooks that exact function; kNpcAllowlist already host-detects
goreSlither/insomniac/fossilhound/antibreather -> EntitySpawn). THE PLAN: generalize
npc_sync's routing into a class-routed HostSpawnWatcher -- NPC classes -> EntitySpawn;
physics-prop classes (pinecone/mushroom/stick/garbage) -> PropSpawn-by-eid (the
trash-clump precedent; the v54 200B PropSpawnPayload already carries class+transform+
initVel) -- and flip ambient_spawner_suppress from client-cancel-ONLY to client-cancel
+ host-mirror. THE ONE EXCEPTION: ticker_fireflySpawner == the v51 firefly_sync (same
class) uses SpawnEmitterAtLocation (EX_CallMath, ProcessEvent-bypass) -> PSC-set-diff;
every ACTOR spawner uses BeginDeferred, only PARTICLE emitters need the diff.
=> THE PINECONE'S REAL FIX is this HostSpawnWatcher (route prop_food_pinecone_C ->
PropSpawn-by-eid), NOT a bespoke detector. The pinecone_probe can re-verify it.
**SCRIPTED EVENTS (agent A):** most event_* need only a host-authoritative TRIGGER
gate, NOT a new packet -- payloads already ride PropSpawn/clump-eid/NPC-sync/keyed
channels; a generic "client-suppress overlap + optional EventStart broadcast" covers
the overlap-driven set (vaccine/funnyGascans/lightsTurnoffer/passwordGuesser/
bottomHole/trashPiles), mirroring garbage_sync's trashPiles handling. HOOK THE TRIGGER
(overlap BndEvt/runTrigger/gamemode UFunction), NEVER the spawned actor's BP-internal
birth. HIGH scares: skyFallingEvent (meteor: spawns debris + PUNCHES the player -- the
one genuinely-new per-player-physical-effect), event_fossilBoarWar (NPC packs),
event_fleshRain (clump rain), event_arirStealsGun, event_bottomHoleController. DONE:
redSkyEvent (weather_redsky), trashPiles (partial, garbage_sync). A tiny gamemode
`activeEvents`@0x0E68 mirror would make tension-music/HUD feel correct on the client.
**TRIGGERS (agent D):** runTrigger(Owner,Index) -- the trigger effect verb dispatched
by BOTH the scheduler (trigger_eventer) and trigger_box volumes -- is
EX_LocalVirtualFunction -> ProcessInternal = UNOBSERVABLE (same trap); the ONLY
observable edge is the overlap BndEvt__...ComponentBeginOverlap delegate (multicast ->
ProcessEvent). So trigger effects sync host-authoritatively at the RESULT level, not the
call. TOP REC: ONE generic host-owned, Key-DEDUPED `WorldEventCue{cueType,key,transform}`
reliable -- solarBoom-boom + bigmRoar + forestMoan + breakDish + jamDoor +
chamber-appear all collapse onto it, AND it doubles as the sync home for the eventer's
(unobservable) story events. It ALSO solves the DOUBLE-FIRE hazard: trigger_box volumes
are deterministic, so if BOTH peers overlap a non-idempotent trigger (spawn/break/scare)
it fires TWICE unless host-authoritative + key-deduped. Shared-world triggers needing
NEW sync: breakDish(HIGH dish::breakServer), jamDoor(HIGH -> fold `jammed` into the door
channel), spawnProp(HIGH -> verify rides prop_lifecycle), solarBoom(HIGH 2D boom; lights
covered transitively), bigmRoar(HIGH global roar), forestMoan(MED, overlap-observable),
wispSwarm/spawnFollowingArir/bloodSkeleton/arirEgg(MED creature->NPC pipeline),
alarm/eventt_arirShip(MED alarmLamp_C[] -- no channel yet), vehtp(MED -> atv_sync).
COVERED TRANSITIVELY (verify, don't rebuild): lights/prop/door/atv/weather. PER-PLAYER
NO-SYNC: achievement/notif/agrav/ambientSound/teleporter(self, pose carries it)/bedEvent
/fakeLmaos/forceObject. None of the 33 trigger classes synced today (greenfield).

THE EMERGING 3-MECHANISM ARCHITECTURE (from A+C+D; B pending):
  (1) HostSpawnWatcher -- ONE POST observer on BeginDeferredActorSpawnFromClass,
      class-routed (NPC->EntitySpawn, physics-prop->PropSpawn-by-eid). Covers ALL
      spawner outputs (the pinecone, mushrooms, creatures). Generalizes npc_sync.
  (2) WorldEventCue -- ONE host-owned key-deduped reliable for trigger/event RESULT
      effects whose dispatch is unobservable (sounds, dish-break, door-jam, story
      events). Solves double-fire on deterministic box volumes.
  (3) Trigger-gate (agent A) -- client-suppress overlap + optional EventStart for the
      overlap-driven event_* set whose payloads already ride existing pipelines.
  + a gamemode activeEvents@0x0E68 mirror (tension music/HUD).
The two ProcessEvent-observable seams the whole system hinges on: BeginDeferredActor
SpawnFromClass (spawns) + ComponentBeginOverlap BndEvt (trigger volumes). runTrigger,
Init, SpawnEmitter are all BP-internal/CallMath -> NOT seams.
Section files DONE: _events_catalog_{A_scripted,C_spawners,D_triggers}.md.
STILL WRITING: _B_scheduler.md (does a CENTRAL event-roll exist? -> the whole-system
leverage: sync the one roll vs the 3 mechanisms above. Synthesize post-compact.)

================================================================
## E. VPS MASTER DEPLOY (out-of-band state change -- a session DID this)
================================================================
The updated coop_master_server.py is LIVE on the VPS (87.121.218.33:10001,
coop-master.service, /opt/coop-master/master_server.py). Deployed via tools/vps.py
(paramiko, key auth; creds in reference_master_server_vps.md LOCAL-ONLY). Verified:
byte-identical to the master_smoke-green artifact (29154 B), systemctl active,
healthz green, log "master listening on 0.0.0.0:10001", stable post-restart.
ROLLBACK: /opt/coop-master/master_server.py.bak-pre-direct (the pre-direct version).
Reliability: Restart=always, MemoryMax=128MB (a leak tripwire, not a data need: 1000
lobbies = ~1MB measured; realistic full load ~20-30MB). The C-1 XFF fix is the
security-critical reason this deploy mattered.

================================================================
## F. STATE / PENDING (post-compact pickup)
================================================================
- UNCOMMITTED: 30 files since 7722510. Includes the connection selector (master +
  client), footsteps (puppet_footsteps + votv_lib), day fix, DEFAULT mask, scoreboard
  Link column + hide toggle, the prop_food_C Init fix, pinecone_probe (dev tool).
  Commit ONLY when the user asks.
- perf_probe=1 + perf_probe_selftime=1 STILL ON in the game inis -> turn OFF before
  any ship build.
- HANDS-ON PENDING: direct-lobby browser round-trip; footsteps audio; LMB clump
  whoosh; day-display visual confirm.
- NEXT WORK: synthesize the 4 catalog sections -> master roadmap -> implement the
  shared spawner/event detector (which also delivers the pinecone scare). Then the
  per-event syncs by priority from the catalog.
- The VPS master is updated -- do NOT re-deploy unless the .py changes again; the
  rollback .bak exists.
