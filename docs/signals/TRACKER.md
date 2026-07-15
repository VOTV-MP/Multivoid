# Signal-processing subsystem — living status tracker

**Update every session.** One row per signal-desk element: native behavior, the coop shape, who
owns it, the wire lane, and the honest status. STATUS legend (docs-piles discipline): **VERIFIED**
(hands-on or matching live log — say which) · **AS-BUILT** (shipped, not yet hands-on) · **DESIGN**
(designed, not built) · **OPEN** (gap, no design) · **N/A** (deliberately per-player).

The RNG-authority PROGRESS of the download machine lives in `COOP_RNG_AUTHORITY.md` T2-5b — this
tracker points there. Wire-lane discoverability lives in `COOP_SYNC_MAP.md`. Neither is restated here.

---

## Master table

| Element | Native behavior (1-line) | Shape | Owner | Wire lane | Status |
|---|---|---|---|---|---|
| Sky-signal generation | director rolls signals into space | 3 host-roller | host | `SkySignalState 52/53` (`signal_sync`) | **AS-BUILT** (host-roller + full consume replay) |
| Signal catch | dish catch → `coord_signalData` = truth | 2 intent→host | host | `SkySignalCatch` (`signal_catch_sync`) | **AS-BUILT** (v70 host-owned catch truth) |
| Dish aim / slew (committed) | aim coords, slews to target | 4 owner-stream | occupant | `DishAimState 55` | **AS-BUILT** (v109: committed-coords-only + connect-snapshot) |
| Saved signals | append/delete decoded signals | 2 intent (CRDT) | host-gate | `SavedSignalAppend/Delete 58/59` | **AS-BUILT** |
| Desk/console occupancy | one peer "inside" an enterable screen | 4 claim | host-arbitrated | `DeviceClaim 51` (`device_occupancy`) | **AS-BUILT** (all 8 devices; hands-on for entry) |
| Desk scalars + log line | discrete desk state + a log append | 1 poll-delta | occupant | `DeskState 54` / `DeskLogLine 65` | **AS-BUILT** |
| Refiner pane (comp) | `comp_progress` refine (NOT the download) | 4 single-sim | occupant | `CompState 60` / `CompData 61` | **AS-BUILT** |
| **Desk coords-panel cursor** | live cursor over the coord screen | 4 owner pose-stream | occupant | `DeskCursorPose 36` (`desk_cursor_sync`) | **VERIFIED** (v109, user TAKE=SMOOTH) — ⚠ see OPEN-1 |
| **Desk alarm-clock** | HH:MM display, frozen host-mirror | 3 host-mirror | host | `ClockPose 37` + reliable connect-edge | **AS-BUILT** (v110 `2dde3e16`, NOT hands-on) |
| **Freq/polarity + download rate** | tune → download SPEED; per-peer sim + 2 RNG | 3 host-auth sim | host | `DeskSimPose=38` (`desk_sim_sync`) | **AS-BUILT** v111 (host streams 8-float output vector; client interpolates+overwrites; NOT hands-on) |
| coord log lines (`CR:`/animated) | `ProduceLogLines` runs on EVERY peer | 4 holder-owned | occupant (planned) | partial (`CR:` filtered off wire) | **OPEN** (root of the divergence cluster) |
| Dishes aim/slew (post-catch) | slew recomputed per-peer after a catch | 3/4 (tbd) | host (planned) | — | **OPEN** |
| Stationary PC / laptop screen | portable-PC power + screen | 1/4 (tbd) | tbd | — | **OPEN** (screens gap-list #5) |

Screens gap-list items beyond the signal desk (reactor rods, generator, SAT-console scrollback, TV,
laptop tasks, serverBox monitors) are tracked in the screens design finding, not here — they are the
generic device layer, not the signal pipeline.

---

## OPEN items — detail

### OPEN-0 · Freq/polarity + download-rate SIM — AS-BUILT v111 (2026-07-15, NOT hands-on)
**As-built (`desk_sim_sync`, `MsgType::DeskSimPose=38`, proto→111):** host owns the sim + streams the
8-float output vector (decoded/resDetec/rate/frData/poData/frOffset/poOffset/cooldown) unreliable ~10 Hz,
newest-wins; client interpolates (multi-channel LerpWindow, cursor pattern) + OVERWRITES its local sim
(the self-accrued garbage never shows). Knob INTENTS stay occupant-authored on DeskState; the live
DeskState apply keeps the local sim-output fields (gate 1: one author). The `/qf` collapsed a suspected
sim-migration to this small fix by measurement: seed-sync refuted (unseeded+transient noise), frData/poData
stream host-auth (NOT rely on native convergence — OPEN-3 upgrade lane absent), coordLog kept separate
(OPEN-2). Audit READY (0 CRITICAL; hot paths O(1), raw-writes offset-guarded, gate-1 one-author). Full
design trail: scratchpad qf_thread.md. **Take (verify):** freq/pol numbers match on both peers + the knob
ramp is smooth not stepped; a 110-vs-111 pair HARD-CLOSEs at the gate.

**Root (MEASURED 2026-07-15, `2de202ed` desk_diag probe):** the entire download sim runs per-peer.
End-state host `decoded=0.0064 pol=1` vs client `decoded=0.0262 pol=0` — different progress AND
polarity with the same setup. `DL_downloading` folds two per-peer RNG terms (detector needle
`DL_resDetecPercent` = `RandomFloatInRange`, `noise` = `RandomFloat`) plus locally-integrated
freq/pol filter offsets → two peers download at different speeds = a live MECHANIC desync.

**Fix (DESIGN, not built — needs its own `/qf 15`):** shape-3 host-authoritative sim — the host runs
the tick, rolls both RNG, owns the outputs (`DL_downloading`, `decoded`, `DL_resDetecPercent`,
`DL_frData`, `DL_poData`); the client SUPPRESSES its own accrual + RNG and mirrors. The freq/pol
knobs are **host-sim-inputs**: client button presses are INTENT up; the host owns the offset/speed/
dir value that drives ITS sim and streams the animated offset back (unreliable, interpolated like
the cursor) for the mirror's display. Every mutator is `EX_Let`/`EX_Local*` → dispatch-invisible →
MIRROR STATE + SUPPRESS the local sim, never verb-hook
(`[[lesson-votv-world-system-sync-mirror-state-not-verb]]`,
`[[lesson-rng-in-rate-path-is-mechanic-desync]]`).
- RE + field-ownership table: `research/findings/computers-devices/votv-desk-download-machine-RE-2026-07-15.md`.
- RNG-authority row: `COOP_RNG_AUTHORITY.md` **T2-5b** (OPEN-DIVERGES, MEASURED).
- **TAKE (verify line):** host + client show the same `decoded %` and the same detector-needle
  position with identical freq/pol knobs, and the download HALTS on both when either knob is zeroed.

### OPEN-1 · Desk cursor degrades to ~5 fps mid-session
The v109 cursor is SMOOTH on a fresh session (user-verified) but has been observed dropping to ~5 fps
part-way through a session. Cause unattributed (not yet reproduced deterministically). Logs:
`scratchpad/desk_divergence_crash_{HOST,CLIENT_1}_0715.log`. Not a divergence — a rate/transport
degradation on the shipped stream.

### OPEN-3 · Upgrade-level sync (NOT a desk detail — its own surface; surfaced by OPEN-0 gate 2)
The freq/pol download formula reads upgrade fields (`upg_scanner`, `upg_downloadSpd`, filter-size) and
**there is NO live upgrade-sync lane in coop** (grep-confirmed 2026-07-15) — upgrades ride only the join
save-transfer (seeded once). So a mid-session filter-upgrade purchase diverges silently. The desk build
(OPEN-0) ROUTES AROUND this by streaming `frData`/`poData` host-authoritatively (6 scalars), so the desk
freq/pol screens converge regardless — **upgrade-sync is NOT needed for OPEN-0**. But upgrades are shared
state with ≥2 display consumers (desk freq/pol derivation + the PC screen), so this is its own workstream.
**Purchase-authority is UNKNOWN and gates the whole design:** if a CLIENT can initiate an upgrade
purchase, it is a client-event→host-authoritative write (the kerfur seam — dupe risk); if host-only, a
simple mirror. **RE the purchase path FIRST** (which fields, who writes, is the purchase `EX_Local*`?),
reconcile with the join-transfer seed, THEN `/qf` the ownership. Do not bolt onto a desk build.

### OPEN-2 · The console_state_sync divergence cluster (ONE root, five symptoms)
Per the 2026-07-15 stress test: MOST desk/console OUTPUT is generated per-peer, not holder-owned +
mirrored. Symptoms: (1) **coordLog** — `ProduceLogLines` runs on every peer and the `CR:` family is
filtered off the wire (host 78 vs client 13 lines; the v109 cursor fix WOKE the host's mirrored
cursor into appending local `MOVE_*` lines); (2) **decode/detector %** = OPEN-0; (3) **freq/polarity
filter values** on no wire lane = OPEN-0; (4) **dishes** aim/slew per-peer = OPEN item above;
(5) **stationary PC** power + screen not mirrored. The fix is shape-4 done properly — the holder/host
owns the sim + the log, non-holders mirror the full state and SUPPRESS local generation. Design +
`/qf` it as one root (`[[project-desk-console-sync-2026-07-15]]`).

---

## CHANGELOG
- **2026-07-15 (later)** — OPEN-0 AS-BUILT v111 (`desk_sim_sync` / `DeskSimPose=38`, proto 110→111,
  DLL `84e431bef0bd6982` deployed x4). `/qf` design pass (5 measurement rounds) collapsed a suspected
  host-auth sim-migration to a small fix; audit READY (0 CRITICAL). NOT hands-on. Surfaced OPEN-3
  (upgrade-sync, its own workstream). coordLog (OPEN-2) kept separate.
- **2026-07-15** — folder created; the signal-processing saga given a home (was scattered across
  `research/findings/computers-devices/` + `COOP_RNG_AUTHORITY` T2-5b + `COOP_SYNC_MAP`). Status
  reconciled to code: transport elements AS-BUILT/VERIFIED (cursor v109 SMOOTH, clock v110); the
  freq/pol + download SIM is the open gap (RE'd, divergence MEASURED, fix UNBUILT). Next: the
  download-machine host-authoritative `/qf`.
