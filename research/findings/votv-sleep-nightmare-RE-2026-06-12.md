# VOTV "SLEEP / TIMELAPSE / NIGHTMARE" flow RE + coop sync design

Date: 2026-06-12. READ-ONLY RE pass (no src/ edits). Companion to
`votv-stolas-signal-catch-RE-2026-06-12.md` (format precedent) and the
host-authoritative world clock already shipped in `coop/time_sync.cpp` +
`ue_wrap/daynightcycle.{h,cpp}` (READ FIRST — the sleep timelapse advances
the SAME clock those files own). Answers the user's two questions: (1) how
to coop the bed-sleep timelapse (Minecraft "all peers in bed" gate vs.
one-sleeps-everyone-accelerates vs. force-sleep), and (2) the nightmare
policy (host-only roll? shared dream? any-fail-ejects-all?).

All bytecode addresses (@NNNN) are byte offsets in the named asset's
`ExecuteUbergraph_*` unless a function name is given. Offsets (0xNNNN) are
from the Alpha 0.9.0-n CXX header dump
(`Game_0.9.0n/.../Win64/CXXHeaderDump/*.hpp`).

---

## 0. Identity — the actors and the one global flag

| user's description | engine object |
|---|---|
| the bed you interact with | `Abed_C : Aprop_C` (objects, has `goSleep()`, `dreamProb @0x0374`, `comfort @0x0378`) |
| "the camera moves behind the base" | `mainGamemode.sleepCam @0x04F0` (an `ACameraActor`) + `SetViewTargetWithBlend` |
| the body lying in bed | `AplayerSleepingPawn_C` spawned + possessed (`mainGamemode.sleepingPawn @0x1258`); the live player pawn is parked in `origPawn @0x04F8` |
| "game speed accelerates" (timelapse) | `SetGlobalTimeDilation(self, 20.0f)` (gamemode sleep @70543) — engine-wide 20x |
| "until morning" (clock fast-forward) | `daynightCycle.ReceiveTick` advances `Day @0x0298` / `totalTime @0x02B0` by the dilated dt (@17936-18341) |
| the nightmare mini-game | a spawned `AdreamBase_C` actor (has `playerSpawn @0x0238`, `Duration @0x0268`, `awoken()`, `naturalWakeup()`); the player is TELEPORTED into it |
| "find the exit / thrown out into ragdoll" | `mainGamemode.leaveDream` → `mainPlayer.teleportWObackrooms(playerPreDream)` + `mainPlayer.punch(...,'chest')` (the ragdoll eject) |

**The single global truth is `mainGamemode.isSleep @0x04EC` (bool).** It is
the ONE world flag that says "the timelapse is running". Sleep state is NOT
spread across many fields — it is `isSleep` + the time dilation + the
possessed pawn. The dream/nightmare is a SECOND, separate flag
`mainGamemode.dreaming @0x0568`.

Everything that follows is **host-vs-client critical** because each peer
runs its OWN `mainGamemode` + `daynightCycle` (we do not use UE replication;
the host authors the clock via `time_sync`, clients free-run at
`TimeScale=0` and receive corrections — `time_sync.cpp:44-61`). So `isSleep`
and `SetGlobalTimeDilation` are PER-PROCESS, not shared, unless WE replicate
them.

---

## 1. The sleep ENTRY chain (mainGamemode.sleep)

