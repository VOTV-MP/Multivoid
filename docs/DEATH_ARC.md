# DEATH ARC -- native death, without losing the world

**Status: COMMIT A IS BUILT AND GREEN (2026-08-31). The travel seam, the veto and the
revive all ship; the announcement lane (commit B) does not.** The whole native death now
plays out in coop -- sound, `dead := true`, ten seconds, the black screen at +5 s -- and
`UGameplayStatics::OpenLevel` is REFUSED, with a revive written in its place. Evidence is
in section 11 (real runs, both configurations). The KO lane that used to be built is
**RETIRED** (`33008d87`) -- see section 5.

**Read section 11 (AS-BUILT) before section 3 and section 4** -- it is what shipped, and its
11.2 records SIX things the design text below did not settle, two of which are writes the
revive needs that section 4 does not list. Then section 6a, which closes six of section 6's
eight measurement items.

**Read this before touching anything that prevents, delays, undoes, detects or
survives the local player's death.** It is the living doc for the arc; the
point-in-time bytecode write-up is
`research/findings/world-systems/votv-player-death-chain-RE-2026-08-31.md`.

---

## 0. The instruction that decides this arc

**USER, 2026-08-31, verbatim:**

> *"Я хочу чтобы игрок ощущал нативную смерть, но без выкидывания в главное меню.
> Вот. Это главное. Полный RE как это сделать - пусть и будут черные экраны и тд -
> т.е. полностью нативно, но момент когда игра захочет отослать игрока в главное
> меню и выгрузить карту и сломать весь мир - тут наш мод вступает и НОВОЕ
> ИЗМЕНЕННОЕ СОСТОЯНИЕ ПИШЕТ - возможно потребуется разобрат все главные
> "смертельные" функции и NEW BEHAVIOUR прямо в ядро игры вписать."*

Three things follow, and they invert the design that was shipped hours earlier:

1. **Everything native runs.** The death sound, `dead := true`, the ten seconds,
   the black screen. The player is supposed to *feel* it. No suppression, no
   gate, no early cancel.
2. **The cut is at the LAST stage** -- the instant the game reaches for the level
   travel -- not at the first.
3. **At that instant we WRITE NEW STATE.** Not "cancel the death"; author the
   thing the game has no concept of, a revive.

This is the opposite of the shipped design (which prevented the death from ever
starting) and the opposite of the two alternatives considered before it. It is
also *better*, for a reason that only becomes visible once the chain is fully
mapped -- see §2.

---

## 1. The death chain, complete and measured

All `[V]`, from `mainPlayer.uasset` / `mainGamemode.uasset` / `lib.uasset`
bytecode (`research/bp_reflection/`, disassembled with `_fn.py` / `_cfg.py`).
Block addresses are real offsets in those dumps.

```
mainPlayer."Add Player Damage"(damage, ...)
  @659   early-out: gamemode.immortal || isDreaming || dead || startInvinc
  @856   dmg *= SelectFloat(0.75, 1.0, isStrong)          -- never scales UP
  @972   VictoryFloatMinusEquals(gamemode.saveSlot.health, dmg, out)
  @1053  saveSlot.health = FClamp(out, 0, saveSlot.maxHealth)   -- never negative
  @2695  if (out <= 0)
  @2739      kill()                                    [EX_LocalVirtualFunction]

mainPlayer.kill()
  @0     if (dead || startInvinc) return
  @57    ragdollMode(true, false, true)                [EX_LocalVirtualFunction]

mainPlayer.ragdollMode(ragdoll, passOut, death)
  @5     IFNOT(canRagdoll) POP                          -- the game's own gate
  @53    if (ragdoll == isRagdoll) JUMP @196
  @196   IFNOT(death) POP                               -- no-op when already down
  @206   fallen(true)
  @250   isRagdoll := true ; spawns playerRagdoll_C ; hides Mesh ; physics on
  @276   PUSH @595  ->  @672 simulateDrop(false,false,true)
                        @689 dropGrabObject()           -- SHARED-WORLD WRITE, see 7.3
  @1649  fallen(death)                                 [EX_LocalVirtualFunction]
  @1672  IFNOT(death) JUMP @2322                        -- death-only body @1686

mainPlayer.fallen(death)  ->  ExecuteUbergraph_mainPlayer(39848)
  @39848 IFNOT(death) JUMP @39685                       -- THE ONLY GATE ON THE CHAIN
  @39862 JUMP @37478
  @37478 PlaySound2D(deathEnd) ; JUMP @37412
  @37412 dead := true
         RetriggerableDelay(5.0 s, linkage=4353, uuid=1246984758)
  @4353  Create(blackScreen_C).AddToViewport(0)
         GameInstance.subArea := None
         RetriggerableDelay(5.0 s, linkage=4277, uuid=598071464)
  @4277  lib_C::loadLevel('menu', 'PQXYyeofZ8cr5rJD4YXLVw', clearSubArea=true, self)

lib.loadLevel(level, option, clearSubArea, __WorldContext)
  @20    GameInstance.subArea := None
  @138   GameInstance.NewVar_1 := option
  @219   gamemode.pause_mainMenu.canvas_loading.SetVisibility(b0)
  @333   gamemode.pause_mainMenu.screenSwi.SetActiveWidgetIndex(0)
  @450   gamemode.transition(level)

mainGamemode.transition(LevelName)  ->  ExecuteUbergraph_mainGamemode(92640)
  @92640 SetGamePaused(self, false)
         Delay(0.0 s, linkage=7160, uuid=1237420728)
  @7160  OpenLevel(self, LevelName, true, '')            <-- NATIVE. THE WORLD DIES HERE.
```

### 1.1 Five facts that shape every design

1. **`ragdollMode` NEVER writes `dead`.** The only write in the class is uber
   `@37412`, and **`dead := false` exists NOWHERE** -- the game has no revive.
   A living player after a death is something only a level reload produces.
2. **The chain is LATENT, not a flag.** Two chained `RetriggerableDelay`s carry
   it, and neither re-reads `dead`. While they are pending, clearing the flag
   cancels nothing.
3. **`fallen(false)` re-enters the chain** when `dead` is already set
   (`@39685 IFNOT(dead)` falls to `@39699 JUMP @37478`). An "undo" attempted
   mid-chain re-arms the thing it is undoing.
4. **The whole chain is INVISIBLE to our ProcessEvent detour.** All seven
   `Add Player Damage` call sites, `kill()`, `ragdollMode()`, `fallen()` are
   `EX_LocalVirtualFunction` / `EX_LocalFinalFunction`; `loadLevel` is
   `EX_Context` + `EX_LocalVirtualFunction` on the `lib_C` CDO. Per
   `docs/COOP_DISPATCH_VISIBILITY.md` none of them reach `ProcessEvent`.
   **`OpenLevel` is the one exception, and it is exceptional because it is
   NATIVE** (§3).
5. **The death does NOT save.** `gamemode.save(true,false,false)` sits at uber
   `@39872`, reached only from `@83653`, which is not on this chain. So nothing
   about the death is persisted, and a revive owes no save surgery.

### 1.2 Complete census of `dead` readers (8 sites, whole class)

Needed because a revive that clears the flag must know what it re-enables.

| site | what it gates | after a revive |
|---|---|---|
| `Add Player Damage` @659 | early-out, no damage while dead | damage applies again -- correct |
| `kill` @0 | early-out | correct |
| uber @21701 | **the PAUSE-MENU gate**, not a generic interaction deny (corrected 2026-08-31): `IFNOT(isDreaming\|\|isSleep\|\|!IsPointInBox(camera)\|\|dead\|\|isRagdoll) JUMP @21852 enterPause()`; any term true -> `PlaySound2D(use_deny)` and return | **a dead player cannot open the pause menu at all** -- so no player-authored quit can ever travel with `dead == true`, which is what bounds §3.1's discriminator |
| uber @26584 | `wakeup()` refuses while dead | **the revive NEEDS this cleared** |
| uber @26904 | the `drown` achievement | fires only for a real drown death |
| uber @37412 | the write | -- |
| uber @39685 | the `fallen` branch | -- |
| **uber @56667** | **health regen `saveSlot.health += dt/6`, suppressed only by `dead`** | resumes -- see 6.2 |

The census is complete **inside `mainPlayer_C`**. The outside-the-class half is
now measured too -- see §6a item 6; it is one achievement gate and five hazard/UI
readers, and every one of them wants the LIVING value the revive writes.

