# DEATH ARC -- native death, without losing the world

**Status: DESIGN + RE, with §6 MEASURED (2026-08-31 afternoon). Nothing of the arc
is built yet.** The KO lane that used to be built is **RETIRED** (`33008d87`) -- see
§5 -- and the instrument that replaces its test is in the tree and expected RED.
**Read §6a before §3 and §4: it closes six of §6's eight items, and two of its
findings change what the arc has to write.** §9 (M0) -- the one measurement that could
have killed the design -- **RAN, and cleared it**: the inherited "~165 MB/s
possessed-ragdoll leak" measures **+2.32 MB/s** against its own alive control.

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

Everything in §1 up to `@4277` is left strictly alone. That is the point.

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
