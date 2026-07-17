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
| **Desk coords-panel cursor** | live cursor over the coord screen | 4 owner pose-stream | occupant | `DeskCursorPose 36` (`desk_cursor_sync`) | **AS-BUILT v115 `c5ff11a4`** (claim-DECOUPLED stream: momentum tail through release [settle 0.25px/500ms, cap 15s > the measured 12.4s max decay], no interp reset on release/flap, adaptive window [EMA 25..80ms] + identical-target dedupe, ZeroMovement at stream-start) — supersedes the v109 shape after the 2026-07-17 user reports (jerks + tail lost); awaiting hands-on. ⚠ OPEN-1 may be absorbed by the adaptive window — re-judge at the take |
| **Desk unit-1 AUDIO effects** | keypress clicks, verb/outcome beeps, coordFail, corrds_loop + pingLoop | presser-authored effect forward | whoever pressed (claim-free) | `DeskSndFx 105` (`desk_snd_fx` + `ue_wrap/desk_audio`) | **AS-BUILT v115 `c5ff11a4`** (Func-patch on the native audio seam — every whitelist-comp call site measured EX_VirtualFunction/native; loops join-re-asserted from bIsActive + host-owned leaver teardown; RULE-2: PlayScanEffects beep + PlayPingSuccess replay RETIRED; e2e self-test proven host->client in smoke 12:39; residual: PlaySound2D buttonshort14 + the laptop's own comps) — awaiting hands-on |
| **Desk alarm-clock** | HH:MM display, frozen host-mirror | 3 host-mirror | host | `ClockPose 37` + reliable connect-edge | **AS-BUILT** (v110 `2dde3e16`, NOT hands-on) |
| **Freq/polarity + download rate** | tune → download SPEED; per-peer sim + 2 RNG | 3 host-auth sim (outputs) + claim-free input deltas | host / presser | `DeskSimPose=38` 7ch + `DeskInput=97`/`DeskScanEvent=98` (`desk_sim_sync` + `desk_input_sync`) | **AS-BUILT v112** (the BUGS-v111 fix; smoke PASS) — awaiting hands-on |
| coord log lines (`CR:`/animated) | `ProduceLogLines` runs on EVERY peer | 4 holder-owned | occupant (planned) | partial (`CR:` filtered off wire) | **OPEN-2** (filter premise now MEASURED false for CR/APPROX/ANALYSIS) |
| Dishes rotation/kinematics (24 big) | rest pose = load-time RNG (unsaved); per-slew RNG; catch targets already synced | 3/4 (tbd) | host (planned) | — (`SkySignalCatch` covers targets only) | **OPEN-4** (RE'd 2026-07-16) |
| Stationary PC / laptop screen | portable-PC power + screen | 1/4 (tbd) | tbd | — | **OPEN** (screens gap-list #5) |
| U1 SHIFT quick-scan (arrows) | spawnDirs UMG arrows + beep + shared cooldown charge | 2 presser-event | presser | `DeskScanEvent 98` (arrows visual + charge, v112 `desk_input_sync` scan classification) + `DeskSndFx 105` (the beep, v115) | **AS-BUILT v112+v115** — was stale-OPEN here; code: `SendScan`/`OnDeskScan` + the fx lane; awaiting hands-on |
| U1 ENTER triangulation ping (the FSM) | latent tick machine gated on `coord_isPing` (@82980 -> @80105 stage engine; ==1.0 latches; verdict spawns signalData + moves the theater) | ONE machine = the presser's, organically; observers: bookkeeping only | presser (FSM) / host (theater+ARM, v113) | CoordIsPing rides `DeskInput 97` as an edge NOTIFICATION (never machine-applied since v115b); catch on `SkySignalCatch`; ARM on `DishArm 99` | **AS-BUILT v115b `de31889e`** — the v112 raw apply WOKE a phantom sim on observers (live-caught 2026-07-17 14:46: divergent verdicts, phantom ARM raising the mirrored detector, double coordLog authorship, false post-catch DISARM). Fix: bookkeeping-only + desk FSM-hold claim (device_occupancy reconciler; deny+ForceExit for others mid-ping) + arm-poll re-init window (DISARM suppressed while signalData lives) + solo-host connect seed (audit CRIT-1). Observers still see no stage visuals (R-a = the old R2 residual). Awaiting hands-on |
| U2 desk gauge/detector sounds | vol=match×\|speed\|, pitch=Lerp(match); loops on toggles | derived (needs speeds mirrored) | occupant/host | speeds on NO lane | **BUG-3** (data starvation) |
| U2 save/delete/lid verbs | SAVE→savedSignals_0 (id mint); DELETE aborts active signal; lid=collision gate | 2 intent | host-gate | `SavedSignalAppend/Delete 58/59` cover the list; capOpened/delete-active NOT | **PARTIAL** |
| U3 play deck (playback/volume/SARV/scroll) | world-audible signalSound; display remaps | 4 holder-owned (tbd) | occupant | — none | **OPEN-6** |
| Drive payload `prop_drive.Data_0` | moves list↔drive↔comp; eraser wipes; LED from payload | 2 intent / state-mirror (tbd) | host (tbd) | — none (`grep prop_drive` = 0 hits) | **OPEN-5** |
| driveSlot occupancy (slot FSM) | freeze+teleport, slot/drive pointers, anti-bounce latch | state-mirror (tbd) | tbd | — (generic prop pose only) | **OPEN-5** |
| signalDriveEraser | 3 s wipe → payload zeroed | 2 intent (tbd) | tbd | — | **OPEN-5** |
| U4 comp pane | RNG rate sim; level-up re-mints ID; upgrade gate | 4 single-sim | occupant | `CompState 60` / `CompData 61` | **AS-BUILT** (payload/slot halves = OPEN-5) |
| physMods (desk modules) | 12 slots; module prop destroyed on plug; hot-plug explosion; gates tape speed/level lamps/weather shield | state-mirror (tbd) | host (tbd) | — none | **OPEN-8** |
| Meadow DATABASE (`saveSlot.savedSignals_0/_comp_0`) | U3 SAVE copies rows in; laptop plays/deletes/moves | 2 intent (tbd) | host (tbd) | — (join save-transfer only) | **OPEN-9** |
| Tape caddy `wallunit_tapes` | 1 Hz accrual ×2 reels; reels ride props; speed from physMods | presser slot edges + host corrector | presser / host | `ReelSlot 102` + `ReelPose 40` (`tape_caddy_sync`) | **AS-BUILT v114** (smoke PASS, NOT hands-on) |
| Daily task `saveSlot.taskNew` | drone `sell` writes reel_big/small; day rollover regrades | 3 host-mirror | host (all writers host-only, census) | `TaskNewState 103` (`daily_task_sync`) | **AS-BUILT v114** (smoke PASS, NOT hands-on) |
| Red phone | per-peer RNG ring event; E does nothing | tbd (low) | — | — | **OPEN-low** |

Screens gap-list items beyond the signal desk (reactor rods, generator, SAT-console scrollback, TV,
laptop tasks, serverBox monitors) are tracked in the screens design finding, not here — they are the
generic device layer, not the signal pipeline.

---

## BUGS-v111 — the 2026-07-16 hands-on verdict (5 bugs, ALL root-caused)

> **FIX AS-BUILT (v112, same day):** all five designed out by the 12-round fix /qf
> (`votv-desk-input-lane-DESIGN-2026-07-16.md`) and BUILT: the claim-free field-granular
> `DeskInput=97` lane + `DeskScanEvent=98` + the 7-channel per-channel-exact-snap DeskSimPose +
> cooldown charge events + kPrefixes={CDOWN}. kProtocolVersion **112**, DLL `C4C1E69A…` deployed
> x4, autonomous LAN smoke PASS (45 s steady, logs clean, retired lanes silent). **NOT hands-on
> verified yet** — the take: client presses knobs/toggles at the download unit and they apply on
> the host + sounds play on both peers + 1/2/3 cooldown works on the client + SHIFT arrows appear
> on the mirror + detector 100% beeps once. The bullets below are the v111 diagnosis (historical).
**The axis fact:** the desk claim NEVER engages for the download unit's world-space buttons
(claim rides only the intComs `activeInterface` edge; physical +1/+5/+15/toggle presses don't set
it) → the whole v111 occupant-intent lane was dead during the test.

- **BUG-1 cooldown erased (client)** — the v111 `cooldown` stream channel raw-writes
  `coord_cooldown` every pump tick with the host's 0; both uplinks discard the client's charge
  (`console_state_sync.cpp:601` keep-local gate; `:206` unclaimed exclusion). Unwinnable race.
- **BUG-2 client knob input dead** — speed intents exist only on the (never-engaged) claimed
  lane; the client's local integrator output is overwritten by the stream. PLUS the reverse
  defect: `(dlDownloading>0)` flaps misclassified as input rebroadcast the client's stale zero
  scalars every ~3 s and stomp the HOST's knobs (`console_state_sync.cpp:217`).
- **BUG-3 sounds dead** — gauge volume ≡ match × |speed| and the speeds ride NO lane → mirror
  peers hear silence; the flap re-applies cut the host's envelope too. Data starvation, not a
  missing audio call.
- **BUG-4 detector stuck-beep (client)** — `SimInterp` never snaps to exactly 1.0 (window
  reopened by every 10 Hz packet; sub-ulp increments freeze just under 1.0), the client's own
  UNSUPPRESSED detector block re-crosses the threshold every frame → `beep_detecFinish` loops.
- **BUG-5 SHIFT scan + coordLog lines unmirrored** — quick-scan is on no lane at all; the
  `CR:`/`APPROXIMATION:`/`ANALYSIS:` log families are filtered off the wire on a premise
  ("regenerate from mirrored scalars") that is MEASURED false — they gate on input bools /
  `coord_pingStage` that never mirror (`console_state_sync.cpp:328-335`).

Fix pass = its own `/qf 15` (user green-light pending). The claim-model axis fact suggests the
fix is architectural, not five patches
(`[[feedback-recurring-bug-is-architectural]]`).

## OPEN items — detail

### OPEN-0 · Freq/polarity + download-rate SIM — AS-BUILT v111; hands-on 2026-07-16 FAILED → BUGS-v111
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
- RNG-authority row: `COOP_RNG_AUTHORITY.md` **T2-5b** (AS-BUILT v111; hands-on 2026-07-16 FAILED → BUGS-v111).
- **TAKE (verify line):** host + client show the same `decoded %` and the same detector-needle
  position with identical freq/pol knobs, and the download HALTS on both when either knob is zeroed.

### OPEN-1 · Desk cursor degrades to ~5 fps mid-session
The v109 cursor is SMOOTH on a fresh session (user-verified) but has been observed dropping to ~5 fps
part-way through a session. Cause unattributed (not yet reproduced deterministically). Logs:
`scratchpad/desk_divergence_crash_{HOST,CLIENT_1}_0715.log`. Not a divergence — a rate/transport
degradation on the shipped stream. **v115 UPDATE (2026-07-17): the user re-reported jerkiness
mid-v114-hands-on; two mechanisms were code-measured and removed (claim release/flap = interp
reset+snap; same-pos-new-seq packets reopening the fixed 33ms window = staircase at sender-fps
dips) — the v115 adaptive window + dedupe + claim-decoupling may close this OPEN entirely;
re-judge at the v115 take (a same-slot claim-flap WARN now attributes any residual).**
**v115b UPDATE (2026-07-17 eve): two MORE mechanisms attributed + removed with the phantom
ping-FSM fix (`de31889e`): the phantom was a PER-TICK uber machine running on the observer during
any ping, and both peers double-authored the right-screen console lines (measured 220+93
lines/min crossing simultaneously) = the "messages jerky like the cursor" report. Also MEASURED
CLEAN: the observer's apply cadence (desk_cursor applying-line spacing steady 5 s = 60 Hz through
the whole live session incl. the jerk window) — the apply side is exonerated. Residual lead: the
smoke env showed client `[WALK-TIME] sync:npc_client ~30 ms/tick` + HITCH-SRC net_pump ~30 ms
steady — a per-tick walk that heavy grows with NPC count = "jerky over time"; NOT measurable in
the live logs (that instrumentation was off there). Re-judge at the take; if jerks persist,
enable the WALK-TIME instrumentation and read `sync:npc_client` first.**

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
filter values** on no wire lane = OPEN-0; (4) **dishes** aim/slew per-peer = OPEN-4;
(5) **stationary PC** power + screen not mirrored. The fix is shape-4 done properly — the holder/host
owns the sim + the log, non-holders mirror the full state and SUPPRESS local generation. Design +
`/qf` it as one root (`[[project-desk-console-sync-2026-07-15]]`).
**2026-07-16 update [MEASURED]:** the `IsAnimatedLogLine` filter premise is now measured FALSE for
`CR:`/`APPROXIMATION:`/`ANALYSIS:` (they gate on `move_*` input bools / `coord_pingStage`, which
never mirror — only `CDOWN:` is regenerable from streamed state). The buffer is
`coord_coordLog2Text @0x0BF8` via `writeToCoordLog_2` (append, Right-capped 1000 chars; the
sibling `writeToCoordLog`/`coord_coordLogText` are DEAD code). = BUG-5's second half.

> **DESIGN CONVERGED for ALL of OPEN-4..9 (2026-07-16, /qf 9 rounds "that holds"):**
> `research/findings/computers-devices/votv-signal-chain-all-units-DESIGN-2026-07-16.md` —
> presser-authored state broadcasts + host-adopted sims + VM-bracket capture (HALT-gated on the
> 0x45 frequency probe); build order v112 → probe → L4 → L7 → L6 → L8 → L5 → L9. The BUGS-v111
> fix design converged separately (12 rounds): `votv-desk-input-lane-DESIGN-2026-07-16.md`
> (v112: claim-free DeskInput lane; impl in progress). The per-lane sections below remain the
> FACT base; the design doc is the plan of record.

### OPEN-4 · Dish rotation/kinematics (24 big dishes) — **BUILT v113 (2026-07-16 night; smoke PASS, NOT hands-on)**

> **AS-BUILT (commit `f204c0f7`, proto 113, DLL `A1C13DB9108775DC` x4):** impl design
> `votv-dish-L4-impl-DESIGN-2026-07-16.md` (10-round /qf "that holds") built as designed:
> `coop/interactables/dish_sync` — client tickers parked (live-checked restores on the teardown
> fanout) + own-ping slews killed-with-cleanup by the reworked catch detector (coord identity-tuple
> change-edge; MovingCount signature retired); HOST streams DishPose=39 (movers 4 Hz + settle-tail);
> ONE ApplyDishRow (stream + DishSnapshot=100 join seed); ARM axis single-author DishArm=99 with
> HOST polarity (mesh|FName|polarity 4 Hz raw poll; client apply = pre-clear -> reflected
> checkFordDishes -> Arm); DishCalib=101 symmetric diff-poll batches; v70 pending-adopt RETIRED
> end-to-end (DeskState 60->52 B). Smoke evidence: client parked both tickers, snapshot 24/24,
> zero lane WARN/ERROR. **NOT yet hands-on** (and the v112 hands-on verdict is still pending —
> combined v113 take or per-lane logs). Residuals + backlog in the design doc §"Accepted residuals".

Legacy fact base (RE, still authoritative for the mechanics):
RE: `votv-dish-rotation-RE-2026-07-16.md` + **impl-gap addendum
`votv-dish-impl-RE-2026-07-16.md` (2026-07-16 eve, 5-agent pass)**: components/write path
(axis_Z=Yaw, axis_Y=ROLL, relative frame; K2_SetRelativeRotation precedent), stop()/kill stale
set (audio cues + `activeDishes[i]` — the OnKeyDown ping-block gate makes it load-bearing),
mirror-vs-live-loop = arrival STARVATION (never co-write), ticker kill/restore seams
(ClearTimer "do" / TickEnabled), pak-wide census (slew starters = desk ping + ticker_disher
ONLY; axis writers = dish uber ONLY), status-UI chain (desk 20 Hz pusher; getDir() live —
mirrored pose feeds the panel free), arm chain (dishesStop handler = formDownload(0,-1) only;
checkFordDishes tail gamemode-inline; **polarity per-peer RNG at arm(-1)**; begin() NOT
cosmetic — RT + camera + triggers + prop_argm spawn hazard), calibrate-terminal = client-side
per-frame calibration writer. Open design decisions listed at the addendum's tail (own-ping
kill-vs-let-run, ARM transport, begin() limbs, calibration authority, park cleanup set).
Pre-L4 divergence picture (FIXED by the build above; kept for context):
catch TARGETS synced (`SkySignalCatch` replays
`StartMovingAll`); everything else diverged: **rest pose is per-peer RNG at BeginPlay
(`randrot`: yaw 0-360, pitch 90-135) and orientation is NEVER SAVED** (no save channel to fight);
per-slew RNG (MaxSpeed 4.5-5.5, start delay 1-12 s, phase delays, calibration decay per frame);
the `isMoving` gate silently DROPS a catch target on a dish already slewing (ticker_disher fires
per-peer); the download-arming `dishesStop` edge fires at each peer's own last-dish time.
Ambient tickers (`ticker_disher` timing, `ticker_dishUncalib` target pick) are per-peer RNG →
RNG-authority rows. Identity for logging: `Index` + `techName` (NATO from Bravo, no Alpha) +
`Key`. User ask 2026-07-16: per-dish logging (cls+key+loc rule).

### OPEN-5 · The drive chain (payload + slot + eraser) — RE'd, zero lanes
`prop_drive.Data_0 @0x550` (the signal payload incl. the identity string) is mutated by FOUR
verbs (deck import/export, comp swap, eraser wipe, level-up) — all EX_Local*/inline, NO wire
lane (`grep prop_drive src/votv-coop` = 0). The mirror drive keeps a stale payload → wrong
LED/hover AND a later interaction on the other peer acts on stale data (dupe/loss risk on the
move semantics — the list row was deleted on one peer only). `driveSlot` FSM (freeze+teleport,
`drive.slot` pointer, `isRecentlyDetached` latch) also unsynced — insertion crosses the wire only
as a generic prop move. The eraser adds a 3 s `processing` latch + the wipe. **Identity note:**
the signal `id` is minted per-peer (`GenerateRandomBytes(16)`) at U2-save and RE-MINTED at every
comp level-up — do NOT design any coop identity on it without a host-authority pass.

### OPEN-6 · U3 play deck outputs
`signalSound` is a WORLD-audible component (`SetSound(level-variant)` + Activate) — a nearby
non-occupant natively hears nothing; playback state (`play_selectIndex`, volume, SARV
`remapValue`, spectrum/typewriter panes) is per-desk local. Scroll/select is occupant-authored.
Needs the holder-owned shape; low risk, but the audible half is player-visible.

### OPEN-7 · Tape caddy + daily task — **BUILT v114 (2026-07-17; smoke PASS, NOT hands-on)**

> **AS-BUILT (commit `ba8ce297`, proto 114, DLL `13874C48D3D7F220` x4):** impl design
> `votv-tape-caddy-L7-impl-DESIGN-2026-07-17.md` (9-round /qf "that holds") built as designed:
> `coop/interactables/tape_caddy_sync` (ReelSlot=102 presser sentinel edges 4 Hz + ReelPose=40
> host 1 Hz corrector; client accrual NOT parked — written park-doctrine deviation, upd()
> makes a park un-holdable) + `coop/world/daily_task_sync` (TaskNewState=103 host change-hash
> mirror; every live writer host-only by census) + the savedScalar identity-at-birth channel
> (PropSpawn/_pad2 + flag 0x40; PropDropIntent 168->172 — BOTH intent kinds carry it) +
> ReelEjectIntent=104 (client-eject birth via the F2 author, reel-class whitelist). The
> `active` toggle stays on the ApplianceState lane. Residuals R1-R5 named in the design doc
> (R1: client-loaded sack contents can't grade until the sack-contents lane). Legacy fact
> base below (still authoritative for the mechanics).
RE: `votv-tape-caddy-daily-task-RE-2026-07-16.md`. The wallunit is the concreteBucket class
(`COOP_WORLD_PROP_DIVERGENCE`): 1 Hz `+= dt/speed` accrual per peer on both reels; `speed`
written cross-object by desk physMods (byte 21); eject/insert/toggle local-only (the reel PROPS
do cross the generic spawn/destroy seams). The daily grade rides `saveSlot.taskNew.reel_big/
small` via drone `sell` (reads struct_save snapshots; reward only if BOTH > 0) + the day-rollover
`processTask`. Host-owned prop sim + taskNew mirroring wanted; join transfer already seeds both.

### OPEN-8 · physMods (desk physical modules)
12 slots; plug destroys the module prop (generic destroy lane covers the prop only); the ARRAY +
its consumers (tape speed byte 21, level lamps + auto-continue byte 3, laptop auto-forward byte
5, weather shield byte 6) + the `explotano` hot-plug explosion are unsynced. Gates OPEN-7 speed
and U4 behavior.

### OPEN-9 · Meadow DATABASE stores
`saveSlot.savedSignals_0 @0x680` (DATABASE tab) + `savedSignals_comp_0 @0x690` (processed DB) get
rows from U3 SAVE (`laptop.addSignal`, EX_Local-invisible) and laptop-side delete/move — join
save-transfer only, no live lane. Same intent-CRDT shape as the shipped
`SavedSignalAppend/Delete 58/59` (which cover the GAMEMODE list only).

---

## CHANGELOG
- **2026-07-16** — the user walked the WHOLE downstream chain hands-on + reported 5 v111 bugs; a
  7-agent RE pass covered units 1-4 byte-level + the drive chain + eraser + tape caddy + the 24
  dishes; ALL 5 bugs root-caused to file:line (**BUGS-v111** section; the claim-model axis fact).
  New findings: `votv-signal-chain-units-RE-2026-07-16.md`,
  `votv-dish-rotation-RE-2026-07-16.md`, `votv-tape-caddy-daily-task-RE-2026-07-16.md`,
  `votv-desk-sim-v111-coop-bugs-audit-2026-07-16.md`. The 07-15 download RE §C SHIFT claim
  CORRECTED (SHIFT=quick-scan; the stage FSM belongs to ENTER; key router =
  `ui_consolesAtlas.OnKeyDown`). New rows OPEN-4..9. Master-table v111 row flipped AS-BUILT →
  hands-on FAILED. NEXT: the fix `/qf 15` over BUGS-v111 (claim-model axis first), then the
  downstream lanes.
- **2026-07-15 (later)** — OPEN-0 AS-BUILT v111 (`desk_sim_sync` / `DeskSimPose=38`, proto 110→111,
  DLL `84e431bef0bd6982` deployed x4). `/qf` design pass (5 measurement rounds) collapsed a suspected
  host-auth sim-migration to a small fix; audit READY (0 CRITICAL). NOT hands-on. Surfaced OPEN-3
  (upgrade-sync, its own workstream). coordLog (OPEN-2) kept separate.
- **2026-07-15** — folder created; the signal-processing saga given a home (was scattered across
  `research/findings/computers-devices/` + `COOP_RNG_AUTHORITY` T2-5b + `COOP_SYNC_MAP`). Status
  reconciled to code: transport elements AS-BUILT/VERIFIED (cursor v109 SMOOTH, clock v110); the
  freq/pol + download SIM is the open gap (RE'd, divergence MEASURED, fix UNBUILT). Next: the
  download-machine host-authoritative `/qf`.