---

## 2. Where the cut goes, and why the last stage is the RIGHT one

The chain has three stages that could be cut. They are not interchangeable.

| cut | what survives | verdict |
|---|---|---|
| **top** -- force `@39848` to the living branch | player never dies at all | fixes the symptom by deleting the experience the user asked to keep; also makes `dead` unreachable process-wide with an uncensused blast radius |
| **middle** -- neutralise a delay | `dead` is set, `wakeup` refuses (@26584), regen off (@56667), damage ignored (@659) | a permanently dead, invulnerable, immobile body. Worse than the bug. |
| **last** -- stop the travel and write new state | the entire native death played out; nothing is armed any more | **this one** |

**The fact that makes the last cut safe is the same fact that made every earlier
"undo" impossible.** The two `RetriggerableDelay`s are what could not be
cancelled -- but by the time `OpenLevel` is reached they have both *fired and
been consumed*. At that instant the death is finished, not in flight, and the
state it left behind is ordinary mutable state. That is why a revive is legal
there and illegal five seconds earlier.

---

## 3. The seam: `UGameplayStatics::OpenLevel`

`[V]` `@7160: OpenLevel(self, K2Node_CustomEvent_LevelName, true, '')` in the
`mainGamemode` ubergraph is the single verb that unloads the world. It is a
**native** `UFunction`, so unlike every BP hop above it, it is interceptable.
**No bytecode patch is required anywhere in this design.**

**THE SEAM IS A MINHOOK DETOUR ON THE C++ FUNCTION, NOT A `UFunction::Func` PATCH.**
This is a correction to the first draft, and the reason is mechanical rather than
aesthetic. IDA, 2026-08-31 (`VotV-Win64-Shipping.exe`, imagebase `0x140000000`):

| symbol | address | how it was found |
|---|---|---|
| `UGameplayStatics::execOpenLevel` | `0x1430114A0` | `FNameNativePtrPair{"OpenLevel", ptr}` at `0x14419BC60`; decompiles to the UHT thunk (4 `FFrame::Step`s: object, FName, bool, FString) |
| `UGameplayStatics::OpenLevel` | `0x142B530B0` | the call the thunk ends in (`0x143011614`) |

A `Func` patch replaces the **thunk**, whose job is to walk the caller's bytecode
and pull the four parameters off it. To CANCEL there, we would have to consume
those parameter expressions ourselves -- otherwise the interpreter resumes in the
middle of an argument and executes it as a statement. That means reimplementing
`FFrame::Step` against a scratch buffer, destroying the `FString` we made it
build, and owning that for every future game build. Detouring `0x142B530B0`
instead gets the parameters already parsed, in registers, by the engine's own
untouched thunk, and cancelling is `return;`.

`[V]` The body is small and self-contained -- `GetWorldFromContextObject` (null ->
early out), `GetWorldContextFromWorldChecked`, `FName::ToString`, optional
`"?" + Options`, an `FURL` built against `WorldContext.LastURL`, a
`MakeSureMapNameIsValid` log check, then **`UEngine::SetClientTravel(World, Cmd,
TravelType)`** and the temporaries freed. `SetClientTravel` is the whole effect:
it assigns `FWorldContext::TravelURL`, which `TickWorldTravel` acts on next tick.
So not calling the original means no travel is ever *requested* -- there is no
half-started teardown to unwind, which is exactly what §6.1 item 2 asked.

Two consequences worth writing down:

* `[V]` `OpenLevelBySoftObjectPtr` (`0x142B53350`) calls the same C++ function, so
  ONE detour covers both entry points. Nothing else in the image calls it.
* the detour owns the by-value `FString Options` (MSVC destroys by-value class
  parameters in the CALLEE -- visible in the decompile as `if (*a4) Free(*a4)` on
  every return path). On the death path `Options` is the empty literal, so its
  data pointer is null and there is nothing to free; a general cancel must still
  free it rather than leak.

COST: one new AOB signature in `sdk_profile.h` (the project has six) and a row in
`docs/VERSION_MIGRATION.md`'s version surface. **The signature is derived and
proven UNIQUE (occ=1) on this build** -- prologue through `mov rsi, rax`, with the
`GS`-style rip-relative `GEngine` load and the `GetWorldFromContextObject` rel32
wildcarded:

```
48 89 54 24 10 55 53 56 41 56 48 8D 6C 24 C1 48 81 EC E8 00 00 00 41 0F B6 D8
48 8B D1 48 8B 0D ?? ?? ?? ?? 41 B8 01 00 00 00 4D 8B F1 E8 ?? ?? ?? ?? 48 89
45 9F 48 8B F0
```

ABI for the detour (MSVC x64, confirmed against the decompile): `RCX` =
`WorldContextObject`, `RDX` = `FName LevelName` by value (8 bytes, POD), `R8B` =
`bAbsolute`, `R9` = `FString* Options`. The alternative -- reflection gives
the thunk address for free via `UFunction::Func`, no AOB -- was rejected above on
the cancel mechanics, not on the resolution.

### 3.1 The discriminator

`OpenLevel('menu')` is also the legitimate quit-to-menu and is used for every
other travel. We must intervene on *exactly* the death travel:

> intervene **iff** the local `mainPlayer_C` has `dead == true` at the moment
> `OpenLevel` is entered.

A deliberate quit-to-menu, a save load, the backrooms -- all arrive with
`dead == false`. This is a positive test on state the death chain itself set, on
the same thread, one frame away. **Fail CLOSED: if `dead` cannot be read, let the
travel proceed** -- a player who reaches the menu is recoverable; a player
stranded in a half-torn-down world is not.

### 3.2 What "intervene" means mechanically

Do not call the original. `OpenLevel` reduces to `GEngine->SetClientTravel`; not
invoking it means no travel is ever requested, and the caller (`@7183: POP->ret`)
simply returns. **Unverified** and first on the IDA list (§6): that nothing
upstream of `@7160` has already begun a teardown, and that `SetGamePaused(false)`
at `@92640` leaves nothing needing an undo.

---

## 4. The NEW STATE -- the revive

Written at the cancelled travel, on the game thread, in the game's own verbs
wherever one exists. Order matters.

| # | write | why / provenance |
|---|---|---|
| 1 | `dead := false` | the game never clears it; a plain BP bool. Until this, `wakeup` refuses (@26584) and regen stays off (@56667). |
| 2 | remove the `blackScreen_C` widget | created at `@4353` with `AddToViewport(0)`. **MEASURED (§6a item 4): it can never remove itself** -- the asset's whole name table is `CanvasPanel` + `Image` + `SlateBrush` + anchors, no function export, no ubergraph, no animation. Today the level travel takes it away; cancel the travel and it is a permanent black screen. **AND THE REMOVAL ITSELF IS NOW MEASURED WORKING (§9.2):** `found=1 removeFn=1 viewportFn=1 inViewport 1 -> 0`. Resolve `RemoveFromParent` off the ENGINE `Widget` class (exact-owner) and `IsInViewport` off `UserWidget`. **`stillFindable=1` is CORRECT and it corrects this design**: `RemoveFromParent` DETACHES, it does not destroy, so `FindObjectByClass` still finds the object -- the completion conjunction must therefore use `IsInViewport`, never "is a `blackScreen_C` findable". |
| 3 | `forceWakeup()` | uber `@25800`, UNCONDITIONAL: movement mode, capsule collision, camera re-attach, `SetSimulatePhysics(false)`, `bUsePawnControlRotation`, `EnableInput`, mesh re-attach, **`isRagdoll := false` (`@26497` -- this row omitted it until 2026-08-31; it is what re-opens the `@21777` pause gate after a revive)**, `SetControlRotation`. `[V]` it never calls `fallen()`, so it cannot re-arm the chain. Not `forceGetUp()` (0.2 s latent delay, and lands in `@39685` which re-reads `dead`), not `wakeup()` (refuses while dead). |
| 4 | restore vitals | `saveSlot.health` must be > 0 or `Add Player Damage` re-kills on the next hit. The game's own regen (`+dt/6`, @56667) then runs unaided. |
| 5 | reposition | the game's own respawn verb is `teleportWObackrooms(<transform>, true, false)`, and the game's own below-Z rescue is exactly `@3907 forceWakeup(); @3921 teleportWObackrooms(spawnLocation, true, false)`. **Prefer that pair -- it is literally the game's existing revive.** But know what `spawnLocation` IS: `[V]` uber `@34642` sets it to `GetTransform()` once in the level-start block (beside `gamemode`, `ragdollComponent`, `lastWalk`, `lastLoc`), so it is **wherever the pawn stood when this level loaded** -- the save's position on a loaded game, the PlayerStart on a new one. It is not a fixed КПП, and after a long session it can be kilometres from the corpse. §10 asks the user which they want. |
| 6 | undo `loadLevel`'s menu prep | **MEASURED (§6a item 5), and two of the four are real.** `pause_mainMenu` is added to the viewport ONCE at gamemode init (`gm` uber `@59352 AddToViewport(3)`, then `@59577 SetVisibility(Collapsed)`) and lives there for the session -- so `canvas_loading.SetVisibility(Visible)` and `screenSwi.SetActiveWidgetIndex(0)` are writes to a widget that is still on the player's screen tree, merely collapsed. `ui_menu`'s ubergraph writes `canvas_loading` **nowhere**, and `enterPause` only un-collapses the menu itself -- so a player who cancels the travel and later presses ESC gets a LOADING SCREEN where the pause menu should be. Both must be restored (index back to 1, which is what `ui_menu`'s own in-game Construct sets; `canvas_loading` back to the value read off it while the player was alive, not to a guessed constant). The other two are benign: `GameInstance.subArea := None` is written by `mainPlayer` itself at uber `@4489`/`@5264` in ordinary play, and `NewVar_1` is overwritten by the next real `loadLevel`. |

