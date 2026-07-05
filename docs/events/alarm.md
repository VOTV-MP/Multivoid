# alarm (`trigger_alarm_C`) — the BASE emergency alarm: fire-alarm siren + red rotating lamps   (STATUS: AS-BUILT, autonomous e2e PASS 2026-07-05 13:50; hands-on pending for the audible/visual axes)

**TWO different "alarms" — do not conflate (user rule 2026-07-05):**
1. **THE BASE ALARM (this doc, this lane)** = `trigger_alarm_C`: the base-wide looping
   siren + every red `alarmLamp_C` beacon + ceiling-lamp flicker + basement grate + the
   activeEvents registry. Real-world fire-alarm feel. This is the shared-world state the
   lane syncs.
2. **The radar TERMINAL alert** (per-terminal beep/ping/blip at the computer) is a
   DIFFERENT concept with its own doc:
   [radar-terminal-alert.md](radar-terminal-alert.md) — per-viewer local by design,
   no lane, no wire.

The base alarm's player-facing stop is the radar panel's "b/Stop alarm" press.

User rule anchor (2026-07-05): «alarm это по сути тоже своего рода ивент, которому нужна
во время new peer join обработка» — this doc + the `alarm_sync` lane are that pass.

## 1. Native behavior (ground truth — all [bytecode] from `trigger_alarm.json` /
##    `panel_radar.json` / `analogDScreenTest.json` / `alarmLamp.json`, disasm 2026-07-05)

- **The actor**: ONE `trigger_alarm_C : AtriggerBase_C` placed in the map, registered
  under gamemode key `alarmTrigger` (all callers resolve it via
  `gamemode.getObjectFromKey(n'alarmTrigger')`). Fields: `active` (bool),
  `alarms` (TArray<`alarmLamp_C`*>, rebuilt by `processKeys` from keyed objects),
  `Audio1` (the looping siren AudioComponent).
- **ON callers — EXHAUSTIVE census** [binary grep of the FName `alarmTrigger` across ALL
  cooked game assets, 2026-07-05 — exactly 4 hits: the map, trigger_alarm itself, and]:
  1. `analogDScreenTest` (the base computer screens actor — the LIVE class despite the
     test/ path) radar sweep loop: a scanned `gamemode.radarObjects` entry whose
     `comp_radarPoint.important == true` → `alarmTrigger->runTrigger(radar, 1)`
     [uber @1246-1250]. **This is how EVENTS light the base alarm**: obelisk et al.
     never call runTrigger themselves (obelisk bytecode: only setEvent for ITSELF —
     verified 2026-07-05); the event's actor IS/spawns an important radar object and the
     computer's scan trips the siren. (The 07-04 probe's `BEGIN class=trigger_alarm_C
     n=2` during the forced obelisk is this indirection observed live.)
  2. `prop_food_snuskLoaf` → `runTrigger(self, 1)` — the joke-food prank also sets off
     the base alarm [bytecode].
- **OFF path (the only one)**: `panel_radar` interaction option "b/Stop alarm" →
  `runTrigger(self, 0)` [panel_radar uber @150-268].
- The computer's OWN per-terminal sounds in the same scan loop (alert beep / sweep ping /
  entity blip) are the OTHER "alarm" — RE'd in
  [radar-terminal-alert.md](radar-terminal-alert.md). One open [?] recorded there that
  touches THIS doc: whether `radar_hasAlarm` (the alarm module upgrade) also gates the
  important→runTrigger branch upstream, or only the beep branch.
- **`runTrigger(owner, index)`** [uber @939→]: IDEMPOTENT — `IntToBool(index) == active`
  → return, no side effects. On a real toggle: `active = index`; basementGrate prop →
  `setPropProps(false, false, !active, false)`; `lib_C::setEvent(active, false, self)`
  → the native activeEvents refcount (docs/COOP_EVENT_JOIN.md registry); each
  `alarms[i]->setActive(active)`; `Audio1->SetActive(active, reset=true)` (klaxon);
  if active → every `ceilingLamp_C` → `solar()` (emergency flicker).
- **`alarmLamp_C::setActive(v)`**: glow sprites + point light visibility + emissive
  material swap (`inst_alamp2_on/off`); its tick spins the `speen` beacon 360°/s from a
  RANDOM initial phase — beacon rotation is per-instance cosmetic noise natively.
- `trigger_eventt_arirShip` drives its OWN `alarms[]` (the arirShip event) — separate
  event, separate future pass; NOT this lane.

## 2. Sync-axis table

| axis | class | carried by |
|---|---|---|
| alarm ACTIVE state (klaxon + all lamps + grate + ceiling flicker + activeEvents count) | shared world state; either peer's radar/press toggles it | **`alarm_sync` lane** (1 Hz state poll + `ReliableKind::AlarmState`); each peer replays `runTrigger` natively — ONE reflected call reproduces the whole fanout |
| radar terminal alert (beep/ping/blip) | per-viewer — own doc: [radar-terminal-alert.md](radar-terminal-alert.md) | none — local by design |
| lamp beacon rotation phase | per-viewer cosmetic (random phase natively) | none — native randomness |
| `radarObjects` contents (what the radar can see at all) | derived world entities | already owned by the entity mirror lanes; not this doc |

## 3. Coop design (`src/votv-coop/src/coop/world/alarm_sync.{h,cpp}`)

