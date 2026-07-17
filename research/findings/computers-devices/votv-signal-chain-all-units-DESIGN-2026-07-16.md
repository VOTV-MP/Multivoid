# Signal-chain ALL-UNITS sync design — OPEN-4..9 (2026-07-16, /qf 9 rounds "that holds")

The architecture-level sync design for the six remaining signal-chain lanes, produced by a
9-round `/qf` design pass (user: "Давай дизайн qf 15 раундов на все юниты"; converged with a
genuine critic hold at round 9/15; thread: scratchpad qf_thread.md, archived on next topic).
Fact bases: `votv-signal-chain-units-RE-2026-07-16.md`, `votv-dish-rotation-RE-2026-07-16.md`,
`votv-tape-caddy-daily-task-RE-2026-07-16.md`, TRACKER OPEN-4..9. Status: **L4 = BUILT v113**
(2026-07-16 night, commit `f204c0f7` — see the impl-level design of record
`votv-dish-L4-impl-DESIGN-2026-07-16.md`, which REVISES two lines of this doc's §L4: the ARM edge
went RELIABLE `DishArm=99` with host polarity, NOT DeskSimPose ch8 — the ch8 float cannot carry the
per-peer-RNG polarity fact; and the 0.5 Hz host calibration sweep was replaced by a SYMMETRIC 1 Hz
diff-poll lane `DishCalib=101` — the impl-RE found client-side legit calibration writers a host
sweep would echo-fight). **L7 = BUILT v114** (2026-07-17, commit `ba8ce297` — impl design of
record `votv-tape-caddy-L7-impl-DESIGN-2026-07-17.md`, which REVISES three lines of this doc's
§L7: the `active`-toggle event is RETIRED (already on the ApplianceState lane); "the HOST adopts
the inserted progress and owns the accrual" became a both-peers-accrue + host ReelPose CORRECTOR
(the park is un-holdable: upd() re-applies SetActorTickEnabled everywhere); and "eject spawns via
the generic lanes" was measured HALF-TRUE — the client half needed ReelEjectIntent=104 + the
PropSpawn savedScalar birth channel). **L6 = BUILT v117** (2026-07-18,
commit `c077e910` — impl design of record `votv-deck-play-L6-impl-DESIGN-2026-07-18.md`, which
REVISES this doc's §L6: SEAM detection replaces the 250 ms poll [the doc's own tier rule], the
gen guard added, playSignal is parameterless [the "playSignal(index)" line was a sketch]).
**L8 = BUILT v118** (2026-07-18, commit `45a886a4` — impl design of record
`votv-physmods-L8-impl-DESIGN-2026-07-18.md`, which REVISES §L8 in three majors: {slot,moduleId}
deltas -> VALUE-ops + a host-canonical array [the array is a SET]; the UNPLUG path exists;
explotano is measured presser-local — "damage rides the choke" described a path explosion_C
never enters). **L5/L9 remain DESIGN** (verified against HEAD `45a886a4` — no drive/meadow lane
modules exist in src; both HALT-gated on the 0x45 frequency probe).
PRECONDITION v112 shipped (`7d57478f`).

## The cross-cutting rules this design stands on

1. **Presser-authored state broadcasts, never intent lanes** (round-1 reframe): every unit verb
   is EX_Local*-invisible and EXECUTES LOCALLY on the presser (including the id mint and the
   explotano roll) before any interception is possible — so the presser is the momentary author;
   its state change broadcasts; every other peer applies + primes; the host is relay +
   store-of-record. (The v112 DeskInput shape, generalized.)
