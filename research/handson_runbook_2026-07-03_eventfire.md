# Hands-on runbook 2026-07-03 (take 3) — the EventFire replay channel (v95)

**Deployed:** DLL `DF0C0295B6E7613F` on all 4 installs (hash-verified; supersedes `8BAD59B4` —
contains everything from the two earlier 2026-07-03 runbooks PLUS this channel). **Protocol
v94 -> v95** — BOTH peers must run the new DLL (an old peer is refused at handshake with a
version-mismatch reason, by design). Paks unchanged.

**Autonomous smoke: PASS** (fresh client vs s_1234 host, 90 s steady-state, RSS stable, no
[Error]/SEH). The full new chain ran live:
- host `event_fire: resolved (saveSlot=0x4B0 passEvents=0xC8 allEvents=0xF0 ...)` +
  `host poll primed (passEvents baseline=1)`;
- host fired `solar` / `arirGraff_0` / `enasus` through the same seam the F1 menu now uses —
  3x native dispatch + 3x `broadcast`;
- client `client scheduler SUPPRESSED (allEvents 69 -> 0)` (exactly the 69 DataTable rows),
  `client REPLAY runEvent 'solar'` + `client REPLAY runSpecialEvent 'arirGraff_0'` (same-second
  latency), and `'enasus' NOT replayed -- prop lane owns the outputs` (the policy refusing a
  row whose props already mirror via PropSpawn — replay would have doubled them).
NOT exercised autonomously (needs a real clock-cross): the scheduler-fire observation path
(settime append -> poll growth -> broadcast). It shares Broadcast() + the receive side with the
dev lane above; only the growth-loop read is unexercised. Your test 2 covers it.

## What this is (your ask: campfire + map smoke on BOTH peers)

The story scheduler (`saveSlot::settime` -> `eventer.runEvent`) is invisible to every hook, and
level-placed event flips (campfire, treehouse builds, server breaks, signal beats) rode NO lane —
a client never saw them. Now:
- the HOST is the only scheduler (host-authoritative, as you said): the client's own scheduler is
  structurally dead while connected (`allEvents.Num=0` — the walk source; the game rebuilds the
  array every world load, so nothing can poison a save; restored on disconnect). This also closes
  a real hole: during accelerated sleep nights the client clock free-runs and WOULD have natively
  fired midnight rows (treehouse_N are all at 00:00) — divergently, on both peers.
- every host fire (scheduler-observed via passEvents growth, or dev-menu) broadcasts
  `EventFire{kind, rowName}`; the client replays the same native verb reflected — but ONLY for
  rows on the replay allowlist (the per-row dupe matrix from the 2026-06-13 events RE): story
  flips (treehouse_0..5, break_*, obelisk, piramid), forceObjects signal rows (looker_*,
  arirSignal, arirSat_*, peace, ...), cosmetic/sound (solar, call0), scare arms (per-viewer by
  SP design), graffiti. Rows whose outputs already mirror (props, NPCs, ATV, sleep, starRain
  cue, devices) or would spawn client-local actors (creatures, arirShip, wisps) are logged +
  skipped — replaying those would re-create the client-local dup disease. `ariralPrank` never
  crosses (host-local RNG roll). EventFire is pre-world-sendable: a story row fired while a
  joiner is mid-download/load queues on the joiner and replays at world-ready (the save-carried
  dedupe makes any overlap idempotent) — no join-window loss.

## Your tests (host + client, both on `DF0C0295`)

1. **THE CAMPFIRE (your original target):** host F1 -> Game -> Events -> Story ->
   `treehouse_0` (red = Ctrl+click). EXPECTED NOW: the ariral camp + campfire + the map-wide
   smoke column appear on the HOST **AND the CLIENT** (client log: `client REPLAY runEvent
   'treehouse_0'`). Compare the smoke column position from both machines. (Repeat-fires are
   deduped on the client — one replay per row per session.)
2. **A real scheduler fire (the poll seam):** on the host, sleep (or wait) across any scheduled
   row's time — s_1234 at day ~4: `ventCrawler` day 4 23:27 or `peace` day 5 23:14 are the next
   ones. When the host clock crosses it, host log must show `host OBSERVED scheduler fire 'X'`
   + `broadcast`; the client replays or logs the policy skip per the row. If you sleep the night
   through, several rows may land in one batch — each gets its own line.
3. **Graffiti mirror:** F1 -> Events -> Prop -> `arirGraff_3` — decal appears on BOTH peers
   (client log `client REPLAY runSpecialEvent 'arirGraff_3'`). This one is repeatable — firing
   it twice replays twice.
4. **A no-replay row behaves correctly:** F1 -> Events -> Prop -> `enasus` (Safe). Host: sushi +
   note props drop. Client: the SAME props appear via the prop lane (grabbable, host-visible),
   log shows `'enasus' NOT replayed`. There must be exactly ONE set of props on the client — if
   you ever see TWO overlapping sets, that's a policy row I classified wrong; report the name.
5. **Disconnect restore:** client leaves the session mid-world -> client log
   `allEvents restored (0 -> 69) -- local scheduler resumes`.

## Known boundaries (deliberate, documented in event_fire_sync.h)

- Trigger-volume fires (bed dream, scares when YOU walk into them) stay per-player native — the
  SP design is per-viewer; arming rows via replay preserves exactly that.
- Creature-spawn rows (ventCrawler, boarwar, graysforest, ...) are still host-only-visible — the
  creature classes have no mirror lane yet (RE 10.2 allowlist thread, separate work).
- A dev-menu fire does not enter the host's passEvents (native ui_eventRun behaves the same), so
  the scheduler can re-fire that row at its scheduled time on the host; the client dedupes.
