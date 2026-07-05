# radar terminal alert — the computer's OWN per-terminal sounds (beep/ping/ent)   (STATUS: ASSESSED — per-viewer local BY DESIGN, no lane, no wire)

NOT the base alarm. The base emergency alarm (siren + red `alarmLamp_C` beacons,
`trigger_alarm_C`) is a shared-world state and lives in [alarm.md](alarm.md) with its
`alarm_sync` lane. THIS doc is the other thing the word "alarm" gets used for (user rule
2026-07-05: do not conflate): the notification sounds the base computer's radar screen
plays AT THE TERMINAL for whoever is looking at it. Moved out of alarm.md 2026-07-05 so
each concept has its own home.

## 1. Native behavior (ground truth — [bytecode] `analogDScreenTest.json` uber, disasm 2026-07-05)

All three live in `analogDScreenTest` (the base computer screens actor — the LIVE class
despite the `test/` asset path), inside the radar scan loop:

- **New-entity ALERT beep** [@1194-1198]: `radar_scanned.Num > radar_prevEnt &&
  radar_hasAlarm && radar_soundActiveAlarm` → one-shot 3D `SpawnSoundAtLocation(
  arirCrateAlarm_s, radar->center, vol 0.75)`; then `radar_prevEnt = radar_scanned.Num`.
  - `radar_hasAlarm` = the radar ALARM module upgrade is installed in `panel_radar`
    (`updModules` → `screens.radar_hasAlarm = Array_Contains(upgrades, ...)`).
  - `radar_soundActiveAlarm` = a per-terminal sound setting (`ui_console`).
- **Sweep PING (sonar)** [@1187-1189]: `radar_soundActivePing` → one-shot `sonar` at
  `radar->center`, vol 0.15 (each sweep revolution).
- **Entity BLIP** [@1240-1241]: `radar_SoundActiveEnt` → `radar->radarBeep->Activate(true)`
  when a point is drawn.
- The same scan loop's `comp_radarPoint.important` branch is what fires the BASE alarm
  (`runTrigger(radar, 1)` on `alarmTrigger`) — that axis belongs to [alarm.md](alarm.md),
  not here. The "b/Stop alarm" radar-panel press is ALSO the base alarm's, not this.

## 2. Coop verdict — per-viewer local BY DESIGN (no sync-axis crosses peers)

Each peer's own computer scans its OWN `gamemode.radarObjects` on its OWN sweep angle.
The radar CONTENT already converges through the entity mirror lanes (mirrored actors
carry their `comp_radarPoint` components and register natively), so what the terminal
shows is convergent; WHEN it beeps is each viewer's own sweep timing — exactly like SP,
and exactly what a player standing at a different terminal would expect. Nothing to
mirror, nothing to suppress; zero coop code exists for this, deliberately.

## 3. Future sync work parking spot

If radar-terminal state ever needs cross-peer work, it goes HERE (and gets its own
`*_sync` file), e.g.: sharing manual importance-marks between peers, mirroring the
selected/tracked radar target, or a shared "seen entities" log. As of 2026-07-05 no such
feature is asked for or planned.