| 7 | zero `umg_damageIndicator.damage_{up,down,left,right}` | **ADDED AFTER THE FIRST RUNS -- section 4 as designed did not have it, and the omission was found by the USER LOOKING AT THE SCREEN.** `[V]` `Add Player Damage` @4269 accumulates `damage/maxHealth*4` into one of four UMG quadrant floats and nothing in the game clears them. Best-effort; see 11.2. |
| 8 | expire `effect_bloodLoss_C` (`time := 0`) | **ALSO ADDED AFTER THE FACT, same discovery.** `[V]` @3414 spawns a `bloodLoss` effect actor whose PostProcess + `ui_bloodLossBlur` wash the WORLD red, and @2838 pins its duration at the 120 s cap for any lethal hit. Not cosmetic: full health plus active blood loss is incoherent. Best-effort; see 11.2. |

Everything in §1 up to `@4277` is left strictly alone. That is the point.

**Rows 7 and 8 are the honest lesson of this section.** The six writes above were derived
from the death chain's CONTROL FLOW -- what `kill` -> `ragdollMode` -> `fallen` -> the two
delays actually do. Neither red is on that chain: both are authored by `Add Player Damage`
BEFORE the chain starts, as side effects of the damage rather than of the death. A revive
designed by walking the chain therefore could not see them, and no amount of further /qf on
the chain would have found them -- only rendering a frame did.

---

## 5. What this supersedes

**Provenance first: the KO lane is a COMMUNITY contribution.** `e230d8df` is
Tarangok's (`docs/CREDITS.md`), and the rework this arc supersedes (`74c48694`)
is ours. The idea -- that VOTV's kick-to-menu permadeath is unacceptable in coop
-- is theirs and is what the user is now doubling down on. Nothing here is a
criticism of that contribution; the measurements below are about the mechanism,
which nobody could have checked without disassembling the chain.


