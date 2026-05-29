# VOTV_MP — Full Architecture Audit (2026-05-29 post-quad-ship)

**HEAD:** `56d039a` (quad-ship: C5 + B1 + B3 + B2)
**Scope:** entire `src/votv-coop/` tree across 8 dimensions
**Method:** 8 parallel `code-reviewer` agents (one per dimension) → ~75 raw findings
**Next phase:** verifier triage → fix CONFIRMED + critical PLAUSIBLE

---

## Dimension 1 — Threading / concurrency / ABBA / lifecycle

### T-1 [CRITICAL] net thread executes UFunctions without GT::Post (PropSpawn/PropDestroy)
- `src/coop/event_feed.cpp:255` PropSpawn calls `remote_prop::OnSpawn(p)` directly; `BeginDeferredActorSpawnFromClass`/`FinishSpawningActor`/`setKey`/`SetSimulatePhysics` all UFunctions
- `src/coop/event_feed.cpp:287` PropDestroy: same problem
- Asymmetry: `EntitySpawn` (line 320) and `EntityDestroy` (line 345) correctly use `GT::Post`
- **Failure:** if `event_feed::Update` is ever invoked from any non-game-thread context, engine asserts; the pattern asymmetry is a latent footgun

### T-2 [CRITICAL] OnDisconnect snapshot-then-drain UAF window (npc_sync)
- `src/coop/npc_sync.cpp:571-582` snapshots raw `Npc*` then loops to destroy actors before `DrainAll`
- A concurrent `OnEntityDestroy` GT::Post lambda can `Take(eid)` between Snapshot and the loop's actor read → dangling pointer
- **Failure:** EntityDestroy delivered during disconnect → use-after-free on `mirror->GetActor()`

### T-3 [IMPORTANT] DropPlayerElement_ atomic-cleared-before-slot-reset window
- `src/coop/players_registry.cpp:385` stores `kInvalidIdCtxPair` BEFORE `playerBySlot_[peerSlot].reset()` at line 391
- Net-thread `LocalPlayerIdentity()` in the window sees `hostElementId=0` → client gets `AssignPeerSlot(0)` → skips mirror install for that round-trip

### T-4 [IMPORTANT] EstablishMirrorForSlot idempotent branch skips atomic republish
- `src/coop/players_registry.cpp:246-256` updates Element ctx in-place but does NOT republish to `g_localPlayerIdentityAtomic`
- **Failure:** reconnect with refreshed context → all outbound WeatherState/RedSky/LightningStrike stamped with stale ctx → receiver drops via VerifySenderContext → silent weather break

### T-5 [IMPORTANT] npc_sync NpcSuppress_Interceptor SetActor / actor-id-map split window
- `src/coop/npc_sync.cpp:270` SetActor write happens outside the mutex used for `g_actorToNpcId` insert at line 289
- **Failure:** transient state where Element is registered but actor pointer is null; NpcDestroy_PRE concurrent → leaked Element

### T-6 [MODERATE] PropMirrors drain race vs queued GT::Post PropSpawn
- `src/coop/net_pump.cpp:245` ForceRelease drains `PropMirrors().DrainAll()` while a previously-queued PropSpawn GT::Post lambda may still be in the GT queue
- **Failure:** lambda runs after DrainAll → installs mirror after session torn down → leaked Registry slot persists into next session

### T-7 [MODERATE] players::Registry has no mutex; functions assume game-thread-only
- `src/coop/players_registry.cpp:246+` all accesses on `playerBySlot_` / `puppetByPeer_` are unsynchronized
- Today: game-thread only in practice — but no lock boundary to enforce; one misplaced refactor breaks it

### T-8 [MODERATE] player_handshake module-scope state has no atomic/mutex
- `src/coop/player_handshake.cpp:24` `g_localNick`, `g_remoteNickBySlot`, `g_joinSentBySlot` are plain (`g_joinSentBySlot` IS atomic; others not)
- Today: only touched from game-thread paths; same one-refactor-away risk

