# COOP — Dispatch Visibility (will my hook fire?)

> **The canonical answer to "will our ProcessEvent hook fire for function X?"** — the question that
> caused a 3-iteration rework + two review agents giving OPPOSITE answers because the fact was scattered
> across comments + RE docs. Read this BEFORE adding any observer/interceptor/poll. Synthesized
> 2026-06-20 from a code-verified trace (4 agents, cross-checked against the live code + the IDA RE).
> Companion: [COOP_ENTITY_EXPRESSION_MAP.md](COOP_ENTITY_EXPRESSION_MAP.md).
>
> Confidence tags on every claim: **[V]** verified-from-code · **[RD]** from-comment-or-RE-doc (not the
> live code) · **[?]** uncertain / needs a runtime probe. Do NOT silently upgrade a [RD]/[?] to truth.

## The one-sentence rule

Our detour sits on **`UObject::ProcessEvent` only** (`ue_wrap/game_thread.cpp:774`, AOB-resolved). A call
is **VISIBLE** iff the engine dispatches it *through* `ProcessEvent`. A call routed through the BP
bytecode VM's `CallFunction → ProcessInternal` **bypasses our hook and is INVISIBLE** — `ProcessEvent`
itself calls `ProcessInternal` one layer below us, so anything entering at `ProcessInternal` is beneath
the hook. **[RD: votv-pile-grab-observable-hook-RE-2026-06-08-pass1.md §1.2, IDA-traced]**

**Visibility is a property of the DISPATCH PATH, not the function.** The *same* UFunction (e.g.
`Actor::K2_DestroyActor`) is VISIBLE when the engine dispatches it but INVISIBLE when BP code calls it on
itself via `EX_VirtualFunction`.

## VISIBLE vs INVISIBLE taxonomy

**VISIBLE (reaches ProcessEvent):** native-engine → UFunction entries — **input action events**
(`InpActEvt_use`), engine lifecycle for PE-dispatched actors (`ReceiveBeginPlay`/`ReceiveTick`),
RPC-style + `BlueprintNativeEvent` entry points, `GameplayStatics` calls
(`BeginDeferredActorSpawnFromClass`, `FinishSpawningActor`) **when issued by a native/engine/spawner
caller**, multicast-delegate `Broadcast()` (`OnComponentHit` `BndEvt__…`), an **engine-initiated**
`K2_DestroyActor`, and our own `reflection::CallFunction` (re-enters the detour; `t_inPump`-guarded).
**[V: the seams fire on these in shipped code]**

**INVISIBLE (routes through the BP VM, below the hook):** `EX_LocalVirtualFunction` /
`EX_VirtualFunction` / `EX_FinalFunction` / `EX_LocalFinalFunction` / `EX_CallMath`, all BP→BP internal
calls, a BP self-destroy, and native C++ internal calls. **[RD: pass-1 RE §1.2]**

> **The same `GameplayStatics` UFunction is in BOTH lists — visibility is the CALLER's opcode, not the
> callee.** `BeginDeferredActorSpawnFromClass` reaches PE from the `garbagePileSpawner`/pinecone caller
> (VISIBLE — host_spawn_watcher fires) but is `EX_CallMath` from a BP ubergraph (the chipPile grab / clump
> re-pile spawn — INVISIBLE, 0 fires). "Catches it for the spawner ⇒ catches it for the clump" is a
> category error (the 2026-06-21 false "observability reversal" — see the section at the bottom + commit
> `0e56ca39`). **[V]**

## Our hook seams (what each can/can't see)

All in `ue_wrap/game_thread.{h,cpp}`. Per-dispatch order: drain task pump → **FireInterceptors** (PRE-cancel)
→ **FireObservers(PRE)** → `g_originalPE` → **FireObservers(POST)**. **[V: game_thread.cpp:650-688]**

| Seam | Fires | Multiple/fn? | Thread | Use / blind spot |
|---|---|---|---|---|
| `RegisterPostObserver` | after original | YES (keyed on (target,cb) pair; ALL fire) | dispatching thread — **may be a parallel-anim worker** | read state the BP just wrote; bind a spawned actor. Cannot see INVISIBLE calls. |
| `RegisterPreObserver` | before original | YES | same | snapshot state about to be cleared/destroyed. |
| `RegisterInterceptor` | before original; **return true ⇒ original SKIPPED** | YES (first true wins) | same | suppress/replace a spawn/effect (client NPC/WorldActor suppression). Can only cancel a ProcessEvent dispatch, never a BP-internal call. |
| ProcessEvent detour | the anchor for all of the above | — | — | `SetTransparentBypass` makes the whole layer dark (flee-on-death). |
| **Native MinHook** (`ue_wrap/hook`) | on a raw native address | — | — | for NON-UFunction native calls: `SaveGameToSlot` (`save_block.cpp`), DXGI Present (`imgui_overlay.cpp`). A PE observer would NEVER fire on these. |
| **Polling** (not a hook) | a throttled host tick | — | game thread | the canonical fallback for INVISIBLE events — poll the observable *result* (see below). |

**Thread-context rule [V]:** an observer/interceptor cb can fire on a worker thread. If it derefs an actor
or calls an engine UFunction, it MUST self-defer (`if (!GT::IsGameThread()) { GT::Post([self]{...}); return; }`,
re-validate with `R::IsLive` inside) OR be pure-param-read. `host_spawn_watcher::OnSpawnPost` instead
gates the worker out (`if (!GT::IsGameThread()) return;`) because spawning is always game-thread.

**Pump-context rule [V: 2026-07-04, `ue_wrap/spawn_gate`]:** a `GT::Post`ed task runs at **top-level
game-thread context** — the pump drain is DEFERRED while the world refuses `SpawnActor` (the detour also
fires on dispatches nested inside another actor's construction script, where `UWorld::SpawnActor`
silently returns null in Shipping: IDA `0x142C12D20` tests `[World+0x10C]&2` = `bIsRunningConstructionScript`
— sole writer `AActor::ExecuteConstruction` `0x1428c5fe4`, scope-guarded around every BP actor's SCS+UCS —
and `[World+0x10D]&0x20` = `bIsTearingDown`). Before the gate, a save-load's mass actor construction made
nearly every dispatch a nested one → every spawn a task issued in that window fire-and-failed (the
2026-07-04 join-window burst: 871 trash-proxy + 92 keyed-prop nulls on the joining client). Consequence
for new code: a posted task may legally spawn; it may run milliseconds later than posted (episode length
= the construction/teardown window). Never call `SpawnActor` from an observer cb directly — post it.

