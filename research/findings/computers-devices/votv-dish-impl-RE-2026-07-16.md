# Dish L4 implementation-gap RE (2026-07-16, pre-design pass)

Companion/addendum to `votv-dish-rotation-RE-2026-07-16.md` (the divergence-roots RE). This doc
closes the impl gaps named by the all-units design (`votv-signal-chain-all-units-DESIGN-2026-07-16.md`
L4) before the L4 impl design pass. Five parallel RE sweeps: status-UI chain, pose write path,
tickers/census, our-code inventory, gamemode arm chain. All facts [MEASURED] from static bytecode
(offset calculators validated against known jump targets / known-positives) unless tagged.
`@N` = uber byte offsets; `[N]` = statement indices.

Two CORRECTIONS to the prior dish doc are recorded in §7/§8 and struck through there.

## 1. Components + the pose write path

| prop | off | type (dish.hpp) |
|---|---|---|
| `Arrow1` | 0x258 | UArrowComponent* (world-rotated to target at slew start @2537; boresight ref) |
| `Arrow`  | 0x260 | UArrowComponent* (child of axis_Y = current aim) |
| `axis_Y` | 0x300 | UBillboardComponent* |
| `axis_Z` | 0x320 | UBillboardComponent* |

All SCS default subobjects — always non-null on a live dish. Attach: `axis_Z` -> {dish mesh,
`axis_Y`}; `axis_Y` -> {dish head mesh, `Arrow`}; `Arrow1` root-level.

Audio/timeline components (the park-cleanup set, dish.hpp): `satellite_move_Cue`
UAudioComponent* @0x308, `satellite_Cue` UAudioComponent* @0x310, `Timeline_0` (speed ramp)
UTimelineComponent* @0x340, `turnoff` (volume fade) UTimelineComponent* @0x350.

**Channel semantics [MEASURED]**: axis_Z phase writes Yaw only (`MakeRotator(0,0,yaw)` @91/@415);
axis_Y phase writes **Roll** only (`MakeRotator(rot_B.Pitch, 0, 0)` @138 — MakeRotator arg order is
(Roll,Pitch,Yaw), so rot_B's Pitch lands in axis_Y's ROLL channel). Matches `getCoords()`:
X = axis_Z.RelativeRotation.Yaw, Y = axis_Y.RelativeRotation.Roll. Each loop write zeroes the other
two channels of the written component. BeginPlay randrot writes axis_Z via K2_Set**World**Rotation
(yaw 0-360 @6743) but axis_Y via K2_Set**Relative**Rotation (roll 90-135 @6880) — **a mirror of
RELATIVE rotations is the correct frame** (matches what the loop and getCoords read/write; world-yaw
mirroring would break on level-rotated dish actors).

**K2_SetRelativeRotation** (Engine.hpp:17945): `(FRotator NewRotation, bool bSweep, FHitResult&
SweepHitResult, bool bTeleport)`; FHitResult 0x88 B, zero-filled by our ParamFrame
(ue_wrap/call.cpp:101). Shipped precedent: remote_player.cpp:567-606 (resolve on SceneComponent
class + ParamFrame Set-by-name). Raw RelativeRotation field writes do NOT render (H7 lesson,
comment at that precedent).

## 2. stop()/kill semantics + the stale set

`stop()` [MEASURED, 3 stmts]: `isMoving := false; isRotate := false; return`. Nothing else.
Public|BlueprintCallable — reflected-callable. `startMovingTo` and `startMove` also
BlueprintCallable (StartMovingAll already ships on this).

**Raw `isMoving=false` @0x384 mid-slew**: next Delay(0) resume enters at the @15 gate
(Linkage=15), POPFLOW_IFNOT(isMoving) -> clean immediate RETURN; the latent chain dies (Delay(0)
re-armed only from inside the loop). Crash-free, not sticky (a later startMovingTo passes its
!isMoving gate and fully re-inits isRotate/Axis/rot_B). BUT it exits through the loop-head gate,
not the arrival branch, so the whole slew-end set is SKIPPED and left stale:

- `gamemode.activeDishes[i]` stays TRUE (set at slew start @3197). Consequences: the desk's
  OnKeyDown ping gate (`activeDishes.Contains(true)` -> "Error code [1] Satellites are active",
  ui_consolesAtlas [39-42]) **blocks all future pings on that peer**; checkFordDishes can never
  broadcast; ticker_disher's no-dish-active gate stays blocked.