`bed_C.goSleep()` (the bed's "Sleep" interaction option, `bed.hpp:19`) drives
the player custom event at **mainPlayer @25601 `gamemode.sleep(_, true, true)`**
(`sleep(bed, dropItem=true, ignoreRagdoll=true)`). `mainGamemode.sleep(bed,
dropItem, ignoreRagdoll)` is a 1-line thunk into the ubergraph @71395.

### 1.1 Validation gates (@71395-72691) — all run on the interacting peer

```
@71414 IsValid(bed)?                       no  -> @73319 (abort)
@71457 mainPlayer.sleepComfort := bed.comfort        (0x0D98 := bed.comfort)
@71528 BROADCAST sleepTriggered                      (delegate, pre-sleep)
@71547 lib.getEvent() active?              yes -> @71607 addHint "can't sleep" + abort
@71749 saveSlot.food <= 10 ?               yes -> @71819 addHint "too hungry" + abort
@71933 (IsFalling OR isRagdoll) AND !ignoreRagdoll AND MovementMode!=Walking
                                           yes -> @72255 addHint + abort
@72359 line-trace down from player, @72705 line-trace around bed (valid surface)
       -> on success falls through to the START block @70319
```

So you cannot start sleep during an active event, while too hungry
(food<=10), or while ragdolling/falling (unless `ignoreRagdoll`).

### 1.2 The START block (@70319-70864) — THE world-authoritative core

```
@70319 origPawn := GetPlayerPawn(0)                  (0x04F8 := live pawn — SAVED)
@70364 mainPlayer.flashlight := false
@70433 IsValid(sleepingPawn)?  no -> @71191 BeginDeferredActorSpawn(playerSleepingPawn_C)
                                       -> FinishSpawningActor -> sleepingPawn (0x1258), JUMP @70476
@70502 GetPlayerController(0).Possess(sleepingPawn)  <-- controller leaves the live pawn
@70543 SetGlobalTimeDilation(self, 20.0f)            <-- ***THE TIME ACCELERATION***
@70559 mainPlayer.dropGrabObject(); @70595 simulateDrop(true,true,true)
@70634 isSleep := true                               <-- ***THE WORLD SLEEP FLAG*** (0x04EC)
@70645 sleepCam.CameraComponent.PostProcessSettings := mainPlayer.Camera.PostProcessSettings
@70786 GetPlayerController(0).SetViewTargetWithBlend(sleepCam, 0, ...)  <-- ***timelapse cam***
@70844 PUSH @70869 (BROADCAST fellAsleep)            (delegate, post-sleep)
@70849 PUSH @70889 (arir-feeding side quest)
@70854 PUSH @71122 (flashlight off)
@70859 PUSH @899   (the 500 s autosave + NIGHTMARE-ROLL timer — §3.2/§4)
@70864 JUMP  @486  (the per-frame wake-check + sleepTime accumulator — §3.1)
```

Net world-authoritative effects of one `sleep()` call:
1. `SetGlobalTimeDilation(20)` — engine-wide 20x on the **calling peer's**
   whole simulation.
2. `isSleep := true` — read by the vitals tick (§5) and the daynight tick.
3. controller possesses a throwaway `playerSleepingPawn_C`; the real pawn is
   parked in `origPawn` (live, just unpossessed).
4. `sleepCam` view-target switch (cosmetic — §6).
5. arms two latent loops (per-frame wake check + 500 s nightmare/autosave).

---

## 2. The time-acceleration mechanism — exactly two multipliers

### 2.1 SetGlobalTimeDilation(20) — the real accelerator

`UGameplayStatics::SetGlobalTimeDilation` writes `AWorldSettings.TimeDilation`,
which scales `DeltaSeconds` for the ENTIRE world on that process. At wake it
is restored to **1.0** (gamemode @68530, §3.3). This is the whole timelapse:
physics, animation, the daynight tick, the camera pan — all run 20x.

### 2.2 daynightCycle clock advance (the fast-forward of the sky/clock)

`daynightCycle.ReceiveTick` (uber @17936-18378, the non-`Realtime` branch):

```
@17936 x := deltaSeconds * timescale
@17982 x := x * diff_mult
@18028 x := x * settingMultiplayer
@18074 x := x * sleepingTimeDilation        (0x0400, DEFAULT 1.0 — a tuning knob)
@18120 Day      += x        (0x0298)
@18341 totalTime += x       (0x02B0)
```

`deltaSeconds` here is ALREADY dilated by §2.1, so during sleep `Day` /
`totalTime` advance 20x. `sleepingTimeDilation` defaults to **1.0**
(JSON-confirmed) — i.e. it is currently a no-op modding/accessibility knob,
NOT the accelerator. **The accelerator is purely `SetGlobalTimeDilation(20)`.**

> Note (false lead, recorded so nobody chases it): gamemode @62361
> `SelectFloat(360.0f, 250000.0f, isSleep)` is the **coffeePower** drain
> divisor (caffeine camera jitter), frozen during sleep. Not time, not
> clock — purely cosmetic, per-player.

### 2.3 Why a client must NEVER call SetGlobalTimeDilation locally

The client's `daynightCycle` runs at `TimeScale=0` (U6, `time_sync.cpp:54`) —
its clock only advances via host corrections. If a lone CLIENT slept and ran
the stock `sleep()`, it would: (a) run its ENTIRE local game at 20x (jarring
fast physics + 20x-fast puppets of the still-awake host players), (b) refill
its own vitals 20x, yet (c) NOT advance the shared clock (host stays awake →
host clock unchanged → no host corrections move it forward). Result: a client
that "sleeps forever" with no time passing. **The acceleration must be
host-gated and host-clock-driven.**

---

## 3. The sleep LOOP and the wake paths

### 3.1 Per-frame loop — wake-check + sleepTime accumulator

`@486 Delay(0)` re-arms every frame; its latent resume offset is **@151**
(decoded from the `EX_SkipOffsetConst.Value=151` inside the Delay struct).
So **@151 runs every frame while sleeping**:

```
@151 wake := (saveSlot.sleep >= 100)        // fully rested
          OR (NOT isSleep)                   // flag already cleared
          OR lib.getEvent()                  // an event started
          OR (saveSlot.food <= 20)           // got too hungry
@452 IFNOT(wake) JUMP @481 (keep sleeping); ELSE @466 wakeup()
@481 PUSH @541 -> @541 accumulate: sleepTime += dt / sleepingTimeDilation (=dt),
                  save_main.stats.sleep_time += dt
```

So **the natural wake trigger is `saveSlot.sleep >= 100`** — i.e. the timelapse
runs until the sleep need is full (the need refills during sleep — §5), or an
event/hunger interrupts. (`sleepTime` is just a stat; it does NOT gate wake.)

### 3.2 The 500 s loop — autosave + the nightmare roll

`@899 Delay(500.0)` resume offset = **@773** (decoded `SkipOffsetConst.Value
=773`). Armed at sleep start (@70859). Every 500 in-sleep seconds (≈25 REAL
seconds at 20x):

```
@773 IFNOT(isSleep) POP                      // stop if no longer sleeping
@783 PUSH @899                               // re-arm the 500 s timer
@788 IFNOT(nightmaresEnabled) POP            // game-rule gate (§4.1)
@798 roll := RandomBoolWithWeight(bedSleepProb())
@859 IFNOT(roll) POP
@869 wakeup(); createDream(_)                 // <-- NIGHTMARE (§4)
@954 (the @899 fallthrough) autosave/save during sleep
```

### 3.3 The wake/cleanup (mainGamemode.wakeup -> uber @68800 -> @70196 -> @68530)

`wakeup()` (gamemode) is the timelapse END. It is a 1-line thunk to @68800:

```
@68800 IFNOT(isSleep) POP                     // idempotent (no-op if not sleeping)
@68810 GetPlayerController(0).Possess(mainPlayer)   // RE-possess the real pawn
@68877 sleepingPawn.SetActorHiddenInGame(true)
@68914 BROADCAST wokenUp                       // -> bedEvent.woken (§6.1)
@68933 PUSH @70196:
        @70196 SetViewTargetWithBlend(mainPlayer)   // camera back to the player
               JUMP @68530:
                @68530 SetGlobalTimeDilation(self, 1.0f)   // <-- restore normal speed
                @68546 isSleep := false                     // <-- clear the world flag
                @68557 IF saveSlot.sleep>=99 AND bed class==bed_C:
                        @68728 RandomBoolWithWeight(0.10) -> gearer.gen_gear()  // 10% gift
```

**Callers of `gamemode.wakeup()`** (every place the timelapse can end):
- gamemode @151/@466 — natural (sleep full / event / hunger).
- gamemode @869 — a nightmare just rolled (wake, then enter the dream).
- mainPlayer @21268 — the player presses the bed "exit/interact" while
  `gamemode.bed` is valid (manual early wake).
- mainPlayer @119659 — a drop/place input with `!dontWakeup` (input wakes you).
- mainPlayer.ragdollMode @595 — **entering ragdoll/faint forces wakeup** (any
  faint ends sleep).
- gamemode @11141 / @106637 — scripted-event special wakes.

> `mainPlayer.wakeup(passOut)` (uber @26792) and `mainPlayer.forceWakeup`
> (@26675) are a DIFFERENT thing — the player's **ragdoll get-up** (re-enable
> input, `isRagdoll:=false`), not the timelapse. Don't conflate them with
> `gamemode.wakeup()`.

