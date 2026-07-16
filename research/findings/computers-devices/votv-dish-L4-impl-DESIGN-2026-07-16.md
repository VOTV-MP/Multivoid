# L4 dishes — implementation-level DESIGN (2026-07-16, /qf 10 rounds -> "that holds")

Status: **DESIGN converged, build follows this doc.** Produced by a 10-round `/qf` impl-design pass
(user-directed; genuine critic hold at round 10/15 after R9 produced zero structural deltas).
Thread: scratchpad `qf_thread.md` (archived on next topic). Fact bases:
`votv-dish-impl-RE-2026-07-16.md` (canonical, [MEASURED]), `votv-dish-rotation-RE-2026-07-16.md`,
arch frame = `votv-signal-chain-all-units-DESIGN-2026-07-16.md` §L4 (this doc REVISES two of its
lines — see "Arch deviations" below). Session measurements cited inline.

## The shape (one paragraph)

Client dish sim is PARKED (two tickers killed with paired restores; the client's own unpreventable
ping slews are KILLED-WITH-CLEANUP by the catch detector itself); the HOST streams per-dish poses
(`DishPose`, unreliable, movers-only 4 Hz + settle-tail) which ONE applier mirrors onto RELATIVE
axis rotations + isMoving + activeDishes + audio-cue edges; the download ARM becomes a
host-authored reliable edge (`DishArm` {decoded, polarity} — polarity is per-peer RNG natively, so
the host's roll is THE roll) detected by a raw-read poll (mesh latch | signal FName | polarity);
calibration is a symmetric peer-authored 1 Hz diff-poll batch lane (any peer's local change
broadcasts; host relay = total order). The pending-adopt mechanism and the client
StartMovingAll/direct-arm halves of the v70 catch replay are RETIRED same-commit (RULE 2).

## D1 — PARK (client-only, connected)

- `ticker_disher` kill = `K2_ClearTimer(ticker, "do")` (space_renderer.cpp:362-380 precedent).
  Restore = PE `ReceiveBeginPlay` on the ticker — bytecode read END-TO-END (ticker_disher.json,
  uber 36 stmts): gamemode-gate -> `EX_BindDelegate` into a LOCAL delegate var -> Random(1800,3600)
  -> `K2_SetTimerDelegate` -> parent BeginPlay. No `do()` call, no spawn, no dupe registration
  (local-var bind cannot stack; SetTimerDelegate replaces same-key handles; restore only fires from
  the parked state).
- `ticker_dishUncalib` kill = `SetActorTickEnabled(false)`; restore `(true)` — zero staleness
  (interval re-rolled per tick; serverbox_sync.cpp:216-235 precedent).
- **1 Hz park latch**: idempotent re-park pass storing the PARKED TICKER INSTANCE pointers,
  GENERATION-KEYED on the (gamemode instance, desk instance) pair — a mid-session level reload
  spawns fresh singletons -> ptr mismatch -> auto re-park (the signal_catch CheckDeskInstance
  shape, :93-102). FindObjectsByClass only on generation change / null cache.
- The same latch pass runs the **CUE RECONCILER invariant**: any dish with local `isMoving==false`
  && wire-shadow false && a satellite cue reading Active -> deactivate + log. Root-covers the
  MEASURED leak: a kill during the phase delay lets the pending latent's one extra pass run
  `movePow` (audio start) AFTER the kill sweep's deactivation (impl-RE §2); the invariant also
  covers any unknown re-activation path. Worst leak = 1 s. (`Timeline_0` started by the same
  movePow is harmless on a dead loop — drives only the unread Speed.)
- Restore runs via the single global OnDisconnect fanout (subsystems.cpp:336-352, fired from
  net_pump.cpp:881 aggregate-disconnect — every session-end path funnels there). Order:
  **wire-residue sweep first** (for every dish with wire-shadow true: isMoving=false,
  activeDishes[i]=false, deactivate cues — safe: the mirror never starts BP loops, so this is
  clearing OUR raw writes, not killing a latent chain), then ticker restores.

## D2 — Catch detector rework + own-ping kill sweep (signal_catch_sync)

- **New catch signature** (replaces the MovingCount rising edge — RULE 2): desk-claim +
  `coord_signalData` IDENTITY-TUPLE (x,y,z,frequency) CHANGE-edge to non-None + !IsRecent.
  Derivation [measured §7/§8]: coord_signalData has exactly TWO native writers — ping-success
  (:= row) and the delete chain (:= None @34134); our wire write is apply+primed. So a local
  tuple change to non-None under claim IS a catch, definitionally. No sky-row corroboration
  (deleted as a crutch in R4 — the old detector needed it only because MovingCount was indirect).
  The cleared detector (-> None edge) is unchanged. ApplyReplay primes the FULL tuple baseline
  (today it primes only objectName).
  - Bonus: the new signature HEALS today's eaten braided catch (a ping landing while dishes
    already move produced no MovingCount rise -> catch eaten; a tuple change always fires).
  - Tuple-equal consecutive catches: RNG floats over huge ranges, measure-zero, accepted.