- `satellite_move_Cue` (+ `satellite_Cue` if past the 1-12 s start delay) stay Active — looping
  dish-motor audio forever (deactivations exist only at @1903 phase-0 arrival and the @3697/@6314
  end chain).
- turnoff fade + stop one-shots never play; isRotate/Axis stale (harmless — re-inited on next
  start).

`stop()` leaves the exact same stale set (only additionally clears isRotate). **Any client park
that kills a slew mid-flight must itself clean up: audio cue deactivation + activeDishes[i]=false.**

**Kill-during-start-delay edge**: slew start enters the frame loop at @25 (AFTER the @15 isMoving
gate) via pending latent continuations (the 1-12 s start delay, the 2-4 s phase delay). A dish
killed during those windows still runs ONE loop pass when the pending latent fires (one rotation
write + Delay(0) re-arm), then exits at the @15 gate on the next resume. `movePow` (move audio +
speed timeline) starts at the post-phase-delay resume — killing before it leaves less audio armed.

## 3. Mirror-vs-local-loop interplay [MEASURED-derived]

The loop is stateless across frames w.r.t. pose: every resume reads the component's CURRENT
RelativeRotation fresh (@295) and steps Speed*dt toward the LOCAL rot_B phase part; its arrival
check (tol 1.0 deg) evaluates its OWN post-write value within the same resume (an external writer
cannot interleave mid-resume). So an external mirror writing between frames does not oscillate or
NaN — but if mirror target != local rot_B, the pose becomes "mirror value dragged toward local
rot_B at Speed*dt/frame" (sawtooth) and the arrival check is **starved indefinitely** -> isMoving
never clears -> the fight is permanent. **Conclusion: a mirror must never co-write a dish whose
local loop is live** — either the loop is dead (parked/killed) or the mirror skips that dish.

Endpoint note: both peers' rot_B derive from the SAME lookAt (the shared catch slew vector) via
the same @2200 compose — final settled poses agree within the 1.0-deg tolerance even when slews
run natively per peer; only mid-slew timing/speed diverge (per-slew RNG).

## 4. Tickers — mechanics, kill/restore seams, singletons

**ticker_disher** [MEASURED]: no actor tick (bStartWithTickEnabled=False). One-shot
`K2_SetTimerDelegate(BindDelegate(do), Random(1800,3600), bLooping=FALSE)` armed at @853-@971;
armed from ReceiveBeginPlay (gameInstance.gamemode 0/1 gate) and re-armed unconditionally at the
end of EVERY `do` path (lose-roll / dish-active / slew-all). `do` fire order: 5% weight roll ->
activeDishes.Contains gate -> loop startMovingTo(fixed const target) -> re-arm.
- KILL: `K2_ClearTimer(Object=ticker, FunctionName="do")` — exact inverse; byte-identical shipped
  precedent space_renderer.cpp:362-380 (KillClientSpawnTimer) + kerfur.cpp:112-121.
  K2_ClearAllTimersForObject equally safe (only one timer in the class).
- RESTORE (MANDATORY — the re-arm chain is the ONLY scheduler; a cleared timer = dead for the
  session): raw `K2_SetTimerDelegate{Object=ticker, FunctionName="do"}` from C++ (exact re-arm, no
  roll), or PE `ReceiveBeginPlay()` (fresh Random(1800,3600), gamemode-gated, parent re-caches
  gamemode ptr — benign), or PE `do()` (immediate 5% roll — avoid). Shipped restore shape:
  console_state_sync.cpp:612-621 OnDisconnect -> SR::RestoreRoller.
**ticker_dishUncalib** [MEASURED]: actor tick, TickInterval initial 5.0; every tick: gamemode 0/1
gate -> `SetActorTickInterval(Random(5,10) * difficultyMult)` (difficulty 0->2.0, 1->1.2, 2->1.0,
3->0.8, 4->0.5) -> `Array_Random(gamemode.dishs).losePrec = true` (raw EX_LetBool). KILL =
`SetActorTickEnabled(false)`; RESTORE = `(true)` — zero staleness (interval re-rolled at top of
every tick). Shipped precedent: serverbox_sync.cpp:216-235 + :330-339 (ticker_serverBreaker).

