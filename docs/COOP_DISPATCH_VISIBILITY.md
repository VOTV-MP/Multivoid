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
| `actorChipPile.toClump` / `turnToPile` / `playerGrabbed` / `playerGrabbed_pre` | `EX_LocalVirtualFunction` | **INVISIBLE** | the **InpActEvt_use PRE** (read `lookAtActor` while the pile is alive) — NOT a hook on these | [RD: pass-1 RE §1.2/2.2, DECISIVE] |
| `actorChipPile.K2_DestroyActor(self)` (the pile's own grab-destroy) | `EX_VirtualFunction` (BP self-call) | **INVISIBLE** | death-watch / morph model (this is *why* it exists) | [RD: pass-1 RE §2.0] |
| `Actor.K2_DestroyActor` — engine-initiated destroy of a tracked actor | engine → PE | **VISIBLE** | we hook the **PRE** (`prop_lifecycle.cpp:293`, `npc_sync.cpp:298`) | [V] |
| **A BP-deferred-spawned actor's `init()`** (chipPile/clump/sandbox Aprop) | `EX_LocalVirtualFunction` from UCS | **INVISIBLE — its Init-POST observer does NOT fire** | the `FinishSpawningActor` POST (keyed) or `BeginDeferred` POST (keyless) seams below | [V: host_spawn_watcher.cpp:130-135,197-208] |
| `BeginDeferredActorSpawnFromClass` POST — **from a native/engine/spawner caller** (`garbagePileSpawner`, ambient pinecone/stick, NPC/WorldActor spawners) | the caller's dispatch reaches PE | **VISIBLE** | actor NOT yet positioned (read the `SpawnTransform` PARAM, not GetActorLocation). `host_spawn_watcher`/`npc_sync`/`world_actor_sync` all POST-observe it | [V] |
| `BeginDeferredActorSpawnFromClass` / `FinishSpawningActor` — **from a BP ubergraph caller** (the chipPile grab clump-spawn; the clump re-pile pile-spawn; **trigger_wispSwarm's 32-wisp loop**) | the CALLER issues it `EX_CallMath` | **INVISIBLE to PE; CAUGHT by a `UFunction::Func` thunk patch** | the thunk patch (`ue_wrap/ufunction_hook`, AS-BUILT commit `d19ae4d4`) — NOT a ProcessEvent observer (see "Catching an EX_CallMath call" below). It owns the re-pile (clump→pile) deterministically; the grab (pile→clump) stays on the InpActEvt-PRE + held-edge adopt for now, `trash_channel`/`trash_collect_sync`. 2026-07-03: a SECOND chained thunk cb (`npc_world_enum::OnBeginDeferredExSpawn`, source-gated by `FFrame::Object`'s class) enrolls the wisp swarm — smoke 32/32 catches | [V: 0 host_spawn_watcher fires across 870 piles + every re-pile, hands-on 2026-06-21, commit 0e56ca39; thunk DETECTION verified read-only `B7EEB1BF`; wisp catch [V smoke 2026-07-03]] |
| `FinishSpawningActor` POST | GameplayStatics → PE | **VISIBLE** | the keyed sandbox-spawn seam (Key minted by now) → `ExpressSpawnedProp` | [V: host_spawn_watcher.cpp:209] |
| `ReceiveBeginPlay` — **save-loaded** actor (pre-exists at connect) | no runtime PE our session sees | **caught by the GUObjectArray SEED WALK**, NOT an Init observer | `SeedKnownKeyedProps` / `RegisterExistingWorldNpcs` (the 872 save-loaded piles, the pre-existing NPCs) | [V] |
| `ReceiveBeginPlay` — a normal runtime-spawned actor | maybe PE | **[?] not separately verified** | probe before relying on it | [?] |
| `SpawnEmitterAtLocation` + cosmetic `EX_CallMath` spawns | `EX_CallMath` | **INVISIBLE** | **POLL** the result (`event_cue_sync` diffs the PSC set), or a `UFunction::Func` thunk patch if a deterministic `(source,product)` is needed (the chipPile/clump re-pile plan) | [V: event_cue_sync.cpp] |
| `UGameplayStatics::SaveGameToSlot` | native C++ | **INVISIBLE to PE** | **native MinHook** (`save_block.cpp`) | [V] |
| `UGameplayStatics::SetGamePaused` — the ESC pause (mainGamemode ubergraph stmts [372]/[375]/[3035]) | `EX_CallMath`; AND the console `pause` cmd bypasses GameplayStatics entirely (PC::SetPause, native) | **INVISIBLE** (and multi-path) | **enforce the STATE, not the call sites**: `coop/session/pause_guard` polls `IsGamePaused` per gameplay tick while connected → `SetGamePaused(false)` (the game's own verb). Poll liveness while paused is guaranteed: `ui_menu_C::Tick` (the pause menu itself) dispatches PE every Slate frame — the exact tickFn `multiplayer_menu` latches | [RD bytecode 2026-07-04; built same day `769d02f7`; e2e autotest queued] |
| kerfur conversion verbs (`dropKerfurProp`/`spawnKerfuro`) | `EX_CallMath`/`EX_LocalVirtualFunction` | **INVISIBLE** | **POLL** (the kerfur conversion death-watch, `kerfur_convert.cpp:559`) | [RD] |
| `trigger_eventer.runEvent/runSpecialEvent` — the game's OWN scheduled fire (`saveSlot::settime` -> `gamemode.eventer.runEvent`) | BP->BP cross-object (`EX_Context` + `EX_VirtualFunction`) | **INVISIBLE** (PE and Func both) | AS-BUILT v95 `coop/world/event_fire_sync`: host POLLS `saveSlot.passEvents` growth (settime Array_Adds each fired row — the ONLY appender; runEvent never touches the array); dev-menu fires broadcast at dispatch through `HostFire`; client scheduler killed structurally (`allEvents.Num=0` — settime's walk SOURCE, boot-rebuilt every load, so it self-heals + can't poison a save) | [RD 2026-07-03; built same day] |
| every screen/panel device verb (`use`/`player_use`/`setActiveInterface`/`actionOptionIndex`/`saveSignal`/`makeAnOrder`...) | `EX_LocalVirtualFunction` (measured $type table) | **INVISIBLE** | **POLL the state field** (the whole shipped L2 device layer: `interactable_channel.h` Channel) — hooks only for the door request edge + the occupancy deny gate | [RD/IDA 2026-06-04; re-confirmed 2026-07-03 screens research] |

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
  ptr-for-ptr). RE: `research/findings/votv-chippile-dispatch-and-thunk-hook-RE-2026-06-21.md` §3/§6. **[V —
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
  `research/findings/votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md`; the autonomous verification
  harness: `[[reference-pile-test-harness]]` (`tools/pile-test-assert.ps1`, 13 log-truth invariants, VERDICT
  PASS). **STILL OPEN:** the WHOOSH throw sound (no ReliableKind exists in `protocol.h`; user-deprioritized,
  best confirmed by hearing); the dead `dropGrabObject` read-only thunk (to be retired, RULE 2). **[V hands-on:
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
  > arc-flight-stream PASS] (user: "дуга ЛЕТИТ"). The dead `dropGrabObject` read-only thunk is STILL PRESENT
  > (`trash_collect_sync.cpp:45,99-126,396`) — to be retired RULE 2 next (NOT YET removed). **[V the zero-fire
  > fact; V hands-on + harness the flight-stream arc; dropGrabObject-retire STILL OPEN]**

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
> `research/findings/votv-chippile-carry-churn-holdplayer-gate-2026-06-22.md`. **[V]**

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