### T-9 [MODERATE] OnDisconnect K2_DestroyActor pre-GC validation
- `src/coop/npc_sync.cpp:577-580` calls K2_DestroyActor on snapshot pointers; `R::IsLive` check not atomic with the call
- Concurrent GC during level unload during disconnect → potential crash

### T-10 [LOW] g_drives lock pattern asymmetric with EntitySpawn (footgun)
- `src/coop/remote_prop.cpp:300` Tick/OnRelease read/write `g_drives[]` without mutex; safe today (game-thread-only) but inconsistent with GT::Post EntitySpawn pattern

---

## Dimension 2 — Element/Registry/Mirror lifecycle

### E-1 [CRITICAL] RegisterMirror does not validate eid range; peer-range eid collides with local free stack
- `src/coop/element/registry.cpp:107` accepts any eid in `[1, kMaxElements)`
- `src/coop/remote_prop.cpp:507` RegisterPropMirror also unvalidated
- **Failure:** client-sourced PropSpawn with eid in `[32768, 65536)` → host RegisterMirror writes `m_byId[X]` without popping `m_localFree` → host AllocLocalId later pops same X → silent slot overwrite → mirror dtor clears the new local element → unreachable Registry::Get(X)

### E-2 [IMPORTANT] PropSpawn/PropDestroy missing VerifySenderContext
- `src/coop/event_feed.cpp:181` (PropSpawn) and 271 (PropDestroy) — both have senderElementId but no syncContext check
- **Failure:** stale-generation PropSpawn from disconnected peer (still in GNS reliable queue) delivered after peer reconnects with fresh ctx → duplicate prop spawn or stale destroy

### E-3 [IMPORTANT] Late-joiner snapshot missing for NPCs
- `src/coop/prop_snapshot.cpp:91` only snapshots `ElementType::Prop`
- Third peer joining mid-session never sees pre-existing kerfurs etc.
- **Failure:** late joiner has permanent NPC blindness; EntityDestroy arrives later for unknown eid → silent no-op

### E-4 [IMPORTANT] Host SendEntitySpawn failure leaks host-range id
- `src/coop/npc_sync.cpp:443+449` Element installed → SendEntitySpawn may fail → no rollback path
- **Failure:** long session + reliable channel backpressure → host id exhaustion + state divergence (host has N NPCs, clients have N-k)

### E-5 [MODERATE] OnEntitySpawn unlocked dupe-eid guard window
- `src/coop/npc_sync.cpp:852` `NpcMirrors().Get()` early-out is racy; redundant duplicate spawn caught by locked Install but actor leak window remains under transient `IsLive == false`

### E-6 [MODERATE] OnDisconnect K2_DestroyActor before DrainAll fires PRE observer
- `src/coop/npc_sync.cpp:571` ordering: K2_DestroyActor first, DrainAll second
- Safe today (client g_actorToNpcId empty), but future client-side bookkeeping → re-entrant SendEntityDestroy during teardown

### E-7 [MODERATE] EstablishMirrorForSlot atomic republish (also covered as T-4)

### E-8 [MODERATE] EnsurePlayerElement_ assignment-vs-atomic-publish ordering window
- `src/coop/players_registry.cpp:361 vs 365` atomic published BEFORE playerBySlot_ assigned (narrow window; safe via Registry::Get path)

---

## Dimension 3 — Wire protocol / payload validation

### W-1 [DESIGN-NOTE] VerifySenderContext 0-on-either-side bypass
- `src/coop/event_feed.cpp:62` returns true when `senderContext==0u || mirrorCtx==0u`
- INTENT: boot/seed-race tolerance per B1 audit fix #2 — but as auditor notes, there is no time window enforcement
- **Triage as design:** if a malicious peer always sends ctx=0 they fully bypass the stale-generation defense. The 0-bypass is necessary at session boot but could be tightened with a "seed window" timer

