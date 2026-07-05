# COOP_EVENT_JOIN — the join-during-event contract (Phase 1 AS-BUILT 2026-07-05)

**Status: Phase 1 AS-BUILT, autonomous e2e PASS 2026-07-05 00:06** (wire v98,
`ReliableKind::EventSnapshot` + the active-override replay + the piramid lane's gather
re-send; Phase 0's probe seam now feeds the wire). Phase 2a (cue join re-send) AS-BUILT
2026-07-05, e2e PASS; Phase 2b (census fill) open-incremental; Phase 3 DESIGN. Bytecode
ground truth — `research/findings/votv-active-events-registry-RE-2026-07-04.md`.
This is the answer to the devs' gauntlet hard case (`docs/DEVS_GAUNTLET.md`): a player
joins while the host is mid-event (pyramid et al.) and must converge to the same world.

MTA precedent: MTA never replays event HISTORY to a joiner — the server sends every
element's CURRENT state at join (CEntityAddPacket stream at CPlayerJoinCompletePacket
time). Our shape is the same: base world (save blob) + per-lane current-state snapshots
+ an active-event snapshot. History replay is only used where an event's own scripted
actor IS the state carrier and restarting it locally converges (see policy below).

## 1. Ground truth (all bytecode/code-verified 2026-07-04)

- The game keeps a NATIVE in-flight event registry on the gamemode:
  `activeEvents` (int refcount) + `activeEvents_senders` (TArray<UObject*> of the live
  event actors). Single writer `lib_C::setEvent(ctx, active, ambient)`; reader
  `lib_C::getEvent` = `activeEvents > 0 OR outside-base-box`. ~95 classes register
  (census in the RE finding).
- The game natively FORBIDS saving during an event (ui_menu.enterPause disables
  button_Save when getEvent; it also skips SetGamePaused — SP does not pause mid-event).
  The save format cannot represent an in-flight event; that is WHY the base-world blob
  alone can never carry a mid-event join.
- Our join live-capture (saveObjects/saveTriggers + SaveGameToSlot direct) enters below
  those gates: it mechanically succeeds mid-event, and event actors (not `int_save_C`)
  are naturally absent from the blob -> the blob IS the clean base layer.
- `passEvents` (fired-rows history) rides the blob. `event_fire_sync::TryReplay` dedupes
  against it (`InClientPassEvents -> skip`).

## 2. The gap, precisely named (AS-IS behavior for a mid-event joiner)

A client joining while the host is mid-pyramid today gets:

| Layer | What arrives | Verdict |
|---|---|---|
| Save blob (live capture) | base world + story flags + `passEvents` ALREADY containing 'piramid' (settime appended it at fire) | OK as base; but poisons replay dedupe |
| EventFire lane | NOTHING — fires are broadcast at fire time to then-connected peers only; the host poll baseline never re-sends history | gap |
| Replay dedupe | even if an EventFire arrived, `InClientPassEvents('piramid')` == true (the blob carried it) -> replay SKIPPED | the killing blow |
| NPC lane | live creature mirrors DO arrive (EntitySpawn ToSlot at join) -> creature-phase events (wisps, ventCrawler...) partially converge | covered by lane |
| Prop lane | prop snapshot + membership sweep -> event-thrown props converge | covered by lane |
| Weather/time/sky/balance | join-edge ToSlot replays exist | covered by lane |
| event_cue lane (starRain) | cue fired once at event start; v98+ join re-send delivers live already-broadcast cues at world-ready | closed (Phase 2, e2e 2026-07-05 09:36) |
| No-lane transients (arirShip flight, droppers, piramid2 sequence...) | nothing | gap |

Net: **the joiner stands in a calm base world while the host watches the pyramid land.**
"People saw completely different events or nothing at all" — the devs' exact words; this
is the mechanism. (v98 closed the first two gap rows AND the killing blow: EventSnapshot
delivers the in-flight set at world-ready, and its active-override bypasses the
passEvents dedupe. The event_cue row closed 2026-07-05 with the Phase 2 join re-send.)

## 3. The design

### 3.1 Core: mirror the native registry (host-side, poll)

New module `coop/world/event_active_sync.cpp` (gameplay/network layer; principle 7):

- Host: 1 Hz poll of `gamemode.activeEvents_senders` (the proven passEvents-poll shape —
  prime baseline, diff membership by object identity + ClassOf name). Produces two edge
  streams: `EventActiveBegin(className)` / `EventActiveEnd(className)` — logged (the
  Phase 0 probe shape) AND feeding the join snapshot (Phase 1, AS-BUILT v98). NOTE: the
  poll is CONNECTION-gated — an event that starts while the host idles alone shows its
  BEGIN only when a transport connects (prime BEGINs the already-active set; firstSeen =
  prime time, so elapsedSec is prime-relative for those).