---

## 4. Nightmares (the dream mini-game)

### 4.1 The roll — host-rollable, game-rule-gated

- Gate: `mainGamemode.nightmaresEnabled @0x1260`, assigned at @24035 from
  `gameInstance.gameRules.enableNightmares` (a game rule).
- Weight: `bedSleepProb()` — returns `gamemode.dreamProbability @0x1030` if
  `>= 0`, else `bed.dreamProb @0x0374` (fallback **0.15** if no bed).
  `dreamProbability` defaults to **-1.0** (sentinel = "use the bed's value").
- Cadence: once per 500 in-sleep seconds (§3.2), ≈ every 25 real seconds of
  timelapse. A typical night-skip yields 0-2 rolls.

### 4.2 createDream (uber @65316) — enter the dream

`createDream(customDream)` (called at @869 right after `wakeup()`):

```
@65326 dropGrabObject + simulateDrop; @65463 ladder drop; @65522 unsit
@65558 save_main.stats.total_dreams++
@65766 playerPreDream := mainPlayer.GetTransform()        // SAVE pre-dream xform
@65793 dreaming := true                                    // 0x0568
@65804 dream := processDream(customDream)                  // spawn the dream actor
@65921 spawnLoc := dream.playerSpawn.GetComponentLocation()  // 0x0238
@66056 mainPlayer.teleportWObackrooms(spawnLoc, false,false)  // TELEPORT into the dream
@66103 daynightCycle.isDream(true)   // hides ExponentialHeightFog/SkyLight/light_sun
@66140 playerInterface.dreamBlur.SetVisibility(visible)
@66200 mainPlayer.lag.CameraRotationLagSpeed := 3.0
```

`processDream` (uber, 81 stmts) builds a weighted map of dream classes:
per-day `dreamers` entries spawn when `daynightCycle.timeZ.Z >= dreamer.day`,
plus a base `dream_dreambase_C` (weight 100) once `timeZ.Z >= 21`, then
`weightedRandomV2` -> `BeginDeferredActorSpawnFromClass`. **The dream is a
single self-contained actor (`AdreamBase_C`)** placed at a fixed transform
(its own pocket of the persistent world; the player is teleported to its
`playerSpawn`). It has its own `Duration @0x0268`, `naturalWakeup()`,
`awoken()`, and `ReceiveTick`. The "find the exit" logic lives inside
`AdreamBase_C`'s ubergraph (its subclasses: dream_boulders, dream_burger, …).

**During a dream, time dilation is back to 1.0 and `isSleep` is false**
(because `wakeup()` ran at @869 BEFORE `createDream`). The dream plays at
NORMAL speed. The dream is its own state machine; the only shared hooks are
`gamemode.dreaming` and `daynightCycle.isDream`.

### 4.3 leaveDream (uber @66268) — the eject (success OR fail)

`AdreamBase_C` calls `gamemode.leaveDream()` (or via `awoken`/`naturalWakeup`)
when the player finds the exit / times out / fails:

```
@66268 IFNOT(dreaming) POP                                 // idempotent
@66331 dreaming := false
@66451 mainPlayer.teleportWObackrooms(playerPreDream)      // TELEPORT back to bed spot
@66498 dream.awoken(); @66534 dream.K2_DestroyActor()      // tear down the dream actor
@66570 daynightCycle.isDream(false)                        // restore sky/fog/sun
@66667 mainPlayer.setMouseSmooth(...)
@66912 mainPlayer.punch(randomDir, ..., 'chest', ...)      // <-- THE RAGDOLL EJECT
```

`punch(...,'chest')` knocks the player into ragdoll (the "thrown out of bed").
`mainPlayer.ragdollMode(true)` then fires `gamemode.wakeup()` again (@595,
no-op since isSleep already false) — so the eject leaves the player
ragdolling on the floor by the bed. **Success and failure both go through
`leaveDream` + the punch**; the difference is purely inside `AdreamBase_C`
(what reward/penalty it applied before calling leaveDream). `ragdollMode`
@67 also blocks NEW ragdolls WHILE `isDreaming` — you can't be knocked down
mid-dream.

---

## 5. Vitals during sleep — the "sleep" need REFILLS (per-peer, accelerated)

The tiredness need is `saveSlot.sleep` (0..100). The per-tick drain runs in
**mainPlayer.ReceiveTick** (uber @73066-74086), reading the LOCAL
`gamemode.isSleep`:

