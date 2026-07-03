# VOTV event TRIGGER GRAPH — why menu-fired events "don't happen" (RE, 2026-07-03)

**Status:** durable RE (bytecode + map-package census; confidence [V] unless noted).
**Trigger:** user hands-on — "OBELISK сработал только когда я зашел (хост) с улицы на БАЗУ".
**Sources:** `research/bp_reflection/{trigger_box,trigger_box_N,trigger_TBoxActivator,triggerBase}.json`
(bp_reflect.py over `objects/triggers/*.uasset`) + `_map_untitled_1.json` (the world package,
50951 exports) census. Complements `votv-event-system-RE-2026-06-13.md` (the eventer dispatch table).

## The chain (14 "arming" runEvent cases)

```
eventer runEvent("obelisk")                        [instant, same frame]
  -> (eventer property event_obelisk) trigger_TBAc_obelisk.runTrigger        [TBoxActivator_C]
       -> box = gamemode.getObjectFromKey(triggerBoxKey); box.isActive=true; box.setColls()
  -> TB_event_obelisk (trigger_box_N_C level volume) now has collision ON
     ... nothing visible happens yet ...
PLAYER WALKS INTO THE VOLUME                        [the gate the user hit]
  -> BndEvt Box BeginOverlap: GetObjectClass(OtherActor) in Filter?  (class filter)
       -> runAll(-1): for i: fireTrigger(objects[i], ids[i])         [triggerBase]
       -> N -= 1; if N <= 0: Box.SetCollisionEnabled(off)            [box_N one-shot]
  -> chain target (triggerBase_obelisk) -> the obelisk actually falls/arms
```

- `trigger_box_C`: `IsActive` bool @0x2B0, `Filter` TArray<TSubclassOf<AActor>> @0x2B8,
  `colls` TArray<ECollisionChannel>; `setColls()` = block-all, then per-`colls` overlap ONLY
  when IsActive. `setActiveTrigger(_, 0/1)` = isActive flip + setColls.
- `trigger_box_N_C`: adds `N` (shots). Its overlap handler does NOT re-check IsActive —
  the gate is physical (collision off = no overlap event). All 14 event boxes ship `N=1`.
- `triggerBase_C.runAll(allIndex)`: allIndex<0 → per-connection `fireTrigger(objects[i], ids[i])`;
  else every object with the given index.
- The eventer's OTHER cases are instant at fire: direct `triggerBase` runs (treehouse builds,
  picnic, dish breaks), `trigger_forceObject` (signals join the SETI pool), direct spawns
  (creatures/UFO droppers), `trigger_bedEvent` (arms the NEXT sleep), `trigger_agrav`
  (gated by lib.isPhysicalEvents), earthTp (immediate player teleport).

## The 14 volume-gated events (map census; all trigger_box_N_C, N=1, isActive=False)

| menu event | activator | box (level volume) | box target (chain head) |
|---|---|---|---|
| obelisk | trigger_TBAc_obelisk | TB_event_obelisk | triggerBase_obelisk |
| arirShip | trigger_TBAc_arirAppear | TB_event_arirAppear | arirShipAppear_2 |
| falseEnter | trigger_TBAc_arirPasslock | TB_event_arirPasslock | passwordLock3_12 (re-lock) |
| mann | trigger_TBAc_mann | TB_event_mann | triggerBase_mannequin |
| vent | trigger_TBAc_vent | TB_event_vent | prop_vent4_3 |
| crys | trigger_TBAc_crys | TB_event_crys | triggerBase_crys |
| fakeGrays | trigger_TBAc_fakeGraysInit | TB_event_fakeGraysInit | trigger_fakeLmaos_2 |
| toeStab | trigger_TBAc_arirToe | TB_event_arirToe | trigger_event_toeStab |
| cookier | trigger_TBAc_cookier | TB_event_cookier | triggerBase_cookier |
| susArir | trigger_TBAc_susArir | TB_event_susArir | triggerBase_susArir |
| arirEgg | trigger_TBAc_arirEgg | TB_event_arirEgg | trigger_arirEgg_2 |
| wisps | trigger_TBAc_wisps | TB_event_wispSwarm | trigger_wispSwarm_2 |
| piramid | trigger_TBAc_piramid | TB_event_piramid | piramidSpawner_2 |
| call0 | trigger_TBAc_bigmRoar | TB_event_bigmRoar | triggerBase_bigmRoar_2 |

**GAME BUG found [V, cooked data]:** `trigger_TBAc_paperGray.triggerBoxKey ==
trigger_TBAc_bigmRoar.triggerBoxKey` (`eA2ki8OGauna6Izl_Ifijg`). The activator resolves its box
at runtime BY KEY (getObjectFromKey), so arming **paperGray actually activates bigmRoar's box**;
TB_event_paperGray can never be armed through its activator. (The editor-only direct `box->`
property points at the right box, but runTrigger ignores it.) Our force feature drives the box
overlap directly, so NOW! on paperGray still completes the paperGray chain — but its [ARMED]
badge will never show through the native arm.

## What the coop mod does with this (coop/dev/event_force, DLL 2026-07-03)

- **Status badges** in the F1 events menu: per volume-gated row, a ~1 Hz game-thread snapshot
  reads the box's `IsActive` + `N` → `[volume-gated]` / `[ARMED - walk-in pending]` / `[FIRED]`.
- **NOW! button** = arm via the SHARED fire seam (event_fire_sync::HostFire → native dispatch +
  v95 EventFire broadcast so clients replay the arm per policy), then drive the box's OWN
  BeginOverlap handler with the local player pawn as OtherActor — the native class filter, N
  decrement and collision-off all run in the game's own bytecode. No global state is faked.
- Per-player scares: forcing completes the HOST's box; a connected client's replayed arm keeps
  its own box armed until that player walks in (native per-player behavior, by design).
- **VERIFIED [V smoke 2026-07-03 late]**: autotest_eventforce (obelisk) -- PRE armed=0 shots=1 ->
  ForceNow -> POST shots=0, host FORCED line + client `REPLAY runEvent 'obelisk'` same second,
  0 errors x2 runs. Forcing **wisps** additionally yields a MIRRORED swarm since the same night's
  wisp lane (`17cde303`, 32/32 x4 legs) -- the box overlap drives trigger_wispSwarm whose
  EX_CallMath spawns are caught + enrolled (see COOP_ENTITY_EXPRESSION_MAP wisp_C section).
