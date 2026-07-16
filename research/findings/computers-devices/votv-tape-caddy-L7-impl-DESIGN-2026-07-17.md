# L7 — TAPE CADDY + DAILY TASK impl design (2026-07-17, /qf 9 rounds "that holds")

Status: **DESIGN OF RECORD, build-ready** (converged round 9/15 with a genuine critic hold).
Arch frame: `votv-signal-chain-all-units-DESIGN-2026-07-16.md` §L7 (this doc REVISES it — see
"Arch deviations"). Fact base: `votv-tape-caddy-daily-task-RE-2026-07-16.md` + this pass's
measurements (thread: scratchpad `qf_thread.md`, L7 topic). Proto 113 -> **114**.

## The two entities

- **wallunit_tapes_2** (ONE level-baked instance): ReceiveTick 1 Hz `reel{Big,Small} += dt/speed`
  FClamp(0,100), gated `active`; `upd()` = mesh visibility + `SetActorTickEnabled(active)`;
  `reelBig @0x288` / `reelSmall @0x28C` DOUBLE as slot state (**-1.0 = empty**); `speed` cross-
  written by desk physMods (10/30; L8 unbuilt). NO RNG; NO >=100 one-shot [measured this pass:
  the uber's only comparisons are >=0 occupancy; 100.0f appears only in the two FClamps].
- **saveSlot.taskNew** @0xCA8 (0x48): `active`, `sigRequired`/`sigCompleted`/`requiredDishes`
  TArray<int32>, `rewardSig`/`rewardSat`, `reel_big`/`reel_small`. Writers [GUID census across
  ALL dumped assets, negative-grep validated]: `daynightCycle.createNewTask` +
  `lib.processTask` + `droneSellLocation.sell` — **all three HOST-ONLY live** (client
  daynightCycle frozen at TimeScale=0 => midnight cascade unreachable, time_sync.cpp:33; client
  drone ReceiveTick suppressed => arrival/sell never runs). Readers: `getSigObj`,
  `kerfurOmega.findTask` (both synchronous). `sigRequired` = fixed MakeArray +
  Array_Set(processLvl, amount); `requiredDishes` = Shuffle(gamemode.dishs) -> dish indices.
  taskNew.reel_* is the task's BEST-SENT pair (few writes/day) — NOT the accruing wallunit pair.

## Key measured facts this design stands on (new this pass)

1. wallunit `Active` is ALREADY wire-synced — the symmetric ApplianceState toggle family
   (appliance.cpp:44). The toggle carries NO new L7 event.
2. PropSpawnPayload has NO float channel, and a CLIENT's fresh Aprop_C spawn does NOT broadcast
   (prop_lifecycle:210; host_spawn_watcher is host-only :241) => a client-ejected reel is a
   LOCAL-ONLY GHOST today. The arch-L7 premise "eject rides the generic lanes" is half-true
   (host yes / client no).
3. The F2 lane (prop_drop_intent) is the proven client->host prop-authoring shape:
   client FinishSpawn post-hook -> Tick drain -> intent -> `HostSpawnPlacedProp` (setKey +
   SP-parity physFlags incl. SLEEP, pre-Finish; dup-guard by key) -> watcher broadcasts ->
   client adopts-by-key. A plain drop/throw of a HELD prop fires NO FinishSpawn (round-1
   reframe: the crossing must happen at the EJECT seam, not at place).
4. The held-prop pose stream reads `grabbing_actor` continuously (local_streams.cpp:234-254);
   unknown-key poses are a clean WARN drop (remote_prop.cpp:346-355); the receiver disables
   physics while pose-driving (:402).
5. The destroy seam is SYMMETRIC — no mirror filter (only wire-echo/child-actor/episode gates;
   prop_destroy_seam.cpp:55-100) => every throw-in overlap permutation converges.
6. Engine TArray rebuild is a proven capability: ue_wrap/inventory.cpp:236-240 via
   R::EngineAlloc (GMalloc vtable, IDA-verified).
7. prop_reel.Progress is written EXACTLY ONCE per prop lifetime (pre-Finish at birth); its only
   consumers are `lookAt` (lazy hover) + `loadData` => a mirror-birth apply may run POST-Finish.
8. prop_snapshot builds the SAME PropSpawnPayload (BuildPropSpawnPayload_ :246) => one shared
   scalar fill helper covers live express + join-window snapshot.
9. Rewards: processTask -> addPoints -> saveSlot.points rides balance_sync [V]; the reward /
   requirements mail rides email_sync. Composition of existing lanes.

## Design (v7)

Files: `coop/interactables/tape_caddy_sync.{h,cpp}` (caddy lane) +
`coop/world/daily_task_sync.{h,cpp}` (taskNew mirror) + `ue_wrap/tape_caddy.{h,cpp}`
(wallunit singleton cache IsLiveByIndex + reel field R/W + upd + reel-prop Progress R/W).

### D1 — SLOT axis: ReelSlot (ReliableKind=102, Lane::Normal, relay row YES)
Presser-authored. Both peers poll {reelBig, reelSmall} at 4 Hz for -1.0-sentinel change-edges
(ONE invariant detector for all writers: playerUsedOn, throw-in overlap, future):
`-1->P` = INSERT{reel, P}; `P->-1` = EJECT{reel}. Apply GT-atomic + PRIME (fields + poll
baselines written in one GT task; the sender primes its baseline at send — poll echo dead by
construction). INSERT apply: slot empty -> write P + upd(); occupied -> **HOST keeps its own
value + WARN** (authority tiebreak; its corrector re-asserts <=1 s), CLIENT write-if-differs
(>0.01) + WARN. EJECT apply: slot := -1 + upd().

### D2 — CORRECTOR axis: ReelPose (MsgType=40, unreliable, HOST-only)
1 Hz while ANY slot occupied (active or not — heals inactive divergence), newest-wins seq
stamp. {reelBig, reelSmall}. Client applies per-channel ONLY if `local != -1 AND wire != -1`
(sentinel transitions live exclusively on the reliable lane) + an IsRecent window after a local
insert. **Park-doctrine deviation (WRITTEN):** the client accrual is NOT parked — `upd()`
re-applies SetActorTickEnabled(active) at every native verb + both our applies + the
appliance-lane apply, so a park is un-holdable without a site-list or a verb hook; the accrual
is RNG-free / deterministic / clamped, so host-owns-progression is delivered by the CORRECTOR.
Sawtooth <= 1 native increment (<=0.1%/s divergence pre-L8).

### D3 — TASK axis: TaskNewState (ReliableKind=103, Lane::Normal, no relay)
HOST-authored ~1 Hz change-hash poll of saveSlot.taskNew (fires a few times per game-day).
Payload: {active u8; counts u8 x3; sigRequired i16[24]; sigCompleted i16[24]; requiredDishes
i16[32]; rewardSig i32; rewardSat i32; reel_big f32; reel_small f32} ~180 B, static_assert
<= 228. SEND-side clamp + WARN (never silent truncation); receiver count guard. Client apply
GT-atomic: scalars raw; arrays in-place when count==Num, else EngineAlloc + copy + EngineFree +
{ptr,num,max} (fact 6; readers censused synchronous — no cross-frame array views exist).

### D4 — PROP-BIRTH axis (the round-1 reframe)
a. **PropSpawnPayload += {float savedScalar; uint8_t hasSavedScalar}** — identity-at-birth for
   per-prop save-scalar state (the reel's Progress IS struct_save.mFloat[0]). ONE shared
   per-class reader `ue_wrap::prop::ReadSavedScalarForClass` consulted by BOTH fill sites
   (host live express + prop_snapshot builder); ONE apply site (mirror birth, post-Finish).
   Kills: the eid-resolve race, any host progress-park map, the 3rd-peer wrong-progress insert,
   and the join-window gap.
b. **CLIENT eject crossing:** a branch in F2's existing Tick drain (explicit reel-class
   whitelist + EMPTY key + unparked = an eject birth) -> mint synth key + setKey + read
   Progress -> **ReelEjectIntent (ReliableKind=104, Lane::Bulk, client->host, no relay)**
   {className, key, progress, transform, physFlags|SLEEP} -> HostSpawnPlacedProp authors the
   spawn (dup-guard; Progress written via the intent; born ASLEEP so it never free-falls; the
   client's held stream takes over <=100 ms) -> the watcher broadcasts PropSpawn(savedScalar)
   -> the ejecting client adopts-by-key. Bulk in-lane order: a fast pocket-DESTROY(key) cannot
   overtake the intent.
c. Downstream client paths all reduce to existing lanes: pocket -> NoteClientKeyedDestroy fires
   natively (the reel is keyed+registered now) -> later place = pure F2; drop/throw -> generic
   pose/release; re-insert -> ReelSlot INSERT + keyed destroy; throw-in on any peer converges
   (fact 5 + equal birth progress).

### D5 — JOIN / TEARDOWN / INSTALL
JOIN: the save transfer seeds the wallunit keyed row + taskNew + reel props (Progress rides
struct_save.mFloat[0]); join-window ejects ride prop_snapshot's savedScalar; prime-on-first-
sight baselines; NO connect-replay rows. TEARDOWN: OnDisconnect resets poll baselines, ReelPose
seq, IsRecent stamps, the singleton cache, the daily_task hash baseline; F2 clears its own
pending+park (key strings only — no suppression exists, no loans); wired as a row in the
subsystems.cpp:336-352 fanout (all session-end paths). INSTALL: latched; Tick gated
connected+world-ready; singleton cached with IsLiveByIndex; the miss path throttled ~1 s with a
cheap class filter (worst case <=1 walk/s transient).

## Arch deviations (vs votv-signal-chain-all-units-DESIGN-2026-07-16.md §L7)

1. The "active record toggle -> ReelSlot event" line is RETIRED — `Active` already rides the
   symmetric ApplianceState lane (fact 1); a second carrier would double-express one axis.
2. "The HOST ADOPTS the inserted progress and from then owns the accrual" is refined: BOTH
   peers keep accruing natively (the park is un-holdable, D2) and the host's ReelPose corrector
   owns convergence. Same authority intent, transport changed.
3. "Eject spawns the reel prop via the generic lanes" was measured HALF-TRUE — the client half
   needed D4b (the eject-seam intent) and the generic lane needed the savedScalar birth channel.

## Named residuals (acceptance record)

- **L7-R1**: client-loaded sack contents (reels AND signal disks) cannot grade — the sack
  container inventory is unsynced. Pre-existing; NOT widened (L7 makes the reel prop itself
  cross so the host can physically receive it). Retirement hook = the sack-contents lane
  (votv-drone-sack design 2026-07-15, unbuilt).
- **L7-R2**: pre-L8 `speed` divergence + the client-eject progress lag (the intent carries the
  client's unit value, <= 1 sawtooth behind host truth) — both corrector-bounded.
- **L7-R3**: 2-4 unknown-key PropPose WARN lines once per client eject (the intent->spawn
  window); bounded burst, not sustained.
- **L7-R4**: same-slot cross-peer EJECT race (~300 ms window; two players at one slot) ->
  duplicate reel with EQUAL progress. Grading-unexploitable (sell = FMax; reward needs both>0).
  Closing it would remote-destroy a natively-granted in-hand reel — worse than the disease.
- **L7-R5**: simultaneous same-slot INSERT -> the losing reel's content is lost,
  host-deterministically (window = wire delay; the native gate refuses insert-on-occupied).
- **Process flag**: v112+v113 hands-on still pending; building v114 stacks a THIRD unverified
  proto layer. Recommendation: run the existing combined runbook first, else rely on per-lane
  log attribution.

## Wiring checklist (build order)

1. protocol.h: kProtocolVersion 114 + changelog; MsgType::ReelPose=40; ReliableKind
   ReelSlot=102 / TaskNewState=103 / ReelEjectIntent=104; PropSpawnPayload savedScalar field
   (+ static_asserts); payload structs.
2. ue_wrap/tape_caddy.{h,cpp} + ue_wrap::prop::ReadSavedScalarForClass.
3. savedScalar plumbing: THREE PropSpawnPayload fills (host live express prop_lifecycle +
   prop_snapshot builder + prop_container_extract) + the PropDropIntentPayload fill in the
   F2 drain (BOTH intent kinds -- the parked place must carry Progress too, or a client
   pocket->place respawns a blank tape; correctness-audit CRITICAL 1, fixed as-built) +
   ONE mirror-birth apply (prop_fresh_spawn) + the HostSpawnPlacedProp write.
4. prop_drop_intent: the reel-class eject branch (+ ReelEjectIntent send) + host
   OnReelEjectIntent (reuse HostSpawnPlacedProp with the progress write).
5. coop/interactables/tape_caddy_sync + coop/world/daily_task_sync (lanes per D1-D3, D5).
6. Router: event_dispatch_state (ReelSlot/TaskNewState) + event_dispatch_intent
   (ReelEjectIntent); session_lanes.h LaneForKind + relay row (ReelSlot); session.cpp ReelPose
   store/drain channel; subsystems Install/Tick/OnDisconnect rows; CMakeLists.