### W-2 [IMPORTANT] PropSpawn physFlags reserved bits not rejected
- `src/coop/remote_prop.cpp:925` reads only bit 0
- Inconsistent with `ItemActivate.flags` reserved-bit rejection added in B3
- **Failure:** future bit assignment silently misinterpreted by older receivers

### W-3 [IMPORTANT] Join with payloadLen < 5 silently degrades to no-mirror
- `src/coop/player_handshake.cpp:212` — v14 prefix is required; under-sized payload sets senderElementId=0 instead of rejecting
- **Failure:** peer permanently without eid-based context guard

### W-4 [MODERATE] PropDestroy empty key (`key.len==0`) only caught one layer deeper
- `src/coop/event_feed.cpp:278` accepts; `remote_prop::OnDestroy:979` rejects
- Inconsistency: PropSpawn check at event_feed boundary, PropDestroy in remote_prop
- Layered-defense fragility

### W-5 [MODERATE] PacketHeader._pad / ReliableHeader pad bytes never validated
- `protocol.h:337` (PacketHeader), 398 (ReliableHeader 5 pad bytes)
- Inert today, but creates invisible attacker-controlled byte channel through every packet header

### W-6 [LOW] Size checks use `<` instead of `!=` for fixed-size payloads
- `src/coop/event_feed.cpp:437+` every handler — accepts oversized payloads (extra bytes silently dropped via fixed memcpy)
- Forward-compat trade-off: accepting newer-sender oversized payloads is intentional; trade-off is invisible padding channel

---

## Dimension 4 — Performance / hot paths

### P-1 [CRITICAL] grab_observer PRE observers UE_LOGI per ProcessEvent at 125 Hz
- `src/coop/grab_observer.cpp:130` (SetPhysicsLinearVelocity_PRE) + 137 (SetPhysicsAngularVelocity_PRE)
- Called from `remote_prop::DriveSetLinearVelocity`/`DriveSetAngularVelocity` in `remote_prop::Tick` (125 Hz per slot)
- Multi-peer multi-prop held → up to 500 formatted log writes/sec → OutputDebugStringW kernel serialization → frame stalls
- **Fix:** add `static std::atomic<uint64_t> sCount` + `(n <= 3 || n % 60 == 0)` throttle (existing pattern in same file)

### P-2 [CRITICAL] grab_observer::Install() no throttle countdown, plain bool latch
- `src/coop/grab_observer.cpp:190` 4× R::FindClass per tick at 125 Hz pre-resolve
- Pre-possession window (OMEGA splash, 15-30s) → ~500 GUObjectArray walks/sec → identical class to install-loop-bomb incident
- **Fix:** atomic latch + retry countdown (`s_installRetryCountdown=60` like npc_sync)

### P-3 [IMPORTANT] K2_DestroyActor PRE observer 3 unconditional mutex acquires
- `src/coop/prop_lifecycle.cpp:626` — fires for every actor destroyed in the world
- Level-unload bursts: ~500 destroys × 3 mutex acquires = 1500 lock cycles/frame
- **Fix:** cheap atomic early-out before mutex paths; only acquire mutex if `g_actorToPropElementId` likely-contains

### P-4 [IMPORTANT] item_activate::Install plain bool + no throttle
- `src/coop/item_activate.cpp:441` same pattern as P-2; mainPlayer_C resolves quickly so less severe but still wrong

### P-5 [IMPORTANT] weather_sync::Install plain bool + no throttle (per agent's verdict table)
- Same pattern; per-tick FindClass/FindFunction until all resolve

### P-6 [IMPORTANT] NpcSuppress_Interceptor wstring alloc per allowlisted spawn
- `src/coop/npc_sync.cpp:409` R::ToString(R::NameOf(actorClass)) per allowlist hit
- Level load bursts → thousands of allocs/sec
- **Fix:** name-comparison via FName equality (no wstring materialization)