**Dispatch fact that shapes everything** (docs/COOP_DISPATCH_VISIBILITY.md): both callers
invoke `runTrigger` as `EX_VirtualFunction` — INVISIBLE to ProcessEvent, same as every
screen/panel verb (visibility map line "every screen/panel device verb → POLL the state
field"). So NO hook: the lane POLLS `trigger_alarm_C.active` (FindBoolProperty real-bit,
cached instance revalidated IsLiveByIndex) at 1 Hz on BOTH peers.

- Poll sees a change that the lane itself did not apply (`g_expected`):
  - HOST → broadcast `AlarmState{active}` to all.
  - CLIENT → send `AlarmState{active}` to the host (its own scan fired ON early, or its
    player pressed Stop). The host applies natively; the host's own poll then detects the
    change and does the one canonical broadcast fanout.
- Receive `AlarmState` → apply = reflected `runTrigger(nullptr, active?1:0)` on the local
  trigger instance (ProcessEvent directly — WE dispatch it, so BP-internal invisibility
  is irrelevant), then latch `g_expected`. The bytecode's own idempotency check makes
  redundant applies free and breaks every echo loop.
- Both peers replay natively → lamps/klaxon/grate/ceiling/`setEvent` all converge,
  including the client's OWN activeEvents refcount (no-save gate + tension behave).
- **Late-join answer (COOP_EVENT_JOIN.md 3.4 row)**: host
  `QueueConnectBroadcastForSlot(slot)` sends the CURRENT state unconditionally at the
  world-ready edge — a mid-alarm joiner starts its klaxon on arrival; idempotent if the
  transferred save already carried it.
- **EventSnapshot dedup**: `trigger_alarm_C` is LANE-OWNED — `event_active_sync::
  SendJoinSnapshotForSlot` skips it with an INFO line instead of shipping an unmapped-row
  WARN to every mid-alarm joiner (the WARN stays meaningful as the Phase-2b fill signal
  for genuinely uncovered classes).
- Wire: `ReliableKind::AlarmState = 87`, `AlarmStatePayload{u8 active}` (4 B padded),
  protocol v101. Family: event_dispatch_world.

MTA precedent: shared-world toggles (doors/lights) sync symmetrically with server relay —
`CClientDoor`-shape state channel, not RPC replay; deliberately NOT host-suppressing the
client's radar scan (both scans read mirrored entities and converge; the lane reconciles).

## 4. Caveats / known quirks

- The ON moment can differ across peers by up to one radar sweep period (each peer's
  sweep angle is local) — the lane converges it within ~1 s of the first peer's edge.
- `ceilingLamp.solar()` only fires on the ON edge natively; whatever its recovery
  behavior is, it is identical SP behavior on every peer (replayed, not synthesized).
- The save persists trigger state via keys — a joiner's transferred save can arrive
  already-ON; the unconditional join send + idempotent apply absorb both orders.
- Autonomy CANNOT verify the audible/visual fanout (klaxon, lamp glow) — smoke asserts
  state + log lines only; hands-on stamps VERIFIED (the geometry-blind e2e lesson:
  [[lesson-e2e-assert-must-discriminate-the-axis]] — the assert here is the *state bit +
  applied log on the receiving peer*, which DOES discriminate this axis).
- Known narrow race (audit 2026-07-05, below reporting bar, recorded for honesty): two
  peers producing OPPOSITE local transitions inside the same ~1 s host poll window can
  net-cancel on the host (its field returns to the pre-round value → no broadcast),
  stranding the losing peer's state until the next real transition. Requires two peers
  toggling the alarm in opposite directions within one second — accept until seen live;
  the fix, if ever needed, is a host reconcile-echo after every client request.

## 5. Verification

- Static RE: complete (section 1; no runtime probes needed — every seam bytecode-proven).
- **Autonomous e2e PASS 2026-07-05 13:50 (DLL `595897C89D88F5EC`, smoke verdict PASS, 0
  ERROR both peers)** — and it exercised MORE than designed, because the ON landed while
  the client was still loading:
  - ON 13:50:14: host `applied active=1`; the live broadcast to the still-loading slot
    was blocked by the world-ready send gate (`host broadcast active=1 send FAILED` — the
    expected single-peer-mid-join shape, see the log-shape caveat below);
  - 13:50:19 client world-ready → **the JOIN answer fired end-to-end**: host
    `event_active: join-edge class=trigger_alarm_C is LANE-OWNED — no EventSnapshot`
    (the kLaneOwnedClasses skip working) + `alarm_sync: connect-snapshot — sent active=1
    to slot 1` → client `applied active=1 (native runTrigger replay)` the same second.
    **This IS the user's mid-alarm-join acceptance case, proven.**
  - OFF 13:50:59: the LIVE transition path — host `applied active=0` + `host broadcast
    active=0` → client `applied active=0` the same second.
- Log-shape caveat recorded: a host transition while EVERY connected slot is still
  pre-world-ready logs `host broadcast ... send FAILED` (WARN) — not a defect; the
  join snapshot is the designed carrier for that window.
- PENDING (hands-on, runbook 0u-ALARM): the axes autonomy cannot see — the audible
  siren + lamp glow on both peers — and a "Stop alarm" press ON THE CLIENT (the
  client→host forward path; the e2e only exercised host-originated transitions).
