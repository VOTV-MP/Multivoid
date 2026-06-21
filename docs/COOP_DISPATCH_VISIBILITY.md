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

## The function table (the specific calls this codebase cares about)

| Edge | Dispatch | Visible? | Catch it with | Conf |
|---|---|---|---|---|
| `mainPlayer.InpActEvt_use` (E-press) | native input → PE | **VISIBLE** (PRE+POST) | we hook it: `trash_collect_sync::OnPileGrabPre`, door/interactable sync | [RD]/[V] |
| `actorChipPile.toClump` / `turnToPile` / `playerGrabbed` / `playerGrabbed_pre` | `EX_LocalVirtualFunction` | **INVISIBLE** | the **InpActEvt_use PRE** (read `lookAtActor` while the pile is alive) — NOT a hook on these | [RD: pass-1 RE §1.2/2.2, DECISIVE] |
| `actorChipPile.K2_DestroyActor(self)` (the pile's own grab-destroy) | `EX_VirtualFunction` (BP self-call) | **INVISIBLE** | death-watch / morph model (this is *why* it exists) | [RD: pass-1 RE §2.0] |
| `Actor.K2_DestroyActor` — engine-initiated destroy of a tracked actor | engine → PE | **VISIBLE** | we hook the **PRE** (`prop_lifecycle.cpp:293`, `npc_sync.cpp:298`) | [V] |
| **A BP-deferred-spawned actor's `init()`** (chipPile/clump/sandbox Aprop) | `EX_LocalVirtualFunction` from UCS | **INVISIBLE — its Init-POST observer does NOT fire** | the `FinishSpawningActor` POST (keyed) or `BeginDeferred` POST (keyless) seams below | [V: host_spawn_watcher.cpp:130-135,197-208] |
| `BeginDeferredActorSpawnFromClass` POST — **from a native/engine/spawner caller** (`garbagePileSpawner`, ambient pinecone/stick, NPC/WorldActor spawners) | the caller's dispatch reaches PE | **VISIBLE** | actor NOT yet positioned (read the `SpawnTransform` PARAM, not GetActorLocation). `host_spawn_watcher`/`npc_sync`/`world_actor_sync` all POST-observe it | [V] |
| `BeginDeferredActorSpawnFromClass` / `FinishSpawningActor` — **from a BP ubergraph caller** (the chipPile grab clump-spawn; the clump re-pile pile-spawn) | the CALLER issues it `EX_CallMath` | **INVISIBLE to PE; CAUGHT by a `UFunction::Func` thunk patch** | the thunk patch (`ue_wrap/ufunction_hook`, AS-BUILT commit `d19ae4d4`) — NOT a ProcessEvent observer (see "Catching an EX_CallMath call" below). It owns the re-pile (clump→pile) deterministically; the grab (pile→clump) stays on the InpActEvt-PRE + held-edge adopt for now, `trash_channel`/`trash_collect_sync` | [V: 0 host_spawn_watcher fires across 870 piles + every re-pile, hands-on 2026-06-21, commit 0e56ca39; thunk DETECTION verified read-only `B7EEB1BF`] |
| `FinishSpawningActor` POST | GameplayStatics → PE | **VISIBLE** | the keyed sandbox-spawn seam (Key minted by now) → `ExpressSpawnedProp` | [V: host_spawn_watcher.cpp:209] |
| `ReceiveBeginPlay` — **save-loaded** actor (pre-exists at connect) | no runtime PE our session sees | **caught by the GUObjectArray SEED WALK**, NOT an Init observer | `SeedKnownKeyedProps` / `RegisterExistingWorldNpcs` (the 872 save-loaded piles, the pre-existing NPCs) | [V] |
| `ReceiveBeginPlay` — a normal runtime-spawned actor | maybe PE | **[?] not separately verified** | probe before relying on it | [?] |
| `SpawnEmitterAtLocation` + cosmetic `EX_CallMath` spawns | `EX_CallMath` | **INVISIBLE** | **POLL** the result (`event_cue_sync` diffs the PSC set), or a `UFunction::Func` thunk patch if a deterministic `(source,product)` is needed (the chipPile/clump re-pile plan) | [V: event_cue_sync.cpp] |
| `UGameplayStatics::SaveGameToSlot` | native C++ | **INVISIBLE to PE** | **native MinHook** (`save_block.cpp`) | [V] |
| kerfur conversion verbs (`dropKerfurProp`/`spawnKerfuro`) | `EX_CallMath`/`EX_LocalVirtualFunction` | **INVISIBLE** | **POLL** (the kerfur conversion death-watch, `kerfur_convert.cpp:559`) | [RD] |

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