- The registry is authoritative and self-maintained by the game's own actors; we never
  write it on the host. (On the client, replayed event actors call setEvent locally and
  maintain the client's own counter for free.)

### 3.2 Join snapshot: `EventSnapshot` (host -> joining slot, reliable) — AS-BUILT v98

At the join edge (`subsystems::ConnectReplayForSlot`, after the lane snapshots), the host
sends ONE EventSnapshot message per CURRENT `activeEvents_senders` entry (as-built
refinement over the count+entries sketch: 0-4 concurrent in practice, and a fixed 98 B
payload fits one datagram): the sender's class name + the mapped list_events row (the
class->row map lives HOST-side in `event_active_sync::kClassRowMap`; '' = unmapped) +
`elapsedSec` (host poll first-seen delta — for an event already running when the peer's
transport connected, this is prime-relative, not true event age; diagnostics-only until
Phase 3 phase hints).

Joiner handling per the EXTENDED dupe matrix (one new column on the existing policy in
`event_fire_sync.cpp` — "late-join answer"):

| Policy class | Late-join answer |
|---|---|
| replay-safe rows (obelisk, treehouse_*, ...; `piramid` FLIPPED to lane-owned 2026-07-04 — host-random path, see docs/events/piramid.md) | REPLAY locally at join — with an **active-override**: the snapshot marks the row in-flight, which bypasses the `InClientPassEvents` dedupe (that dedupe exists for COMPLETED history, not in-flight events). Joiner sees the event from t=0; endpoint converges (story flags already in blob; replay re-runs the visuals/sequence). AS-BUILT: `event_fire_sync::ReplayInFlightRow` (queue-upgrade if an EventFire copy is already pending; the passEvents-skip no longer marks the session replayed-set, so a history-skip can never block a later in-flight override) [V e2e 2026-07-05 00:06: `client REPLAY runEvent 'obelisk' (in-flight active-override)`]. |
| lane-owned rows (wisps, ventCrawler, props, weather...) | NOTHING at event level — the owning lane's join snapshot delivers current state. Each lane's join answer is audited in the contract table (3.4). |
| host-local rows (agrav, pranks RNG...) | skip, as today. |
| unknown class (not in the map) | log LOUD + skip (same default-no-replay discipline). |

Wire (AS-BUILT): `ReliableKind::EventSnapshot = 86` (v98), payload = ONE entry
{classNameFixed[48], rowNameFixed[48], elapsedSec u16} (98 B), one message per in-flight
event; wired in TWO places (enum/payload + the world-family switch in
event_dispatch_world — the SyncRouter consolidation retired the third). NOT
pre-world-sendable (deliberate divergence from this sketch): it is only ever sent AT the
world-ready edge, when the slot's gate is already open; the receiver-side eventer race is
absorbed by event_fire_sync's `g_pending` drain, same as EventFire.

### 3.3 Counter parity on the client (accepted asymmetry)

For lane-owned events the joiner has no local event actor -> its `activeEvents` stays 0
-> local getEvent=false -> native UI would allow save/pause. Both are already coop-held
(disableSave hold + save disk hook; pause_guard). Documented as ACCEPTED — no extra
mirroring of the counter itself (a synthetic counter bump with no sender actor would
fight the game's own clamp logic; RULE 1 says don't fake the registry, deliver the
events).

### 3.4 The per-lane late-join contract (the audit table this doc owns)