2. **Detection tier rule:** PE-visible seam > raw-field poll > VM-bracket 0x45 capture.
   The VM bracket (substrate BUILT: ue_wrap/vm_dispatch.cpp; kerfur capture-in-window [V]) is
   used ONLY where the poll is blind or too coarse; in-bracket work = dirty-mark + operand-
   identity MEMORY reads only (no engine calls); heavy state is read at the net-pump barrier.
   **HALT GATE: the 0x45 frequency-counter probe with desk+laptop matchers MUST run and pass
   before any L5/L9 code** (the same run measures meadow row/image sizes).
   **GATE RUN 2026-07-18 (gnatives_probe v4, 120 s autonomous LAN smoke, host log 22:28-22:30)
   — PASS with one matcher rejection + one measurement miss:**
   - Opcode census (static, scan_call_opcodes over UAssetAPI JSONs w/ resolved imports): ALL
     candidate verbs = EX_LocalVirtualFunction (0x45) — saveSignal/deleteSignal x3 each +
     comp_uploadData (analogDScreenTest), addSignal x2 (analogDScreenTest), removeSignal x1 +
     sortSignal x2 (ui_laptop uber; sortSignal IS the "move" verb, moveUp/moveDown -> uber ->
     sortSignal), putDriveIn x2 / drivePulledOut x3 (driveSlot + prop_drive).
   - Ambient frequency (coop steady state, no desk occupancy): saveSignal/deleteSignal/
     removeSignal/sortSignal/comp_uploadData/putDriveIn/drivePulledOut = ZERO hits -> clean
     dirty-mark matchers. addSignal = 3 hits/2 min (boot/world-load window; a same-FName
     collision from a non-laptop class — consumer callback MUST class-check ctx, the standing
     doctrine). **upd = ~238/s ambient (boot burst on uicomp_gameRuleSlot_C; steady ~238/s)
     -> REJECTED as a matcher.** Consequence: the eraser wipe (EX_Let + upd, no named verb)
     is detected by the 1 Hz payload poll ONLY (already the design's safety sweep).
   - Perf: widened 11-name filter + full diag = 0.010-0.015 ms/frame@120 (gate <=0.1) — PASS.
   - SIZES: store offsets resolved [V] (gamemode.savedSignals_0=0x968, saveSlot
     .savedSignals_0=0x680, .savedSignals_comp_0=0x690) but s_1234 held 0 rows in all three
     stores -> **image-blob size NOT measured; L9 must NOT depend on a bounded image size —
     route the ADD payload through blob_chunks whenever it exceeds the packet budget** (the
     doc already names blob_chunks as the overflow path).
3. **PumpBarrier() = the structural ordering owner:** one pump function drains ALL VM-capture
   emits, THEN runs ALL poll sweeps. Lanes register (emit, sweep) pairs. A capture emit advances
   the corresponding poll baseline (never a double-emit; the sweep only covers matcher gaps).
4. **Apply+prime is GT-atomic** (one game-thread task) on every lane — echo-proof.
5. **JOIN:** L5/L7/L8/L9 are seeded by the existing save transfer (the joiner's world IS the
   host's copy) + prime-on-first-sight — NO new adopt lanes. L4 alone needs a snapshot
   (orientation is never saved). **LEAVER:** presser lanes hold no standing state (teardown =
   local baselines); L6 self-terminates via native `fin`; split in-flight reliable loss on
   disconnect = the shared transport-truncation class (documented, not new).

## L4 — DISHES (OPEN-4): host-auth pose mirror; client sim parked
> **BUILT v113 (`f204c0f7`) per `votv-dish-L4-impl-DESIGN-2026-07-16.md`** — with the two
> deviations named in the Status header (reliable DishArm, symmetric DishCalib). The section
> below is the ARCH-level frame as designed; the impl doc is the as-built truth.

- Client slew PARKED: kill the client's ticker_disher + ticker_dishUncalib timers with a
  PAIRED RESTORE on disconnect (the sky-roller precedent), and **RETIRE the client half of the
  SkySignalCatch replay (StartMovingAll) in the same commit** (RULE 2) — one writer of client
  dish rotation. Slew-entry census: catch chain + the two tickers (RE §4-5). The calibration
  decay lives in the slew LOOP (event-started) → dormant on a parked client.
- HOST streams **DishPose** (unreliable MsgType): movers-only rows {index, yaw, pitch, isMoving,
  calibration} at ~2-3 Hz + a 0.5 Hz full-24 calibration sweep (the status screen reads per-dish
  isMoving + calibration×getDir — uber [1661-77]; event_solar mass-writes calibration).
  Mirror writes = K2_SetRelativeRotation on axis_Z/axis_Y (raw transform writes don't render —
  the H7 lesson). Worst case 144 dispatches/s (all 24 moving — rare), typically 1-3 dishes.
- **DishSnapshot** (reliable) full-24 at the world-ready edge (measured ordering: ready =
  post-BeginPlay = post-randrot) + a one-shot verify log after apply.
- The client's download-ARM edge (natively its own dishesStop — dead when parked) rides the
  v112 DeskSimPose as **channel 8, DISCRETE** (snap-on-arrival, NO interp ease — the
  mirrored-threshold-latch class): rising → ArmDownloadFromSignal(streamed decoded/polarity),
  falling → ResetDownloadMachine. Tickers stay host-only rollers (RNG authority).
- Per-dish identity logging (index + techName + Key) at slew start/stop, both peers (user ask).

## L5 — DRIVE CHAIN (OPEN-5): VM-captured payload broadcasts + slot events

- **DrivePayload** (reliable) {drive eid, name, id, level, size, decoded}: VM-bracket dirty-mark
  on the four drive verbs (deck import/export, comp swap, eraser wipe); the barrier emits the
  payload read from the live drive. A ~1 Hz payload poll stays as the safety sweep. The signal
  `id` string is CONTENT, never coop identity (the drive prop eid is the identity).
- **DriveSlotState** (reliable) insert/eject edge events (slot-field poll detection; the
  driveIn_*/driveOut_* deck delegates are measured tutorial-only — NOT used). Mirror apply =
  **reflected putDriveIn(driveActor)** (measured: a BlueprintCallable UFunction taking the
  drive actor — the native freeze+teleport+slot-pointer path); raw state writes = fallback.
  **Slotted-latch:** the apply latches the eid as slotted → the mirror DROPS generic pose
  applies for that eid until the eject event unlatches (kills the in-flight straggler pose
  that would displace a frozen drive forever).
- Deck list deltas ride the existing SavedSignalAppend/Delete 58/59 unchanged.
- Documented residual: a sub-frame stale-row import dupe (two peers racing the same deck row;
  physically narrowed by the single drive slot per unit; the VM capture shrinks the window
  from ~1 s to ~a frame).

## L6 — DECK PLAYBACK (OPEN-6): presser-owned effect replay

- Detection: poll the RAW `signalSound.bIsActive` component field + play_selectIndex @250 ms
  (zero dispatches). → **PlayDeckEvent** (reliable) {play|stop, selectIndex}.
- Mirrors replay reflected `playSignal(index)` / `stopSound()` — measured-safe read-set
  (the synced deck row + a static DataTable; gates active_play [v112] + valid index +
  downloaded>=size; per-peer spectrum jitter is cosmetic). World-audible on every peer.
- Owner = the presser (OWNER-EFFECT). A leaver mid-playback self-terminates via the native
  `fin` (track end) on every peer. Residual: a JOINER misses in-flight playback (playSignal
  has no seek; documented).

## L7 — TAPE CADDY + DAILY TASK (OPEN-7): regime-bounded host sim
> **BUILT v114 (`ba8ce297`) per `votv-tape-caddy-L7-impl-DESIGN-2026-07-17.md`** — with the
> three deviations named in the Status header (toggle retired to the appliance lane; corrector
> instead of host-adopted accrual; client-eject birth via ReelEjectIntent + savedScalar). The
> section below is the ARCH-level frame as designed; the impl doc is the as-built truth.

- **Regime boundary:** transitions across the -1.0 empty sentinel (insert/eject, both
  directions) AND the `active` record toggle (measured: accrual gated on `active` @2022) =
  presser-authored **ReelSlot** events {reel, progress|empty|toggle}; the HOST ADOPTS the
  inserted progress into ITS wallunit and from then owns the accrual; continuous values ride
  **ReelPose** (unreliable, 1 Hz, changed-only, non-empty reels). The client's own woken
  accrual sits bounded under the corrector (pure FClamp tick, no >=100 latch — measured;
  saturation converges bitwise at the literal 100.0f).
- Eject spawns the reel prop via the generic lanes; its Progress rides the ReelSlot event
  (PropSpawnPayload has NO generic float channel — measured).
- **TaskNewState** (reliable): presser-authored saveSlot.taskNew snapshot after drone sell /
  processTask. Money is ALREADY host-authoritative (balance_sync, [V]).

## L8 — PHYSMODS (OPEN-8): slot-granular state events

- Poll the 12-slot physMods array ~1 Hz → **PhysModsState** {slot, moduleId} events; mirrors
  apply the array write + re-run consumers via reflected `updPhysMods()` (measured:
  parameterless 43-stmt body). Module prop destroy/spawn rides the generic prop lanes;
  ordering (same sender, same lane): PropDestroy lands before the slot event — a <=1 s
  cosmetic window, documented. explotano outcome = presser-authored (damage rides the
  existing VictoryFloatMinusEquals choke).

## L9 — MEADOW DATABASE (OPEN-9): VM-captured row deltas

- VM-bracket dirty-mark on laptop.addSignal / delete / move; in-bracket capture = op + the
  TARGET ROW's id string (memory read via the operand index). Barrier: ADD → deep-copy the
  live array tail; DELETE/MOVE → emit {op, rowId}. **MeadowDelta** (reliable).
- Identity = the row's id string: content-replicated (byte-identical on every peer via save
  copy + deltas); the per-peer-mint hazard applies only to CREATION. Index-keying REJECTED
  (concurrent-delete shifts corrupt).
- Mirror apply = **RAW TArray row append** (the ApplySignalSet precedent) — NEVER an
  addSignal replay (it derives new ids for copies = silent re-mint).
- The image blob (@0x58, render-target bytes) rides the existing blob_chunks when it exceeds
  the packet budget; its real size is measured by the frequency-probe run.

## Bookkeeping

- **8 new ReliableKinds** (DishSnapshot, DrivePayload, DriveSlotState, PlayDeckEvent, ReelSlot,
  TaskNewState, PhysModsState, MeadowDelta — each: the 3-place router checklist + a relay row +
  a COOP_SYNC_MAP row) + **2 unreliable MsgTypes** (DishPose, ReelPose) + DeskSimPose ch8.
- ONE kProtocolVersion bump covering the whole train at its first user-ship; internally the
  lanes land as SEPARATELY-SMOKED increments in this order:
  **v112 (desk input) → the 0x45 frequency probe (HALT gate) → L4 → L7 → L6 → L8 → L5 → L9.**
- Each lane's build starts with its own implementation /qf if the wiring shows real choices
  (per the phase discipline); the impl-gated measurements named per lane above are the first
  steps of those passes.

## Documented residuals (the acceptance record)

1. L5 sub-frame stale-row import dupe (narrowed by the single slot + VM capture).
2. L6 joiner misses in-flight playback; leaver playback fin-bounded.
3. L7 client accrual sawtooth <= one native increment under the 1 Hz corrector.
4. L8 <=1 s destroy-vs-slot cosmetic window.
5. Split in-flight reliable loss on a leaver = the shared transport-truncation class.
6. L4 hypothetical sublevel late-BeginPlay dish (none known on the base level) + the one-shot
   verify log.
7. Mid-session upgrade divergence = OPEN-3 (separate workstream).