**Singletons** [MEASURED, pak-wide census over 19,429 .uasset + 264 .umap headers]: both spawned
ONLY by mainGamemode ReceiveBeginPlay (disher stmt 1460 @43210; dishUncalib stmt 1644 @49466;
gated gameInstance.gamemode != 6); zero level placements. One instance per world per peer, present
from world start. Runtime find: FindObjectsByClass + skip Default__ + IsLive (serverbox pattern).
Join-window caveat: the park walk can run before the ticker exists — the spawn_authority 15 s
re-park reconcile precedent applies (spawn_authority.cpp:320-340).

## 5. CENSUS — complete slew-entry + axis-writer sets [MEASURED, pak-wide]

`startMovingTo` callers in the ENTIRE pak: the dish class itself (decl), **ticker_disher @386**,
**desk uber @9908 region** (ping-success loop). `startMove`/`movePow`/`calibLose`: dish-internal
only. `losePrec` writers: dish + ticker_dishUncalib only. No events / tablet / tools / level BPs
can start a slew. **axis_Z/axis_Y rotation writers: the dish's own ubergraph ONLY** (slew loop +
BeginPlay randrot) — 22 candidate assets disassembled, zero external writers.

=> On a client with both tickers parked, the ONLY local slew source is the client's OWN
ping-success loop (EX-invisible, cannot be intercepted — it WILL start locally).

## 6. Desk dish-status UI chain [MEASURED]

Widgets are 100% passive (uicomp_dishStatusSlot_C / ui_atlasDishesStatus_C hold no dish refs, no
Tick). The DESK pushes: `eventUpdateDishStats` timer, armed once in desk BeginPlay
(`K2_SetTimerDelegate(0.05, looping)` [508-509]) — **20 Hz, one dish per firing** (queue refill
pass every 25th) => full 24-dish sweep ~1.25 s. NO power/occupancy gate — runs for the desk's
lifetime on every peer. Per firing (uber [1646-1678]): read `dishs[i].isMoving` -> red/green flare;
read `dish.server.isBroken` -> if broken, bar frozen; else `level = 16 * getDir()^2 *
calibration^2` -> dynamic-material scalar. `getDir()` is a LIVE geometry call
(|dot(Arrow1.fwd, Arrow.fwd)|) — **a mirrored axis pose feeds the client's own panel
automatically**; no scalar cache to write. techName label written once at widget construct +0.2 s.
The pusher writes ZERO dish/gamemode state — safe on clients.

Client panel is correct iff per dish the local actor has: correct axis pose, `isMoving`,
`calibration`, `server.isBroken` (already synced — serverbox lane). `activeDishes`/`losePrec` are
not read by the panel.

Other dish-state readers pak-wide: ui_console (terminal "list": nameDish + calibration*100;
findDish scan), ui_consolesAtlas OnKeyDown (`activeDishes.Contains(true)` -> blocks ping, Error
[1]), panel_SATconsole (nameDish), mainGamemode `setPrec` (averages all calibration ->
`analogPanels.DL_precMult` — download precision multiplier), daynightCycle/createNewTask,
droneSellLocation::sell, kerfurOmega::findTask (dish-fix task), lib::setTaskNew,
serverBox::loadData, trigger_breakDish, trigger_eventer::runSpecialEvent.

**Client-side calibration WRITERS** (fight-relevant for a host-auth calibration sweep):
- `ui_console` calibrate machine: `dish.calibration += DeltaSeconds/60` per frame (Delay(0) loop,
  [102-105]; /10 fast variant [140-142]; `= 1.0` on completion [85-86]) — a PLAYER ACTION on
  whichever peer uses the terminal.
- `tool_setDishCalibration` (toolgun, by Index).
- The virus-event scramble (§8) — initiating peer only.
- The dish's own losePrec decay (slew loop, dormant when parked).

## 7. The arm chain — checkFordDishes / dishesStop / formDownload [MEASURED]

