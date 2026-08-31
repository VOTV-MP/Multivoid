# DEATH ARC -- native death, without losing the world

**Status: DESIGN + RE. Nothing in this doc is built.** The KO lane that *is* built
(`74c48694`) is SUPERSEDED by it -- see §5.

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
| uber @21701 | interaction deny-sound (`isDreaming\|\|isSleep\|\|!inBox\|\|dead\|\|isRagdoll`) | `isRagdoll` still gates it while down |
| uber @26584 | `wakeup()` refuses while dead | **the revive NEEDS this cleared** |
| uber @26904 | the `drown` achievement | fires only for a real drown death |
| uber @37412 | the write | -- |
| uber @39685 | the `fallen` branch | -- |
| **uber @56667** | **health regen `saveSlot.health += dt/6`, suppressed only by `dead`** | resumes -- see 6.2 |

The census is complete **inside `mainPlayer_C`** and has NOT been run outside it:
another asset may read `player.dead` through its own `EX_InstanceVariable`, and
only `mainPlayer.json` has been scanned. Treat "no external readers" as UNKNOWN.

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
**native** `UFunction`, so unlike every BP hop above it, it is interceptable by
the seam this project already uses for native targets -- the `UFunction::Func`
patch (precedent: `AudioComponent::Play`, `ActorComponent::SetActive/Activate`,
shipped v115). **No bytecode patch is required anywhere in this design.**

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
| 2 | remove the `blackScreen_C` widget | created at `@4353` with `AddToViewport(0)`. Asset: `research/pak_re/extracted/VotV/Content/umg/blackScreen.uasset`. **Its lifecycle is UNMEASURED** -- it may animate and self-remove. §6. |
| 3 | `forceWakeup()` | uber `@25800`, UNCONDITIONAL: movement mode, capsule collision, camera re-attach, `EnableInput`, ragdoll mesh detach, physics off. Not `forceGetUp()` (0.2 s latent delay, and lands in `@39685` which re-reads `dead`), not `wakeup()` (refuses while dead). |
| 4 | restore vitals | `saveSlot.health` must be > 0 or `Add Player Damage` re-kills on the next hit. The game's own regen (`+dt/6`, @56667) then runs unaided. |
| 5 | reposition | the game's own respawn verb is `teleportWObackrooms(<transform>, true, false)`. `spawnLocation` (set at level start, uber `@34642`) is what the game's own below-Z rescue uses (`@3907 forceWakeup(); @3921 teleportWObackrooms(spawnLocation, true, false)`). **Prefer that pair -- it is literally the game's existing revive.** |
| 6 | undo `loadLevel`'s menu prep | it already ran: `GameInstance.subArea := None`, `NewVar_1 := option`, `pause_mainMenu.canvas_loading.SetVisibility(b0)`, `pause_mainMenu.screenSwi.SetActiveWidgetIndex(0)`. Whether any of those is visible to a player who never reaches the menu is UNMEASURED. §6. |

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
is superseded whole.** It holds `canRagdoll` shut for the session so the death is
never authored -- the exact opposite of the instruction in §0. Per RULE 2 it does
not get to sit beside this; when the arc is built, the standing gate goes.

Do NOT delete it blindly: `coop/player/ragdoll_gate` is also held by
`wisp_attack_sync` for the Killer Wisp false-grab window, which is a real,
pre-existing need. The gate MODULE stays; `ko_respawn`'s hold on it goes.

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

**Until this arc lands, the shipped build is worse than the plain bug on one
axis** (H1 can take a single-player's ragdoll away permanently). Either retire it
or fix H1/H2/H3 first -- that is a decision for the session that picks this up.

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

The existing instrument (`python tools/mp.py korespawn`,
`harness/autotest_korespawn.cpp`) has the right shape -- falsifier-first arms,
its own `DONE` line, non-zero exit on FAIL -- and it already caught two real
defects before passing. It must be re-aimed:

* **First artifact: make it go RED on the currently deployed build.** It passes
  today while H1/H2/H3 are live. A harness that cannot fail on the known-broken
  build cannot certify the replacement (this is the round-2 critic's point and it
  is correct).
* New arms this arc needs: the full native death is allowed to run (assert the
  black screen appeared and ~10 s elapsed); `OpenLevel` was reached and
  cancelled; `dead` went true and then false; the player is standing, has
  positive health, and is at the respawn point; **and a deliberate quit-to-menu
  still travels** (the discriminator's negative control -- without it, a fix that
  cancels every travel passes).
* Still owed from the previous lane: the lethal arm on a CLIENT, and the
  cross-peer appearance of a dead player.

---

## 9. Open product questions for the user

* **Single-player.** The `OpenLevel` interception is process-wide and does not
  depend on a session, so a solo player would get the revive too. Wanted?
* **Where does the revive place the player?** The game's own `spawnLocation`
  (level-start transform) or the coop KPP start point.
* **Does anything mark the death?** Right now a revived player is
  indistinguishable from one who never died. A chat line, a stat, a cost?