**`74c48694` ("KO respawn: a death cannot be undone, so stop the death instead")
is superseded whole, and as of `33008d87` it is RETIRED, not merely superseded.**
It held `canRagdoll` shut for the session so the death was never authored -- the
exact opposite of the instruction in §0. Per RULE 2 it does not get to sit beside
this, and the retire went FIRST rather than last for two reasons: fixing H1/H2/H3
would have been work on code scheduled for deletion, and H1 was live on the
deployed build. The decision the previous session left open ("retire the lane with
the arc, or fix H1/H2/H3 in place") is therefore answered: retired.

**Interim behaviour, stated plainly:** until the seam lands, a local death is
VANILLA -- ten seconds, then the main menu -- with `net_pump`'s flee still in
front of it. That is a step back from what the KO lane *claimed* to do and a step
forward from what it *cost* (no ragdoll key, no fall knockdown, no faint, and H1
made that permanent after a cancelled join). Reverting is one `git revert` away if
§9 kills the arc.

The gate MODULE stayed, as planned: `coop/player/ragdoll_gate` is also held by
`wisp_attack_sync` for the Killer Wisp false-grab window, which is a real,
pre-existing need and releases on its own teardown. What went with the lane:
`Holder::KoRespawn`, `ScopedOpen`, `ReleaseAll` and `Holds`, all caller-less
afterwards. H3 was fixed on the way past -- `g_pawn` is a `CachedObjRef`, so
`Hold`/`Release` re-apply through a world-stamped slot read.

**The shipped build has live defects that outlive the design change** (post-ship
audit, 2026-08-31, verified against `74c48694`):

* **H1** `ragdoll_gate::ReleaseAll()` has ZERO call sites, and
  `ko_respawn::OnDisconnect` is only reachable via paths that require a peer to
  have actually connected. A **cancelled or failed join** therefore leaves
  `canRagdoll = false` for the rest of the process: no ragdoll key, no fall
  knockdown, no faint -- and no death either. Both the header and the code
  comment promise this cannot happen.
* **H2** the post-respawn immunity samples `hp` *before* the pin runs and the KO
  trigger then reads that stale value, so the immunity gives zero protection
  against the hit it exists for; with `ko_spawn_at_start = 0` it is a permanent
  KO loop.
* **H3** `tools/reflection/islive_gate.ps1` now exits 1 -- a CI gate regression --
  and the substance is that `g_pawn` is a bare cached pointer we **write a byte
  into** after a world dies (the measured 2026-08-23 dying-world storm shape).
* Plus M1 an unthrottled 125 Hz re-flop with no precondition gate, M2 a
  disconnect mid-KO stranding a host flopped at 1 HP, M3 a widened
  `ResolveRagdollFns` latch turning every ragdoll call into an uncached
  `FindFunction` walk if `forceWakeup` fails to resolve, M4 an unlatched
  `UE_LOGW` on a 125 Hz path (125 synchronous `fflush`/s), and a NaN health
  passing `!(hp > 0.f)`.

All of the above died with `33008d87`. They are kept written down because H1 is
the shape to watch for in the arc's own code: a safety that is only released on a
path that assumes the happy case ran.

Also retired by this design: the health poll as the KO trigger. It raced the
game's own regen (§6.2) and won by ordering accident.

---

## 6. Measure FIRST -- the IDA + runtime list

The user is launching IDA for this. In priority order:

### 6.1 The seam
1. **`UGameplayStatics::OpenLevel`** -- resolve it, confirm the `UFunction::Func`
   patch intercepts a BP `EX_FinalFunction` call to a native (the precedent says
   yes; this target has never been patched). Read the `LevelName` param out of
   the frame so the `'menu'` filter is available as a second discriminator.
2. **Cancelling it is clean** -- that returning without calling the original
   leaves the engine consistent, and that nothing upstream of `@7160` (notably
   `SetGamePaused(false)` at `@92640`, and `loadLevel`'s menu prep) has already
   started a teardown that assumes the travel follows.
3. **Is `@7160` the ONLY `OpenLevel`?** One hit in the `mainGamemode` ubergraph;
   the rest of the game has not been censused. A second travel author would need
   the same treatment.

### 6.2 The state
4. **`blackScreen_C`'s lifecycle** -- does it self-remove, or must we
   `RemoveFromParent`? It is the one artifact a player would see if we get it
   wrong.
5. **Health regen (`@56667`) vs the death window** -- `saveSlot.health += dt/6`
   every tick, suppressed ONLY by `dead`. Confirms two things worth stating:
   the flag really is load-bearing, and any design that triggers on
   `health <= 0` is racing a value that leaves zero within one frame.
6. **External readers of `dead`** -- scan the other dumped assets for
   `EX_InstanceVariable` paths ending in `dead` on a `mainPlayer_C` reference.
   §1.2's census stops at the class boundary.
7. **`spawnLocation`** -- set once at level start (uber `@34642`); confirm it is
   the KPP/bed spot and not wherever the pawn happened to be, and decide whether
   the arc uses it or the coop KPP constants.

### 6.3 The coop dimension
8. Does the local death travel exist on a CLIENT the same way? Everything above
   is per-machine BP; the assumption that a client's death runs the identical
   chain is untested.

---

## 6a. MEASURED, 2026-08-31 afternoon

Six of §6's eight items are answered. Every row is `[V]` -- IDA on the shipped
`VotV-Win64-Shipping.exe`, the kismet dumps in `research/bp_reflection/`, and the
extracted pak. §6 above is kept as the question list; this is the answer sheet.

| # | question | answer |
|---|---|---|
| 6.1.1 | resolve `OpenLevel`, decide the seam | `execOpenLevel` `0x1430114A0`, `UGameplayStatics::OpenLevel` `0x142B530B0`. **The seam moved from a `Func` patch to a MinHook detour on the C++ function** -- §3 carries the reasoning and the decompiled body. `LevelName` is available as a second discriminator (parameter 2, an `FName` by value). |
| 6.1.2 | is cancelling clean? | **Yes.** The function's only engine-visible effect is its last call, `UEngine::SetClientTravel(World, Cmd, TravelType)`, which assigns `FWorldContext::TravelURL` for `TickWorldTravel` to act on next tick. Returning before it means the travel is never *requested*. Upstream, `transition`'s whole body is `SetGamePaused(self,false)` + `Delay(0.0)` (`gm` uber `@92640`) -- and the game is not paused during a death, so that is a no-op -- and `loadLevel`'s four writes are itemised in §4 row 6. Nothing has begun a teardown. |
| 6.1.3 | is `@7160` the only travel? | **Yes, in the game.** `OpenLevel` appears in exactly ONE of the 301 disassembled assets (`mainGamemode`), and `OpenLevelBySoftObjectPtr` / `ServerTravel` / `ClientTravel` / `RestartLevel` / `LoadStreamLevel` appear in NONE. `ExecuteConsoleCommand` exists in 7 assets but never issues an `open`. On the engine side the only caller of `0x142B530B0` is `OpenLevelBySoftObjectPtr` (`0x142B53350`), so one detour covers both. |
| 6.2.4 | `blackScreen_C`'s lifecycle | **It has none.** The asset's name table is `CanvasPanel`/`Image`/`SlateBrush`/`LinearColor`/anchors and contains **no function or ubergraph export at all** (`.uexp` is 894 bytes). It is a static full-screen black image that is destroyed only with the world. Cancel the travel and it stays forever -- §4 row 2 is mandatory. |
| 6.2.5 | regen vs the death window | unchanged from §1.2: `saveSlot.health += dt/6` at uber `@56667`, suppressed only by `dead`. Restated here because it is the reason a `health <= 0` trigger is unusable -- the value leaves zero within a frame. |
| 6.2.6 | external readers of `dead` | **COMPLETE, and it is a complete census, not a sample.** Scanning every `.uasset` in the game for the name-table entry `"dead"` gives **9 assets in the whole 8.17 GB pak**, and the same scan over the 3,050 extracted assets gives the same 9 -- so the extraction contains all of them and the list is closed: `main/mainPlayer` (the owner), `objects/fossilhound`, `objects/rufus`, `objects/components/comp_disintegrate`, `objects/misc/basevoid`, `objects/misc/laserEmitter`, `umg/components/ui_damageIndicator` (all six cast to `Main_Player`), plus `objects/prop_fish` and `objects/dogshaite51/base51` (own variables, no cast). The one disassembled reader is `fossilhound`: `IFNOT(AsMain_Player.dead) POP` -> `progressAchievement('deadFossil')` -- an achievement gate, the same shape as the in-class `drown` one. **The risk direction is benign**: the revive writes the LIVING value, which is what every reader already expects of a live player. |
| 6.2.7 | what `spawnLocation` is | `[V]` uber `@34642`: `spawnLocation := GetTransform()`, once, in the level-start block. See §4 row 5 -- it is the pawn's transform at level load, not a fixed КПП. |
| RUNTIME | does the game do what the bytecode says? | **YES, confirmed 2026-08-31 12:31** (§9.1): `dead` 0 ms, `isRagdoll` 0 ms, `blackScreen_C` 5 125 ms, level travel 10 672 ms, against 0 / 5 000 / 10 000 predicted from the disassembly alone. |
| 6.3.8 | the client's chain | still OPEN. Everything above is per-machine BP so the chain is structurally identical, but no run has ever put a client through it. The instrument in §8 is deliberately SOLO first. |

### 6a.1 What the measurements changed

Three things, and they are the reason this section is above §3/§4 in the reading
order rather than an appendix:

1. **The seam is a plain function detour, not the `Func` patch the first draft
   named.** Cancelling a native thunk means owning the caller's bytecode cursor;
   cancelling the C++ function means `return;`.
2. **The black screen is not "probably self-removing".** It is provably inert, so
   removing it is part of the revive, not a contingency.
3. **The menu prep is not cosmetic.** `pause_mainMenu` is a live, permanently
   viewport-resident widget; leaving `canvas_loading` visible hands the player a
   loading screen the next time they press ESC.

---

## 7. Coop

### 7.1 Authority
Death is per-peer local state. `saveSlot.health` is per-machine and the damage
model is already victim-authoritative (`docs/COOP_DISPATCH_VISIBILITY.md:95`,
measured VERDICT #6 2026-08-29: the DIRECT damage entries are possession-guarded
no-ops on an unpossessed puppet). So a peer reviving ITSELF writes no shared
state and owes no act-as-host intent (`COOP_SYNCER_MODEL` §2b).

### 7.2 What peers see
The KO'd body rides the existing pose stream's ragdoll bit. **Never observed** --
no run has ever put a dead player in front of another peer. It is an evidence
hole, not a claim.

### 7.3 The one real shared-world write
`ragdollMode` runs `simulateDrop(false,false,true)` and `dropGrabObject()` at
`@672`/`@689`, on the COMMON `ragdoll==true` path -- before any `death` branch.
So **every death drops the held/grabbed actor into the world.** Whether that
routes through the existing `prop_drop_intent` lane is **UNMEASURED**. It was
invisible before this arc only because the victim left for the menu; once the
player stays, it is live on every peer's machine. This is the one item on which
"the player now survives" makes an existing gap reachable.

### 7.4 Host death
`net_pump`'s death policy ends the session when the HOST dies. With this arc the
host never leaves the world, so that path stops being reached -- but it is not
fixed, only bypassed, and the fail-safe still needs to route somewhere sane.

---

## 8. Acceptance

**BUILT (`33008d87`): `python tools/mp.py death`, `harness/autotest_death.cpp`.**
It is the old KO test re-aimed (`git mv`, so `--follow` still reaches its
history), SOLO and **SESSIONLESS** -- launched with `set_net_role=False`, which is
what keeps `net_pump`'s flee (gated on a live session) from pre-empting the very
thing being measured. No dev flag and no suppression were needed to get the
window.

It prints two halves, and only one of them can fail:

* **OBSERVATION** (never fails): the timeline `dead` / `isRagdoll` /
  `blackScreen_C` / level-travel in ms after the hit, against the RE's prediction
  of 0 / 5 000 / 10 000; plus §9's memory differential.
* **ACCEPTANCE** (fails): D1 the death ran, D2 the ritual played (a black screen
  appeared), D3 the world survived, D4 `dead` went false again, D5 the player is
  standing with health, D6 no balloon. **D3/D4/D5 are EXPECTED RED and `mp.py
  death` exits non-zero until the seam lands.** That is §8's own requirement met:
  the instrument being replaced passed 5/5 while three HIGH defects were live in
  the lane it certified, so this one is built to fail on the build that lacks the
  fix.

D1 and D2 are the falsifiers. Without them "the world survived" would go green on
a run where the hit simply never landed -- which is exactly how the old test's
first run failed (a 1000-damage hit vanished into `startInvinc` and the verdict
could not say why). The pre-hit state line names every early-out term.
* New arms this arc needs: the full native death is allowed to run (assert the
  black screen appeared and ~10 s elapsed); `OpenLevel` was reached and
  cancelled; `dead` went true and then false; the player is standing, has
  positive health, and is at the respawn point; **and a deliberate quit-to-menu
  still travels** (the discriminator's negative control -- without it, a fix that
  cancels every travel passes).
* Still owed from the previous lane: the lethal arm on a CLIENT, and the
  cross-peer appearance of a dead player.

---

## 9. M0 -- the one measurement that can still kill this design

**Nothing above matters if a dead player standing in the gameplay world leaks
memory, because keeping them there is the whole arc.**

`src/votv-coop/src/coop/session/net_pump.cpp` has asserted since 2026-06-01, from
"4 hands-on + an autonomous probe arc", that:

> *"The balloon is VOTV's OWN possessed-ragdoll leak in the GAMEPLAY world
> (~165 MB/s to OOM). VOTV's native death never leaves the world -- it just
> leaves you ragdolling. So the cure is to LEAVE the gameplay world."*

At 165 MB/s the arc's own ten-second window costs ~1.6 GB before the revive even
runs, and if the rate does not stop at the revive it is unbounded. So this is a
gate, not a footnote.

**The claim is inherited, not measured, and one half of it is already false.**
"VOTV's native death never leaves the world" is contradicted by §1: the chain
reaches `loadLevel('menu')` at +10 s. The most likely explanation is that nobody
ever saw those ten seconds -- our own flee fires within one pump tick of `dead`
going true, so the observed window was ours, not the game's. The other half (the
rate) has **no surviving finding doc anywhere in `docs/` or `research/`**; it
exists only in that comment and in `autotest_menutravel_probe.cpp`'s header. It
also predates the v122 no-passive-mint root fix, which removed ~2,200 zombie
element rows per join -- exactly the shape of thing that produces a per-tick
balloon. And it cannot be a pure-VOTV effect at that magnitude without vanilla
single-player players noticing a 1.6 GB spike on every death.

So it is re-measured before anything is built on it, and as a DIFFERENTIAL rather
than an absolute (`[[lesson-measure-the-differential-and-the-guard-disappears]]`):
the instrument samples RSS across a 10 s window with the player ALIVE and standing
still, then across the dead window, same process, same save, same cadence. Shared
drift cancels; `D6 no-balloon` fails if the dead window exceeds the alive control
by more than 20 MB/s -- an order of magnitude under the claim and an order of
magnitude over ordinary streaming churn, so neither answer is a coin flip.

### 9.1 RUN, 2026-08-31 12:31, and the claim does not survive it

`python tools/mp.py death`, solo + sessionless, fresh save, DLL built at 12:20:09.
Verbatim from `Game_0.9.0n_HOST/.../multivoid.log`:

```
death_test: pre-hit state -- havePawn=1 canRagdoll=1(read=1) health=100.00
            startInvinc=0(read=1) immortal=0(read=1) dead=0 inGameplay=1 rss=3312.7 MB
death_test: ALIVE control window -- 3312.7 -> 3290.5 MB over 10000 ms (-2.21 MB/s, peak 3313.1)
death_test: delivered Add Player Damage(1000) (health was 100.00, invoke=ok)
death_test: TIMELINE (ms after the hit) -- dead=0 ragdoll=0 blackScreen=5125 travel=10672
            [RE predicts dead~0, blackScreen~5000, travel~10000]
death_test: DEAD window memory -- 3289.3 -> 3290.5 MB over 10672 ms (0.11 MB/s, peak 3308.3);
            ALIVE control -2.21 MB/s; DIFFERENTIAL 2.32 MB/s
death_test: D1 death-ran PASS / D2 ritual-played PASS
death_test: D3 world-survived FAIL / D4 revived FAIL / D5 standing FAIL   (expected)
death_test: D6 no-balloon PASS
death_test: VERDICT FAIL (3 pass / 3 fail)
```

**M0: the dead window grew 1.2 MB in 10.7 seconds.** Differential against the
alive control: **+2.32 MB/s**, against an inherited claim of ~165 MB/s -- about
**1.4% of it**, and the absolute slope inside the dead window (`+0.11 MB/s`) is
noise. There is no possessed-ragdoll balloon. **The arc is cleared on the one risk
that could have killed it.**

Two things that measurement does NOT say, and both matter:

* It falsifies the claim **as written** -- *"VOTV's OWN possessed-ragdoll leak in
  the gameplay world"*. It does not prove no balloon exists with a **coop session
  running**, because this run had none by design (that is what let the death play
  out). If a session-scoped balloon does exist it is OURS -- puppets, element
  mirrors, prop shadows -- which makes it a root to fix, not a reason to leave the
  world. That re-run is owed once the seam lands and the arms can run in a lobby.
* The window is 10.7 s because the travel ended it. After the revive the player is
  an ordinary living player again, so the dead window IS the interval at risk --
  but a long-soak arm after a revive is cheap and should exist.

**And the bytecode RE is now confirmed at runtime.** `dead` at 0 ms, `isRagdoll` at
0 ms, `blackScreen_C` at 5 125 ms, the level travel at 10 672 ms -- against
predictions of 0 / 5 000 / 10 000 from the disassembly alone, at a 250 ms sample
interval. §1's chain is not a reading of bytecode any more; it is a description of
what the game does.

A third, quieter data point: the travel ran with our ProcessEvent detour fully
live and no bypass armed, the teardown completed, and RSS fell 3290 -> 2948 MB at
the menu. `net_pump`'s "our detour HANGS the 50k-actor teardown" is about the
session path and is not re-opened here, but nothing on this path reproduced it.

---

### 9.2 The second measurement pass, 2026-08-31 13:56-13:58 (`58a13231`)

Three of the design's remaining unknowns needed the GAME rather than another round of
critique. `mp.py death` gained a second configuration, because the two exclude each
other: the flee is gated on a live session, so a run that HAS one never sees the
native chain, and a run that LACKS one never reaches the veto the design turns on.

| | sessionless (`mp.py death`) | solo host (`mp.py death --session`) |
|---|---|---|
| `sessionRunning` | 0 | **1** -- a zero-client host satisfies the gate at RUNTIME, not just per `session_start.cpp:234` |
| `dead` / `ragdoll` | 0 / 0 ms | 235 / 235 ms |
| `blackScreen` | 5 125 ms | **-1 (NEVER APPEARS)** |
| travel | 10 703 ms | **4 469 ms** |
| verdict | FAIL 3/3 | FAIL 2/4 (D2 also fails) |

**1. `RemoveFromParent` works on a widget the game owns** -- the last unmeasured step
of the revive. `found=1 removeFn=1 viewportFn=1 inViewport 1 -> 0, stillFindable=1`.
Two corrections came out of it, both from the probe reporting more than one fact:
`stillFindable=1` is correct (detach is not destroy), so **the completion conjunction
must key on `IsInViewport`, not on findability**; and removing the widget did NOT
perturb the chain -- the travel still landed at 10 703 ms against 10 672 ms in the run
where it stayed, so the black screen is not load-bearing and the revive may dispose of
it first.

**2. The flee pre-emption is OBSERVED, and it is worse than the design assumed.** With
a live session the black screen NEVER APPEARS and the world is gone at 4.5 s. So in
coop today the player is not merely thrown to the menu -- they are yanked out **less
than halfway through the death**, before the ritual §0 asks us to keep even begins.
That is the sharpest available statement of what this arc fixes, and until now it was
read out of `net_pump` and never watched. (Prediction corrected: §6a said the travel
stamp would read ~8 ms. It reads 4 469 because the instrument stamps the WORLD CHANGE,
not the travel REQUEST -- the request goes out ~1 s after the hit and the teardown plus
menu load takes the rest. ~8 ms was the flee's reaction latency to observing `dead`;
two different quantities.)

**3. The grab question is NOT answered, and the instrument says so.** `grabbing_actor`
resolves, but nothing was grabbed in either run, so `grabCleared=235` means "was
already clear" and the post-sample's `haveGrab=0` is a READ FAILURE (no pawn after the
travel), not a grab state. Whether `dropGrabObject` reliably clears it before a revive
teleports needs an arm that GRABS FIRST; a passive sample structurally cannot answer
it. Left open rather than counted.