**CORRECTION to `votv-dish-rotation-RE-2026-07-16.md` §5**: the prior phrasing "dishesStop
broadcast -> the desk arms the download + objectRenderer.begin() + spaceRenderer.signalFound()"
mis-attributes the tail. Measured @60831-61237: `checkFordDishes` = if
`Array_Contains(activeDishes,true)` -> ret; else BROADCAST `dishesStop` (delegate @0x368), then
the GAMEMODE ITSELF inline: aim `objectRenderer.cameraAxis` K2_SetWorldRotation to the sky coords
(convertCoords -> rotator * 0.5), `objectRenderer.begin()`, `master_spaceRenderer.signalFound()`.
Subscribers of dishesStop pak-wide: EXACTLY ONE — the desk; its bound handler body =
`formDownload(0.0, -1)` and nothing else (@34223). checkFordDishes callers: dish slew-end @3660,
ui_cheatMenu. The delegate broadcast -> desk handler goes through the PE outer door => **host-side
`desk.dishesStop` is a PE-hookable seam** [RD per the dispatch-visibility delegate rule].

**formDownload(decoded, polarity)** [MEASURED, export 183]: DT row lookup by
`coord_signalData.objectName` (None -> silent no-op) -> `analogPanels.DL_signalDownloadData := Row`
(@0x900; **`.mesh` @+0x20 becomes valid = THE ARM LATCH** — gates the @66736 accrual + the
playSignall screen) -> `objectRenderer.Type := objectName` -> `objectRenderer.init_objectRenderer`
-> `initDownloadSignal(vec2(coords), decoded, polarity)`. `initDownloadSignal` [export 180]:
builds `DL_SignalDownloadDLData` (@0x978): `.decoded := arg`, **`.polarity := (b2-mode ? 2 :
(arg<0 ? RandomIntegerInRange(0,2) : arg))` — PER-PEER RNG when armed with -1**, identity fields
from the DT row, then `downloadTexts()`. No start button exists — once armed, accrual runs when
powered+tuned+polarity-matched.

**Un-arm edges**: all deliberate deletes funnel through ONE choke `gamemode.deleteActiveSignal()`
-> desk `intComs_signalDeleted` -> @33832 block (DL_signalDownloadData := None-struct,
DL_resDetecPercent := 0, initDownloadSignal({0,0},0,-1), DL_frData/poData := 0, coord_signalData
reset @34134) + `objectRenderer.deleteSignalActor()`. Its callers (desk only): download-complete
save @27793 (saveSignal -> laptop.addSignal -> delete), adjacent save path @28477, virus event
@78527. The NEW-PING edge resets both DL structs directly (@10050/@10244) without
deleteActiveSignal (mirrored today by ResetDownloadMachine in the catch replay).

**Host-readable "armed" state**: `DL_signalDownloadData.mesh` (@0x900+0x20) is the single best
latch — already wrapped as `CD::DownloadMeshValid()` (console_desk.cpp:813-818).
`DL_SignalDownloadDLData.signal` FName ('None' = unarmed) is the identity carrier.
`activeDishes` all-false is NOT discriminating alone (also all-false pre-ping).

**objectRenderer.begin()** [MEASURED, 73 stmts]: NOT cosmetic — fires `sigCam.trigger` +
`argemia_magenta` triggers, creates the 352x288 RT (`setRT` -> activeCam.TextureTarget — the
object-photo RT the desk screen material reads), `lifecrystal` special **SPAWNS `prop_argm_C`**
near rozship + fires ariralChamber trigger, `findSignalCam` camera swap. Natively per-peer
(each peer's own arm chain ran it pre-L4). `signalFound()` = world/story trigger only
(`trigger_OnFound.runTrigger`), zero desk-display state.

objectRenderer function flags [MEASURED, decompiled dump]: `begin`, `setRT`, `findSignalCam`
(HasOutParms), `init_objectRenderer`, `deleteSignalActor`, `requestImage` are ALL
Public|BlueprintCallable — the display limbs (setRT / findSignalCam) are individually
reflected-callable if the design replicates begin()'s display half without its spawn/trigger
limbs (activeCam/camera-root moves are inline in begin's body around findSignalCam — a partial
replication must do those writes itself).

**v112 lane coverage vs a parked client's missing arm**: the sim stream carries only `.decoded` /
resDetec / rates / filter data; DeskInput carries the 13 input fields. MISSING without a local
dishesStop: the DL_signalDownloadData row+mesh (arm latch), DL_SignalDownloadDLData identity
fields (signal/object/size/**polarity**/location/frequency/quality), objectRenderer
Type/init/begin + camera aim, downloadTexts. `CD::ArmDownloadFromSignal(decoded, polarity)` =
reflected formDownload (console_desk.cpp:793-801) covers everything formDownload does — i.e. all
of the above EXCEPT the checkFordDishes tail (camera aim, begin(), signalFound()); the existing
JOINER path already ships without that tail (known display gap on join).

## 8. Ping decision + the solar correction

Ping ENTER -> `gatherSignal` (local BP on the pressing peer; nothing native crosses peers).
Success: `coord_signalData := row` -> @9337 deleteSignal(sky) -> loop
`dishs[i].startMovingTo(shared vector)` -> pingSuccess sound + both DL structs zeroed. The slews
set THAT PEER's `gamemode.activeDishes[i] := true` (@3197 via startMove). Failure paths: log +
pingFailed only, no state.

**CORRECTION to `votv-dish-rotation-RE-2026-07-16.md` §6 ("Solar event")**: `event_solar` is NOT a
broadcast/event — it is an instance var on `trigger_eventer_C` holding an int_ttrigger reference
to a level-placed trigger. The dish-calibration scramble (`dishs[i].calibration = RandomFloat()`
@6636) lives in the DESK's `virusEvent` chain (@6150-6760: run event_solar trigger -> scramble ->
shuffle servers + break up to 16). `virusEvent` fires on (a) the peer that downloads/saves the
virus signal (@26837), (b) ui_cheatMenu. Dispatch = EX_LocalVirtualFunction -> PE-INVISIBLE
organic; BlueprintCallable for us. **It runs on the INITIATING peer only** (not per-peer);
trigger_eventer's scheduled-event path runs the world trigger but contains zero calibration
writes.

