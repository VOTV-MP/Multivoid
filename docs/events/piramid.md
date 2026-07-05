# piramid — the walking three-leg pyramid (Rozital tripod)   (STATUS: AS-BUILT v100 2026-07-05; walk-at-true-scale V live; facing 0s-FACING2 + true mid-join pending)

The devs'-gauntlet acceptance case (docs/DEVS_GAUNTLET.md; docs/COOP_EVENT_JOIN.md Phase 1
names "pyramid mid-join" as the acceptance test). Ground truth:
`research/findings/votv-piramid2-RE-2026-07-04.md` (bytecode, 113 exports decoded) + the
wiki sweep (voicesofthevoid.wiki.gg /Events/Story_Mode + eternitydev.wiki.gg /Pyramid_Tripod,
2026-07-04).

## 1. Native behavior (ground truth)

- **Trigger chain** [bytecode + wiki]: scheduler row `piramid` (story day 30/31, version-
  dependent; `piramid_sig` the day before is a forceObjects signal append only — no actor)
  ARMS the level-placed `TB_event_piramid`; a player standing in the Signal Lab after 18:00
  fires `piramidSpawner_2.runTrigger`, which: spawns **4 `killerwisp_C`** at random 40-69 km
  ring positions, **destroys the save's `wispSpawner`** (nightly yellow-wisp spawning ends
  permanently — known native latch quirk: may take a save/reload to stick), then
  BeginDeferred-spawns **`piramid2_C` at scale (2,2,2)** with `spawner` ExposeOnSpawn.
- **Registry** [bytecode]: `setEvent(true)` at BeginPlay, `setEvent(false)` at Destroyed —
  the Phase 0 probe sees `BEGIN class=piramid2_C`. Meta-paranoia (no pause/save/sleep)
  holds for the whole flight; coop already enforces both (pause_guard + disableSave hold).
- **Movement** [bytecode]: NO AI controller, NO navmesh, NO root motion. Per-tick
  `K2_SetActorLocation`: terrain-following hover (downward sphere trace, `10000*scale`
  above ground, FInterp Z@5 XY@15) + forward march along `movementVector` at
  `speed(1500)*scaleX*dt*isWalking`; yaw RInterpTo scaled by the 10 s `mov` timeline.
  Brain = five looping timers (seeWisps 1 s, checkIfReached 1 s, randLoc 5 s wander via
  RandomPointInBoundingBox, changeLook 1 s, ping 30 s). **The path is HOST-RANDOM** (wander
  + live-wisp chase) — a client-side replay walks a DIFFERENT path. Legs are 100%
  procedural in the AnimBP (CCDIK from world motion): a pose-mirrored copy animates
  correctly for free, footstep notifies (`step(0|1|2)`) fire from the same motion.