**M0, third and fourth points, and a correction to the metric.** Dead-window slopes
0.06 and 0.00 MB/s. But the DIFFERENTIAL moved 2.32 -> 3.78 purely because the alive
control drifted more negative (-2.21 -> -3.72) while the dead window's own slope stayed
~0.1 -- so **the stable number is the dead-window slope, not the differential**; the
control subtracts a drift that is not constant. Against a claimed 165 MB/s either
framing is decisive, but the larger one is the flatterier one.

---

## 10. The three product decisions -- ANSWERED (USER, 2026-08-31)

All three came back the same afternoon they were asked. They are decisions about WHAT the mod does,
so they are recorded verbatim-in-substance and are not re-litigated by the design.

1. **SINGLE-PLAYER IS NOT TOUCHED, AND "SINGLE-PLAYER" IS DEFINED.** *"Gate of course, we only work
   in coop, single player games are not touched by us."* Refined the same day, when the `/qf` pass
   asked whether a lone host counts: *"Solo host in a session a a coop session and revive should
   work there. Single player is when playing solo game in solo save, no session."* So the term is
   **`Session::running()`** and nothing more -- a HOST with zero clients IS a coop session and gets
   the revive; single-player means no session exists, and that path is untouched because the gate's
   first term is false. No peer-count term, no opt-in row. **The detour is still installed
   process-wide** (it has to be -- it is a function detour), so the SESSION TEST BELONGS IN THE
   VETO, and with no session the veto returns "let it travel" on the cheapest possible path.
   This also keeps the acceptance rig honest: `running_.store(true)` fires at `Start()` with
   `state_ = Handshaking` (`session_start.cpp:234`), so a solo host satisfies the gate and commit A
   stays provable in ONE process.