## 9. Our-code inventory deltas that matter for L4 (pointers)

- signal_catch_sync.cpp (456 LOC): CATCH detector = `MovingCount()` rising edge, claim-gated
  (LocalHolds desk), 1 Hz — **reads the same isMoving fields a park would clear; ordering hazard**
  (a park sweep clearing isMoving before the detector's next 1 s sample eats the catch edge).
  ApplyReplay kind=0 currently calls `D::StartMovingAll` (the client half the design retires) or
  direct `ArmDownloadFromSignal(0,-1)` on joiners. Pending-adopt applies once DownloadMeshValid.
- ue_wrap/dish.{h,cpp} (60/162 LOC): resolved = dishs array, lookAt, isMoving, startMovingTo;
  API = Count/MovingCount/ReadAllDishStates/ReadSlewFromMovingDish/StartMovingAll. NO
  axis/rot_B/Speed/stop/calibration/audio-cue access yet. Headroom fine.
- desk_sim_sync.cpp SimInterp: all 7 channels uniform ease-to-deadline; NO per-channel policy
  knob — a discrete ch8 needs structure (or the ARM edge goes reliable instead).
- protocol.h: kProtocolVersion 112; next free unreliable MsgType = 39; next free ReliableKind
  = 99. session.cpp (1110 LOC, past soft cap): a new unreliable stream adds ~40 LOC there
  (ClockPose/DeskSimPose pattern) — extraction proposal must accompany.
- World-ready edge: event_feed.cpp:187-188 -> subsystems::ConnectReplayForSlot (ordering within
  it is meaningful; re-runs on re-seed — rows must be idempotent). Host self-slot marked ready at
  session start.
- Parking precedents: serverbox_sync (tick park + latch + OnDisconnect restore), drone_sync
  (host-auth singleton pose mirror + SuppressTick/RestoreTick), weather_sync (interceptor flag
  gates + restore-as-loan).

## Open items the L4 design pass must decide (facts above, decisions deferred)

1. Client's OWN ping slews (unpreventable): kill-with-cleanup vs let-run-and-skip-mirror; the
   catch-detector ordering hazard either way.
2. ARM transport: reliable event carrying host polarity (RNG at -1 makes polarity
   host-authoritative by necessity) vs DeskSimPose ch8 (cannot carry polarity).
3. The checkFordDishes display tail on clients (camera aim + begin() RT) — replicate which limbs,
   and the prop_argm dupe hazard inside begin().
4. Calibration authority vs the ui_console calibrate machine (a per-frame client-side writer,
   legit player action) + tool_setDishCalibration + virusEvent (initiating-peer scramble that must
   broadcast).
5. Park cleanup set: audio cues + activeDishes (the ping-block gate makes this load-bearing).