### P-7 [IMPORTANT] net_pump per-tick wstring alloc for held-prop key
- `src/coop/net_pump.cpp:296` `GetInteractableKeyString(heldActor)` per tick when prop held
- **Fix:** cache key string on grab transition; reuse until release

### P-8 [MODERATE] DrainChunk unconditional UE_LOGI per tick during drain
- `src/coop/prop_snapshot.cpp:247` 20-30 logs per peer connect

### P-9 [MODERATE] event_feed::Update 4-slot loop + atomic loads at 125 Hz in steady state
- Minor but persistent

### P-10 [MODERATE] GrabObserver_Aprop_Init_POST_Body wstring alloc for CDO filter check
- `src/coop/prop_lifecycle.cpp:506` allocated regardless of whether CDO match

---

## Dimension 5 — Modularity / file size / dead code / comment rot

### M-1 [IMPORTANT] Five files over 800 LOC soft cap
| file | LOC | over by |
|---|---|---|
| `remote_prop.cpp` | 1070 | +270 |
| `npc_sync.cpp` | 1067 | +267 |
| `prop_lifecycle.cpp` | 931 | +131 |
| `protocol.h` | 896 | +96 (header exempt-ish if constants-only) |
| `session.cpp` | 871 | +71 |
| `weather_sync.cpp` | 807 | +7 |
| `item_activate.cpp` | 761 | (approaching) |

### M-2 [IMPORTANT] Dead code: GetDriveActor() in remote_prop
- `src/coop/remote_prop.cpp:952` + `remote_prop.h:87` declaration
- Comment: "Public API kept for header compatibility"
- Zero call sites — RULE 2 violation (no compat shims)

### M-3 [IMPORTANT] Duplicate helpers across coop/
- `RotatorToQuat()` defined in `remote_prop.cpp:605` AND `npc_sync.cpp:194` (identical)
- `GetWorldContext()` defined in both (identical)
- `npc_sync.cpp:184-186` explicitly comments the duplication as a known issue
- **Fix:** extract to `ue_wrap/engine.h` (math + world-context helper)

### M-4 [MODERATE] Comment rot — Finding #N / PR-X / audit-N labels
- `session.cpp:211,229,323,338,713,720,732,734,788` "Finding #1..#15"
- `npc_sync.cpp:93,603,705,879,948` audit labels
- `remote_prop.cpp` multiple "Audit C-1/C-2/I-1" labels
- `event_feed.cpp` "B3 audit fix C1/C2/I1" labels
- PR-4.12 stripped these once; new commits (B1/B2/B3) re-introduced

### M-5 [MODERATE] event_feed.cpp 4 redundant local trust-boundary float constants
- `kMaxLinVel`/`kMaxAngVel`/`kMaxCoord`/`kMaxVel` all = `1.0e6f`
- protocol.h already exports `kMaxCoord`/`kMaxSpeed`

### M-6 [MODERATE] Stale "Inc3 will" / "NEXT" comments describing shipped work
- `npc_sync.cpp:345-353` — Inc3 already shipped
- `event_feed.cpp:291,326` — "NEXT" markers for shipped switch-cases

### M-7 [LOW] dev/pos_hud.cpp `g_local` dead write
- Variable written line 109, never read; comment "keep for legacy diagnostics"

---

## Dimension 6 — Architecture / Principle 7 / MTA fidelity

### A-1 [IMPORTANT] Principle 7 leak: item_activate raw struct offsets + raw R::FindClass
- `src/coop/item_activate.cpp:152-154, 229-233, 564-576` reads AmainPlayer fields and resolves LightComponent classes
- Should be behind `ue_wrap/light_component.h` etc.
- No sdk_check coverage on these offsets