2. **THE КПП, not `spawnLocation`.** *"Кпп"*. The revive teleports to the coop start landmark, the
   same point the client is teleported to after a load under the user's 2026-05-23 "spawn remote on
   КПП" rule -- `P::name::kKPPSpawnX/Y/Z` = `(-37695, 69978, 6420)`, which survives the KO lane's
   retirement because it lives in `sdk_profile_names.h`. `spawnLocation` is NOT used. **Known
   fragility, flagged not hidden:** that constant is a hardcoded world position derived from one save
   and its own comment says "Re-derive per game version" -- so the arc inherits a
   version-and-save-shaped assumption that `spawnLocation` would not have had. It is the same
   constant the retired lane used and the same one the client teleport uses today, so the arc adds no
   NEW fragility; it does mean a bad constant now costs a revive, not just a spawn offset.
3. **THE DEATH IS ANNOUNCED IN CHAT.** *"В чат пишем"* -- a chat line, not the activity feed and not
   a silent revive. So the revive authors one chat message. That makes the revive's last step a
   NETWORK write, which nothing else in the revive is: everything else is local state on the dying
   peer's own machine (§7.1). Ordering and authorship of that line are the design's problem, not the
   user's.

---

## 10a. Superseded: the questions as they were asked

These three are genuinely the user's -- each changes WHAT the mod does, not how.
A recommendation is attached to each so that none of them blocks building.

* **Single-player.** The `OpenLevel` detour is process-wide and does not depend on
  a session, so a solo player would get the revive too -- which changes vanilla
  VOTV's balance for someone who never asked for coop. *Recommendation: gate the
  revive on a live coop session, plus a `[death]` config row so a solo player can
  opt in.* Building it session-gated costs nothing and is reversible in one line.
* **Where does the revive place the player?** `spawnLocation` is the game's own
  answer and is measured (§4 row 5): the pawn's transform at level load, which
  after a long session can be kilometres from where they fell. The alternatives
  are the coop КПП start point, or in place. *Recommendation: `spawnLocation` via
  the game's own `forceWakeup` + `teleportWObackrooms` pair, because it is
  literally the game's existing revive and it keeps death costing something --
  reviving in place makes dying free.*
* **Does anything mark the death?** Right now a revived player would be
  indistinguishable from one who never died. *Recommendation: one activity-feed /
  chat line ("<Nick> died"), which the existing `peer_action_feed` grammar already
  supports, and nothing else -- no stat, no penalty -- until the user asks for a
  cost.*

---

## 11. AS-BUILT -- commit A, 2026-08-31

Three source changes and one instrument change. Everything in this section is `[V]` from a
real log; nothing here is a plan.

### 11.1 What shipped

| # | file | what it is |
|---|---|---|
| 1 | `ue_wrap/engine/level_travel.{h,cpp}` | the seam. AOB-resolves `UGameplayStatics::OpenLevel` (`profile::kSigOpenLevel`, occ=1, re-verified against the shipping PE the day it was consumed) and MinHook-detours it. Offers `SetVeto`; with no veto published it is a pure pass-through. Owns the by-value `FString Options` free on the cancel path ONLY -- freeing it AND calling the trampoline would be a double free. SEH-wrapped around the callback, failing OPEN. |
| 2 | `coop/player/death_revive.{h,cpp}` | the gameplay half. Publishes the veto's inputs from the pump, ARMS on the `dead` rising edge, and runs the six-write revive on the pump task after the detour refuses a travel. |
| 3 | `net_pump.cpp` | the death edge now asks `death_revive::ArmedForThisDeath()` FIRST. Armed -> the flee stands down and the block FALLS THROUGH (the pump must keep ticking; it is what performs the revive). Not armed -> the flee is unchanged. |
| 4 | `teleport_client::ApplyLocally` | returns `bool` now. It reports that a call was dispatched, NOT that the player moved -- the revive verifies the position separately. |
| 5 | `harness/autotest_death.cpp` + `mp.py death` | config-aware acceptance (11.3). |

### 11.2 Eight things the implementation settled that the design text did not

1. **THE SEAM IS ARMED UNCONDITIONALLY, AT THE TIMELINE TICK -- not from the pump.** The
   first build installed it lazily from `net_pump::Tick`, which only runs with a live
   session. The sessionless negative control then passed while reporting `installed=0`:
   it was grading a hook that had never been created, so "the seam refused nothing outside
   coop" was true for the wrong reason and the single-player guarantee rested on the hook's
   ABSENCE rather than on the veto's own session test. Installing it always (a pass-through
   until a death arms it) is what makes the negative control mean anything.
   `[[lesson-an-instrument-blind-to-the-phenomenon-always-passes]]`.
2. **The instrument's mutating BLACKSCREEN PROBE is RETIRED (RULE 2).** Its question --
   does `RemoveFromParent` work on a widget the GAME owns -- was answered on 2026-08-31, and
   once the revive performed that removal for real the probe STOLE the step: it removed the
   black screen at +5.7 s, so at +10 s the revive found nothing and its own black-screen term
   graded a no-op. The replacement is a passive arm (D9) that watches the widget reach the
   viewport and leave it.
3. **THE DEATH LEAVES TWO INDEPENDENT REDS ON THE SCREEN, AND SECTION 4 LISTED NEITHER.**
   Both were found by the USER LOOKING AT A RUN, not by any arm -- the design's six writes had
   been reasoned about entirely from the death chain's control flow, and neither red is on it.
   They are the same CLASS as the black screen (an artifact the level travel used to dispose
   of) and they are two different mechanisms:
   * `[V]` `Add Player Damage` @4269 does `VictoryFloatPlusEquals(gamemode.playerInterface
     .umg_damageIndicator.damage_{up,down,left,right}, damage/maxHealth*4)` -- it ACCUMULATES
     into one of four UMG quadrant floats, chosen by the hit's direction. Nothing in the game
     clears them.
   * `[V]` @3414 also calls `lib_C::addEffect('bloodLoss', ...)` -> `mainGamemode::addEffect`,
     which SPAWNS an `effect_bloodLoss_C` (its own `PostProcessComponent` + a
     `ui_bloodLossBlur_C` widget) and tracks it in `gamemode.effects`/`effects_names`. @2838:
     `time = FClamp(Lerp(10,5,maxHealth/100) * (dmg/5 + 0.01) * 1.5, 0, 120)`, so **any**
     lethal hit pins it at the **120 second cap** at strength 1.0. That one is NOT cosmetic:
     a revive that writes full health and leaves the player BLEEDING OUT is incoherent.
   The revive now zeroes the four quadrants and writes `time := 0` on live
   `effect_bloodLoss_C` actors -- deliberately NOT calling the actor's own `destroy`, whose
   body has not been read; writing the countdown to zero makes the effect's own `ReceiveTick`
   run the expiry path it runs every time an effect ends naturally, so the gamemode's two
   parallel arrays cannot desynchronise. Scope is `bloodLoss` ONLY: the death adds exactly
   that one effect, and clearing food poisoning / LSD / sleepiness would be the revive helping
   itself to state it did not author. Both are BEST-EFFORT at runtime (a red screen is not
   worth fleeing to the menu over) and both are ASSERTED by the instrument (D10, D11) -- the
   runtime's give-up threshold and the test's bar are different questions and may differ.