- **On local catch (client, claim holder)** — one sequential path, ordering by construction:
  build payload (`ReadSlewFromMovingDish` — dishes ARE moving, the coord write and the slew
  fan-out are one synchronous BP chain §8) -> send SkySignalCatch -> **kill sweep**: for every
  dish where local `isMoving && !wire-shadow` (two-writer discrimination: on a parked client only
  the own ping loop or our mirror write isMoving, and the mirror starts no loops): reflected
  `stop()` + Deactivate `satellite_move_Cue` + `satellite_Cue` + `activeDishes[i] = false`.
  Decline branches log ([dish] index+techName).
- **RETIRED same-commit**: ApplyReplay kind=0's client `StartMovingAll` half (a client never slews
  from wire; the host half stays — host receives a client catch -> native StartMovingAll), the
  :207 slewValid=0 direct arm, and the whole pending-adopt mechanism (see D4).

## D3 — DishPose stream + snapshot (ONE applier)

- `MsgType::DishPose = 39` (unreliable, HOST -> clients): movers-only rows
  {index u8, isMoving u8, yawZ f32, rollY f32} at 4 Hz while any dish moves, plus a
  **settle-tail** — full-24 sweeps at 1 Hz x3 after MovingCount hits 0 (self-heals lost
  falling-edge rows; bounds any stuck shadow).
- `ReliableKind::DishSnapshot = 100`: full-24 {yawZ, rollY, calibration, isMoving} + activeDishes
  mask, sent per-slot from ConnectReplayForSlot (AFTER the desk rows + kind=0 catch row) — and it
  re-rides the world-change re-seed automatically (ConnectReplayForSlot re-runs on re-seed §9;
  rows idempotent).
- **ONE `ApplyDishRow`** consumed by stream rows AND the snapshot (ANTI-SMEAR one owner): write
  wire-shadow FIRST -> `K2_SetRelativeRotation(axis_Z, {0,0,yaw})` + `(axis_Y, {roll,0,0})`
  (RELATIVE frame is the mirror frame §1; raw transform writes don't render — H7;
  remote_player.cpp:567-606 precedent) -> raw `isMoving` write (plain bool, panel flare reads it;
  raw-written natively §2 — not setter-managed) -> `activeDishes[i] := isMoving` (native ping-gate
  parity: mirrored true blocks the console ping AND native startMovingTo skips a "moving" dish) ->
  cue Activate/Deactivate on the isMoving EDGE (D3b, cosmetic; inferred — the one build-time risk;
  fallback = ship the mirror silent, cues in a follow-up).
- Apply guard: skip a dish where local `isMoving && !shadow` (own-ping pre-kill window; logged
  once per window). A mid-slew joiner gets cue rising edges from the snapshot itself.
- getDir() is live geometry -> the mirrored pose feeds the client's status panel for free (§6).

## D4 — ARM lane (host polarity authority)