- **Actions** [bytecode + wiki]: hunts killerwisps (scan <=100 km front hemisphere inside
  the +-70 km origin box) -> walks to one -> `gather` montage; notifies `begin/gather/del`
  activate the two `eff_piramidSucc` beams (per-tick beam params tie `p_L/p_R` to the
  wisp's `eff_L/eff_R/center`) and finally `wispTarget.K2_DestroyActor()`. Every 30 s:
  ping sound + particle + map-wide `piramidPingShake` (outer radius 500 000). Foot plants:
  4-ring distance sounds + stomp particles + `piramidStepShake` (can trip the base alarm).
  Never damages the player or terrain; no drops. Halloween variant (gamemode==5): pumpkin,
  gore material, orange lights, spooky laugh.
- **End** [bytecode]: when `gamemode.killerWisps` is empty it walks to `spawner.walkFinish`
  (SW corner per wiki) -> `progressAdvancement('piramid')` -> self-destroy. Wiki-documented
  quirks: may miss wisps and leave anyway; "chance it may get stuck".

## 2. Sync-axis table

| axis | class | carried by |
|---|---|---|
| actor transform (walk path) | (b) host-random | WorldActor pose stream (allowlist piramid2_C) |
| spawn SCALE (the event spawns at 2,2,2) | (b) host spawn transform | **v99 (2026-07-05 `419e3894`)**: EntitySpawnPayload +Scale3D, applied into the mirror's spawn transform. MISSING until then — the unit-scale mirror rendered half-size + floated at the host's scale-2 hover Z («далеко и маленькая», user live). [V live: «пирамида идёт»] |
| body FACING (visible heading) | (b) host state — the actor root NEVER yaws ([WA-TRACE] yaw=0.0 all walk); heading = movementVector/Arrow components' world rotation, body oriented by the AnimBP | **v100 (2026-07-05 `75e5ab10`)**: WorldActorPoseSnapshot.auxYaw streams the host component yaw; client writes both components post-apply. This axis was MISSING from this table until the user saw it live; the interim delta-derived heading was LIVE-REFUTED (native heading keeps easing toward the target up to 10 s AFTER motion stops). [AS-BUILT; 0s-FACING2 pending] |
| isWalking / gait speed | (b) host state | pose stream delivers position; walking-anim state derives from motion (procedural legs) — nothing extra |
| legs / footstep sounds / stomp particles / step shakes | (a) derived from motion | AnimBP notifies fire on the mirror from the pose-driven motion — free |
| terrain-hover Z | (a) derived | mirror runs the same trace... NO — mirror AI is suppressed (below); Z rides the pose stream verbatim |
| gather (arm-extend + beams + wisp consume) | (b) host event | new relay: `PyramidGather{pyramidEid, wispEid}` -> mirror plays the `gather` montage; beams follow via notifies |
| consumed wisp's death | (b) host event | npc/kwisp lane destroy (authoritative) — the mirror's `del`-notify destroy is SUPPRESSED (would double-kill) |
| the 4 spawned killerwisps | (b) host spawns | npc lane (killerwisp_C allowlisted — kwisp choreography lane) |
| 30 s ping (sound+particle+shake) | (a) timer-driven cosmetic | mirror's own ping timer stays LIVE (pure cosmetic, no state writes) — per-viewer timing skew accepted |
| ping/step camera shakes | (c) per-viewer | local CameraShakeSource on each peer's copy |
| wispSpawner destroy + no-more-wisps flag | (b) host world flip | host-authoritative; client scheduler already suppressed (allEvents=0), client NPC spawns already interceptor-suppressed |
| story flag progressAdvancement('piramid') | (b) host save | rides passEvents/save blob as today |
| radar dot / Halloween skin | (a) derived from actor presence + gamemode | free on the mirror |

## 3. Coop design (the pyramid mirror lane) — AS-BUILT (v97, 2026-07-04 late)

Built in `coop/creatures/piramid_sync.{h,cpp}` + extensions to `world_actor_sync`,
`npc_world_enum`, `event_fire_sync`; wire v97 `ReliableKind::PyramidGather = 85`
(8 B: pyramidEid u32 + wispEid u32). Four discoveries changed the design during the
live-run loop — each proven by a failing autonomous run, then fixed and re-proven:

- **The spawner is EX_CallMath-invisible** [V: live force run 22:49 — pyramid BeginPlay hit
  the registry probe while BOTH PE interceptors (npc + WA) caught zero of runTrigger's 5
  spawns]. Neither the killerwisps NOR the pyramid ever reached the BeginDeferred
  interceptors. Fix: `piramidSpawner_C` joined `npc_world_enum`'s source-gated Func-thunk
  EX-catch (the wisp-swarm seam — ONE owner for the EX-spawn axis); its drain gained a
  WorldActor branch feeding the new `world_actor_sync::HostEnrollExSpawn()` (same end state
  as interceptor+POST: element + bind + reverse-map + WorldActorSpawn broadcast).
- **The END self-destroy is PE-invisible too** (SELF `K2_DestroyActor` — the npc_sync:841
  class), so the WA destroy PRE observer never fires for it. Fix (generic, in the axis
  owner): pose-walk DEAD-RETIRE in `world_actor_sync::TickPoseStream` — a BOUND actor that
  reads dead retires + broadcasts WorldActorDestroy. This closed a LATENT lifecycle leak for
  ALL 17 allowlisted WA classes, not just the pyramid. [V: host dead-retire line + client
  mirror K2 + registry END elapsed=211s, run 23:19]
- **Mirror brain suppression, as built**: PRE-cancel exactly the three STATE-WRITING timer
  handlers (`seeWisps`/`checkIfReached`/`randLoc` — every `walkTo` caller). `ReceiveTick`
  stays ALIVE (deliberate divergence from the earlier tick-off sketch: the gather-beam
  per-tick params, head look-at and hover-Z live there and are pure derivations of mirrored
  state; march/turn are structurally zero — isWalking/multiplyWalk can never latch with the
  walkTo callers cancelled). `changeLook` + the 30 s ping stay alive (per-viewer cosmetics
  per section 5). The lane re-enables the mirror's actor tick (world_actor parks generic
  mirrors tick-off) and unstages any pre-arm brain writes. Hook table: kMaxInterceptors
  24 -> 40 (census stood 23/24; the 3 brain slots hit table-FULL on a live run).