```
@73181 rate := SelectFloat(-15.0, 45.0, gamemode.isSleep)   // awake:45, ASLEEP:-15
@73250 x := worldDeltaSeconds / rate                         // negative when asleep
@73296 x := x * sleepComfort                                 // bed comfort (0x0D98)
@73342 mult := SelectFloat(1.0, sleepDraining, isSleep)      // asleep uses sleepDraining
       ... * movement-speed factor * difficulty SWITCHVAL ...
@73963 saveSlot.sleep -= x        // x<0 while asleep -> sleep INCREASES (rest)
@74086 saveSlot.sleep := FMax(result, 0)
```

So **asleep, the divisor flips negative (-15) → the stat refills**; awake it
drains (rate 45). And `worldDeltaSeconds` is the LOCAL dilated dt, so under
the 20x sleep dilation it refills ~20x. Other needs (food/etc.) keep draining
normally during sleep (only `sleep` flips); that is why a too-low `food`
interrupts sleep (§3.1).

**Coop-critical:** this block reads `gamemode.isSleep` + LOCAL
`worldDeltaSeconds`. Each peer simulates its OWN player's vitals in its OWN
tick. Therefore:
- A peer with local `isSleep=false`, dilation=1.0 → vitals drain normally.
- A peer with local `isSleep=true`, dilation=20 → vitals refill 20x.

This is exactly the user's option-(b) hazard: if one peer's sleep forced a
shared `isSleep`/dilation, the AWAKE peers would have their world run 20x and
could starve/collapse. The fix is to only ever set `isSleep`/dilation on a
peer that is ITSELF in the gated timelapse.

---

## 6. The timelapse camera + bedEvent (cosmetic / per-peer)

### 6.1 sleepCam is local cosmetic

`sleepCam` (`ACameraActor`, gamemode 0x04F0) is a pre-placed camera; sleep
copies the player's post-process into it (@70645) and switches the LOCAL
controller's view target to it (@70786), restoring to the player at wake
(@70196). `SetViewTargetWithBlend` is a per-PlayerController, per-peer view
change — **purely local presentation**. A client mirroring a host's sleep
does NOT need the host's camera; each sleeping peer drives its own sleepCam.

### 6.2 bedEvent — the wake-position helper

`AbedEvent_C` (dumped) is a tiny transient actor. `ReceiveBeginPlay` binds
`woken` to `gamemode.wokenUp`. On wake (`woken`, uber @113): it moves
`gamemode.bed` to the bedEvent's transform, teleports the player to that
spot + offset (@513 `teleportWObackrooms`), resets the bed prop
(`setPropProps(false×4)`), then `K2_DestroyActor()`. It is the "you wake up
standing by the bed" teleport — per-player, spawned per sleep.

---

## 7. Consolidated field / function map

### mainGamemode (CXXHeaderDump/mainGamemode.hpp)
| field/fn | off | role | coop class |
|---|---|---|---|
| `isSleep` | 0x04EC | the world "timelapse running" flag | **world-auth (host)** |
| `sleepCam` | 0x04F0 | timelapse camera (ACameraActor) | per-peer cosmetic |
| `origPawn` | 0x04F8 | the live pawn parked during sleep | per-peer local |
| `gearer` | 0x0500 | 10%-on-full-rest gift spawner | host-auth (spawn) |
| `dreaming` | 0x0568 | in-nightmare flag | per-player |
| `bed` | 0x05A8 | the bed being slept in (Abed_C) | per-player |
| `sleepTime` | 0x0BDC | accumulated sleep seconds (stat only) | per-peer |
| `dreamProbability` | 0x1030 | nightmare weight override (-1=use bed) | host-auth |
| `sleepingPawn` | 0x1258 | the throwaway sleep pawn | per-peer local |
| `nightmaresEnabled` | 0x1260 | game-rule gate for nightmares | host-auth |
| `sleep(bed,dropItem,ignoreRagdoll)` | — | ENTRY (uber @71395) | PE-visible (§8) |
| `wakeup()` | — | END (uber @68800) | PE-visible (§8) |
| `createDream` / `leaveDream` / `processDream` / `bedSleepProb` | — | dream lifecycle | BP-internal (poll) |

### daynightCycle (CXXHeaderDump/daynightCycle.hpp) — already wrapped
| field | off | role |
|---|---|---|
| `Day` | 0x0298 | day counter (advances 20x in sleep) |
| `MaxTime` | 0x02AC | day length |
| `totalTime` | 0x02B0 | absolute clock (advances 20x in sleep) |
| `TimeScale` | 0x02B4 | base rate (client forced 0 by U6) |
| `timeZ` | 0x02D0 | FIntVector H/M/S-ish (timeZ.Z used as hour, e.g. `>=21`) |
| `sleepingTimeDilation` | 0x0400 | clock-only knob, DEFAULT 1.0 (no-op) |
| `isDream(bool)` | — | toggles fog/skylight/sun visibility for dreams |

### mainPlayer / bed / dreamBase
| field/fn | off | role |
|---|---|---|
| `mainPlayer.sleepComfort` | 0x0D98 | := bed.comfort; scales vitals refill |
| `mainPlayer.sleepDraining` | 0x0B84 | asleep drain multiplier |
| `mainPlayer.isRagdoll` | 0x087F | ragdoll state (eject) |
| `mainPlayer.teleportWObackrooms(xf,b,b)` | — | the in/out-of-dream teleport |
| `mainPlayer.punch(dir,..,'chest',..)` | — | the ragdoll eject |
| `bed_C.dreamProb` / `comfort` | 0x0374 / 0x0378 | nightmare weight / comfort |
| `bed_C.goSleep()` | — | the bed "Sleep" action |
| `dreamBase_C.playerSpawn` | 0x0238 | where the player teleports in |
| `dreamBase_C.Duration` | 0x0268 | dream length |
| `dreamBase_C.awoken()` / `naturalWakeup()` | — | dream-end hooks |