### A-2 [IMPORTANT] Principle 7 leak: net_pump::ReadLocalPose raw CMC offsets
- `src/coop/net_pump.cpp:132-141` reads ACharacter::CharacterMovementComponent and MovementMode via raw struct-offset
- Write path `ue_wrap::puppet::DriveCharacterMovement` is wrapped; read path is not — asymmetric boundary

### A-3 [IMPORTANT] Principle 7 leak: flashlight_click_sound USoundAttenuation construction
- `src/coop/flashlight_click_sound.cpp:102, 127-151` raw offset writes via sdk_profile att:: offsets
- No sdk_check assertions on att:: offsets

### A-4 [IMPORTANT] Principle 7 leak: grab_observer engine resolver cache + raw PHC offset
- `src/coop/grab_observer.cpp:193` shadows engine.h resolvers; line 76-77 hardcoded PHC.GrabbedComponent offset

### A-5 [IMPORTANT] RemotePlayer::Spawn() bypasses players::Registry
- `src/coop/remote_player.cpp:52` calls `R::FindObjectByClass(MainPlayerClass)` instead of `players::Registry::Local()`
- 3-peer scenario: first GUObjectArray hit may be a puppet → wrong skin/anchor

### A-6 [IMPORTANT] npc_sync host-side bespoke maps diverge from MirrorManager pattern
- `src/coop/npc_sync.cpp:135-137` `g_npcElements + g_actorToNpcId` hand-rolled vs client-side `MirrorManager<Npc>`
- 5-step register/drain pattern must be maintained in two shapes; next entity type (Door, Vehicle) will face the same fork

### A-7 [LOW] MTA citation missing on event_feed/player_handshake/remote_prop/npc_sync .cpp files
- RULE 2026-05-28 requires citing MTA equivalents in lifecycle/network code
- e.g. event_feed.cpp should cite `CClientGame::ProcessPacket`

---

## Dimension 7 — Net layer / disconnect / lifecycle

### N-1 [CRITICAL] PropDestroy/EntityDestroy on different lane than corresponding Spawn
- `src/coop/net/session.cpp:553` PropDestroy/EntityDestroy → Normal lane (weight 2)
- PropSpawn/EntitySpawn → Bulk lane (weight 1)
- GNS does NOT guarantee in-order delivery across lanes
- **Failure:** under backpressure (large prop fan-out), Destroy can drain before its Spawn → phantom actor never cleaned up
- **Fix:** move Destroy variants to Bulk lane (same as Spawn) for ordering

### N-2 [IMPORTANT] Connect-edge missing NPC snapshot replay
- `src/coop/net_pump.cpp:217` fires prop_snapshot + item_activate + weather replay on connect; no NPC equivalent
- Late joiner permanently blind to pre-existing NPCs (same as E-3)

### N-3 [IMPORTANT] Per-slot disconnect skips puppet unregister when puppet was never spawned
- `src/coop/net_pump.cpp:200` `if (g_puppets[slot].valid())` skip path
- Player mirror Element from `playerBySlot_[slot]` not dropped
- **Failure:** eid reallocated later to a Prop/NPC → Registry::Get(X) resolves to stale Player mirror → type-confusion

### N-4 [IMPORTANT] event_feed gates on IsSlotConnected; net_pump on IsSlotReady — split-brain
- `src/coop/event_feed.cpp:106` MaybeSendJoinToSlot fires as soon as IsSlotConnected (set in Connecting callback)
- ConfigureConnectionLanes runs in Connected callback — later
- **Failure:** first Join message rides GNS default lane (lane 0 / HIGH) instead of Normal — undermines PR-3 HOL isolation

### N-5 [IMPORTANT] OnSlotDisconnected leaves stale nickname in g_remoteNickBySlot
- `src/coop/player_handshake.cpp:182` only resets `g_joinSentBySlot`
- Next peer reusing the slot before its Join lands → stale name displayed

