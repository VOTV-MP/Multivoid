# L5 drive chain — implementation design of record (v119)

**Date:** 2026-07-18 (s20-cont). **Status: DESIGN CONVERGED — /qf 7/15 rounds, genuine
"that holds" + complete map (thread archived at scratchpad qf_thread_L5-drive-impl-DESIGN).**
Supersedes the ARCH sketch in `votv-signal-chain-all-units-DESIGN-2026-07-16.md` §L5 in
FOUR majors (each /qf-measured):
1. DriveSlotState events -> **idempotent per-slot STATE lines** (any peer announces its
   organic FSM transitions; host canonical on conflict). The authored-event + wire-driven-
   pose-predicate idea DISSOLVED (round 2: the predicate is unreliable at exactly the
   sampling moment — simulateDrop ends the stream before the port arrival).
2. The eject ordering CANNOT ride lane FIFO (round 3 retraction: the grab-half is the
   UNRELIABLE held-pose stream — no cross-channel order exists). Correctness =
   deterministic latch-completion on every eject apply.
3. **prop_driveRack IS IN SCOPE** (round 1/5: rack = the L8 canonical shape generalized to
   0x70 rows; N=16 CDO; real divergence today).
4. The slotted-latch (drop wire pose applies for slot-bound eids) is REINSTATED with an
   owner (round 4 — it had silently vanished from the sketch).

HALT gate: PASSED 2026-07-18 (gnatives_probe v4; see the gate-run block in the all-units
doc). All L5 verbs = EX_LocalVirtualFunction 0x45; `upd` REJECTED as matcher (~238/s
ambient); eraser wipe = poll-only.

## Measured fact base (all this session unless cited)

- driveSlot_C SUPER=Actor; desk slots = ChildActorComponents (obj_driveSlot_play @0x0648,
  obj_driveSlot_comp @0x0A88 — properties of the ONE cached desk actor); eraser
  signalDriveEraser_C SUPER=Actor (NO eid), own driveSlot child. Slot actors have NO eids
  (child-actor lesson).
- putDriveIn body: drivePort collision off; driveIn.Broadcast; setPropProps(frozen);
  teleport to port; **lib.getMainPlayer().dropGrabObject()** — PHYS-GRAB release only
  (measured @95933: grabHandle.ReleaseComponent + constraints; hand-item untouched);
  OnDestroyed+=dest; drive.upd(); drive.slot:=self; audio. NO internal Delay -> a reflected
  apply is fully covered by ScopedWireApply.
- Overlap insert: gates ONLY (!isRecentlyDetached && !IsValid(drive)); **Delay(0.0)** ->
  putDriveIn(OtherActor) — capture-decoupled; SELF-SIMULATES on receivers when the wire
  pose drags a drive in.
- drivePulledOut body: unbind dest; drive:=None; driveOut.Broadcast; isRecentlyDetached:=
  true (cleared ONLY by port EndOverlap); collision on; NO player refs -> reflected-safe.
- dest (destroy-while-slotted) CALLS drivePulledOut (0x45-visible); UE fires EndOverlap on
  actor destruction -> the latch clears natively on every peer.
- The E-insert path force-clears isRecentlyDetached before putDriveIn; the latch gates ONLY
  the overlap handler (a reflected insert can never be latch-refused).
- Eraser wipe: Delay 3.0 s -> gate reads driveSLot_obj.drive LIVE -> wipe writes THROUGH
  the current slot field. Mid-eject race resolves natively (last-action-wins).
- driveKick: condition = drive.kicked_0 (kicker-local, never synced) -> no phantom awards.
- comp_uploadData(drive,succ): gate comp_isDecodeActive->deny; else two-way MOVE
  drive.data_0 <-> desk comp_data_0 (+updComp). The desk half = the existing v65
  CompState/CompData lane; the drive half = DrivePayload.
- prop_drive construction: dataPaste empty by CDO default -> shop/delivery drives born
  payload-empty on every peer (converges free); signalToDynamic = pure field maps, zero
  calls, zero RNG.
- Drives SELF-REGISTER in gamemode.allDrives (raw, no liveness contract) — we do NOT use
  it; the poll iterates OUR registry drive-eid subset.
- freshBirth drain gate (prop_drop_intent.cpp:265) is an explicit class whitelist
  (reel|module) — client-born drives would be local ghosts without widening.
- rack: arrays {slots(comps), has(bool), data(16 x 0x70 rows CDO)}; putDriveIn = harvest
  getData row -> K2_DestroyActor(drive) -> data[idx]:=row -> gen(); getDrive = spawn
  prop_drive deferred + SetStructurePropertyByName(data_0) PRE-Finish + hand/inventory +
  clear row + gen(); gen() = full visual re-derive FROM data (mirror-safe re-applier);
  addDrive = internal (loadData rebuild only).