---

## 8. PE-visibility / dispatch (our ProcessEvent detour)

Per the session-13 dispatch ground truth (BP-internal calls never hit the PE
detour; PE-visible = cross-object, no-out-param script calls + latent
resumes):

- **`gamemode.sleep(bed,dropItem,ignoreRagdoll)`** — called from mainPlayer
  (cross-object), no out/return params → **PE-VISIBLE**. We can detour the
  entry: read params, decide host-gating, optionally block.
- **`gamemode.wakeup()`** — cross-object from mainPlayer/ragdollMode, no
  params → **PE-VISIBLE**. Detourable.
- **`sleep()`'s body**, the per-frame loop, the 500 s nightmare roll,
  `createDream`/`processDream`/`leaveDream` — all BP-internal (same-object
  ubergraph calls) → **NOT PE-visible**. Detect via polling `isSleep` /
  `dreaming` edges, the same shape as the desk/email pollers.
- Delegates `sleepTriggered`/`fellAsleep`/`wokenUp` fire via
  `ProcessEvent` on bound handlers (e.g. bedEvent.woken) → visible if we bind,
  but polling `isSleep` is simpler and host-authoritative.

---

## 9. Coop consequences — what is what

| piece | mechanism | classification | rule |
|---|---|---|---|
| timelapse camera (`sleepCam`, view target) | per-PlayerController view | **per-peer cosmetic** | each sleeping peer drives its own; never mirror |
| `SetGlobalTimeDilation(20)` | engine-wide on the calling process | **world-authoritative** | only on a peer that is ITSELF in the gated timelapse |
| clock advance (`Day`/`totalTime`) | daynight tick × dilated dt | **world-authoritative (HOST)** | host already owns it (time_sync); a client must NOT self-advance |
| `isSleep` flag | per-process bool | **world-auth, but applied per-peer** | host decides; each gated peer sets its own local copy for vitals |
| `saveSlot.sleep` refill | local mainPlayer tick × local isSleep × local dt | **per-player vitals** | refills correctly iff that peer's local isSleep+dilation are set |
| `sleepingPawn` possess / `origPawn` | local controller possess | **per-peer local** | each peer possesses its own sleep pawn |
| nightmare roll | host 500 s loop, RNG | **host-authoritative** | roll on host only; broadcast the result |
| dream actor (`AdreamBase_C`) + teleport | spawn + teleport one player | **per-player** | a solo experience; not trivially shareable (one playerSpawn) |
| ragdoll eject (`punch`) | local on the dreamer | **per-player** | local to whoever failed |
| `gearer.gen_gear()` 10% gift | host spawn on full-rest | host-authoritative | spawn on host, mirror via existing prop spawn |

The exact seams the Minecraft-gate needs:
1. **Intercept the entry**: detour `gamemode.sleep()` (PE-visible, §8). On the
   sleeping peer, run the cosmetic/local half (it is fine to enter the bed
   pose: possess `sleepingPawn`, switch to `sleepCam`) but PREVENT the world
   half from taking hold until gated — i.e. immediately after `sleep()`
   returns, force `SetGlobalTimeDilation(1.0)` and clear the LOCAL `isSleep`
   so neither the clock nor the vitals accelerate yet. The peer is now "lying
   in bed, waiting", time normal. (Alternative, cleaner per RULE 1: block the
   stock `sleep()` in the detour and reproduce only Possess(sleepingPawn) +
   SetViewTargetWithBlend(sleepCam) ourselves — no dilation, no isSleep — so
   there is nothing to "undo".)
2. **Broadcast sleep-count**: a new reliable `SleepState` message
   {peerSlot, inBed:bool}. The host tallies "N/total in bed" and rebroadcasts
   the count for the "1/4 sleeping" HUD line (existing event_feed/chat
   channel can render it).
3. **Start the accelerated phase simultaneously**: when the host sees ALL
   connected peers `inBed`, it broadcasts `SleepState{phase=ACCELERATE}` AND
   begins authoring the fast clock. On receipt EACH peer locally:
   `SetGlobalTimeDilation(20)` + `isSleep := true` (so its own vitals refill +
   its sky tracks) + keeps its sleepCam. The host additionally lets its
   daynight tick advance the authoritative clock; clients keep `TimeScale=0`
   and ride the host corrections — but during the 20x phase the host clock
   moves fast, so bump the `time_sync` push rate (e.g. 2000ms → ~150-250ms)
   for the duration so client skies/timers don't stair-step. (`time_sync`
   already structurally supports this — only `kPushInterval` changes.)
4. **End simultaneously**: the END condition stays host-authoritative — the
   HOST runs the stock wake-check (`saveSlot.sleep >= 100` of… see §10 on
   whose stat) and on wake broadcasts `SleepState{phase=END}`. Each peer
   locally `SetGlobalTimeDilation(1.0)` + `isSleep:=false` + re-possess its
   real pawn + restore camera (i.e. call the LOCAL `gamemode.wakeup()` path,
   which is exactly @68800 and is idempotent).
