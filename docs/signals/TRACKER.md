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
| Signal catch | dish catch → `coord_signalData` = truth | 2 intent→host | host | `SkySignalCatch` (`signal_catch_sync`) | **AS-BUILT v116 `613f2ac4`** — the v70-v115 claim-gated detector LOST the 17:04 live catch (the successful ping's own completion releases the FSM-hold in the same second; the 1 Hz claim-gated poll raced it and the baseline roll-forward ate the edge PERMANENTLY -> host NO SIGNAL / frozen host dishes / client 24-dish self-slew). v116 (RULE 2): detector + NoteIncomingSnapshot + host holder-validator gates RETIRED (the UNPRIMED change-edge is the authority; writer set grep-enumerated), host IsRecent dup guard, kind=2 connect STATE-SEED (feed-silent), settled-dish lookAt slew fallback, catch -> activity feed (peer_action_feed, one line per peer). Smoke x3, awaiting hands-on take 4 |
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
| Stationary PC (laptop_C) power + floppy | ACTIVATE boot/shutdown (isOpened, fused latent chain) + floppy insert/eject (insert DESTROYS the disc into laptop scalars; eject SPAWNS a fresh one) | presser edges + host content authority | presser (edges) / host (disc content) | `LaptopState 106` (`laptop_sync` + `ue_wrap/devices/laptop`) | **AS-BUILT v116 `613f2ac4`** — power = isOpened edge + native actionOptionIndex(b8) replay under want-target predicate; floppy = 4 Hz slot edges, ATOMIC scalars+content apply at chunk assembly (audit IMPORTANT-1), content chunked 192 B/4 KB; disc destroy rides the v106 K2_DestroyActor seam (one owner), eject spawn rides birth channels; DiscContent by eid (adoption-binding correlation for client ejects; ground-truth rows at join). RE `votv-laptop-pc-RE-2026-07-17.md`. Smoke x3, awaiting hands-on. **v121: op=4 chunker RETIRED -> LaptopBlob (blob_chunks); + lid op=6; buffer quad + floppyBox = OPEN-10 BUILT (see below)** |
| U1 SHIFT quick-scan (arrows) | spawnDirs UMG arrows + beep + shared cooldown charge | 2 presser-event | presser | `DeskScanEvent 98` (arrows visual + charge, v112 `desk_input_sync` scan classification) + `DeskSndFx 105` (the beep, v115) | **AS-BUILT v112+v115** — was stale-OPEN here; code: `SendScan`/`OnDeskScan` + the fx lane; awaiting hands-on |
| U1 ENTER triangulation ping (the FSM) | latent tick machine gated on `coord_isPing` (@82980 -> @80105 stage engine; ==1.0 latches; verdict spawns signalData + moves the theater) | ONE machine = the presser's, organically; observers: bookkeeping only | presser (FSM) / host (theater+ARM, v113) | CoordIsPing rides `DeskInput 97` as an edge NOTIFICATION (never machine-applied since v115b); catch on `SkySignalCatch`; ARM on `DishArm 99` | **AS-BUILT v115b `de31889e`** — the v112 raw apply WOKE a phantom sim on observers (live-caught 2026-07-17 14:46: divergent verdicts, phantom ARM raising the mirrored detector, double coordLog authorship, false post-catch DISARM). Fix: bookkeeping-only + desk FSM-hold claim (device_occupancy reconciler; deny+ForceExit for others mid-ping) + arm-poll re-init window (DISARM suppressed while signalData lives) + solo-host connect seed (audit CRIT-1). **v116 `613f2ac4` closed the SECOND defect the same FSM-hold exposed**: the catch detector's claim gate raced the hold's release (see the Signal-catch row). Observers still see no stage visuals (R-a; SURFACED to the user 2026-07-17 as a product question — display-only mirror if wanted). Awaiting hands-on take 4 |
| U2 desk gauge/detector sounds | vol=match×\|speed\|, pitch=Lerp(match); loops on toggles | derived (needs speeds mirrored) | occupant/host | speeds on NO lane | **BUG-3** (data starvation) |
| U2 save/delete/lid verbs | SAVE→savedSignals_0 (id mint); DELETE aborts active signal; lid=collision gate | 2 intent | host-gate | `SavedSignalAppend/Delete 58/59` cover the list; capOpened/delete-active NOT | **PARTIAL** |
| U3 play deck (playback/volume/SARV/scroll) | world-audible signalSound; display remaps | presser-authored edge events (seam) | presser (any peer may stop) | `PlayDeckEvent 107` (`deck_play_sync`) + volume/index/toggle on `DeskInput 97` (v112) | **AS-BUILT v117 (2026-07-18)** — L6 built per `votv-deck-play-L6-impl-DESIGN-2026-07-18.md` (7-round /qf "that holds"): detection at the v115 audio Func seam (signalSound Activate x1 = playSignal / Deactivate x1 = stopSound, whole-asset census; Deactivate = the 4th Func patch), GEN GUARD decouples correctness from fin()'s inferred PE visibility, mirrors replay reflected playSignal/stopSound under the shared wire guard, selectIndex rides the v112 apply author. SARV/scroll/volume were ALREADY on DeskInput (stale-open half of this row). Smoke x2 + e2e self-test chain proven; audits 0 CRIT; NOT hands-on (the audio positive = the take). Residuals a-e in the design doc |
| Drive payload `prop_drive.Data_0` | moves list↔drive↔comp; eraser wipes; LED from payload | state-mirror | writer-authored, host store | `DrivePayload 110` | **AS-BUILT v119** |
| driveSlot occupancy (slot FSM) | freeze+teleport, slot/drive pointers, anti-bounce latch | state-mirror | any-peer lines, host canonical | `DriveSlotState 109` | **AS-BUILT v119** |
| signalDriveEraser | 3 s wipe → payload zeroed | poll-detected (verbless wipe) | eraser-side peer | `DrivePayload 110` | **AS-BUILT v119** |
| U4 comp pane | RNG rate sim; level-up re-mints ID; upgrade gate | 4 single-sim | occupant | `CompState 60` / `CompData 61` | **AS-BUILT** (payload/slot halves = v119 `DrivePayload 110`/`DriveSlotState 109`) |
| physMods (desk modules) | 12-slot SET (byte unique); plug destroys the held module; UNPLUG (hit the socket) rebirths it into the hand; hot-op in EITHER direction explodes (presser-local, measured); gates tape speed/lamps/laptop-fwd/weather shield | value-ops + host-canonical array | host (canonical) / presser (ops) | `PhysModsState 108` (`physmods_sync` + `ue_wrap/desk/phys_mods`) | **AS-BUILT v118 (2026-07-18)** — L8 per `votv-physmods-L8-impl-DESIGN-2026-07-18.md` (8-round /qf "that holds"; the arch's slot-deltas REVISED to value-ops + canonical — the SET fact makes layout divergence structurally impossible): 1 Hz diff poll + drain-before-adopt; deny/refund op (dup plug refunded, raced unplug ghost swept + birth reaped); client unplug births via the widened kind-104 whitelist (IsChildOf(Aprop_physModule_C)); consume rides the bidirectional destroy seam (zero lane code). Smoke PASS + join canonical proven; NOT hands-on |
| Meadow DATABASE (`saveSlot.savedSignals_0`; `_comp_0` = deck save mirror, no lane) | U3 SAVE copies rows in; laptop deletes/moves (order SYNCED, user rule-1) | content-hash multiset + host-canonical order | peer-symmetric rows / host-canonical order | `MeadowAppend 112`/`MeadowDelete 113`/`MeadowOrder 114` (`meadow_db_sync` + `ue_wrap/desk/meadow_store`) | **AS-BUILT v120 (2026-07-19)** — smoke x2 + e2e digest 0->1->0 cross-peer, NOT hands-on |
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
**v116 UPDATE (2026-07-17 nite): the live-env measurement noise is now REMOVED and the missing
instrument ADDED — the deployed inis had a closed-measurement diag battery still ON (kerfur_census=1:
a full ~281k-object GUObjectArray walk measured 8-25 ms EVERY 10 s inside sync:npc_client, + 6 more
probes); all flipped off in all 4 installs, desk_diag KEPT (the proven 1 Hz discriminator), and the
HOST gained perf_probe=1 — host fps was never measured in any prior take (the observer of the mirror
cursor IS the host). The npc_client ~30 ms/tick walk reproduced in the v116 smoke WITH census OFF ->
the walk itself (mirror Tick loop) is the remaining named suspect, smoke-env-conditioned. Attribution
set for take 4: host [perf] frames + desk_diag 1 Hz pacing + desk_cursor ema.**

### OPEN-3 · Upgrade-level sync (NOT a desk detail — its own surface; surfaced by OPEN-0 gate 2)
> **RE'd + design in `docs/upgrades/SIGNAL_UPGRADES.md` (2026-07-21).** The upgrade domain now has its
> own home `docs/upgrades/`. Measured: the levels are ONE persistent struct `Fstruct_upgrades` (18 int32),
> 20 laptop-shop `uicomp_upgradeSlot_C` slots, parametrizing the download/ping/coord/comp/radar/detector
> sims. Design of record: mirror `Fstruct_upgrades` host->client; buying = client intent -> host validates
> research points -> broadcasts. The exact purchase path (is it `EX_Local*`?) is still the [?] to read.
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

### OPEN-5 · The drive chain — **BUILT v119 (2026-07-18/19 night; smoke x2 + connect-seed payloads proven, NOT hands-on)**

> **AS-BUILT:** `coop/interactables/drive_sync` + `coop/interactables/drive_rack_sync` (RackState extracted 2026-07-18, `73dc9ba1`) + `ue_wrap/desk/drive_chain` per
> `votv-drive-chain-L5-impl-DESIGN-2026-07-18.md` (7-round /qf + 4 build deviations, all
> evidence-driven — see the doc's BUILD OUTCOME). Three lanes: DriveSlotState=109 (idempotent
> any-peer slot FSM lines, host canonical; the deterministic eject-latch completion),
> DrivePayload=110 (data_0 rows via the v65 signal_wire codec sans image; 0x45 dirty-marks +
> 1 Hz diff poll; authored-birth broadcast-at-adoption), RackState=111 (prop_driveRack 16-row
> storage: index ops -> host + canonical + CONTENT-correlated deny/reap). The pre-build text
> below is kept as fact-base context.

#### (pre-L5 fact base, as it read before v119)
`prop_drive.Data_0 @0x550` (the signal payload incl. the identity string) is mutated by FOUR
verbs (deck import/export, comp swap, eraser wipe, level-up) — all EX_Local*/inline, NO wire
lane (`grep prop_drive src/votv-coop` = 0). The mirror drive keeps a stale payload → wrong
LED/hover AND a later interaction on the other peer acts on stale data (dupe/loss risk on the
move semantics — the list row was deleted on one peer only). `driveSlot` FSM (freeze+teleport,
`drive.slot` pointer, `isRecentlyDetached` latch) also unsynced — insertion crosses the wire only
as a generic prop move. The eraser adds a 3 s `processing` latch + the wipe. **Identity note:**
the signal `id` is minted per-peer (`GenerateRandomBytes(16)`) at U2-save and RE-MINTED at every
comp level-up — do NOT design any coop identity on it without a host-authority pass.

### OPEN-6 · U3 play deck outputs — **BUILT v117 (2026-07-18; smoke x2 + e2e self-test, NOT hands-on)**

> **AS-BUILT:** `coop/interactables/deck_play_sync` per
> `votv-deck-play-L6-impl-DESIGN-2026-07-18.md` (7-round /qf; the arch doc's 250 ms poll
> REVISED to seam detection per its own tier rule; + the gen guard). The row above is the
> as-built truth. SARV `remapValue` remains per-desk display-local (pure contrast remap, no
> signal data — deliberately unsynced; spectrum jitter per-peer cosmetic). Legacy fact base:
`signalSound` is a WORLD-audible component (`SetSound(level-variant)` + Activate) — a nearby
non-occupant natively heard nothing; playback state panes are rebuilt by the mirror's own
playSignal replay from the synced deck row + static DataTable. Scroll/select/volume ride
DeskInput (v112).

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

### OPEN-8 · physMods (desk physical modules) — **BUILT v118 (2026-07-18; smoke PASS + join canonical proven, NOT hands-on)**

> **AS-BUILT:** `coop/interactables/physmods_sync` + `ue_wrap/desk/phys_mods` per
> `votv-physmods-L8-impl-DESIGN-2026-07-18.md` (8-round /qf; 4 self-caught corrections incl.
> the UNPLUG-path reframe — "modules permanent" was false — and the explosion-damage-path
> retraction). The master-table row is the as-built truth. Consumers (tape speed byte 21,
> lamps/auto-continue 3, laptop-fwd 5, weather shield 6) converge via the mirrored
> updPhysMods (measured a pure function of the array). The OPEN-7 speed gate is now fed.
Legacy: 12 slots; plug destroys the module prop; the explotano hot-op explosion is
presser-local natively (explosion_C targets the local player only — measured).

### OPEN-10 · Laptop v2 — **BUILT v121 (2026-07-18, NOT hands-on)**
Design of record: `votv-laptop-v2-OPEN10-impl-DESIGN-2026-07-18.md` (11-round /qf "that holds").
Two of the old premises were MEASURED FALSE at design time: `floppyTypes/floppyData` belong to
`prop_floppyBox_C` (the disc crate), not the portable PC; and the portable PC is a REMOTE
TERMINAL to the base laptop (`bindPC(gamemode.laptop.laptop)`) whose screen converges FOR FREE
via v116 — so the shared "laptop" claim key is CORRECT and the claim-discrimination question
DISSOLVED. Shipped: (A) `laptop_buffer_sync.cpp` — the buffer QUAD {floppyData, floppyBuffer,
floppyBufferUIDs, rw} as client edit-script batches (measured no-move grammar) -> host
content-anchored apply -> unconditional post-batch canonical (the ack), eager widget rebuild
(bufferSlots teardown + genFloppyBuffer + updFloppy — measured: updFloppy REGENERATES
floppyBuffer from bufferSlots), int pre-filter idle path; (B) portable-PC LID op=6 in
laptop_sync (1 Hz element sweep, reflected Open() re-applier, join rows); (C)
`floppybox_sync.cpp` — the crate's LIFO arrays as push/pop value-ops + host canonical
(RackState shape), pop-DENY -> reap-if-alive/skip-if-consumed. The v116 op=4 custom chunker
RETIRED (RULE 2) -> blob_chunks on the sibling kind LaptopBlob; LaptopStatePayload shrank to
16 B. Kinds LaptopBlob=115/LaptopQuad=116/FloppyBoxState=117, proto 121. Audits folded: perf
0 CRIT (digest pre-filter, incremental shadow-advance-on-accept, sweep gate); correctness
CRIT-1 (56 KB blob cap + ignored canonical send results -> bounded pack + retry arms) +
IMPORTANT-2 (per-sender op=1 park map). Smoke x3 PASS + BOTH selftest circles proven
cross-peer (host organic + client derivation; matching raw+widget digests). Residuals: the
4 KB slot-content cap (OPEN-9 class) + the canonical transport tail-drop WARN; buffer-UID
cross-pair on duplicate-content rows (content-identical, canonical heals).

### OPEN-9 · Meadow DATABASE stores — **BUILT v120 (2026-07-19, NOT hands-on)**
Design of record: `votv-meadow-db-L9-impl-DESIGN-2026-07-19.md` (15-round /qf "that holds").
Scope narrowed by measurement: `savedSignals_comp_0 @0x690` = the DECK list's SAVE MIRROR
(saveObjects) — NO lane; L9 = `saveSlot.savedSignals_0 @0x680` alone. `meadow_db_sync.cpp` +
`ue_wrap/desk/meadow_store`: content-hash MULTISET shadow (v65's positional walk + pointer
RowKeys both INVALID here — sortSignal moves deep-copy FStrings), pre-gated 1 Hz poll (count +
scoped 0x45 marks), id-PRESERVING reflected `ui_laptop.addSignal`/`removeSignal` applies (the
arch's "never addSignal replay" claim was WRONG — corrected), tombstone counts, symmetric
per-slot join seed `seedDelta(h)=cur-snap-unmaskedPendingNet` (snapshot at save_transfer
OnRequest blob instant; closes the [snapshot, world-ready] permanent-loss window), client
send gate until own ClientWorldReady. MeadowAppend=112/MeadowDelete=113/MeadowOrder=114
(one FIFO lane; order sends deferred while lines pend), proto 120. ORDER SYNCED
(mid-build user decision per rule 1): order = state, host-canonical (RackState shape),
sortSignal = 3rd 0x45 matcher, byte-permute + reflected genSignalList apply, seed ships
the canonical order. Audits: perf 1 CRIT (60 Hz pre-world FindClass) + order-lane 1 HIGH
(order overtaking a pending line) — both root-fixed pre-commit. Smoke x2 PASS on
`452973c707d9cb8d` x4 + e2e selftest digest 0->1->0 proven cross-peer (matching sums;
the first run also exercised pending+retry through the B2 gate live). NOT synced (user
pile): image bytes, wire-delete playback-stop.

---

## CHANGELOG

- **2026-07-21 (TAKE-4 HANDS-ON RAN — many "awaiting hands-on" rows now have results, and they have
  BUGS)** — the batched take-4 hands-on (proto 122, DLL `96719c4230d531ba`) exercised the whole
  workstation. Full triage: `research/findings/computers-devices/votv-take4-hands-on-bugs-2026-07-21.md`
  (21 symptoms -> 16 roots). Rows whose "awaiting hands-on" status is now SUPERSEDED by measured bugs:
  **Dish rotation** (R1: pose is a 4 Hz `K2_SetRelativeRotation` HARD SNAP, no interp -> ~5 fps stepping,
  `dish_sync.cpp:145`); **Freq/pol + download / DeskInput** (R2 250 ms net-delta poll aliases fast
  toggles; R4 SYNCHRONIZED 2 Hz client flicker of GUI + unit-2 lamps = suspected DeskInput ping-pong echo;
  R13 client SAVE SIGNAL does nothing — saveable state host-authored); **Desk AUDIO** (R5 no sound on a
  client polarity toggle); **Saved signals / comp** (R17 export/import list lags — `signal_sync.cpp:27`
  + `comp_sync.cpp:29` poll at 1000 ms); **DishAimState** (R3 triangulation not visible on host — H);
  **Stationary PC** (R7 power syncs very slowly — 250 ms poll + converge-by-toggle; R8 RT-screen TAB
  switching has NO lane at all). Non-signal roots in the same run: garage `garageDoor` client-open dropped
  as expired (R9), drone red light not synced (R10) + delivery sack contents empty on client (R11), the
  DRIVE-DISC data-loss cluster (R14/15/16: `prop_drive_C` content not bound to birth + identity churns per
  interaction). CONFIG: `vitals_keepalive_sec` was left at 180 in the deployed host ini -> set 0 (needs a
  restart). Each root gets its own `/qf` next; NO fixes shipped this session.

- **2026-07-18 (v121, OPEN-10 laptop v2)** — buffer quad + portable-PC lid + floppyBox built
  per an 11-round /qf (design `votv-laptop-v2-OPEN10-impl-DESIGN-2026-07-18.md`). TRACKER's
  own OPEN-10 premises corrected by measurement (floppyTypes/floppyData = the CRATE's; the
  portable PC = a remote terminal -> claim-discrimination dissolved). Three new kinds
  (LaptopBlob=115/LaptopQuad=116/FloppyBoxState=117), LaptopStatePayload 216->16 B (op=4
  chunker retired, RULE 2), proto 121. Perf 0 CRIT + correctness CRIT-1/IMPORTANT-2 folded;
  smoke x3 PASS with BOTH e2e selftest circles (host organic + client derivation) digest-proven
  cross-peer. NOT hands-on.
- **2026-07-19 night (v119, L5 drive chain)** — the 0x45 HALT gate run + PASSED (gnatives_probe
  v4: desk/drive/laptop matchers, upd REJECTED ~238/s ambient, SIZES offsets [V] but 0 rows in
  s_1234 -> L9 image size unmeasured, blob_chunks mandated). L5 built per its own 7-round /qf
  (idempotent slot-state lines + host canonical REVISING the arch's authored events; eject
  ordering RETRACTED to deterministic latch-completion; the RACK discovered IN scope — the L8
  canonical shape on 0x70 rows; the slotted-latch satisfied by the standing frozen-pose gate).
  DriveSlotState=109/DrivePayload=110/RackState=111, proto 119; kind-104 whitelist widened to
  drives. Smoke x2 PASS + connect-seed payloads proven cross-peer in-log; audits: perf 0 CRIT
  (F-1/2/3/6/7 folded), correctness 1 CRIT (stale pending-replay -> per-role coalesce +
  baseline-stash drop) + 1 MAJOR (slot-only reap -> CONTENT-correlated taken-ring/payload-hash
  reap; HostShouldReapDriveBirth retired) — both fixed pre-handoff. drive_sync.cpp 1007 LOC >
  soft cap: rack extraction DONE 2026-07-18 (`73dc9ba1`, drive_rack_sync 553; digest-equality
  proven -- votv-rack-extraction-DESIGN-2026-07-18.md). OPEN-5 flipped AS-BUILT.
- **2026-07-18 (v118 `45a886a4`, L8 physMods)** — L8 built per its own 8-round /qf
  (value-ops + host-canonical array REVISING the arch's slot-deltas; the unplug path
  discovered — the R1 reframe; the explosion measured presser-local — an inference
  retracted). PhysModsState=108, proto 118; the kind-104 client-birth whitelist widened to
  desk modules. Smoke x2 PASS + the joiner canonical hand-off proven in-log; BOTH audits converged on ONE
  critical (the host self-op misroute) -- fixed pre-handoff. OPEN-8 flipped AS-BUILT.
- **2026-07-18 (v117 `c077e910`, L6 deck playback)** — the event_dispatch_signal.cpp
  extraction landed first (e88cc5e0: the 18 signal-pipeline cases out of the 791-LOC state
  router; every future lane case lands in the signal family). Then L6 built per its own
  7-round /qf (design `votv-deck-play-L6-impl-DESIGN-2026-07-18.md`): PlayDeckEvent=107,
  proto 117, the 4th Func patch (Deactivate), the gen guard, the fin PE bracket. Smoke x2
  PASS + the e2e self-test chain proven end-to-end (organic mint -> wire -> gate-WARN ->
  stale-stop drop); perf audit PASS (ambient Deactivate ~26/s, miss path = 2 compares);
  correctness audit 0 CRIT/IMPORTANT. OPEN-6 row flipped AS-BUILT. NOT hands-on (take gains
  the v117 STEPS section). PRODUCT QUESTION (default NO): a deck-play activity-feed line.
- **2026-07-17 nite (v116 `613f2ac4` + ue_wrap split `9d24ac0c`, PUSHED)** — the take-3 live test
  (17:00-17:09) caught the LOST-CATCH root: the claim-gated catch detector raced the FSM-hold release
  the successful ping itself triggers (measured 17:04:46/47) -> the whole R2-R5 symptom set from ONE
  eaten edge. v116 retires the claim gates (/qf rounds 1-6), adds catch->activity-feed, and ships the
  laptop_C power+floppy lane (LaptopState=106, /qf rounds 7-9; RE `votv-laptop-pc-RE-2026-07-17.md`);
  proto 115->116. Diag-battery ini hygiene + host perf_probe (OPEN-1). Rows flipped: Signal catch ->
  v116; Stationary PC -> AS-BUILT v116 (was stale-OPEN "screens gap-list #5"); +OPEN-10. Smoke x3;
  NOT hands-on (runbook take 4, SIX layers).
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