- **Gather replay pre-gate must sit ABOVE the native arrive radius**: both actors FREEZE at
  the host commit, so the mirrors converge to the host's frozen distance — <= 10000 by
  construction but often only just (proven: replay OK at dist=9495 attempts=1 after a 9000
  pre-gate starved to deadline). As built: pre-gate 10500, native re-check is the arbiter,
  branch-not-taken unstages + retries until a 5 s deadline (a missed gather degrades to the
  npc-lane wisp destroy — beams only).

Original design points (all shipped as described unless noted above):

1. **Dupe-matrix flip**: move `piramid` from `kReplayRows` to `kNoReplayRows`
   ("pyramid mirror lane") in event_fire_sync.cpp. TODAY'S BUG (RE finding, flagged gap):
   the client replay arms the CLIENT's own TB box, so a client walking into the Signal Lab
   spawns a client-local pyramid + 4 unmirrored client wisps — the exact
   "players saw different events" disease. The arrival must arrive by MIRROR, not replay.
   (Host-side trigger semantics stay native: any body in the host's Signal Lab — including
   a client's puppet — overlapping the host's armed box fires the real event. Verify the
   box's overlap filter accepts the puppet; if it filters to the possessed pawn, relay the
   client's lab presence as a host-side overlap dispatch, the eventforce shape.)
2. **Pose mirror**: add `piramid2_C` to the world_actor_sync allowlist (currently only
   `piramidSubpawn_C` = the sandbox-menu `piramidTest_C` variant, NOT this). Host streams
   the transform; client mirror interps it.
3. **Mirror AI suppression** (the hard part; RE section 7): the client mirror is OUR spawn
   but its BeginPlay starts the five timers + per-tick SetActorLocation — it would fight
   the pose drive and double-destroy mirrored wisps. Suppress ON THE MIRROR ONLY:
   PRE-cancel its `ReceiveTick` + the five timer entrypoints per-self (the npc-lane
   interceptor shape), keep BeginPlay itself (setEvent(true) runs -> the client's own
   activeEvents registry stays honest, closing the counter-parity asymmetry of
   COOP_EVENT_JOIN.md 3.3 for this event). `del`-notify's wisp destroy is part of the
   suppressed graph; the wisp dies via the npc lane instead.
4. **Gather relay**: host observes the gather commit (montage start / wispTarget set — the
   exact VISIBLE seam is in the RE doc; if the montage call is EX-dispatch-invisible,
   observe `wispTarget` by 1 Hz poll on the host actor) -> reliable
   `PyramidGather{pyramidEid, wispEid}` -> client plays the montage on the mirror; beams +
   footwork follow from notifies.
5. **Late-join answer** (extends COOP_EVENT_JOIN.md 3.4): lane-owned — the WorldActor
   join snapshot delivers the in-flight pyramid at its current transform; killerwisps
   arrive via the npc join snapshot; registry parity comes from the mirror's own
   BeginPlay setEvent. **This answer was FALSE until 2026-07-05 `ff338d87` for the
   canonical case (pyramid spawned while the host was ALONE): the enroll seams were
   connected()-gated, so nothing was tracked and the join snapshots were empty — the
   user's live 0s repro ("joiner sees nothing"). Fixed as a class (hosting-gated
   tracking, COOP_EVENT_JOIN 3.4 rule); re-test pending (runbook 0s-FIX).** The v98 EventSnapshot carries 'piramid' to the joiner but the
   verdict table skips it as lane-owned — the correct answer, not a gap. No
   replay-from-t0 (host-random path makes replay divergent by construction). v98 also
   closed the gather sub-gap: `piramid_sync::QueueConnectBroadcastForSlot` re-sends an
   in-flight gather commit ToSlot at the join edge (reads live `gathering`/`wispTarget`
   off the host pyramid; wisp already consumed = skip, the joiner misses only the beam
   tail). AS-BUILT 2026-07-05, code-verified only — the ~10 s join-during-gather window
   has no autonomous repro.