RULE going forward: **every lane must state its late-join answer**; a new lane PR adds
its row here. **AND the tracking rule those answers stand on (2026-07-05, the 0s live
failure): host-side ENROLL/TRACKING seams gate on HOSTING (session + Host role), never
on `connected()` — an event actor spawned while the host is ALONE must still be tracked,
or the connect-snapshot has nothing to re-send (the joiner's "empty world mid-event").
Only SENDS are peer-gated. Was violated by the EX-catch + both spawn interceptors + both
pose-tick lifecycles; fixed as a class.** Current rows (code-verified 2026-07-04):
npc=EntitySpawn ToSlot [V]; world_actor=WorldActorSpawn ToSlot [V]; props=prop_snapshot+sweep [V]; weather/time/sky/
balance=ToSlot replays [V]; item_activate+voice=ReplayPeerStatesToSlot [V];
event_cue=**join re-send AS-BUILT (Phase 2, 2026-07-05): `event_cue_sync::
QueueConnectBroadcastForSlot` walks live PSCs at the join edge and re-sends each
ALREADY-BROADCAST cue's {cueId,pos} ToSlot; a cue newer than the last ~1 s poll is left to
the next Tick broadcast (the slot's gate is open post-world-ready) — re-sending it too
would double the emitter. Emitter PHASE is not synced (joiner replays from t=0 — accepted
for transient cosmetics). [V autonomous e2e 2026-07-05 09:36: starRain fired pre-client,
host `connect-snapshot -- re-sent live 'starRain' (cue 0) to slot 1`, client exactly ONE
`replayed 'starRain'`, 0 ERROR both]. NOTE (observed same run): the join-edge registry
showed `0 in flight` — starRain's active phase in the registry does not span the shower,
so the CUE lane, not EventSnapshot, is the load-bearing late-join carrier for it**; keypad/doors/lights=trigger states ride saveTriggers in the blob [V];
kerfur=reconcile lane [V]; atv=lane [V];
piramid=WA connect snapshot delivers the in-flight pyramid at its current transform +
npc snapshot the remaining wisps; the joiner's mirror BeginPlay restores registry parity
(setEvent). v98 closed the gather sub-gap: `piramid_sync::QueueConnectBroadcastForSlot`
re-sends an in-flight gather commit ToSlot at the join edge (reads CURRENT `gathering` +
`wispTarget` off the live pyramid — the lane-local answer, not an EventSnapshot field as
this doc first sketched; a wisp already consumed = skip, the joiner misses only the beam
tail; a wire wisp-destroy landing mid-replay-montage resolves to a pending-kill ref the
native beam code None-checks) [AS-BUILT 2026-07-05, join-during-gather untested by hands-on].

### 3.5 Phases

- **Phase 0 (probe, read-only): AS-BUILT 2026-07-04** — `coop/world/event_active_sync.{h,cpp}`:
  host 1 Hz `activeEvents_senders` membership diff -> edge log
  (`event_active: BEGIN/END class=... n=... elapsed=...`), plus the join-edge would-be-snapshot
  log in `subsystems::ConnectReplayForSlot`. Proves the seam on real events before any wire.
  (probe-don't-guess) **Seam PROVEN [V: 2026-07-04 21:38 autonomous eventforce run, host log]:**
  forced obelisk -> `BEGIN class=obelisk_C n=1` within 1 s of the native FORCED dispatch; its
  alarm chain -> `BEGIN class=trigger_alarm_C n=2` (refcount exercised) and
  `END class=trigger_alarm_C n=1 elapsed=65s`; `eventforce_test: VERDICT PASS` + client REPLAY
  line unaffected.
- **Phase 1: AS-BUILT 2026-07-05 (wire v98), autonomous e2e PASS** — EventSnapshot wire +
  joiner replay with the active-override + the piramid gather re-send. Evidence (00:04-00:08
  run, DLL E09121F58CE2A5C6): host forced obelisk BEFORE the client existed; at the client's
  transport connect the host poll primed n=2 and BEGIN'd obelisk_C + trigger_alarm_C; at
  world-ready `join-edge slot=1 SNAPSHOT class=obelisk_C row=obelisk` + `class=trigger_alarm_C
  row=<unmapped>`; client: `join snapshot -- in-flight class=obelisk_C row=obelisk` -> queued
  (eventer racing) -> `client REPLAY runEvent 'obelisk' (in-flight active-override)` ->
  `runEvent('obelisk', special='None') dispatched`; unmapped alarm logged LOUD + skipped
  (the Phase 2 fill signal working); 0 ERROR both peers. NOTE: the dev force does not append
  passEvents, so this run exercised the override MARKER but not the passEvents-carried
  bypass branch (that needs a scheduler fire mid-join or a hands-on; the branch is a
  two-boolean short-circuit, code-verified). Pyramid mid-join stays the HANDS-ON acceptance
  test (section 4).
- **Phase 2a (cue join re-send): AS-BUILT 2026-07-05, autonomous e2e PASS 09:36** —
  `event_cue_sync::QueueConnectBroadcastForSlot` (see the 3.4 event_cue row for the
  as-built + evidence; driver `autotest_cueforce.cpp` fires starRain pre-client). No wire
  change (reuses EventCue ToSlot — protocol stays v98).
- **Phase 2b (census fill), open + incremental by design:** class->row map filled toward
  the full census (the ~95, most map to existing dupe-matrix rows — the receiver's
  `NO class->row map entry` WARN lines name exactly the classes still missing; each
  granular per-event pass adds its registrant per docs/events/README.md).
- **Phase 3 (only if hands-on shows it matters):** phase hints — elapsedSec-driven
  fast-forward for timeline-driven senders; per-event, evidence-first.

## 4. Verification bar

Phase 1 is VERIFIED only by a hands-on: host starts pyramid (dev event_force), client
joins mid-flight, both players see the pyramid; client log shows the snapshot arrival +
active-override replay; host log shows registry BEGIN/END edges. Autonomous smoke can
only prove packet flow (snapshot sent/received/queued) — not the visuals.

**Attempt 1 FAILED (2026-07-05 ~11:00, user live): the joiner saw NOTHING.** Root cause
was NOT this contract's wire — the host-side enroll/tracking seams were connected()-gated,
so a pyramid spawned while the host was alone never entered the mirrors and the 3.4 lane
snapshots had nothing to send. Root-fixed as a class same hour (`ff338d87`; the 3.4
tracking rule above was written from it). Re-test pending (runbook 0s-FIX).