4. **THE INSTRUMENT'S LETHAL HIT WAS 10x MAX HEALTH, AND THAT WAS ITS OWN DEFECT.** It made
   the first red catastrophic (40 units instead of 8) and read as a bug in the arc. `[V]` the
   only scaling anywhere on the damage path is `SelectFloat(0.75, 1.0, isStrong)` (@856) --
   damage is NEVER scaled UP -- so 2x is lethal with a 100% margin. A synthetic trigger has to
   stay inside the range the game itself produces, or it measures its own exaggeration.
5. **`dead` is resolved through `FindBoolProperty` (byte + MASK), not the plain-byte read the
   sender path uses.** We WRITE it, and a masked bool shares its byte with its neighbours.
6. **THE CONJUNCTION IS ABOUT THE PLAYER, NOT ABOUT THE SCREEN -- and getting that backwards
   cost a run.** `blackCleared` was a conjunction TERM as first built. On 1 run in 4 the
   same-frame `IsInViewport` read after `RemoveFromParent` still came back TRUE, the
   conjunction refused, and the revive fell back to `FleeToMainMenu` -- so the player was
   ejected to the MAIN MENU, and on a host the LOBBY ENDED, because a widget had not detached
   yet. That trade is upside down: a black screen on a living player is bad; being thrown out
   of the world is the exact failure this arc exists to remove. The three screen artifacts
   (black screen, damage quadrants, bloodLoss) are now RETRIED on the following pump ticks
   (`TickScreenCleanup`, up to 120, stopping the moment all three read clear) and the
   conjunction keeps only terms that mean THE PLAYER IS ALIVE AND PLAYABLE: `dead` cleared,
   not ragdolling, health > 0, at the KPP, and the four calls that reported success. A
   cleanup that never finishes now logs a warning and leaves the player in the world.
7. **THE INSTRUMENT'S HIT PASSED `blood=false`, SO THE BLOOD-LOSS ARM WAS GREEN FOR NOTHING.**
   `[V]` `Add Player Damage` @2784 gates the whole `addEffect('bloodLoss', ...)` block on its
   `blood` parameter, and `E::InvokeAddPlayerDamage` set only `Damage` -- so the synthetic
   death never created the effect at all and D11 reported "0 live effect_bloodLoss_C" as a
   PASS on the very run that was added to catch it. The wrapper now takes `blood` (defaulting
   false, so every existing caller is unchanged) and the instrument passes true. This is the
   second instrument-blindness in this section and the third in this arc; see 11.2 item 1.
8. **`ArmedForThisDeath()` does NOT latch `g_localDeathHandled`.** That flag is SHARED with
   the host-close arm, and latching it would stop the very block that has to keep ticking
   through the death window. `death_revive` owns its own per-death state.

### 11.3 The acceptance, and why there are two runs

`python tools/mp.py death --session` (SOLO HOST) is the acceptance; `python tools/mp.py
death` (SESSIONLESS) is the **negative control**, and it is not a lesser run -- without it a
fix that cancelled EVERY travel would pass. The user's decision is that single player is
untouched, so a sessionless death must still travel and the armed seam must refuse nothing.

**Run 1 -- `mp.py death --session`, 14:56, VERDICT PASS (9 pass / 0 fail).** Verbatim:

```
level_travel: seam INSTALLED (UGameplayStatics::OpenLevel@00007FF60A3E30B0; pass-through
              until a veto is published)
death_revive: revive verbs resolved (remove=... inViewport=... setVis=... setIdx=...
              pause_mainMenu=0x4A8 canvas_loading=0x320 screenSwi=0x350)
death_revive: local death ARMED -- the native death runs to completion
death_revive: level travel REFUSED at UGameplayStatics::OpenLevel -- the world is kept
death_revive: REVIVE OK -- black=1 vitals=1 wake=1 tele=1 menu=1 deadClr=1
              | readback: ragdoll=0 dead=0 hp=100.0 distKPP=0 cm (tol 300)
death_test: SEAM -- installed=1 travelsRefused=1 lastReviveOk=1 sessionRunning=1
death_test: TIMELINE (ms after the hit) -- dead=0 ragdoll=0 blackScreen=5141
            blackGone=10266 travel=-1 grabCleared=0
death_test: D1 death-ran PASS / D2 ritual-played PASS / D3 world-survived PASS
            D4 revived PASS / D5 standing PASS / D7 at-KPP PASS (259 cm)
            D8 menu-restored PASS (screenSwi=1 canvas_loading vis=1)
            D9 black-screen-cleared PASS (viewport 5141 ms -> gone 10266 ms) / D6 PASS
```

The timeline is the arc's whole claim in one line: the ritual played to +5 141 ms, the
travel reads **-1** (it never happened), and the black screen left at **10 266 ms** -- about
125 ms after the refusal, which is the revive removing it.

**Run 2 -- `mp.py death`, 14:55, VERDICT PASS (6 pass / 0 fail), on the same bytes.**
`sessionRunning=0`, `travel=10718` (vanilla: ten seconds, black screen, main menu), and
critically `installed=1 travelsRefused=0` -- the seam IS armed and DECLINES. That is the
single-player guarantee proven rather than assumed, and it is only meaningful because of
11.2 item 1.

**ONE UNEXPLAINED RESIDUAL, stated rather than smoothed over.** The revive's own read-back
reports `distKPP=0 cm` at the instant it completes, but the acceptance sample 11 s later
reads **259 cm**. So the player lands exactly on the KPP and then settles or slides ~2.6 m
over the following seconds. It is inside D7's 500 cm tolerance and it looks like ordinary
gravity settling at that spawn point -- the same thing a joining client's KPP teleport
would do -- but WHY has not been measured, so it is written down rather than called nothing.

### 11.3a THE RED SCREEN -- RETRACTED WHOLE 2026-08-31, and what it really was

> **THIS SECTION'S CONCLUSION WAS WRONG, AND SO WAS EVERY ROW OF ITS NEGATIVE-EVIDENCE
> TABLE. The red is the DAMAGE SCREEN EFFECT -- exactly what the user said from the
> first message. There was never a second, world-level, unexplained red.**
>
> **The error was an INSTRUMENT ARTIFACT, not a reasoning slip.** `mp.py`'s
> `_shot_before_and_after` waited for the log line `death_test: ALIVE control window`
> before capturing what it named `A_prehit`. Measured from a preserved log
> (`scratchpad/death_run_180149.log`, lines 10373-10374):
>
> ```
> 18:01:24  death_test: ALIVE control window -- ...
> 18:01:24  death_test: delivered Add Player Damage(200, blood=true)   <- THE LETHAL HIT
> 18:01:25  A_prehit_33988.png written                                 <- the "pre-hit" shot
> ```
>
> Those two log lines are ADJACENT and land in the SAME SECOND, so the poll always fired
> after the hit. **Every `A_prehit` frame this instrument ever produced was a POST-hit
> frame.** The table's first row then paired that PNG with `health=100.00` from the
> `pre-hit state` line logged ELEVEN SECONDS EARLIER, and concluded "the tint is present
> at full health, therefore not the death". Two different instants, read as one.
>
> **What the red actually is**, all measured in the same run: all four
> `damage_{up,down,left,right}` quadrants at **8.00** (the all-quadrant branch -- the
> synthetic hit has `damageLocation == (0,0,0)`), `dmg_tunnel` at alpha **1.0**
> (`1 - health/100` with health 0), and the `blood=true` `effect_bloodLoss_C`
> post-process. "I'm too injured" in the frame is the damage reaction line.
>
> **The falsifying controls** (`mp.py hudtint`, `coop/dev`-style probe in
> `harness/autotest_hud_tint.cpp`), each a real captured frame at `health=100.00`:
> a New Game is **NOT red**; `s_test_screens2` -- the save every death run loads, the one
> this section blamed -- is **NOT red**; sessionless is **NOT red**; and **not red even
> with `VOTVCOOP_RUN_DEATH_TEST` armed**, pre-hit. The instrument now waits for
> `death_test: pre-hit state` (~11 s before the hit) and its frame is clean.
>
> So these rows are RETRACTED specifically: "the arc itself -- not the death" (it IS the
> death), "the save -- a New Game is equally red" (a New Game is not red at all), and the
> whole "AND THE SOURCE IS NAMED ... it is VOTV's own BAD SUN" paragraph. The Bad Sun
> reading was already self-retracted at the bottom of this section; it is now moot.
>
> **What still stands, and is the real defect this dig found:** `ui_damageIndicator_C`'s
> Tick death branch does `@2292 dmg_full.SetVisibility(Visible)` while `dmg_full` is
> authored `Collapsed`, and the ALIVE path never writes that field -- a ONE-WAY LATCH
> vanilla never has to undo because it always tears the world down. We keep the world, so
> a revived player keeps a full-screen red image forever. See 11.3b.
>
> Lesson: `[[lesson-a-screenshot-and-a-log-line-are-two-instants]]`.