## The function table (the specific calls this codebase cares about)

| Edge | Dispatch | Visible? | Catch it with | Conf |
|---|---|---|---|---|
| `mainPlayer.InpActEvt_use` (E-press) | native input → PE | **VISIBLE** (PRE+POST) | we hook it: `trash_collect_sync::OnPileGrabPre`, door/interactable sync | [RD]/[V] |
| `mainPlayer.InpActEvt_drop_0/1` (**R key = the "Drop" ACTION**, Input.ini — the R-pickup-to-inventory AND R-drop both enter here) | native input → PE | **VISIBLE** (POST) | (v106: unused — the v105b reconcile_trigger observers are RETIRED; pickup/drop coherence rides the K2_DestroyActor + FinishSpawningActor Func seams. The dispatch FACT stays valid for future hooks) | [V live smoke 2026-07-06 14:21 — observed while the v105b observer existed] |
| `ui_playerInventory.BndEvt__Button_a_drop(_1)...` (the inventory drop buttons) | widget delegate → PE | **VISIBLE (expected — widget-bound click events are PE-dispatched)** | (v106: unused — same retirement; the inventory-drop spawn is caught at the FinishSpawningActor Func seam inside takeObj) | [RD; the v105b observers resolved+installed 2/2 before retirement; a live FIRE was never observed] |
| `mainPlayer.updateHold` (the hotbar switch itself) | BP-internal from the input/inventory flow | **INVISIBLE as a verb — but no hook is needed: ONE SYNCHRONOUS call** (destroy @935 + spawn @1023 + holding_name @3183 same invocation, bytecode 2026-07-06) → a per-tick `holding_actor` poll sees only settled states; a polled null IS the stow (the v105 250ms debounce guarded a nonexistent flicker) | poll `holding_actor` (hand_item::TickOwner) | [RD bytecode, DECISIVE] |
| `actorChipPile.toClump` / `turnToPile` / `playerGrabbed` / `playerGrabbed_pre` | `EX_LocalVirtualFunction` | **INVISIBLE** | the **InpActEvt_use PRE** (read `lookAtActor` while the pile is alive) — NOT a hook on these | [RD: pass-1 RE §1.2/2.2, DECISIVE] |
| `actorChipPile.K2_DestroyActor(self)` (the pile's own grab-destroy) | `EX_VirtualFunction` (BP self-call) | **INVISIBLE** | death-watch / morph model (this is *why* it exists) | [RD: pass-1 RE §2.0] |
| `Actor.K2_DestroyActor` — engine-initiated destroy of a tracked actor | engine → PE | **VISIBLE** | v106: prop_lifecycle moved to the **Func patch** row below (superset); `npc_sync`/`world_actor_sync` still hook the PE PRE | [V] |
| `Actor.K2_DestroyActor` — ANY BP destroy incl. `putObjectInventory2`@719 (R-pickup) + pile/clump morph destroys | `EX_CallMath`/`EX_FinalFunction` → `UFunction::Func` | **PE-INVISIBLE, Func-VISIBLE** (Func funnels every route) | **`prop_lifecycle` Func patch = THE destroy seam** (v106 `29dfd079`; the callback CONTEXT = the dying actor). **CAVEAT (v106b `4a280375`): the widened seam ALSO fires on morph-HUSK destroys** (the pile's self-destruct inside playerGrabbed; the clump's at re-pile) — safe ONLY because identity migrates to the successor AT ITS BIRTH (migration-first: a husk dies eid-less → the seam body no-ops, no morph gate needed). The pre-v106b husk-owns-eid state broadcast DESTROY(E) + drained the element row = the 11:43 grab regression (OPUS_48_DISCIPLINE §8). **SECOND §8 instance — FIXED v107 (2026-07-08, `3180c4ab`, USER-VERIFIED): the widened seam ALSO fires on a CLIENT's join-window `mainGamemode.loadObjects` world-load PRE-DELETE churn** — the game's world-restore destroys+respawns the client's keyed props (local, net-zero via `join_membership_sweep`); the v106 Func-patch broadcasts each DESTROY to the HOST → host destroys its copies by Key → **3345→1255 on a bare join, never recovers** (clean bare-join log 2026-07-08 11:54; `[HOSTWIPE-CALLER]` probe 15:43-15:45 measured **2270/2270 caller class `mainGamemode_C`**). DEAD candidates: `InPurgeEpisode()` (timing-invalidated — flag @11:54:40 vs burst @:38), `eid=0` gate (97.9% held a real eid = drain race), a caller-CLASS gate (REFUTED — gamemode issues `K2_DestroyActor` from 9 fns incl. `putObjectInventory`=R-pickup + `RemoveEquipment`/`undo`, all cls=`mainGamemode_C`). **FIX (AS-BUILT v107) = source-anchored CLIENT-scoped WORLD-LOAD EPISODE LATCH** (`coop::world_load_episode`: arm at the client join boot `DriveMenuModeJoinWorldBoot` before `BootStorySaveBlocking` = CAUSAL; clear at load-tail quiescence `join_membership_sweep.cpp:633`; suppress OUTBOUND KEYED destroy broadcasts in `DestroySeamBody` while in-episode — eid-only pile destroys UNTOUCHED). Rejected (all /qf-refuted, rounds 0-13): FFrame::Node gate (§9 site-list), authority-default-deny (pile-touch), recreate-pairing (no successor at destroy time), mark-victims (victims born AFTER arm, inside un-hookable loadObjects). Punch-list (non-blocking): host-symmetric arm + within-session world-change = unmeasured extensions. NOT the pile sweep (separate, fixed), NOT piles (keyed props). See `research/findings/props-lifecycle/votv-destroy-seam-hostwipe-and-rock-rdrop-RE-2026-07-08.md` (FIX AS-BUILT section). | [V live 2026-07-07: 11:43 host log shows the Func seam firing on every grab] · [V clean bare-join log 2026-07-08 11:54 (host 3345→1255); V probe 15:43-15:45 (caller = loadObjects/mainGamemode 2270/2270); **FIXED v107 `3180c4ab` DLL `04ebfdb0`, USER-VERIFIED 2026-07-08**] |
| `FinishSpawningActor` — EX_CallMath spawns (`propInventory.takeObj`@729: R-drop/place/UI/stack-drop) | `EX_CallMath` → `UFunction::Func` | **PE-INVISIBLE, Func-VISIBLE** | **`host_spawn_watcher` Func patch + 1-tick drain** (key is a NewGuid at Finish-POST inside takeObj — loadData restores it after; hence the drain) | [V hands-on 2026-07-07 0ae: Q-menu spawns appear INSTANTLY on clients — was delayed pre-v106] |
| **A SCRIPT (BP bytecode) UFunction called via `EX_Local*Function`** (e.g. `takeObj` itself) | `ProcessLocalScriptFunction` inline | **INVISIBLE to BOTH ProcessEvent AND a Func patch** (EX_Local* never goes through `UFunction::Func`). **THIS IS THE ONLY REMAINING INVISIBLE CLASS** — and it is SOLVABLE: `docs/COOP_VM_DISPATCH_PLAN.md` (2026-07-13, /qf-converged) = the GNatives table swap gives the hook engine a third primitive (VM verb brackets, entry+exit+context). Until it lands: patch the NATIVE calls INSIDE (the v106 shape) or mirror state. NOTE the sharpened boundary: EX_CallMath was NEVER part of this wall (its targets are native = Func-patchable) — check the CALLEE's nativeness before declaring a wall | [RD — the reason takeObj-POST missed R-drops while the UI route fired; boundary sharpened 2026-07-13] |
| **A BP-deferred-spawned actor's `init()`** (chipPile/clump/sandbox Aprop) | `EX_LocalVirtualFunction` from UCS | **INVISIBLE — its Init-POST observer does NOT fire** | the `FinishSpawningActor` POST (keyed) or `BeginDeferred` POST (keyless) seams below | [V: host_spawn_watcher.cpp:130-135,197-208] |
| `BeginDeferredActorSpawnFromClass` POST — **from a native/engine/spawner caller** (`garbagePileSpawner`, ambient pinecone/stick, MOST NPC/WorldActor spawners) | the caller's dispatch reaches PE | **VISIBLE** | actor NOT yet positioned (read the `SpawnTransform` PARAM, not GetActorLocation). `host_spawn_watcher`/`npc_sync`/`world_actor_sync` all POST-observe it. **CAUTION 2026-07-04: "spawner ⇒ visible" is NOT a law — `piramidSpawner_C.runTrigger` issues ALL 5 of its BeginDeferreds `EX_CallMath` (row below); verify per spawner with a live catch line** | [V; piramid counterexample V live 22:49] |
| `BeginDeferredActorSpawnFromClass` / `FinishSpawningActor` — **from a BP ubergraph caller** (the chipPile grab clump-spawn; the clump re-pile pile-spawn; **trigger_wispSwarm's 32-wisp loop**) | the CALLER issues it `EX_CallMath` | **INVISIBLE to PE; CAUGHT by a `UFunction::Func` thunk patch** | the thunk patch (`ue_wrap/ufunction_hook`, AS-BUILT commit `d19ae4d4`) — NOT a ProcessEvent observer (see "Catching an EX_CallMath call" below). It owns BOTH pile morph directions: the re-pile (clump→pile) since `d19ae4d4`, and the grab (pile→clump) since v106 `29dfd079` (birth certificate + v106b migration-first rebind at birth — the InpActEvt-PRE hand-off is RETIRED; use-HOLD grabs bypass press seams). 2026-07-03: a SECOND chained thunk cb (`npc_world_enum::OnBeginDeferredExSpawn`, source-gated by `FFrame::Object`'s class) enrolls the wisp swarm — smoke 32/32 catches. 2026-07-04: **`piramidSpawner_C` is the SECOND proven EX source** (runTrigger's 4x killerwisp_C + piramid2_C — zero PE-interceptor catches on a live force run); same thunk, source list += piramidSpawner_C, drain gained a WorldActor branch (`world_actor_sync::HostEnrollExSpawn`) | [V: 0 host_spawn_watcher fires across 870 piles + every re-pile, hands-on 2026-06-21, commit 0e56ca39; thunk DETECTION verified read-only `B7EEB1BF`; wisp catch [V smoke 2026-07-03]; piramid catch [V e2e 2026-07-04 23:19]] |
| **Event-actor SELF `K2_DestroyActor` at event end** (piramid `checkIfReached` END; likely every WA's self-despawn) | `EX_*` self-call | **INVISIBLE — the K2_DestroyActor PRE observer never fires** | pose-walk **DEAD-RETIRE** in the axis owner: `world_actor_sync::TickPoseStream` retires a BOUND-but-dead actor + broadcasts WorldActorDestroy (the npc-lane dead-retire shape) | [V: piramid END dead-retired + client mirror K2'd + registry END, e2e 2026-07-04 23:19] |
| `FinishSpawningActor` POST | GameplayStatics → PE | **VISIBLE** | the keyed sandbox-spawn seam (Key minted by now) → `ExpressSpawnedProp` | [V: host_spawn_watcher.cpp:209] |
| `ReceiveBeginPlay` — **save-loaded** actor (pre-exists at connect) | no runtime PE our session sees | **caught by the GUObjectArray SEED WALK**, NOT an Init observer | `SeedKnownKeyedProps` / `RegisterExistingWorldNpcs` (the 872 save-loaded piles, the pre-existing NPCs) | [V] |
| `ReceiveBeginPlay` — a normal runtime-spawned actor | maybe PE | **[?] not separately verified** | probe before relying on it | [?] |
| `SpawnEmitterAtLocation` + cosmetic `EX_CallMath` spawns | `EX_CallMath` | **INVISIBLE** | **POLL** the result (`event_cue_sync` diffs the PSC set), or a `UFunction::Func` thunk patch if a deterministic `(source,product)` is needed (the chipPile/clump re-pile plan) | [V: event_cue_sync.cpp] |
| `UGameplayStatics::SaveGameToSlot` | native C++ | **INVISIBLE to PE** | **native MinHook** (`save_block.cpp`) | [V] |
| `UGameplayStatics::SetGamePaused` — the ESC pause (mainGamemode ubergraph stmts [372]/[375]/[3035]) | `EX_CallMath`; AND the console `pause` cmd bypasses GameplayStatics entirely (PC::SetPause, native) | **INVISIBLE** (and multi-path) | **enforce the STATE, not the call sites**: `coop/session/pause_guard` polls `IsGamePaused` per gameplay tick while connected → `SetGamePaused(false)` (the game's own verb). Poll liveness while paused is guaranteed: `ui_menu_C::Tick` (the pause menu itself) dispatches PE every Slate frame — the exact tickFn `multiplayer_menu` latches | [RD bytecode 2026-07-04; built same day `769d02f7`; e2e autotest queued] |
| kerfur conversion verbs (`dropKerfurProp`/`spawnKerfuro`) | **the VERB is `EX_LocalVirtualFunction` ubergraph self-call** (bytecode-measured 2026-07-13). Its INNER natives are Func-VISIBLE: EX_CallMath spawns AND **`K2_DestroyActor(self)` fires the Func-patch** (measured 2026-07-14 provenance `vmActive=1 ctxSelf=1`) — "EX_Local-invisible K2" was FALSE, only the verb is invisible | **NOW CAUGHT (observe-only) via the GNatives[0x45] swap** — VM-dispatch substrate SHIPPED `722fbe18` + G1 increment `21cbc3e7` (2nd capture scope `ActiveRequestVerbEid` for the CallFunction route); verb entry + in-bracket FinishSpawningActor/K2_DestroyActor all captured. **FINAL measured fix (2026-07-14 pm, drain DEAD):** bug1's relay is the generic `grab_hook[destroy-seam]` CLIENT broadcast (key, eid=0); `TryCaptureKerfurPropDestroy` (prop_destroy_seam:118) is the guard on it, REACHED but DECLINES ("1 fresh stamp but NONE qualified" — the dying prop's post-destroy anchor reads ~(0,0,0) so 500cm proximity finds no B). FIX = feed the assembler's in-window captured-B to that guard (bypasses the anchor) + `ConvergeAfterConversion(capturedForm)` (foundation `e785cb04`). G1 GREEN: host `formInReqScope=2`. **WIRED `44f8c69b` (captured-B converge) → 19:09 take: bug1/bug2 converge FIRED but host-own turn_off DUPED** (double-EXPRESS: keyed real-key PropSpawn via Init POST + synthetic KerfurConvert; client census 6→8, 2 UNCLAIMED orphan props). **SOLE-EXPRESS FIX `de3dccb5`:** the Init POST suppressor now gates on `kerfur_form_assembler::IsCapturedForm` (deterministic non-consuming peek) NOT `TryAdoptFreshKerfurProp`'s racy dead-NPC proxy (which raced the verb's spawn-vs-destroy order) — a captured conversion successor is TRACKED but its generic PropSpawn SUPPRESSED (KerfurConvert = sole signal; track-but-don't-broadcast preserves the 3-broadcaster tracked-flag coordination). Pre-existing 6-month race 44f8c69b only made VISIBLE (silent orphan-NPC → visible orphan-prop). **VERIFIED GREEN 20:20 take** (14/14 host conversions suppressed, client census 6=6 across 13 samples, no visual dupes). The stamp-walk proximity FALLBACK inside `TryCaptureKerfurPropDestroy` is **RETIRED `3d9da0cd`** (0 fallback fires across 85 captured-B HITs; captured-B is the sole which-B path; the loud in-bracket ORDER ASSERT survives). `[[project-kerfur-sole-express-dupe-2026-07-14]]` | interim (still live): POLL as client request driver + `TryAdoptFreshKerfurProp` CONVERGE (its SUPPRESSION role REPLACED by the capture-peek `de3dccb5`). bug1/bug2 converge measured 19:09; turn_off dupe fix GREEN 20:20 | [V opcode+containment+provenance+hands-on; runtime 2026-07-13/14] |
| `trigger_eventer.runEvent/runSpecialEvent` — the game's OWN scheduled fire (`saveSlot::settime` -> `gamemode.eventer.runEvent`) | BP->BP cross-object (`EX_Context` + `EX_VirtualFunction`) | **INVISIBLE** (PE and Func both) | AS-BUILT v95 `coop/world/event_fire_sync`: host POLLS `saveSlot.passEvents` growth (settime Array_Adds each fired row — the ONLY appender; runEvent never touches the array); dev-menu fires broadcast at dispatch through `HostFire`; client scheduler killed structurally (`allEvents.Num=0` — settime's walk SOURCE, boot-rebuilt every load, so it self-heals + can't poison a save) | [RD 2026-07-03; built same day] |
| every screen/panel device verb (`use`/`player_use`/`setActiveInterface`/`actionOptionIndex`/`saveSignal`/`makeAnOrder`...) | `EX_LocalVirtualFunction` (measured $type table) | **INVISIBLE** | **POLL the state field** (the whole shipped L2 device layer: `interactable_channel.h` Channel) — hooks only for the door request edge + the occupancy deny gate | [RD/IDA 2026-06-04; re-confirmed 2026-07-03 screens research] |
| **`ui_consolesAtlas.OnKeyDown/OnKeyUp`** — the desk keyboard router (SHIFT/1/2/3/ENTER/arrows resolve by KEYBIND NAME here; the panel actor's own `playerHandAnyKey`/`intComs_anyKey` are DEAD stubs) | UMG widget input → ProcessEvent | **VISIBLE — but ONLY on the occupant's machine** (widget input never exists on a mirror peer); gates inside: `controllingCoordinatePanel`, `coord_isPing` swallows all keys, `active_coords` | occupant-side verb capture is possible here; cross-peer still needs state-mirror (all downstream mutations are EX_Local*/inline) | [RD bytecode 2026-07-16, signal-chain RE §0] |
| `trigger_alarm_C.runTrigger` — the base alarm ON/OFF verb (ALL callers: analogD radar scan, panel_radar stop-press, snuskLoaf prank — census exhaustive via binary grep of the `alarmTrigger` FName over every cooked asset) | `EX_VirtualFunction` after `getObjectFromKey` | **INVISIBLE** | **POLL the `active` bool** (v101 `coop/world/alarm_sync`, 1 Hz both peers; apply = WE dispatch runTrigger via ProcessEvent ourselves — outbound reflected calls are always "visible" because we ARE the dispatcher) | [RD 2026-07-05; lane e2e PASS same day incl. mid-alarm join] |
| **The timer/delay/tick-interval DRIVER natives** — `K2_SetTimerDelegate` / `K2_SetTimer` / `Delay` / `RetriggerableDelay` / `SetActorTickInterval` from ANY BP ubergraph (ticker re-arms, mainGamemode event timers) | `EX_CallMath` | **INVISIBLE to the PE interceptor table** — measured LIVE 2026-07-10: a 150 s LAN window produced ZERO interceptor records against 973 PE-visible BeginDeferreds; the world-load BeginPlay arms (beehive K2_SetTimer, ufoDropper K2_SetTimerDelegate) were the positive control and were silent | **`ufunction_hook` Func-patch POST observer** (`coop/dev/rng_roll_census`, `7109efd1`) — and the post callback's `sourceObject = FFrame::Object` is the CALLING BP ACTOR, i.e. free caller attribution with zero FFrame stepping (better than reading the param owner). Same correction applies to `QuitGame` (EX_CallMath from calculateAreaError) — a PE-table hook on it would never fire | [V live 2026-07-10 12:36 (blind) → 12:48 (Func seam: mainGamemode 157 arms, dishUncalib 11 re-arms measured)] |

## HOW TO PICK A SEAM (decision guide)

1. **Native-engine → UFunction dispatch** (input action, engine lifecycle, RPC-style, BlueprintNativeEvent,
   GameplayStatics)? → VISIBLE. Use `RegisterPreObserver` (read state about to be cleared),
   `RegisterPostObserver` (read state just written), or `RegisterInterceptor` (cancel/replace).
2. **BP→BP call (`EX_*`), a BP self-destroy, or a BP-internal `init()`?** → **INVISIBLE.** Do NOT hook it
   — the observer registers fine and NEVER fires (the exact 3-iteration trap). Instead:
   - **deferred-spawned actor** → `BeginDeferred` POST (keyless; actor at origin → read the param) or
     `FinishSpawningActor` POST (keyed). Template: `host_spawn_watcher`.
   - **actor already in the world at connect (save-loaded)** → the GUObjectArray **seed walk** + connect
     snapshot. Init-POST will not have fired for it.
   - **cosmetic / `EX_CallMath` effect or an unobservable BP self-destroy** → **POLL** the result on a
     throttled host tick (`event_cue_sync`; the kerfur conversion poll; the ambient death-watch).
     Proximity/identity-gate a death-watch so a stream-out/bump is not misread as the event.
   - **an `EX_CallMath` call whose `WorldContextObject`/`ReturnValue` you need DETERMINISTICALLY** (a
     `(source, product)` pair, e.g. the chipPile/clump re-pile) → a **`UFunction::Func` thunk patch** on the
     callee — a ProcessEvent observer can NEVER fire on it (see "Catching an EX_CallMath call" below). Do NOT
     register a BeginDeferred POST for a BP-ubergraph spawn; it won't fire. **The facility now exists:**
     `ue_wrap/ufunction_hook` (AS-BUILT, commit `d19ae4d4`) — reuse it.
   - **⚠ a `UFunction::Func` patch only catches dispatches that INVOKE the thunk** — i.e. ProcessEvent
     dispatches (native code → BP event, e.g. the AnimBP `BlueprintUpdateAnimation` head-fix) and calls
     whose TARGET is native (`EX_CallMath` / `EX_FinalFunction` → native thunk). **A BP function called
     from BP LOCALLY (`EX_LocalFinalFunction` / `EX_LocalVirtualFunction` with a SCRIPT target) dispatches
     through `ProcessLocalScriptFunction` and touches NEITHER ProcessEvent NOR `UFunction::Func` — a Func
     patch on it NEVER fires.** [V 2026-07-02: gm `loadObjects` (ubergraph `EX_LocalFinalFunction` call) —
     Func POST installed on the live class, loadObjects demonstrably ran (its spawns landed), the POST never
     fired; two DLL takes confirmed]. For such a seam: hook a VISIBLE caller upstream, a NATIVE callee
     downstream (with a source filter), or **OBSERVE the function's EFFECT** (the pose gate uses load-tail
     quiescence to observe loadObjects' end). Same blindness applies to `save_indicator_suppress`'s
     saveAnim/addHint POST hooks (probe-only; latent).
   - **an INPUT that triggers a BP-internal action** (the pile grab) → hook the **input UFunction**
     (`InpActEvt_use` PRE) and read what the BP is about to act on — the input is VISIBLE even though
     everything it triggers is not.
3. **Native C++ function (not a UFunction)** → a PE observer can never fire. Use a **native MinHook**.
4. **Always handle the thread context** (worker-thread fires): self-defer with `GT::Post` (+ re-validate),
   or restrict to pure param reads. Never call an engine UFunction from a possibly-off-thread cb.

## OBSERVING vs DRIVING an `InpActEvt_*` (a reflection-call trap) — VERIFIED 2026-06-20

`reflection::CallFunction(player, InpActEvt_use, frame)` **fires our PRE/POST observers** (it re-enters
ProcessEvent) **but does NOT run the input-gated BP gameplay body.** Proven (smoke 2026-06-20, the chippile
grab test): the call logged our `OnPileGrabPre` (`pile_morph: grab armed`) yet spawned NO clump — the grab
never happened. Root cause (RCA workflow): the `_NN` input-event stub is driven by the **engine input
system** threading the ubergraph to the right node on a live IE_Pressed edge; a synthetic reflection call
runs the stub but not that gameplay path (and `_41` is the *release* edge — the grab is on `_42`). This is
GENERAL — the flashlight (`autotest.cpp:667`), `spawn_menu.cpp:81`, and the savebtn test all hit it.

- **To OBSERVE the input** (read what it's about to act on): hook `InpActEvt_use` PRE — VISIBLE, works.
- **To DRIVE the real gameplay from code** (a test/harness): do NOT call the `InpActEvt_*` stub. Call the
  **gameplay UFunction the input would invoke**, directly via `CallFunction`/`ue_wrap::Call` — that routes
  ProcessEvent → ProcessInternal → the real BP body. For the pile grab: `Call(pile, playerGrabbed{Player,
  HitResult})` runs the genuine conversion (spawn clump → `pickupObjectDirect` → destroy pile). The grabbed
  clump lands in **`grabbing_actor`** (the PHC light-grab path), not `holding_actor`. Template + proof:
  `harness/autotest_chippile.cpp`.

## Overriding an AnimBP variable that BUA recomputes — the POST-BUA `Func` patch (2026-07-02)

**A game-thread tick write to an AnimInstance variable that `BlueprintUpdateAnimation` recomputes
ALWAYS loses.** Per-anim-update order (UE4.27, game thread): **BUA (event graph, recomputes the var)
→ the PropertyAccess FastPath copy batch (samples class vars into node pins) → the anim-graph node
update (state machines evaluate transitions from the just-copied pins)**. Any write landing outside
that window is overwritten by the next BUA before the copies read it — which is why FOUR attempts at
the puppet head-freeze failed before the seam was named. **The seam: `ufunction_hook::InstallPostHook`
on the AnimBP's OWN `BlueprintUpdateAnimation` override** (it dispatches through `UFunction::Func`
whether called via PE or not) — the callback runs after the recompute, before the copies. Instance
filtering happens IN the callback (e.g. puppet-only by identity: outer chain → `mainPlayer_C` with
null `Controller`). Guard: verify `OuterOf(fn) == the BP class` — resolving a SUPER's declaration
would over-hook every AnimInstance. **[V — hands-on 2026-07-02 take-2, BOTH peers (`5b2cb5ff` +
capacity fix `b77793d7`): head tracks back-turned on host and client screens; kerfur NPCs stay
native by the identity filter.]**

**Install-capacity caveat (2026-07-02, `b77793d7`): the Func-patch table refusal is PER-PEER.**
Peer roles are asymmetric — the HOST installs host-authoritative Func hooks a client never does —
so a full table refuses an install on one peer while the other succeeds, and the feature
half-works (exactly how take-1 shipped: 4 slots, head-gate was the host's 5th). The table is now
GENERATED from `kMaxNativeHooks` (16; grow by editing the one constant), and any install-success
marker must be verified in EVERY peer's log, not one. **[V — pinned from both peers' logs.]**

**Corollary (the head-freeze lesson): a node's Alpha field ≠ its effective weight.** Anim nodes
hosted INSIDE a state-machine state contribute NOTHING when the machine leaves that state — the
node's own fields read unchanged (Alpha 1.0) while its output is ignored. Before trusting "the node
is on, alpha=1", check WHERE the node lives (BakedStateMachines + CDO pose-link trace in the
bp_reflection JSON). The kerfur AnimBP hosts BOTH head/neck `FAnimNode_LookAt` nodes inside state
`lookAtPlayer`; `lookingAtPlayer` gates that state's transitions = the de-facto head on/off. **[V-static]**

## Catching an `EX_CallMath` (BP→native-thunk) call — a `UFunction::Func` patch, NOT a ProcessEvent observer

**The 2026-06-21 correction.** A ProcessEvent observer can NEVER fire on an `EX_CallMath` call (e.g. a BP
ubergraph's `BeginDeferredActorSpawnFromClass` / `FinishSpawningActor` / a math native): the BP-VM dispatches
it via `UObject::CallFunction → UFunction::Invoke → (Context->*UFunction::Func)(...)` — the native thunk at
`UFunction+0xD8` — **one layer BELOW `UObject::ProcessEvent`**, our sole observer seam. ProcessEvent is the
OUTER door (engine/native/delegate → BP); it is not re-entered for an inner BP call. **[V]**

- The s35/08 redesign assumed the chipPile-grab clump-spawn + the clump re-pile pile-spawn were catchable at
  a `host_spawn_watcher` BeginDeferred POST. **They are not** — those callers issue `BeginDeferred` as
  `EX_CallMath`. Live proof: **0 host_spawn_watcher fires** across 870 piles + every re-pile, hands-on
  2026-06-21 (commit `0e56ca39`). `host_spawn_watcher` catches BeginDeferred ONLY from the
  native/spawner caller (pinecone/`garbagePileSpawner`), whose dispatch reaches PE. **[V]**
- **To catch an `EX_CallMath` call deterministically (with its `WorldContextObject`=source + `ReturnValue`):
  patch `UFunction::Func` of the callee** to a transparent native thunk (read `FFrame::Object` @0x18 = the
  source = `EX_Self`; read `*Result` for the spawned actor; forward to the original). This is below the BP
  VM, so it sees the inner call. **The facility is AS-BUILT:** `ue_wrap/ufunction_hook`
  (`InstallPostHook(ufn, cb)`, stamped per-slot transparent thunks, SEH + re-entrancy guarded; offsets
  `UFunction::Func`@0xD8 / `FFrame::Object`@0x18 pinned in `sdk_profile.h`). The chipPile re-pile uses it:
  `trash_collect_sync::OnBeginDeferredSpawnObserve` → `OnHostConvert(kToPile)` the same tick the pile spawns.
  Detection VERIFIED hands-on (read-only pass `B7EEB1BF`: thunk `*Result` == the old death-watch's pile,
  ptr-for-ptr). RE: `research/findings/piles-trash/votv-chippile-dispatch-and-thunk-hook-RE-2026-06-21.md` §3/§6. **[V —
  AS-BUILT, commit `d19ae4d4`; the convert itself is deployed-pending-hands-on.]**
- **The former interim (the proximity death-watch convert) is RETIRED** (RULE 2, same commit): the thunk
  converts the EXACT spawned pile the same tick → the ~5s reaper-vs-rebind glitch is gone by construction.
  The GRAB direction still uses the VISIBLE InpActEvt-PRE + held-edge adopt (a future tightening moves it to
  the same thunk). **[V/AS-BUILT]**
- **The thunk catches the HOST's authoring. The CLIENT MIRROR needs no dispatch observation at all** — as of
  the phase-1 trash proxy (`coop/trash_proxy`, HEAD `a5282f57`, deployed `015F0AC9590B6B23`, proto v83), the
  client's mirror of a chipPile/clump is an `AStaticMeshActor` WE spawn/destroy/re-skin via our OWN
  `SpawnActor`/`DestroyActor`/`SetStaticMesh` (driven by the reliable PropSpawn/PropConvert/PropDestroy wire) —
  **visible by construction; there is no BP self-morph/self-destroy dispatch to observe for the mirror
  anymore.** The EX_CallMath invisibility problem here is purely a HOST-side authoring concern (the thunk); the
  client just renders host-authoritative state. **Status:** the dup-gone + the resting/landed-pile mirror are
  hands-on VERIFIED **[V hands-on]** (a runtime `AStaticMeshActor` is STATIC-mobility by default →
  `SetStaticMesh`/`SetActorLocation` no-op, so it MUST be set Movable — `SetComponentMobility`, `245148c6` — or
  the proxy is invisible). The **live clump CARRY-FREEZE is [V hands-on] FIXED** via the **`!carrying`
  release-edge gate** in `local_streams.cpp` (`else if (g_lastHeldProp &&
  !coop::trash_channel::IsCarrying(g_lastHeldEid))`) — the client clump UPDATES through the carry now (no freeze
  between E-events). The earlier "carry MIRRORS on a settled join / it was the JOIN RACE" claim was **WITHDRAWN
  as FALSE**; the actual release-edge cause was `updateHold` PUPPET RECREATION (the `heldActor` ptr changes with
  `pendingSettle=0`), NOT a contact-re-pile churn (which is why the `carrying && HasPendingSettle` gate, commit
  `16ac153f`, FAILED). **Carry JANK — FIXED [V hands-on]** (shipped `df158728`): the `key.len=4`→keyless theory
  was DISPROVEN by bytecode (BP `GetKey` returns the FName `"None"` for BOTH `prop_garbageClump_C` and
  `actorChipPile_C`, so `key.len=4` is the literal string "None"; the receiver already guards
  `keyW != "None"`→eid at `remote_prop.cpp:403`, so forcing keyless was a no-op). REAL root (code-proven): an
  interpolation PHASE-STALL — `BeginLerpToPose` set `lerpStartMs=nowMs`, `AdvanceLerp` sampled the same `nowMs`
  → alpha=0 every new-pose tick at vsync-60. FIX = fixed-delay snapshot interpolation (`remote_prop.cpp`
  ActiveDrive buffers 2 timestamped poses, renders `nowMs-span` behind; MTA `CClientVehicle` shape) — user
  hands-on: carry now SMOOTH. **THROW ARC — VERIFIED [V hands-on take-29 + harness]:** the thrown clump's arc
  flies (user: "дуга ЛЕТИТ"); the autotest now does a real DIRECTIONAL throw; harness arc-flight-stream PASS.
  Streamed THROUGH the release, not via a release verb (see the durable dispatch fact below). **Pile-landing
  ROTATION — FIXED [V hands-on take-30]:** rotation is re-read from the SETTLED pile at the land-settle COMMIT
  (`trash_channel.cpp:248`; the thunk passes the clump as a fallback, `trash_collect_sync.cpp:91`). **Throw
  SOUND (the wrong pickup-cue) — FIXED [V hands-on take-30]:** the flight branch streams the carry key with no
  re-StartDrive + a trash eid sends NO PropRelease; `OnHostRelease` RETIRED (RULE 2). **Z/HEIGHT — FIXED [V
  harness]** (regression arc): take-31 read the pile transform from `newActor` at the BeginDeferred POST =
  `(0,0,0)` (pile unpositioned pre-FinishSpawning) → derived piles snapped to world origin; take-32 FIX re-reads
  the pile's REAL transform at the land-settle COMMIT (`trash_channel.cpp:248-256`), harness drift=0cm. The
  native-destroy was INNOCENT (harness-confirmed); the bug was the `(0,0,0)` loc. **Proxy SCALE — AS-BUILT**
  (re-read `GetActorScale3D` at the same COMMIT; not separately eyeballed, covered by the commit re-read).
  **LEVEL-PILE DUP — DESTROY BUILT + VERIFIED [V harness]:** DERIVED piles dup GONE [V hands-on]; for ORIGINAL
  (level-placed) piles the client's NATIVE level-loaded chipPile coexisted with the host's proxy (the divergence
  sweep is BLIND to natives) — now `remote_prop_spawn.cpp:387-410` DESTROYS the co-located native at a pile
  proxy-spawn (exact ~1cm + chipType + IsLiveByIndex, exact-or-skip on >1, graceful on 0, gated on
  `g_claimTrackingActive` = the join bracket only); harness dup-destroy-clean 12 twins / 0 SKIP. (The read-only
  PILE-PROBE that confirmed coexistence is REPLACED by this destroy — RULE 2, retired.) **FPS — the ~4s stutter
  FIXED [V harness]:** `net_pump.cpp:559` guards the steady-world re-seed on the GUObjectArray high-water mark
  (NumObjects) + a ~20s safety census, so the ~237k-object walk is SKIPPED at rest; harness re-seed rate
  0.073/s (was ~0.25/s every 4s). The **`simulateDrop` throw-velocity flip is DEAD — REPLACED by carry/flight
  stream-continuity** (shipped `136ed779`; see the durable dispatch fact below). Host carries fine; other props
  mirror fine. Option 1 FAILED; option 2 (the `holdPlayer` convert/ctx gate) is **DISPROVEN by bytecode** —
  `holdPlayer` is set ONCE on grab and NEVER cleared (DEAD, not pending). Root + fix:
  `research/findings/piles-trash/votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md`; the autonomous verification
  harness: `[[reference-pile-test-harness]]` (`tools/pile-test-assert.ps1`, 13 log-truth invariants, VERDICT
  PASS). **STILL OPEN:** the WHOOSH throw sound (no ReliableKind exists in `protocol.h`; user-deprioritized,
  best confirmed by hearing). The dead `dropGrabObject` read-only thunk was RETIRED 2026-07-10 `fb490e36` (RULE 2). **[V hands-on:
  dup-fix(derived) + visibility + carry-freeze + carry-JANK + throw-arc + rotation + sound; V harness: Z-fix +
  level-pile dup-destroy + FPS-fix; SCALE AS-BUILT; option 2 DISPROVEN.]**

  > **Durable dispatch fact — BOTH the `simulateDrop` AND `dropGrabObject` Func-thunks fire ZERO times for the
  > chipPile clump release.** Empirically (7 grab/release cycles, deployed `c2a5f49cc98add31`): `SIM-DROP=0`
  > AND `DROP-GRAB=0`, while the same `UFunction::Func`-thunk facility (the BeginDeferred slot-0 thunk) fired
  > all run. The clump's release path uses NEITHER verb — `simulateDrop` is the equipment/`holding_actor`
  > drop, but the clump rides `grabbing_actor` (the physics-handle PHC grab); RE pinned `dropGrabObject` as the
  > PHC release verb but it STILL never fired for the clump. So verb-detection for the throw is ABANDONED.
  > **The throw arc was solved by streaming THROUGH the release, not by hooking a release verb:** the host's
  > thrown clump really flies (physics) until it re-piles, so `local_streams.cpp`'s release-edge
  > `!carrying`-SKIP branch CONTINUES streaming `g_lastHeldProp`'s pose under the same eid E while it is a LIVE
  > garbageClump (`IsLive` is the churn/flight discriminator — a churn re-pile kills the clump → skip; a real
  > release leaves it flying → stream); the client's fixed-delay interp renders the arc; it ends when the clump
  > re-piles (ToPile re-skins+snaps). Shipped `136ed779`; the arc FLIES — VERIFIED [V hands-on take-29 + harness
  > arc-flight-stream PASS] (user: "дуга ЛЕТИТ"). The dead `dropGrabObject` read-only thunk was RETIRED
  > 2026-07-10 `fb490e36` (observer + install block + latch removed; retirement notes left in
  > trash_collect_sync.cpp at both sites). **[V the zero-fire fact; V hands-on + harness the
  > flight-stream arc; dropGrabObject-retire DONE `fb490e36`]**

> **⚠ A render-blind smoke caveat (the 2026-06-21 trap).** Our autonomous smoke can verify log markers and
> that a UFunction `Call()` returned, but it CANNOT verify that the call's *effect* actually landed on the
> GPU. `SetStaticMesh`/`SetActorLocation` on a STATIC-mobility component **silently no-op yet `Call()` still
> returns true** — so the trash proxies were INVISIBLE for a whole smoke that "passed" (log markers fired,
> screenshots are black). A smoke proves dispatch + no-crash; it does NOT prove rendering or that a state
> push took visual effect. Confirm anything visual (mesh swap, position follow, mobility) on a real hands-on,
> never from a smoke alone. **[V]**
>
> **⚠⚠ A SECOND trap on the same chipPile carry (the 2026-06-22 false "carry proven").** A render-blind smoke
> that ALSO drives the entity through a non-representative code path can produce a doubly-false PASS.
> `autotest_chippile.cpp` grabs the clump by calling **`playerGrabbed`** directly → the clump lands in
> **`grabbing_actor`** (the PHC slot), whereas a real **E-press** carries it in **`holding_actor`** (the
> chipPile morph slot). That slot decides whether the native re-pile gate aborts (`@2927` checks
> `holdPlayer.grabbing_actor`): with the autotest's `grabbing_actor` valid, the gate ABORTS → the clump never
> re-piles while held → a clean carry; a real E-press leaves `grabbing_actor` null → the gate never fires →
> the held clump re-piles on cluster contact ~1/s → CHURN → the client chokes (0.5–2 fps). So the smoke proved
> a grab path the user never takes. **Lesson:** an interaction smoke must drive the entity through the SAME
> seam the player uses (here: `holding_actor`, not `playerGrabbed`→`grabbing_actor`) — a different entry slot
> can change the engine's branch and hide the real bug. Root + fix:
> `research/findings/piles-trash/votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md`. **[V]**

## NEEDS-PROBE

- **[?]** `ReceiveBeginPlay` for a normal runtime-spawned (non-deferred) actor reaching ProcessEvent — not
  verified; probe before hooking it.
- **[V — RESOLVED 2026-06-21]** the `FFrame::Object`/`Func` offsets for the thunk patch are IDA-pinned
  (`FFrame::Object`@0x18, `FFrame::Locals`@0x28, `UFunction::Func`@0xD8) AND validated by the read-only log
  pass (`B7EEB1BF`); shipped in `ue_wrap/ufunction_hook` (commit `d19ae4d4`). The thunk reads the spawned
  actor from `*Result`, not `Locals+returnOff` (empirically confirmed in the disasm). See the chippile RE §6.
- **[RD→needs symptom probe]** The "`init()` is `EX_LocalVirtualFunction`" root cause is IDA-traced in the
  pass-1 RE; the *symptom* (Init-POST never fires for a Q-menu/morph spawn) is what a runtime probe
  confirms (force-spawn → check for an Init-POST log line vs a Finish-POST line).
