# VOTV chipPile grab — the TRUTH about `playerGrabbed` observability + the correct observable hook (2026-06-08, pass 1)

**Status: DECISIVE. Every dispatch claim below is traced to the actual caller + the
x64 VM, not inferred.** This corrects the FALSE claim that
`actorChipPile_C::playerGrabbed` is ProcessEvent-observable (it is NOT — proven below,
matching the smoke's 0 firings). The 2026-06-08 prop_lifecycle
`GrabObserver_ChipPile_PlayerGrabbed_PRE` hook can never fire and must be retired.

RE sources (all re-runnable, VERIFIED):
- BP disassembly via `research/bp_reflection/_cfg.py` / `_disasm.py` (offset-aware; the
  mainPlayer ubergraph computed total == header `ScriptBytecodeSize` 119731, all jump
  targets land on boundaries → byte-exact). `actorChipPile.json` total == 4022 == header.
  (Fixed `research/pak_re/size_ubergraph.py` `EX_ArrayGetByRef` sizer:
  `ArrayVariable`/`ArrayIndex` keys, and `_cfg.py` `EX_StructMemberContext` render to
  use `StructMemberExpression` — both needed for the mainPlayer ubergraph.)
- IDA Pro on `VotV-Win64-Shipping.exe` (imagebase 0x140000000): the VM dispatch chain.
- Existing shipped code: `coop/interactable_sync.cpp` `OnUseInput` (proves `InpActEvt_use`
  POST + `lookAtActor` is observable), `coop/grab_observer.cpp`, `coop/prop_lifecycle.cpp`.

---

## Q1 — How is `actorChipPile_C::playerGrabbed` invoked, and does it pass through our ProcessEvent hook?

### 1.1 The caller is `AmainPlayer_C`'s ubergraph, via a NAME-based virtual call on `lookAtActor`

The grab is dispatched from `ExecuteUbergraph_mainPlayer` on the E-press (`InpActEvt_use`)
path. Byte-exact (offsets are kismet byte offsets in `ExecuteUbergraph_mainPlayer`):

```
@91790: K2Node_DynamicCast_AsInt_Player_4 := icast(lookAtActor)      ; cast lookAtActor -> the "Player" BP interface
@91826: K2Node_DynamicCast_bSuccess_17 := pcast<InterfaceToBool>(...)
@91855: IFNOT(success) JUMP @91916                                   ; not an interface impl -> skip
@91869: (lookAtActor as Player)->playerGrabbed_pre(self, hitResult)  ; PRE notify
@91916: useAction(false, ...)
@91940: K2Node_DynamicCast_AsInt_Player_5 := icast(lookAtActor)      ; cast AGAIN
@92005: IFNOT(success) JUMP @92066
@92019: (lookAtActor as Player)->playerGrabbed(self, hitResult)      ; *** THE GRAB ***
```

(A second, identical call site at `@114810→@114889` lives on the action-radial path —
same `icast(lookAtActor)->playerGrabbed(self, hitResult)` shape.)

So `mainPlayer`, on E-press, casts the actor under the crosshair (`lookAtActor`) to the BP
"Player" interface (which `actorChipPile_C` implements) and calls `playerGrabbed` on it.
`actorChipPile_C::playerGrabbed` is a thin wrapper → its own `ExecuteUbergraph_actorChipPile(3905)`.

### 1.2 The dispatch opcode is `EX_LocalVirtualFunction` — which routes to ProcessInternal, NOT ProcessEvent

Raw JSON of the `@92019` call node (`_dumpstmt`-confirmed):

```
$type = EX_LocalVirtualFunction
VirtualFunctionName = "playerGrabbed"
Parameters = [ EX_Self, EX_InstanceVariable(hitResult) ]
(parent node $type = EX_Context, holding it as ContextExpression; ObjectExpression = icast(lookAtActor))
```

**Both `playerGrabbed` call sites are `EX_LocalVirtualFunction` (NOT `EX_VirtualFunction`,
NOT `EX_FinalFunction`).** I traced the VM handling of these opcodes in IDA:

- The bytecode VM's `CallFunction` is **`sub_1414573C0`**. Its three exit paths are:
  `sub_141453550` (FUNC_Net early branch), `sub_14146BC40` (inline BP-VM body execution),
  and **`sub_141302DC0` (= `ProcessInternal`, the codebase's documented bypass).** *None*
  of the three is `UObject::ProcessEvent` (`UObject_ProcessEvent` @0x141465930).
- The opcode handlers that feed `CallFunction`:
  - `sub_141473DE0` — reads a **pointer** operand (a resolved `UFunction*`) then tail-calls
    `CallFunction`. That is `execFinalFunction`/`execLocalFinalFunction` (EX_FinalFunction /
    EX_LocalFinalFunction — the function is baked into the bytecode).
  - `sub_141477DE0` — reads a **12-byte FName** operand, resolves it via `sub_14145DE50`
    (FindFunction-by-name), then tail-calls `CallFunction`. That is
    `execVirtualFunction`/`execLocalVirtualFunction` (EX_VirtualFunction /
    EX_LocalVirtualFunction — the function is looked up by name at runtime). **This is the
    handler for our `playerGrabbed` call.**

  (The shipping build ICF-folded the Final/Virtual + Local/non-Local pairs into these two
  bodies; the operand width — 8-byte ptr vs 12-byte FName — is the discriminator. Both fold
  into the same `CallFunction`.)

- Confirmed our detour sits on **`UObject::ProcessEvent` ONLY**: `g_processEvent` is
  AOB-resolved (`reflection.cpp:52-54`, sig `P::kSigProcessEvent` → 0x141465930) and
  MinHook-detoured by `game_thread.cpp` (`ProcessEventDetour` → `g_originalPE` trampoline).
  The detour is NOT on `CallFunction`, `ProcessInternal`, or `sub_14146BC40`. And
  `UObject_ProcessEvent` itself *calls* `ProcessInternal` (xref @0x141465c10) — confirming
  ProcessInternal is the layer *below* our hook, so anything entering at ProcessInternal is
  invisible to us.

**VERDICT (Q1): `actorChipPile_C::playerGrabbed` is invoked as
`(lookAtActor as Player)->playerGrabbed(...)` via `EX_LocalVirtualFunction` →
`execLocalVirtualFunction` (sub_141477DE0) → `CallFunction` (sub_1414573C0) →
`ProcessInternal` (sub_141302DC0). It BYPASSES `UObject::ProcessEvent`. A ProcessEvent
detour on `playerGrabbed` can NEVER fire.** This is exactly the smoke result; the
"0 internal callers ⇒ externally dispatched ⇒ observable" inference was wrong — the call
*is* external (mainPlayer → pile), but external BP→BP calls go through the VM, not
ProcessEvent. Only a *native/engine → UFunction* entry (or our own `reflection::CallFunction`,
which uses `g_processEvent`) hits the hook.

---

## Q2 — The earliest PROVEN-observable edge with the pile still alive + eid-resolvable

### 2.0 What `playerGrabbed` actually does to the pile (so we know what "before" means)

`actorChipPile_C::playerGrabbed` → `ExecuteUbergraph_actorChipPile(@3905 → @2607)`,
byte-exact:

```
@2748: clump := BeginDeferredActorSpawnFromClass(self, <clump class>, xform, ...)
@2790: clump.chipType  := this.chipType
@2831: clump.holdPlayer := player
@3013: FinishSpawningActor(clump)
@3051: player->pickupObjectDirect(clump, _)     ; <-- the player grabs the CLUMP
@3097: JUMP @2563
@2563: K2_DestroyActor()                          ; <-- destroy SELF (the pile)
```

So one E-press: spawn a clump → hand-grab the **clump** → **destroy the pile**. The pile
exists only up to the `K2_DestroyActor()` *inside this same `playerGrabbed` call*.

`@2563 K2_DestroyActor` is **`EX_VirtualFunction K2_DestroyActor` (self)** → CallFunction →
ProcessInternal → **also UNOBSERVABLE** (same VM path as Q1). This resolves the DECISIVE
doc §4 open sub-question: **the pile's grab-destroy is NOT visible to our K2_DestroyActor
PRE observer** (the very reason the clump death-watch had to exist). A destroy-observer
or naive death-watch on the OWNER's pile-grab cannot catch it on the destroy edge.

### 2.1 (a) The grab INPUT is `InpActEvt_use` (E-press) — PROVEN observable — and `lookAtActor` is the pile

The grab is reached from the E-press input action `InpActEvt_use_K2Node_InputActionEvent_41`
(the thin handler enters `ExecuteUbergraph_mainPlayer @112867`, flowing to the
`playerGrabbed` blocks above). There is **no dedicated grab input or mouse-button** for a
chipPile — picking one up is the E/use action, gated only by the `icast(lookAtActor)`
interface-cast succeeding.

- This is the **same UFunction our door sync already POST-hooks** (`interactable_sync.cpp`
  `OnUseInput`, registered on `MainPlayerUseInputEventFn = InpActEvt_use_..._41`). That
  hook firing in shipped code PROVES `InpActEvt_use` is ProcessEvent-dispatched (the native
  input system → the BP input UFunction → our detour). So we can observe it.
- At the grab, the target IS `lookAtActor`: `playerGrabbed` is called on `icast(lookAtActor)`.
  `lookAtActor` (`AmainPlayer_C::lookAtActor`, reflection-resolved offset 0x0AA0;
  `engine::ReadMainPlayerLookAtActor`) is populated continuously by the player's interaction
  trace *before* the press — exactly as the door sync reads it. So at the press edge it holds
  the **pile** (pre-conversion).

**PRE vs POST — this is the crux and the reason the POST `OnUseInput` shape is WRONG for
piles:** the thin `InpActEvt_use_41` handler runs `[0] copy Key; [1] FINAL ExecuteUbergraph;
[2] Return`. The ubergraph (which runs `playerGrabbed` → spawns clump + destroys pile) runs
*inside* the single ProcessEvent dispatch of `InpActEvt_use_41`, between its `[0]` and `[2]`.
Therefore:
- **POST** observer (what `OnUseInput` uses) fires *after* `[2]` → **after** `playerGrabbed`
  ran → the pile is already destroyed (`@2563`). Reading `lookAtActor` at POST gives a
  dead/cleared actor. POST is correct for doors (the door is not destroyed by its toggle) but
  WRONG for a pile.
- **PRE** observer fires *before* `[0]` → **before** the ubergraph → the pile is **alive,
  eid-resolvable.** PRE is required.

### 2.2 (b) The PhysicsHandle never grabs the pile — it grabs the clump

`@3051 player->pickupObjectDirect(clump, _)`; and `pickupObjectDirect`
(`ExecuteUbergraph_mainPlayer @99488`) takes `clump.K2_GetRootComponent()`, casts it to
`PrimitiveComponent`, and that is what the handle/grab machinery holds. **The grabbed
Component is the CLUMP's mesh, never the pile's.** Our existing `GrabComponentAtLocation`
POST hook (`grab_observer.cpp`) therefore sees only the clump being grabbed —
`params.Component.GetOwner()` is the clump, and by the time it fires the pile is gone.
**(b) is a dead end for identifying the pile.** This also corrects the 2026-05-27 doc's
guess that pickup routes through `playerTryToHold`/`toClump`: on `actorChipPile_C` both
`playerTryToHold` and `playerTryToGrab` are **no-ops** (`collected := false; return`), and
`toClump` is NOT on the grab path at all — the live grab path is
`playerGrabbed → BeginDeferredActorSpawnFromClass(clump) + pickupObjectDirect + K2_DestroyActor`.

### 2.3 (c) Any other ProcessEvent-dispatched UFunction on the pile/player at the grab edge

- `playerGrabbed` / `playerGrabbed_pre` on the pile — **unobservable** (EX_LocalVirtualFunction, §1.2).
- the pile's `K2_DestroyActor` — **unobservable** (EX_VirtualFunction, §2.0).
- The only ProcessEvent-observable edge at the grab is the input UFunction itself,
  **`InpActEvt_use_..._41`** (native input → UFunction → ProcessEvent), which our detour
  sees. There is no other.

### 2.4 RECOMMENDED HOOK (Q2 answer)

**A PRE observer on `AmainPlayer_C::InpActEvt_use_K2Node_InputActionEvent_41`**
(`MainPlayerUseInputEventFn`), CLIENT *and* HOST (whoever physically grabs). At the PRE edge:

```cpp
void OnUseInput_PileDestroy_PRE(void* self /* mainPlayer */, void*, void*) {
    auto* s = LoadSession(); if (!self || !s || !s->connected()) return;
    void* aimed = ue_wrap::engine::ReadMainPlayerLookAtActor(self);   // the pile, PRE-conversion
    if (!aimed || !ue_wrap::prop::IsChipPile(aimed)) return;          // chipPile-only (add a class gate)
    // its cross-peer identity: a pile WE own carries a local Prop Element id; a pile WE
    // mirror carries a wire eid.
    coop::element::ElementId eid = PT::GetPropElementIdForActor(aimed);
    if (eid == coop::element::kInvalidId) eid = coop::remote_prop::ResolveMirrorEidByActor(aimed);
    if (eid == coop::element::kInvalidId || eid == 0u) return;        // untracked -> no peer mirror
    coop::net::PropDestroyPayload dp{}; dp.key.len = 0; dp.elementId = eid;   // chipPile is eid-only (Key=None)
    s->SendPropDestroy(dp);   // peer drops its mirror by identity (event_feed -> remote_prop::OnDestroy)
}
```

- Register with `GT::RegisterPreObserver(InpActEvt_use_41_fn, ...)` — the same UFunction the
  door `OnUseInput` POST-registers; `RegisterPreObserver` exists and is used elsewhere.
- Reading the pile + its eid: `engine::ReadMainPlayerLookAtActor` → the pile actor;
  `PT::GetPropElementIdForActor` (owner pile) or `remote_prop::ResolveMirrorEidByActor`
  (mirror pile) → the eid. PropDestroy(eid) routes via `event_feed` → `remote_prop::OnDestroy`
  (eid teardown) to despawn the peer's mirror.
- Gate to **chipPile only** (not "any keyed interactable"): an E-press while aiming at a
  non-pile must not emit a destroy. Add `ue_wrap::prop::IsChipPile` (the
  `actorChipPile_C`-lineage class check — `RegisterExtraKeyedInitObservers` already resolves
  `actorChipPile_C`).
- **DELETE (RULE 2):** `prop_lifecycle.cpp`
  `GrabObserver_ChipPile_PlayerGrabbed_PRE` + `RegisterChipPileGrabObserver` + the
  `if (e.done == &sChip) RegisterChipPileGrabObserver(cls);` line + the stale "playerGrabbed
  is the ONLY observable hook" comment block. It hooks a UFunction (`playerGrabbed`) that
  never reaches our detour; it has been dead since it shipped.

PRE-debounce note: `InpActEvt_use` dispatches on BOTH the press and the release of one tap
(the door `OnUseInput` already debounces this, 300 ms). Mirror that debounce so one grab
emits one PropDestroy. (After the first press the pile is destroyed, so a second emit would
no-op anyway — but debounce keeps the log clean.)

This is the same "non-authority sends an INPUT EDGE on the observable `InpActEvt_use`, never
entity state" shape the door sync already uses (MTA single-syncer / trust-the-edge).

---

## Q3 — Is the landed mirror pile a fully-functional chipPile? Is a death-watch on mirror piles viable?

### 3.1 The landed mirror pile IS a normal, fully-grabbable `actorChipPile_C`

`remote_prop_spawn::OnSpawn` `kFreshLanded` spawns the pile via the standard
BeginDeferred → setKey → FinishSpawningActor pipeline, then `turnToPile(vel)` for the impact
dust+sound. The **only** thing disabled on a mirror trash entity —
`SetActorRootNotifyRigidBodyCollision(spawned, false)` — is gated on
`classW.find(L"garbageClump")` (`remote_prop_spawn.cpp:436-446`); it applies to the CLUMP
BALL, **not** the chipPile. So the landed mirror pile keeps full collision + its
auto-bound interaction interface. When the LOCAL player aims at it and presses E, its OWN
`playerGrabbed` runs the §2.0 sequence → spawns a clump, hand-grabs the clump, and
`K2_DestroyActor`s the mirror pile locally. **The mirror pile self-destroys on local grab,
exactly like a native pile.**

### 3.2 A death-watch on mirror piles IS viable — but the destroy edge is unobservable, so it must POLL liveness, AND be proximity-gated against stream-out

- The mirror pile's local grab-destroy (`@2563 K2_DestroyActor`, EX_VirtualFunction) is
  **unobservable** (§2.0) — so a K2_DestroyActor PRE observer will not catch it. A death-watch
  must **poll `R::IsLiveByIndex` per game-thread tick** (the same mechanism
  `trash_collect_sync::TickWatchReleasedClumps` already uses for the owner's clump).
- **Stream-out false-fire IS a real hazard and HAS a proven fix in this codebase.** The naive
  ungated death-watch flooded false destroys on the connect-teleport sublevel **stream-out**
  (a mirror pile vanishing from the index because its sublevel unloaded looks identical to a
  grab). The grime death-watch (`coop/grime_sync.cpp`, the documented precedent) solved it
  with a **PROXIMITY GATE**: an actor that vanishes **NEAR the local camera** (grime uses
  ~8 m; `kWipeProximityCm2`) = a real local interaction (you can only grab/sponge what you
  stand at) → emit the destroy; **FAR** (a streamed-out sublevel is far, ~14 m measured at the
  connect teleport) = ignore. The same gate transfers directly: a mirror pile that goes dead
  **near the local player** = the local player grabbed it → broadcast PropDestroy(eid); a
  mirror pile that goes dead **far away** = a stream-out → drop it from the watch silently
  (a stream-back re-registers it on the next rebuild). (`K2_DestroyActor` PRE is, per grime's
  own comment, "NOT usable" here — same reason: the BP-internal self-destruct bypasses the
  detour. They reached the same conclusion this RE proves for piles.)

### 3.3 Which mechanism to ship

The **§2.4 `InpActEvt_use` PRE hook is strictly better than a mirror-pile death-watch** and is
the recommendation:
- It fires the instant the player commits to the grab (PRE), with the pile still alive and
  eid trivially resolvable — no per-tick liveness poll, no proximity heuristic, no stream-out
  ambiguity (a stream-out is not an E-press, so it never enters this path).
- It is symmetric: it covers BOTH "owner re-grabs its own tracked pile" and "peer grabs a
  mirror pile", because it keys off `lookAtActor`'s eid regardless of which side owns it.
- It reuses the exact observable edge + helpers (`ReadMainPlayerLookAtActor`,
  `GetPropElementIdForActor`/`ResolveMirrorEidByActor`, `SendPropDestroy`) the door sync and
  prop pipeline already ship.

A mirror-pile death-watch is the **fallback** only if a grab path that does NOT go through
`InpActEvt_use_41` is ever found (none exists for chipPiles per this RE). If it is ever
needed, it MUST be proximity-gated (3.2).

---

## Summary table — observability of every grab-edge UFunction (all VERIFIED in IDA/bytecode)

| UFunction (edge) | Dispatch opcode | VM path | Hits `UObject::ProcessEvent`? | Pile alive at this edge? |
|---|---|---|---|---|
| `mainPlayer.InpActEvt_use_..._41` (E-press) | native input → UFunction | engine → ProcessEvent | **YES** (PRE & POST both fire) | **PRE: yes** / POST: no (already converted) |
| `actorChipPile.playerGrabbed` | `EX_LocalVirtualFunction` | CallFunction → ProcessInternal | **NO** | n/a (it IS the conversion) |
| `actorChipPile.playerGrabbed_pre` | `EX_LocalVirtualFunction` | CallFunction → ProcessInternal | **NO** | yes (but unobservable) |
| `actorChipPile.K2_DestroyActor` (self, in playerGrabbed) | `EX_VirtualFunction` | CallFunction → ProcessInternal | **NO** | being destroyed (unobservable) |
| `mainPlayer.pickupObjectDirect(clump)` | `EX_LocalVirtualFunction` (BP→BP) | CallFunction → ProcessInternal | **NO** | grabs the CLUMP, not the pile |
| `PHC.GrabComponentAtLocation` (already hooked POST) | engine → ProcessEvent | — | YES | sees the CLUMP's mesh, pile already gone |

**Bottom line:** the ONLY ProcessEvent-observable edge with the pile alive is a **PRE observer
on `InpActEvt_use_..._41`**, reading `lookAtActor` → the pile → its eid → `PropDestroy(eid)`.
Implement that; delete the dead `playerGrabbed` PRE hook in `prop_lifecycle.cpp`.

---

## Files / addresses cited (implementation + verification map)

- Caller (E-press → grab): `ExecuteUbergraph_mainPlayer` @91790-92019 (icast(lookAtActor) →
  playerGrabbed_pre/playerGrabbed), @114810-114889 (radial path); entry from
  `InpActEvt_use_K2Node_InputActionEvent_41` → @112867. Computed ubergraph total 119731 ==
  header.
- Pile conversion: `ExecuteUbergraph_actorChipPile` @3905→@2607: @2748 BeginDeferred(clump),
  @3013 FinishSpawningActor, @3051 pickupObjectDirect(clump), @2563 K2_DestroyActor(self).
  `actorChipPile.playerTryToHold`/`playerTryToGrab` = `collected:=false` no-ops.
- VM dispatch (IDA, VotV-Win64-Shipping.exe @ imagebase 0x140000000):
  `UObject::ProcessEvent` = 0x141465930 (named `UObject_ProcessEvent`; our detour target,
  AOB `P::kSigProcessEvent`); `ProcessInternal` = sub_141302DC0 (called from ProcessEvent
  @0x141465c10 and from CallFunction @0x1414575cd/0x141457601); `CallFunction` = sub_1414573C0
  (exits: sub_141453550 / sub_14146BC40 / ProcessInternal — none is ProcessEvent);
  `execFinal/LocalFinalFunction` = sub_141473DE0 (ptr operand → CallFunction);
  `execVirtual/LocalVirtualFunction` = sub_141477DE0 (12-byte FName operand →
  sub_14145DE50 FindFunction → CallFunction).
- Our hook install: `ue_wrap/reflection.cpp:52-61` (AOB resolve g_processEvent),
  `ue_wrap/game_thread.cpp` (ProcessEventDetour on 0x141465930 only;
  `RegisterPreObserver`:161 / `RegisterPostObserver`).
- `lookAtActor`: `ue_wrap/reflected_offset.cpp:99` (FindPropertyOffset on mainPlayer_C
  "lookAtActor", 0x0AA0); reader `ue_wrap/engine_mainplayer.cpp:288 ReadMainPlayerLookAtActor`.
- Observable-input precedent: `coop/interactable_sync.cpp:678 OnUseInput` (POST on
  InpActEvt_use_41 + ReadMainPlayerLookAtActor + 300 ms press/release debounce) — proves the
  edge fires; PRE is the only change for the pile case.
- DELETE: `coop/prop_lifecycle.cpp:388-425` (`GrabObserver_ChipPile_PlayerGrabbed_PRE` +
  `RegisterChipPileGrabObserver`) + `:552` (the `RegisterChipPileGrabObserver(cls)` call).
- Recommended-hook helpers: `coop/prop_element_tracker.h:89 GetPropElementIdForActor`,
  `coop/remote_prop.h:146 ResolveMirrorEidByActor`, `Session::SendPropDestroy` →
  `coop/event_feed.cpp:315 PropDestroy` → `remote_prop::OnDestroy` (eid teardown).
- Landed mirror pile (fully functional): `coop/remote_prop_spawn.cpp:436-446` (collision-off
  is garbageClump-ONLY), `:463-468` (kFreshLanded turnToPile). Stream-out proximity-gate
  precedent: `coop/grime_sync.cpp` (`kWipeProximityCm2`, ~8 m near = wipe/grab, far =
  stream-out). Owner-clump death-watch (the poll-liveness pattern):
  `coop/trash_collect_sync.cpp:199 TickWatchReleasedClumps`.
- RE-tool fixes (dev-only, RULE 3): `research/pak_re/size_ubergraph.py` EX_ArrayGetByRef
  sizer (ArrayVariable/ArrayIndex); `research/bp_reflection/_cfg.py` EX_StructMemberContext
  render (StructMemberExpression).