### 11.3a-orig (superseded) THE RED SCREEN: what it was, and the two hours it cost

The user watched a run and reported the revived player standing at the KPP with the whole
screen washed red. Four successive investigations followed, each finding a real thing, each
measuring its own target clean, and each followed by "still red":

1. `umg_damageIndicator.damage_{up,down,left,right}` -- REAL, and the revive now clears them.
2. `effect_bloodLoss_C` and its 120 s PostProcess -- REAL, and the revive now expires it.
3. `ui_bloodLossBlur_C` -- measured gone at 10 203 ms with the actor's own teardown.
4. A full enumeration: every viewport widget, every `effect_C` descendant,
   `gamemode.effects_names.Num=0`. All clean.

**Then one screenshot taken during the ALIVE CONTROL WINDOW -- before the test's own lethal
hit, player at full health, `dead == false` -- came back FULLY RED.** The tint is a property of
the WORLD in the save these runs load (day 324, `gamerule_permfog`), present before the death,
during it and after it. `research/death_shots/A_prehit_*.png` and `B_postrevive_*.png` are the
pair, from one run.

So the reported symptom was never the arc. Items 1 and 2 above are still real defects and still
worth the fix -- a revive that writes full health while leaving the player bleeding out is
incoherent whatever else is on screen -- but they were not what the user was looking at, and
the user's first screenshot was those items STACKED ON the world's own red (which is also why
it looked opaque: `[V]` @1234 branches on `damageLocation == (0,0,0)` and the synthetic hit
takes the ALL-FOUR-QUADRANTS path, so the instrument was painting a 360-degree indicator no
real directional hit produces).

**AND THE SOURCE IS NAMED, BY MEASUREMENT, AT THE END OF A FULL RENDER-PIPELINE SWEEP: it is
VOTV's own BAD SUN.** The tint is `pp_megasun_Inst`, a post-process MATERIAL on
`PostProcessVolume2_3` -- read live at `w=1.00 unbound=1`, and an UNBOUND volume at full
weight tints the entire world from any position, before UI, which is exactly the observed
signature (sky, stars and near ground equally red, HUD untouched). That volume is LEVEL
CONTENT: `_map_untitled_1.json` export 28753 serializes `bUnbound=true` with `BlendWeight` and
`bEnabled` at class default. What switches it on is `daynightCycle`'s new-day block:

```
@4552: IFNOT(false) JUMP @4595              <- the direct path is dead code
@4704: if (GameInstance.gamemode == b7) JUMP @4558
@4558:     gamemode.Spawn Bad Sun()
@4822: else if hasAchievement('badsun')
@4896:     RandomBoolWithWeight(0.001)      <- 0.1% per day
@4931:     -> Spawn Bad Sun()
```

Everything else in the pipeline was read and cleared on the way there, and the list is worth
keeping because it is the order a future tint hunt should follow: camera fade `enabled=0`; fog
inscattering `(0.447,0.638,1.000)` -- BLUE, so fog was never it; `PostProcess_pl`'s one
blendable at weight **0.000**; no `bOverride_Color*` on any volume; `PostProcessVolume_1` =
`inst_depthPP`; `Camera` = `PP_main` + `pp_mirror`(w=0) + `pp_stencilOutline`. And OUR MOD
holds ZERO references to `PostProcess*` anywhere in the tree, with every fog path
`g_isClient`-gated and therefore inert on a solo host.

**WHAT THE SOURCE IS NOT, so the next hunt starts where this one stopped.** Everything below was
read live, in this order, and every one came back negative:

| mechanism | reading | verdict |
|---|---|---|
| the arc itself | tint present in the PRE-HIT frame (`health=100.00`, `dead=0`, quadrants `0.00`, bloodLoss actors `0`) | not the death |
| the save | a **New Game** (`mp.py death --fresh`) is equally red | not the world's state |
| camera fade | `bEnableFading=0` | no |
| fog inscattering | `(0.447,0.638,1.000)` x2 + `(0.006,0.006,0.010)` -- **BLUE** | no |
| player post-process | its one blendable `NewMaterial8` at weight **0.000** | no |
| volume colour grading | no `bOverride_Color*` set on any of the three volumes | no |
| volume/camera blendables | `inst_depthPP`, `pp_megasun_Inst`, `PP_main`, `pp_mirror`(w=0), `pp_stencilOutline` -- all level/BP content, always resident | inconclusive, see below |
| the Bad Sun branch | `GameInstance.gamemode = b0`, NOT `b7` | the deterministic `Spawn Bad Sun` path did NOT fire |
| graphics settings | still red with `sg.PostProcessQuality` and `sg.ShadingQuality` raised 1/0 -> 3/3 | no |
| **our overlay** | **DX11 and DX12 render it IDENTICALLY** -- two separate backends of ours, so a state leak in `RenderDrawData` would have to exist in both | **our rendering is exonerated** |
| our mod generally | ZERO references to `PostProcess*` anywhere in the tree; every fog path is `g_isClient`-gated and inert on a solo host | not us |

One reading here is RETRACTED and must not be re-derived: `pp_megasun_Inst` sitting on an unbound
volume at `w=1.00` was read as proof of VOTV's Bad Sun. It is not. `APostProcessVolume` defaults to
`BlendWeight=1.0` / `bEnabled=true`, so that volume is loaded and blending in EVERY VOTV world; its
material must be parameterised, and its mere presence proves only that the level ships it. The
`gamemode=b0` reading then killed the deterministic trigger outright.

**The process lesson is the expensive one and it is written up separately**
(`[[lesson-a-symptom-needs-a-baseline-before-it-needs-a-hypothesis]]`): after the SECOND clean
probe with the symptom unchanged, the frame of reference is what is wrong, not the hypothesis.
The baseline capture cost one minute and would have been correct in the first.

### 11.4 What commit A does NOT do

* **No announcement.** The user asked for a chat line ("В чат пишем"); chat is HOST-AUTHORED
  since v133, so a client cannot author one. That is ReliableKind 128, two directions
  (client intent -> host state + line), with the per-slot death state SEEDED ON JOIN per
  principle 8. Commit B, and it bumps the protocol.
* **No two-peer witness.** Section 7.2 (what other peers see of a dead player) and section
  6.3.8 (does a CLIENT's chain run identically) are still unobserved. Both need a two-peer
  run, which is commit B's acceptance.
* **The death's DROP is still unanswered.** Section 7.3: every death runs `dropGrabObject()`,
  and `[V]` the drop reaches `BeginDeferredActorSpawnFromClass` -> `FinishSpawningActor`, the
  exact seam `prop_drop_intent` hooks -- but only for keys in that client's park set. Both
  runs had nothing grabbed (`grabValid=0` pre-hit), so the arm that would answer it has never
  run. It needs a run that GRABS FIRST; a passive sample structurally cannot answer it.
* **M0 in a real lobby.** Both runs are one process. The differential measured 6.11 and 1.83
  MB/s -- and the coop run's window is now 22 s covering 10 s dead plus 11 s REVIVED, which
  is the post-revive soak section 9.1 said was owed. A multi-peer re-take is still owed.

### 11.5 Two product consequences the user should know

Both follow from the flee moving off the death edge, and both were raised during the design
pass rather than discovered afterwards:

1. **"HOST DEATH ENDS SESSION" no longer happens on the happy path.** That is a recorded USER
   DECISION (2026-05-30, `votv-player-vitals-death-RE-2026-05-30.md` 4.5), and it was
   mechanically produced by `session.Stop()` inside `FleeToMainMenu`. A host that dies and is
   revived never calls `Stop()`, so the session simply continues. This is almost certainly
   what the 2026-08-31 instruction wants -- the whole point is not losing the world -- and it
   closes a known-open item. But it IS a product change, so it is written here rather than
   discovered later.
2. **A FAILED revive still ends the lobby.** The failure path is `FleeToMainMenu`, whose
   `Stop()` is what lets the second travel through our own veto. So the fork above governs
   only the happy path: revive succeeds -> lobby lives; revive fails -> lobby still ends.
