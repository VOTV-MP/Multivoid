# COOP_EVENT_JOIN — the join-during-event contract (DESIGN 2026-07-04)

**Status: DESIGN** (bytecode ground truth verified — see
`research/findings/votv-active-events-registry-RE-2026-07-04.md`; no code shipped yet).
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
| event_cue lane (starRain) | cue fired once at event start; NO join snapshot -> joiner mid-starRain sees no rain | gap (small) |
| No-lane transients (arirShip flight, droppers, piramid2 sequence...) | nothing | gap |

Net: **the joiner stands in a calm base world while the host watches the pyramid land.**
"People saw completely different events or nothing at all" — the devs' exact words; this
is the mechanism.

## 3. The design

### 3.1 Core: mirror the native registry (host-side, poll)

New module `coop/world/event_active_sync.cpp` (gameplay/network layer; principle 7):

- Host: 1 Hz poll of `gamemode.activeEvents_senders` (the proven passEvents-poll shape —
  prime baseline, diff membership by object identity + ClassOf name). Produces two edge
  streams: `EventActiveBegin(className)` / `EventActiveEnd(className)` — LOGGED first
  (Phase 0 probe), wired to the join snapshot (Phase 1+).
- The registry is authoritative and self-maintained by the game's own actors; we never
  write it on the host. (On the client, replayed event actors call setEvent locally and
  maintain the client's own counter for free.)

### 3.2 Join snapshot: `EventSnapshot` (host -> joining slot, reliable)

At the join edge (same place the other ToSlot replays fire, AFTER the blob announce),
host sends, for each CURRENT `activeEvents_senders` entry: the sender's class name +
the mapped list_events row (where one exists) + `elapsedSec` (host poll first-seen
timestamp delta; phase hints are Phase 3).

Joiner handling per the EXTENDED dupe matrix (one new column on the existing policy in
`event_fire_sync.cpp` — "late-join answer"):

| Policy class | Late-join answer |
|---|---|
| replay-safe rows (piramid, obelisk, treehouse_*, ...) | REPLAY locally at join — with an **active-override**: the snapshot marks the row in-flight, which bypasses the `InClientPassEvents` dedupe (that dedupe exists for COMPLETED history, not in-flight events). Joiner sees the event from t=0; endpoint converges (story flags already in blob; replay re-runs the visuals/sequence). |
| lane-owned rows (wisps, ventCrawler, props, weather...) | NOTHING at event level — the owning lane's join snapshot delivers current state. Each lane's join answer is audited in the contract table (3.4). |
| host-local rows (agrav, pranks RNG...) | skip, as today. |
| unknown class (not in the map) | log LOUD + skip (same default-no-replay discipline). |

Wire: new `ReliableKind::EventSnapshot` — wired in the THREE places per
[[feedback-reliablekind-router-checklist]]; payload = count + per-entry
{classNameFixed[48], rowNameFixed[48], elapsedSec u16}. Pre-world-sendable like
EventFire; entries queue in the existing `g_pending` drain until the eventer is up.

### 3.3 Counter parity on the client (accepted asymmetry)

For lane-owned events the joiner has no local event actor -> its `activeEvents` stays 0
-> local getEvent=false -> native UI would allow save/pause. Both are already coop-held
(disableSave hold + save disk hook; pause_guard). Documented as ACCEPTED — no extra
mirroring of the counter itself (a synthetic counter bump with no sender actor would
fight the game's own clamp logic; RULE 1 says don't fake the registry, deliver the
events).

### 3.4 The per-lane late-join contract (the audit table this doc owns)

RULE going forward: **every lane must state its late-join answer**; a new lane PR adds
its row here. Current rows (code-verified 2026-07-04): npc=EntitySpawn ToSlot [V];
world_actor=WorldActorSpawn ToSlot [V]; props=prop_snapshot+sweep [V]; weather/time/sky/
balance=ToSlot replays [V]; item_activate+voice=ReplayPeerStatesToSlot [V];
event_cue=**NONE (gap; Phase 2: host enumerates live cue PSCs at join edge, sends
EventCue ToSlot)**; keypad/doors/lights=trigger states ride saveTriggers in the blob [V];
kerfur=reconcile lane [V]; atv=lane [V].

### 3.5 Phases

- **Phase 0 (probe, read-only):** host poll + edge log of the registry
  (`event_active: BEGIN/END class=... n=...`), plus join-edge log of what WOULD be
  snapshotted. Proves the seam on real events before any wire. (probe-don't-guess)
- **Phase 1:** EventSnapshot wire + joiner replay with active-override for replay-safe
  rows. Pyramid mid-join = the acceptance test.
- **Phase 2:** event_cue join snapshot (closes the starRain gap); class->row map filled
  for the full census (the ~95, most map to existing dupe-matrix rows).
- **Phase 3 (only if hands-on shows it matters):** phase hints — elapsedSec-driven
  fast-forward for timeline-driven senders; per-event, evidence-first.

## 4. Verification bar

Phase 1 is VERIFIED only by a hands-on: host starts pyramid (dev event_force), client
joins mid-flight, both players see the pyramid; client log shows the snapshot arrival +
active-override replay; host log shows registry BEGIN/END edges. Autonomous smoke can
only prove packet flow (snapshot sent/received/queued) — not the visuals.