- Row codec: coop/signal_wire.h Serialize/Deserialize (v65, proven; caps 96/48/64/64;
  rides BlobChunkPayload; **image deliberately absent** — L5 inherits the same deferral,
  one shared bulk-lane residual).

## The design

### Lane 1 — DriveSlotState (ReliableKind=109, Lane::Normal, relay-whitelisted, 8 B)
`{u8 role (0=DeskPlay 1=DeskComp 2=Eraser), u8 occupied, u16 censusIdx, u32 driveEid}`
- ANY peer announces its slot FSM edges (organic self-sim OR local action), detected by:
  0x45 capture on putDriveIn/drivePulledOut (ctx=slot actor -> resolve role; dirty-mark +
  {slotKey,eid} memory reads only; EMISSION AT THE NET-PUMP BARRIER — no mid-verb session
  calls) + a 1 Hz slot.drive poll sweep (matcher-gap safety).
- Receiver pre-checks BEFORE any reflected call (already-true => prime-only no-op — a dup
  line never re-runs dropGrabObject/teleport). Insert apply = reflected putDriveIn(actor);
  eject apply = reflected drivePulledOut() + the DETERMINISTIC LATCH-COMPLETION: read
  drivePort.IsOverlappingActor(drive); if not overlapping -> isRecentlyDetached=false raw
  (an EX_Let-managed plain latch — completing the EndOverlap transition; not suppression).
- Apply+prime GT-atomic (baseline primed INSIDE the apply task).
- HOST CANONICAL on conflict: host re-announces its state on mismatch; 1 Hz host sweep +
  connect-broadcast (QueueConnectBroadcastForSlot) seed joiners.
- The SLOTTED-LATCH: the lane owns a per-peer set of slot-bound eids (add on every local
  bind — organic or applied; remove on every unbind incl. dest). The generic wire pose
  APPLY consults it (session-running()-gated) and DROPS poses for bound eids (kills the
  in-flight straggler displacing a frozen drive).
- dropGrabObject side effect on the apply path: only reachable on the self-sim MISS path
  (pre-check eats the common case); releases at most a phys-grab. Documented residual.

### Lane 2 — DrivePayload (ReliableKind=110, Lane::Normal; header {u32 eid} + signal_wire
row blob via BlobChunkPayload — ALWAYS the blob path, the 58/59 precedent)
- Writers detected by: 0x45 dirty-marks on saveSignal/deleteSignal (ctx=GAMEMODE — the cb
  is MARK-ALL: marks the <=3 slotted drives, the barrier diff decides; no ctx keying =>
  no silent-never-fire) + comp_uploadData (ctx=desk) + putDriveIn ctx=rack (harvest) +
  the 1 Hz Data_0 baseline poll over the registry drive-subset (eraser wipe = poll-only;
  IsLiveByIndex-guarded).
- Emission gated on bytes-changed-vs-baseline (a spurious mark with unchanged Data_0
  provably sends nothing).
- BIRTH INVARIANT: the birth author broadcasts the payload once at adoption for any
  non-default row (covers rack-take, pocket-replace; delivery converges free; save-load =
  save copy). No struct birth channel needed.
- Receiver apply: raw row write via the signal_wire deserialize + the proven FString apply
  path + reflected drive.upd() + prime, ONE GT task.
- freshBirth drain whitelist + the ReelEjectIntent host author whitelist + the deny-reap
  idiom all WIDEN to IsDriveClass (prop_drive_C IsChildOf) — client-born drives cross.

### Lane 3 — RackState (ReliableKind=111, Lane::Normal; ops {u32 rackEid, u8 op(0=set 1=
take 2=deny 3=canonical), u8 idx, u16 _pad} + row blob (set) / full-array blob (canonical)
via BlobChunkPayload)
- The L8 shape generalized: presser index-ops peer->host HOST-TERMINAL; host serializes
  (occupied/absent races -> deny + refund); host-canonical full array broadcast converges
  layouts; receivers apply = array write + reflected gen() + prime (gen is a pure
  re-applier). Host organic changes broadcast canonical directly (host-organic lesson).
- Take deny refund: taker destroys its LOCAL ghost (untracked => no v106 leak); if already
  adopted, host reaps via the deny-record TTL (the v118 idiom widened).
- Rack rows carry payloads: order vs the v106 drive destroy is BENIGN (ops carry row
  VALUES; receivers never harvest).

### Cross-cutting
- JOIN: symmetric self-restore (identical save copy both sides; whether SP re-binds slots
  via load-overlap is [inferred, design-insensitive — symmetric either way]) + host
  canonical sweep + connect-seed (baselines prime silently from current state at session
  start; announces only on post-prime edges).
- TEARDOWN: OnDisconnect fanout clears latch set, baselines, dirty set, drive-subset, slot
  baselines, rack baselines, deny records, pending canonicals; re-cleared at session start
  (recycled-eid belt). Leaver: only their deny records (per-peer) die.