5. **Nightmare-fail cancels everyone**: a nightmare on the host's loop calls
   `wakeup()` (ending the host's `isSleep`) then `createDream()`. Since the
   host's wake is the gate's END condition, the host broadcasts
   `SleepState{phase=END}` and everyone exits the timelapse together (the
   user's intuition: "any fail throws all out of bed"). The dream itself stays
   a solo, host-local (or target-local) experience — see §10 Q2/Q3.

---

## 10. COOP DESIGN RECOMMENDATION (answers to the user's questions)

Protocol today: `kProtocolVersion = 69`, last `ReliableKind = PropStickState
= 64` → next kind **65**, next version **70**. New module
`coop/sleep_sync.{h,cpp}` (one feature/file) + a thin `ue_wrap/sleep.{h,cpp}`
(or extend `ue_wrap/daynightcycle` + a gamemode wrapper) for the reflected
`Possess`/`SetGlobalTimeDilation`/`SetViewTargetWithBlend`/`isSleep` writes.

### Q1 — sleep / timelapse: adopt the Minecraft gate (option a). RECOMMENDED.

The engine supports it cleanly because the sleep state is ONE flag
(`isSleep`) plus ONE engine call (`SetGlobalTimeDilation`) plus a local pawn
possess — all reproducible by reflected writes/calls, and the entry
(`gamemode.sleep`) is PE-visible so we can gate it. Concretely:

- **Lone sleeper = inert bed pose.** Detour `gamemode.sleep()`; let the peer
  enter the bed visually (sleepingPawn + sleepCam) but keep dilation=1.0 and
  local isSleep=false. No clock advance, no vitals refill, no nightmare — the
  peer just lies there. Broadcast `SleepState{inBed=true}`.
- **HUD "1/4 sleeping".** Host tallies inBed flags, rebroadcasts the count;
  render on the existing event-feed/chat line.
- **All in bed → host starts the 20x phase on every peer at once** (§9 step 3),
  host authors the fast clock, clients ride boosted corrections.
- **End** when the host's sleep need fills (or host hunger/event/manual exit),
  broadcast END, everyone wakes together (§9 step 4).

Option (b) "one sleeps, everyone accelerates" is explicitly rejected — §5
shows awake peers would refill/again-drain vitals at 20x and their whole
world would run 20x (collapse/ragdoll risk, exactly the user's worry).
Option (c) "force-sleep everyone" is worse (yanks control). The gate is the
only one that keeps awake peers in a normal world.

**Whose `sleep` need ends it?** The stock wake-check reads the local
`saveSlot.sleep`. In coop, make the GATE END authoritative on the HOST's own
`saveSlep>=100` OR a max-duration cap, so one peer who joined the bed already
rested doesn't end it for a still-exhausted peer too early — or, simplest and
SP-faithful: end when the HOST (the clock owner) would naturally wake, then
each client's own sleep need is set to full on END (they all "slept the
night"). Recommend: **host wake-check drives END; on END each peer's
`saveSlot.sleep := 100`** (everyone wakes rested — matches the shared-night
fiction). This avoids per-peer divergence and is one extra field write.

### Q2 — nightmares: host-only roll, solo dream, any-fail-wakes-all.

- **Suppress nightmare rolls on clients; roll on the host only.** Clean,
  because the 500 s roll loop is BP-internal and only fires where `isSleep` +
  `nightmaresEnabled` are true. In the gate, only the HOST runs the
  authoritative timelapse loop; clients are in the 20x phase for cosmetics +
  vitals but we do NOT need their roll. Easiest enforcement: set
  `gamemode.dreamProbability := 0` (or `nightmaresEnabled := false`) on
  CLIENTS for the duration (their roll, even if it fired, weights 0), leaving
  the host's roll as the single source. (`dreamProbability` is a clean
  per-peer override at 0x1030; -1 restores SP behaviour on disconnect.)
- **If the host rolls a nightmare, do ALL peers enter the same dream? —
  Recommend NO (not the same instance).** The dream is a single
  `AdreamBase_C` with ONE `playerSpawn`; the engine teleports exactly one
  player into it. Mirroring one dream instance across peers (shared geometry,
  shared exit, multiple players in one pocket) is far beyond minimum-viable
  and has no SP precedent. Two implementable policies, pick per scope:
  - **(Recommended, minimum-viable) Nightmare ends the shared timelapse for
    everyone; only the host (the roller) plays the dream solo; the other
    peers simply wake up (END broadcast) and resume normal play.** The host's
    `createDream` runs locally; `dreaming`/`isDream` are host-local; clients
    see the host's puppet vanish into the dream pocket (or we hide it) and
    teleport back on `leaveDream`. This is the cheapest and is internally
    consistent with "any nightmare wakes the house".
  - **(Stretch) Each peer rolls/owns its OWN solo dream in parallel.** Only if
    a future scope wants everyone to dream — each peer spawns its own
    `AdreamBase_C` and plays solo; no shared state. More work, no shared
    fiction benefit.
- **If ANY peer fails its nightmare, are ALL thrown out of bed? — YES,
  structurally.** A nightmare calls `gamemode.wakeup()` at @869 BEFORE the
  dream even starts, which clears the host's `isSleep`. Since the host's
  `isSleep` is the gate's END signal, the host broadcasts END and the whole
  house wakes the instant the nightmare begins — independent of dream
  success/failure. The fail→`punch`→ragdoll is local to the dreamer
  (`leaveDream` @66912); success and failure both eject via the same punch,
  so "fail throws you out of bed into ragdoll" is faithfully local while
  "everyone else wakes" is automatic. The user's expectation holds.

- **USER DOMAIN KNOWLEDGE (2026-06-12, post-RE): nightmares are FULL game
  LEVELS** — walking/exploration or climbing mini-game levels — and
  COMPLETING one rewards the sleeper ("slept well"); failing interrupts the
  sleep. Two consequences for this design:
  1. The success/fail REWARD DIFFERENTIAL lives inside the `AdreamBase_C`
     subclasses (4.3 traced only the shared eject path — "the difference is
     purely inside AdreamBase_C" — without decoding the reward writes).
     BEFORE implementing the nightmare half, a follow-up RE pass must dump
     the concrete dream-level BPs (the AdreamBase_C children) and pin the
     success path's writes (saveSlot.sleep restore amount? stats? items?) so
     the coop policy can replicate the reward to the gated co-sleepers
     (e.g. host completes the dream -> everyone keeps the full-rest bonus;
     host fails -> the house already woke early, which IS the penalty).
  2. The "shared nightmare" idea is UPGRADED from "no shared fiction
     benefit" to a LEGITIMATE STRETCH FEATURE (real co-op content: the
     levels exist in every install's pak; peers could be teleported into
     the same pocket at offset spawns and walk/climb it together; the open
     problems are the single playerSpawn, dream-actor spawn mirroring, and
     exit semantics — first exit wins vs all-must-exit vs any-fail-ejects-
     all). It stays OUT of minimum-viable v1 (host plays solo, others wake)
     but is the designated v2 of this feature.

### MTA precedent (RULE 2026-05-28)

- **Server-authoritative game speed = `CGame::SetGameSpeed(float)`**
  (`Server/.../logic/CGame.h:291-292`, `.cpp:313,4537`). MTA owns one global
  game-speed scalar server-side and replicates it to all clients (the
  `setGameSpeed` element-RPC). Our `SetGlobalTimeDilation(20)` broadcast is
  the same shape: host owns the speed, pushes it to all peers, all run at it
  together. This is the architectural blessing for "start/stop the accelerated
  phase simultaneously".
- **Server-authoritative clock = `CClock` / `CGame::SetMinuteDuration` /
  `SetTime`** (`Server/.../logic/CClock.cpp`, `CGame.cpp:4547`). The server
  owns world time and pushes it; clients never advance it locally. We already
  do this (`time_sync`); the gate just temporarily speeds the push.
- **The "all players ready" gate** has no single named MTA function (MTA's
  resources do it in Lua), but the SHAPE is the standard host-authoritative
  state machine: per-element boolean state on the server (here `inBed` per
  peer slot), the server tallies and broadcasts the aggregate + the
  transition — identical to how `CPlayerManager` tracks per-player join/ready
  state and `CGame` broadcasts a global state change. Cite: host tally +
  broadcast, never client-decided.

### Minimum-viable scope / explicitly out

- Sharing one dream instance across peers (use solo dreams or host-only).
- `gearer.gen_gear()` parity beyond the existing prop-spawn mirror.
- `sleepTime`/`stats.*` per-peer divergence (stats aren't scoped).
- The coffeePower jitter, sleepCam post-process parity (per-peer cosmetic).
- `event_arirTreehouseSleep` (a scripted event sleep variant — does not touch
  `isSleep`/dilation; out of this scope).

---

## 11. Validation list (pre-implementation / during smoke)

- V1 — entry detour: solo-host, interact bed; confirm `gamemode.sleep` hits
  our PE detour (log the params bed/dropItem/ignoreRagdoll). If it does NOT
  fire, the call is being dispatched BP-internally on some path → fall back to
  polling `isSleep` 0→1 edges.
- V2 — lone-sleeper inert: 2 peers, ONE sleeps. Assert: the sleeper's
  `GetGlobalTimeDilation()==1.0`, local `isSleep==false`, `Day/totalTime`
  unchanged, `saveSlot.sleep` NOT rising; the awake peer's world runs at 1x,
  vitals drain normally. HUD shows "1/2 sleeping".
- V3 — gate fire: both sleep. Assert: on BOTH peers
  `GetGlobalTimeDilation()==20`, `isSleep==true`, sky/clock advancing in
  lockstep (host authors, client tracks within the boosted push interval),
  both `saveSlot.sleep` rising. No awake peer remains at 20x.
- V4 — natural end: let the host fill. Assert both wake together
  (dilation→1.0, isSleep→0, real pawn re-possessed, camera restored),
  `saveSlot.sleep==100` on both, clock landed at morning, `time_sync` push
  interval restored to 2000ms.
- V5 — nightmare-any-wakes-all: force a host roll (`dreamProbability:=1`).
  Assert: the instant it rolls, BOTH peers exit the timelapse (END
  broadcast); the host enters its solo dream (`dreaming==1`,
  `daynightCycle.isDream` hides sun on the host only); the client is awake and
  normal. On host `leaveDream`, host teleports back + ragdolls; client never
  ragdolls.
- V6 — client roll suppressed: confirm a client in the 20x phase never spawns
  a dream (its `dreamProbability==0`/`nightmaresEnabled==false` for the
  duration; restored on END/disconnect to -1/the game rule).
- V7 — disconnect mid-sleep: a peer drops while the gate is ACCELERATE.
  Assert the remaining peers either continue (if still all-in-bed) or the host
  ends the phase; no peer is left at 20x with `isSleep` stuck (idempotent
  `gamemode.wakeup()` clears it).
- V8 — clock anti-stairstep: during the 20x phase, log client `totalTime`
  every 100ms; confirm the boosted push keeps |client-host| within one push
  interval's worth of advance.

---

## 12. Tooling used (exact commands, from repo root)

```
# function disassembly + ubergraph trace + raw-stmt decode
python research/bp_reflection/_fn.py mainGamemode {sleep,wakeup,createDream,
   leaveDream,bedSleepProb,processDream,trySpawnInsomniac}
python research/bp_reflection/_fn.py mainPlayer {wakeup,forceWakeup,ragdollMode}
python research/bp_reflection/_fn.py daynightCycle isDream
python research/bp_reflection/_fn.py bedEvent {woken,ReceiveBeginPlay}
python research/bp_reflection/_cfg.py mainGamemode ExecuteUbergraph_mainGamemode [71395|65301|66268]
python research/bp_reflection/_cfg.py daynightCycle ExecuteUbergraph_daynightCycle
python research/bp_reflection/_cfg.py mainPlayer  ExecuteUbergraph_mainPlayer
python research/bp_reflection/_cfg.py bedEvent     ExecuteUbergraph_bedEvent
python research/bp_reflection/_dumpstmt.py mainGamemode ExecuteUbergraph_mainGamemode 486 899  # latent resume decode
python research/bp_reflection/_scan.py mainGamemode {isSleep,sleepTime,"wakeup(","dreaming :=","nightmaresEnabled :="}
python research/bp_reflection/_scan.py mainPlayer  {sleep,Sleep,bed}
python research/bp_reflection/_scan.py daynightCycle {sleepingTimeDilation,TimeDilation,isSleep}
```

CXX header greps: `CXXHeaderDump/{mainGamemode,mainPlayer,daynightCycle,bed,
dreamBase}.hpp` (offsets in §7). JSON default extraction over
`{daynightCycle,mainGamemode}.json` (`sleepingTimeDilation=1.0`,
`dreamProbability=-1.0`). MTA greps over
`reference/mtasa-blue/Server/mods/deathmatch/logic/{CGame,CClock}.{h,cpp}`.
Sources read: `src/votv-coop/src/coop/time_sync.cpp`,
`src/votv-coop/src/ue_wrap/daynightcycle.cpp`,
`src/votv-coop/include/coop/net/protocol.h`.

## 13. v71 IMPLEMENTATION (2026-06-13, the section-10 design built + shipped)

Protocol 70 -> 71, ReliableKind::SleepState=66 (4 B: op Report/Tally/
Accelerate/End + flag + count/total). New modules `coop/sleep_sync.{h,cpp}` +
`ue_wrap/sleep.{h,cpp}` + the dev exerciser `coop/dev/sleep_probe.{h,cpp}`
(ini sleep_probe=1; reflected gamemode.sleep on both peers T+15/T+25, host
wakeup T+40 -- the wire-proof smoke driver); `time_sync` grew
SetSleepAccelerate (client clock TimeScale=1 during the phase so the
timelapse sky pans instead of stepping on the 2 s corrections; time_sync
stays the only TimeScale writer).

DESIGN DIVERGENCES from section 10, both deliberate:

- **No sleep() detour.** The recommended PE observer on gamemode.sleep was
  replaced by the per-tick isSleep EDGE POLL + an edge-latched dilation undo
  (latch only on success). One mechanism covers every entry path -- bed
  interaction, the probe's reflected sleep(), a host already asleep when the
  first client connects -- with no new hook surface, at a <= 1-tick undo lag
  (~0.3 game-seconds at 20x, imperceptible).
- **Any falling edge ends the night for everyone** (not only the host's
  natural wake): natural host wake / manual exit / hunger / event / nightmare
  all funnel through gamemode.wakeup -> the isSleep falling edge -> End.
  natural=1 only when the host's own need hit >= 100; only then does every
  peer get sleep=100 (a night that actually passed) -- an interrupt leaves
  each peer's 20x-accrued need as is.

Implementation truths worth keeping:

- **The gift threshold dictates TWO things**: receivers run wakeup() FIRST
  and write the need AFTER (wakeup rolls the 10% gearer gift iff need >= 99
  AT CALL TIME), and the client clamp sits at 98 (a 99 clamp would arm the
  gift on every clamped client every night). The clamp also guarantees only
  the HOST can end the night naturally (clients never reach the >= 100
  wake-check).
- **The placed base bed is a SUBCLASS** (bed_b_C / bed_m_C ...; zero exact
  bed_C instances live -- the 00:17 smoke), so FindBed matches by
  descendant-of-bed_C over the object array (probe-only walk).
- **THE ROUTER-MISS BUG (process lesson, memory
  feedback-reliablekind-router-checklist)**: the first wire test silently
  dropped the client's Report -- the new kind had its dispatcher case but was
  missing from event_feed.cpp's master-switch family list (`default: break`).
  The same audit found v70's DeskLogLine had shipped with the identical miss
  (wire-dead from day one; idle smokes can't exercise event-driven kinds).
  Both routed now; a new ReliableKind wires in THREE places and the first
  smoke must exercise the wire (the sleep_probe shape).

Audits: perf 0 CRITICAL (all hot paths O(1); FindBed/dilation calls confirmed
WARM/COLD; note-A latch-on-success applied); correctness ALL-PASS truth table
(9 scenarios incl. claim-cycle, joiner-mid-phase, leaver, reload, disconnect;
2 hardening notes applied: gm-change clears phase/tally; End zeroes both
lastCount/total via the tally reset). Cosmetic fix from the probe smoke: the
"N/M sleeping" counter is waiting-room-only (a count drop during the phase IS
the End transition; the counter flashing before "Sleep interrupted" read as
nonsense).

Verified end-to-end (00:28 probe smoke, both peer logs): client Report ->
host tally 1/2 -> host sleeps -> ACCELERATE both peers (dilation 20 + client
TimeScale 1) -> host manual wakeup -> END natural=0 both peers (dilation 1,
TimeScale 0, isSleep 0, feed lines). Hands-on still pending: the visual
timelapse, the natural-end full-rest grant, a real nightmare night
(host-only roll), vitals refill rates.

### 13.1 v71.1 -- the WAITING-state camera hold (user directive 2026-06-13)

"When 1/4 are asleep, the lone sleeper's camera must NOT go to the base view
until everyone sleeps." The native entry retargets the view to sleepCam
instantly (@70786); the gate now holds the WAITING camera at the BED
(SetViewTargetWithBlend -> the sleepingPawn, via the SLEEPING PAWN's
controller -- the local mainPlayer is unpossessed mid-sleep so the usual
local-player controller route is null) and hands it to the gamemode's own
sleepCam only at ACCELERATE; END restores natively (wakeup @70196). Null-safe
at every hop (camera polish never blocks the gate). Verified in the 00:43
probe smoke on both peers: 'WAITING ... camera held at the bed' ->
'camera -> sleepCam (the timelapse view)' at ACCELERATE; in-bed view angle
itself is a hands-on item (the sleeping pawn may or may not carry a camera
component -- if the view reads wrong, retarget to origPawn instead).