### N-6 [IMPORTANT] SetConnectionPollGroup skipped if hPollGroup_==0 (race vs Stop)
- `src/coop/net/session.cpp:195-209` connection accepted + slotted but never joined to PollGroup
- ReceiveMessagesOnPollGroup never drains messages from it
- Host sees "connected peer" but receives nothing → silent blackhole

### N-7 [MODERATE] connectedPeerCount() races with Connecting-state slot
- `src/coop/net/session.cpp:337` counts Connecting peers → aggregate Disconnected branch delayed
- prop_snapshot fires 1700-candidate enumeration toward half-open connection

### N-8 [LOW] Pose fan-out NetThread doesn't gate on IsSlotReady
- `src/coop/net/session.cpp:811-820` sends UnreliableNoDelay before lanes configured
- GNS silently discards for non-Connected; benign today but inconsistent

### N-9 [LOW] Stop() linger pump triggers redundant HandleConnStatusChanged
- `src/coop/net/session.cpp:436+` log spam during teardown

### N-10 [LOW] ItemActivate self-echo wastes HIGH-lane bandwidth
- Host sends to ALL connected slots including originator; event_feed drops the loopback but GNS already paid the cost

---

## Dimension 8 — RULE 1 / RULE 3 / standalone compliance

### R-1 [CRITICAL — RULE 1] catch(...) in game_thread::Pump swallows exceptions
- `src/ue_wrap/game_thread.cpp:267` `catch (...) { log + return; }`
- RULE 1 explicit ban: "catch-and-ignore" pattern
- Outer ProcessEventDetour SEH already catches AVs; this catch only masks our own C++ bugs
- **Fix:** remove the catch or narrow to specific exception types

### R-2 [IMPORTANT — RULE 1] "good enough for HUD" RTT first-peer-wins
- `src/coop/net/session.cpp:849` literal banned phrase "good enough for HUD"
- Multi-peer: if peer-0 disconnects, the `break` skips remaining peers → lastRttMs_ frozen
- **Fix:** remove `break`, iterate all slots, store min or sum/count

### R-3 [IMPORTANT — RULE 1] NPC suppress interceptor depends on unconfirmed BP codegen
- `src/coop/npc_sync.cpp:498-505` zeros return-value relying on K2Node null-check codegen NOT yet IDA-confirmed on VOTV spawner BPs
- Comment explicit: "NOT YET IDA-confirmed"
- **Failure:** any spawner BP without the null-check → `FinishSpawningActor(nullptr,…)` → crash
- **Fix:** RULE 1 path is IDA-decompile the actual VOTV spawner BPs before relying on the convention

### R-4 [MODERATE — RULE 2] AmushroomSpawner_C interim collision-restore fix retired-condition unmet
- `src/coop/remote_prop.cpp:630` documented as INTERIM with retirement criterion (Stream B-Spawners ships); spawner-suppressor allowlist does NOT include `AmushroomSpawner_C` per `garbage_sync.cpp:161-174`
- Plan documented inline (meets transitional-crutch exception); flagged for tracking

### R-5 [LOW — RULE 3 spirit] sdk_check.cpp diagnostic mentions UE4SS in user-facing string
- `src/harness/sdk_check.cpp:102` user-facing recovery text says "Run UE4SS against the current VOTV install"
- Ships in DLL; couples a runtime diagnostic to a dev-tool dependency

### R-6 [LOW — RULE 2] ENABLE_ICE forced ON without PR-6 P2P shipped
- `CMakeLists.txt:78` builds ICE/STUN/TURN code into DLL ahead of any caller

---

## Aggregated severity tally
| Severity | Count |
|---|---|
| CRITICAL | 7 (T-1, T-2, E-1, P-1, P-2, N-1, R-1) |
| IMPORTANT | ~35 |
| MODERATE | ~20 |
| LOW / DESIGN-NOTE | ~13 |

**Next step:** verifier pass to flip each into CONFIRMED / PLAUSIBLE / REFUTED. Then fix CONFIRMED + critical PLAUSIBLE.