## 4. Caveats / known quirks

- Native: wisp-latch bug (spawning may persist until reload), can miss wisps, can get
  stuck. A stuck HOST pyramid is a stuck event for everyone — the registry probe's >5 min
  same-sender BEGIN with no END is the observable; no coop workaround (native behavior).
- `piramidTest_C`/`piramidSubpawn_C` (sandbox menu) are a DIFFERENT implementation
  (ACharacter); nothing here applies to them.
- The no-spawner fallback (`spawner==null`) walks toward player 0 and can never finish —
  only reachable if something spawns piramid2_C outside the native chain; our mirror spawn
  must therefore ALSO suppress the brain, not rely on the fallback being sane.

## 5. Verification

- **Autonomous e2e PASS [V: 2026-07-04 23:19 run, host+client logs]** via
  `autotest_piramidforce` (env `VOTVCOOP_RUN_PIRAMIDFORCE_TEST=1`, mp.py smoke --duration
  330; night sun forced so the bait wisps survive; wisps re-pinned onto a 150 m ring around
  the walking pyramid every 5 s): host ex-enroll -> registry `BEGIN piramid2_C n=1`
  -> client mirror materialized -> brain armed + tick restored -> gather relay -> client
  `replay OK (dist=9495 attempts=1)` -> wisp death via npc lane -> END dead-retire ->
  registry `END elapsed=211s`. **CAVEAT (learned 2026-07-05): this e2e was GEOMETRY-BLIND
  to the pose stream** — the wisps were re-pinned around the HOST pyramid, which marches
  only ~50 m to arrive, so `dist=9495 attempts=1` was equally consistent with a FROZEN
  client mirror. The e2e proves spawn/gather/death lanes, NOT client-side motion.
- Registry probe on the pyramid: PROVEN (BEGIN/END both live runs).
- **Mid-join hands-on attempt 1 FAILED (2026-07-05 ~11:00, user)**: joiner saw NOTHING —
  connection-gated host tracking, root-fixed as a class same hour (`ff338d87`).
- **Spawn delivery + pose chain [V live 2026-07-05 11:25 run]**: joiner (join-before-event)
  materialized the pyramid; `[WA-TRACE]` (permanent 5-hop 1 Hz telemetry, `c98c6543`)
  showed host-read/serialize/store/apply/drive all moving, host-vs-client delta ~100 units.
  The visible failure was NOT the pose: **missing spawn SCALE** (v99 `419e3894`) — walk
  then confirmed live by the user («пирамида идёт»).
- **Facing arc (2026-07-05)**: delta-derived heading (`eb8a1a40`) worked roughly but was
  LIVE-REFUTED («сбивается, смотрит немного не туда») → v100 auxYaw streams the true host
  component heading (`75e5ab10`). **Pending: 0s-FACING2** (turns synchronous incl. the
  standing ease toward the wisp; log assert `[WA-TRACE ... aux=]` equal both ends).
- **Still pending (hands-on)**: the TRUE mid-join case (join while the pyramid is already
  walking — the devs'-gauntlet acceptance; 11:25 was join-before-event); gather
  beams/montage visual on both screens; a join DURING a gather (v98 re-send, code-verified
  only); the native (non-forced) trigger via a client puppet in the armed TB box.