- `ReliableKind::DishArm = 99` {armed u8, decoded f32, polarity i32} (armed=0 -> Disarm).
- **Host detection** = 4 Hz poll, ALL RAW READS (SigRead/FieldPtr — zero UFunction dispatches):
  fire on `DownloadMeshValid` EDGE **or** `DL_SignalDownloadDLData.signal` FName CHANGE **or**
  polarity CHANGE while mesh valid (arm-over-arm cover; decoded EXCLUDED — it accrues on the
  10 Hz v111 stream, desk_sim_sync.cpp:26). Deliberate tier-rule deviation (poll over the [RD]
  dishesStop PE seam): the poll is an INVARIANT over every arm source and polarity is only
  readable POST-arm anyway. [Measured] every formDownload path passes checkFordDishes'
  `Contains(activeDishes,true)` gate — the ONLY formDownload site in the full 2668-stmt desk uber
  is the dishesStop handler (desk_uber.txt:2333) => an arm implies host dishes settled.
- **Client apply** (ONE GT task — nothing interleaves): (1) pre-clear all mirrored moving state
  (shadow/isMoving/activeDishes/cues; WARN if it actually cleared anything = gate-bypass
  indicator), (2) reflected `gamemode.checkFordDishes()` — gate passes by construction; full
  native display tail runs: camera aim + `objectRenderer.begin()` + `signalFound()`
  ([measured §7] the gate's FALSE branch executes the tail; [measured R1] `init_objectRenderer`
  converges on double-call: guarded `deleteSignalActor` -> DT row -> display-actor respawn;
  cosmetic RNG deform scalars are natively per-peer), (3) `ArmDownloadFromSignal(decoded,
  hostPolarity)` — overwrites the inner transient formDownload(0,-1) local roll in the same task.
- **Disarm apply**: `ResetDownloadMachine` ([measured :766-780] it clears the mesh latch =
  genuine un-arm) + reflected `objectRenderer.deleteSignalActor()` (native un-arm parity — the
  native @33832 chain deletes the display actor; our reset alone did not).
- **Joiner arm** = a DishArm CONNECT-REPLAY row (host reads current {decoded, polarity} at
  connect) after the desk rows + kind=0 row — one ordered reliable lane (protocol.h:988
  "ordered+delivered") => identity always precedes the arm. Stale kind=0 re-expressions are
  IsRecent-filtered; a post-replay kind=0 is a genuinely NEW catch whose machine-reset clobber is
  native new-ping parity (host Disarm + re-Arm follow).
- **RETIRED same-commit (RULE 2, the ARM axis gets ONE author)**: signal_catch_sync:207 direct
  arm; :346/:436 pending-adopt + its Tick block + g_pending; console_state_sync:524-527 adopt
  call; DeskState payload fields dlDecoded/dlPolarity STRIPPED (proto 113 bumps anyway).
  **Axis split**: kind=0/1 remain the CATCH/DELETE VERB replays (their ResetDownloadMachine calls
  stay — verb-chain parity); Kind 99 owns the ARM-STATE edge. Residual overlap = two idempotent
  resets (verb replay + Disarm ~250 ms later), order-independent.
- Timing delta vs today: the client's display tail fires at HOST-arm time, ~3-16 s earlier than
  its own native theater used to — same firing set ([measured :198-211] today both peers run
  begin() on every catch).

## D5 — Calibration lane (symmetric, event-authored — NO steady sweep)

- 1 Hz all-peer diff-poll over the 24 `dish.calibration` values vs baseline; on LOCAL change ->
  batch `ReliableKind::DishCalib = 101` {n, (index u8, value f32) x n}; host applies + relays
  (arrival order = TOTAL ORDER); apply+prime GT-atomic (v112 DeskInput shape) — echo-proof.