- Deck-list halves of import/export ride the EXISTING 58/59 lane unchanged. The two-lane
  transient (signal briefly in both places / neither for <=1 s) = the standing residual
  class — hands-on testers: NOT a bug, converges.
- Counters: 60 s summary line (marks, emits, applies, denies, latch-completions) — dead
  matchers visible (dead-guard lesson).

### Documented residuals (acceptance record)
1. Sub-frame stale-row import dupe (arch doc; narrowed by single slot + capture).
2. Image bytes deferred lane-wide (the shared v65 bulk-lane residual).
3. dropGrabObject phys-grab release on the insert-apply MISS path (rare, SP-semantic).
4. Eraser wipe vs eject same-second race: last-action-wins, converged, SP-consistent.
5. Two-lane import/export transient (<=1 s, converges).
6. Split in-flight reliable loss on a leaver = the shared transport-truncation class.

### Build checklist (3-place router rule x3 + wiring)
protocol.h (119 + 3 kinds + payloads) | session_lanes.h (3 pins + relay rows: 109 relay-
whitelisted, 110 relay-whitelisted, 111 NOT — host-terminal ops, host-authored canonical)
| event_dispatch_signal.cpp (3 cases) | ue_wrap/desk/drive_chain.* (NEW) |
coop/interactables/drive_sync.* (NEW; rack_sync split if near 800) | prop_drop_intent
(whitelist widen) | pose-apply latch consult | subsystems (Install/Tick/OnDisconnect/
connect-broadcast) | COOP_SYNC_MAP + DISPATCH_VISIBILITY rows.

## BUILD OUTCOME (as-built deltas, 2026-07-18/19 night)

Built as coop/interactables/drive_sync.* + ue_wrap/desk/drive_chain.* (+ the checklist
touches). FOUR deviations/strengthenings vs the converged text, each evidence-driven:
1. **Slotted-latch = the EXISTING frozen/static pose gate** (remote_prop.cpp "frozen/
   static non-attachable -- ignored", measured): a slotted drive is frozen by putDriveIn
   on every peer, so straggler poses are already dropped. RULE 2: no second mechanism
   built; the design's eid-set is SATISFIED BY the standing gate.
2. **Birth-invariant broadcast narrowed to AUTHORED births** (smoke 1 measured the
   joiner re-broadcasting the host's own connect-seed rows + 2 unmatched-eid strays):
   host first-sight broadcasts (authoritative organic world); a CLIENT first-sight
   broadcasts ONLY for a drain-noted local birth (NoteLocalDriveBirth from the
   freshBirth path). A joiner's save-loaded drives prime silently.
3. **Audit CRIT-1 (stale pending slot-line replay)**: one pending line per role
   (newest supersedes) + the baseline-at-queue stash; the replay DROPS when the slot
   moved on. Never resurrects a stale occupant; host canonical stays sound.
4. **Audit MAJOR-1 (slot-only deny reap = false positives)**: the v118 reap idiom
   REJECTED for drives (one class, no byte discriminator). Reworked CONTENT-correlated:
   every take (host op apply + host organic diff + client op derivation) records the
   removed row's hash in a taken-ring; a deny arms {senderSlot, rowHash}; the ghost is
   destroyed client-side by content match, and host-side the reap fires at the ghost's
   adoption-payload arrival (hash match) -- exact, a legitimate same-peer birth never
   matches. HostShouldReapDriveBirth RETIRED (RULE 2).
Perf audit: 0 CRITICAL; F-1 (vm_dispatch install-failure latch), F-2 (ActorForEid ->
O(1) Registry::Get), F-3 (eraser census 5 s negative-cache), F-6 (DriveClass accessor),
F-7 (pending cap 256) all applied. **drive_sync.cpp = ~1007 LOC > 800 soft cap:
extraction QUEUED as its own commit -- the rack lane (~350 LOC: canonical blob codec +
sweep + host ops + client apply + deny/taken rings) splits to
coop/interactables/drive_rack_sync.cpp before the next lane touches the file.**

## /qf round map (7 rounds)
R1: self-sim discovery + eject-never-self-sims + dropGrabObject/rack/slot-offset reads +
birth invariant. R2: predicate DISSOLVED -> idempotent state lines + host canonical; latch
hole found; codec reuse; rack bodies read. R3: RETRACTION (pose-stream = no ordering) ->
deterministic latch-completion; barrier emission restored; slot resolution idioms. R4:
latch reinstated w/ owner; freshBirth whitelist measured closed; JOIN honesty. R5: dest->
drivePulledOut measured; destroy-EndOverlap native; poll universe = registry subset; rack
N=16 -> blob_chunks; eraser race native. R6: apply-time priming correction; teardown
enumeration; dataPaste/signalToDynamic closed; layouts + no-over-cap-file check. R7:
mark-all ctx answer + driveKick measured-excluded + residual filed => "that holds".