- Covers ALL FOUR writers invariantly [§6]: host losePrec decay (slew loop), ui_console calibrate
  terminal ([measured ui_console.txt 102-107] bare Delay(0) `+= dt/60`, NO isMoving gate — ANY
  peer), tool_setDishCalibration, virusEvent scramble (initiating peer). Simultaneous two-writer
  overlap = bounded 1 Hz interleave converging to the last event in host order <=1 s after either
  writer stops (absolute values; the terminal's completion `= 1.0` wins its session).
- Baseline-gated: primed at snapshot apply, every wire apply, and generation change — a joiner's
  first poll can never diff save-loaded values against nothing. [Measured §4] ticker_dishUncalib
  writes only the losePrec FLAG (decay lives in the slew loop, dead on a parked client) — no
  join-window drift broadcast exists.
- REVISES the arch doc's "0.5 Hz host calibration sweep" (it would echo-fight the terminal-
  calibrating client — the impl-RE found the client-side legit writers after the arch pass).

## D6 — Wiring, protocol, files, logs

- net-pump: host pose sweep + arm poll 4 Hz (raw reads only); calib diff-poll 1 Hz (all peers);
  park latch + cue reconciler 1 Hz (client). Client apply write-load worst 192
  K2_SetRelativeRotation dispatches/s (24 movers x 2 axes x 4 Hz — rare), typical 8-24/s.
- protocol.h: `MsgType::DishPose = 39`; `ReliableKind::DishArm = 99`, `DishSnapshot = 100`,
  `DishCalib = 101`; DeskState payload strips dlDecoded/dlPolarity; **kProtocolVersion -> 113**.
- Files: NEW `coop/interactables/dish_sync.{h,cpp}` (the lane owner); `ue_wrap/dish.{h,cpp}` grows
  axis read/write, stop, cues (deactivate/activate/IsActive), activeDishes, calibration, techName,
  isMoving write, ticker find/park/restore (NO network logic — principle 7); signal_catch_sync.cpp
  detector rework + retirements; console_state_sync.cpp adopt removal; session.cpp routes (+~40
  LOC — **extraction proposal accompanies**: session.cpp is 1110 LOC, past the soft cap).
- Logs (user rule: identity logs carry index+techName): [dish] slew start/stop edges on BOTH
  peers + EVERY decline exit (kill-sweep skip, apply-guard skip, pre-clear WARN, reconciler hit,
  re-park on generation change).

## Arch-doc deviations (both grounded in post-arch measurements)

1. ARM rides a reliable event (not DeskSimPose ch8): polarity payload + discrete edge + the ch8
   float channel cannot carry the per-peer-RNG polarity fact.
2. No 0.5 Hz host calibration sweep: replaced by the symmetric D5 lane (client-side legit writers
   measured after the arch pass).

## Accepted residuals (documented, each bounded)

1. Cue Activate/Deactivate mirroring is INFERRED (no shipped precedent on UAudioComponent cues) —
   the one build-time risk; verify in smoke; fallback = silent mirror, cues follow-up (cosmetic).
2. External-BP direct formDownload callers not pak-censused — defense-in-depth: 4 Hz rows heal any
   hypothetical gate-bypass in <=250 ms; pre-clear WARN flags it.
3. Cheat-menu same-FName + same-rerolled-polarity re-arm invisible to the poll — the resulting
   states are identical on all peers; no divergence possible.
4. <=250 ms ping-gate hole before the first mover row lands — needs a successful ping inside it;
   converges anyway (tuple detector fires, host StartMovingAll skips moving dishes, last catch
   wins — and the new signature heals today's eaten-catch variant of this race).
5. Remote calibration moves in 1 Hz steps during a live terminal calibrate — cosmetic.
6. prop_argm / story-trigger both-peer firing = STATUS QUO preserved; lifecrystal display-actor
   dupe pre-exists L4 -> BACKLOG: audit signal-DT display-actor classes vs spawn-catch tracked
   classes.
7. DEPLOY GATE (user-facing): L4 ships proto 113 and strips DeskState fields while the v112
   hands-on verdict is pending — recommend running the v112 hands-on first (runbook ready), else
   accept a combined verdict (v112 desk-input log lines vs [dish] lines are per-lane attributable).
