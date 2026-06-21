# VOTV clump/pile lifecycle — DEFINITIVE ProcessEvent-observability map + robust sync design (pass 2) — 2026-06-08

> **⚠ PARTIALLY SUPERSEDED 2026-06-21 — authority is now [docs/COOP_DISPATCH_VISIBILITY.md](../../docs/COOP_DISPATCH_VISIBILITY.md) + [docs/piles/08](../../docs/piles/08-HOST-AUTH-TRASH-CHANNEL.md).**
> STILL TRUE: `playerGrabbed`/`pickupObjectDirect`/`K2_DestroyActor(self)` are INVISIBLE (the PRE-observer
> conclusion). **NOW FALSE: the claim that `BeginDeferredActorSpawnFromClass`/`FinishSpawningActor` are
> "UNOBSERVABLE"** — they ARE ProcessEvent-VISIBLE (host_spawn_watcher catches the identical opcode live;
> DISPATCH map line 65). That reverses this doc's design conclusion: the clump↔pile convert is caught at
> the **convert-spawn POST** (zero proximity), NOT a proximity death-watch. The death-watch design below is
> RETIRED with the morph (the proximity land-watch false-fired in clusters — hands-on 2026-06-21). Read 08
> for the current host-authoritative design; this doc is history for the playerGrabbed dispatch facts only.

Supersedes the detection half of `votv-clump-pile-dupe-DECISIVE-RE-2026-06-08.md` and
`votv-clump-ball-to-pile-conversion-RE-and-event-fix-2026-06-08.md`. The eid-morph
storyline in those docs is correct; **the one fatal assumption — that
`actorChipPile_C::playerGrabbed` is "externally-dispatched, therefore observable" — is now
DISPROVEN by disassembly.** That is why the last fix's PRE observer never fired. This doc
gives the verified dispatch path for every clump/pile lifecycle UFunction and a design that
uses ONLY proven-observable edges.

RULE 1: every "observable / not" verdict below is proven from the binary's BP-VM dispatch
path, NOT inferred from "has no internal callers".

---

## 0. TL;DR (the decisive facts)

1. **The observability rule (IDA-proven, `VotV-Win64-Shipping.exe`):** our hook is a MinHook
   detour on `UObject::ProcessEvent` (`0x141465930`). A UFunction is observable **iff its
   invocation goes through `ProcessEvent`.**
   - **BP→BP `CALL` / `CALLVIRT`** in bytecode (`EX_FinalFunction` / `EX_VirtualFunction` /
     the `Local*` variants) → dispatched by `execFinalFunction` (`sub_141474FB0`) /
     `execVirtualFunction` (`sub_1414751A0`) / `CallFunction` (`sub_1414573C0`) →
     `ProcessLocalScriptFunction` (`sub_141453550`) or `ProcessInternal` (`sub_141302DC0`)
     **DIRECTLY. They NEVER call `ProcessEvent`. → UNOBSERVABLE.**
   - **Native funcs via `EX_CallMath`** (`BeginDeferredActorSpawnFromClass`,
     `FinishSpawningActor`, math) → native thunk directly. **→ UNOBSERVABLE** (as our hook
     target; they are not BP UFunction dispatches at all).
   - **Multicast-delegate `Broadcast()`** (physics `OnComponentHit` via
     `UPrimitiveComponent::DispatchBlockingHit`; input-action events; any `BndEvt__…`
     `K2Node_ComponentBoundEvent`) → `FMulticastScriptDelegate::ProcessMulticastDelegate`
     (`sub_1407F5710`) which, per bound entry, calls `Object->ProcessEvent(Func, Params)`
     (vtable slot `+0x220`, confirmed = `0x141465930`). **→ OBSERVABLE.**
   - **Engine-driven entry events** the engine itself dispatches via `Obj->ProcessEvent`
     (BeginPlay, Tick, latent resumes, timer callbacks). **→ OBSERVABLE.**

2. **`actorChipPile_C::playerGrabbed` is UNOBSERVABLE.** `mainPlayer`'s ubergraph calls it
   via **`EX_LocalVirtualFunction` (CALLVIRT) — 2 call sites** (proven by walking
   `ExecuteUbergraph_mainPlayer`). CALLVIRT → `ProcessInternal`, bypasses our hook. The last
   fix's `GrabObserver_ChipPile_PlayerGrabbed_PRE` could never fire. **DELETE it (RULE 2).**
   Same for `playerTryToCollect` (3× CALLVIRT), `playerTryToHold` (2×), `playerTryToGrab`
   (1×), `toClump` (1× CALLVIRT), `turnToPile` (1× CALLVIRT).

3. **The clump's hit handler IS observable.** The convert trigger
   `BndEvt__prop_garbageClump_StaticMesh_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature`
   is bound to `StaticMesh.OnComponentHit` (a multicast delegate; binding lives in the clump
   CDO's `ComponentDelegateBindings`). The clump StaticMesh has `bNotifyRigidBodyCollision =
   True` + `ECC_PhysicsBody` and simulates, so a blocking contact runs
   `DispatchBlockingHit → OnComponentHit.Broadcast → ProcessMulticastDelegate → ProcessEvent`
   **on the OWNER's real clump. A PRE observer on this handler FIRES → instant convert is
   achievable (symptom 2).**

4. **Every BP-internal destroy is unobservable** (the death-watch's reason to exist):
   `K2_DestroyActor` is CALLVIRT in BOTH the clump ubergraph (the convert self-destroy) and
   the chipPile ubergraph (`toClump`/`turnToPile` self-destroy). So neither a converting
   clump's death nor a grabbed pile's death reaches the `K2_DestroyActor` PRE observer.

5. **Net design:**
   - **Convert (instant):** PRE-observe the clump hit handler; when the BP's own gate
     passes, broadcast `PropConvert{oldEid, newEid, xform, chipType, vel}`. Receiver
     atomically destroys the ball by eid + spawns the pile (`kFreshLanded`). Death-watch
     stays as a backstop ONLY (LifeSpan/no-convert death → `PropDestroy(eid)`).
   - **Grab-destroy propagation (the re-grab disappear, symptoms 1+3):** since `playerGrabbed`
     is unobservable, use a **mirror-pile death-watch**: a landed pile that this peer is
     mirroring, when grabbed locally, runs its own `playerGrabbed → turnToPile →
     K2_DestroyActor` and **dies locally**; the death-watch sees it die and broadcasts
     `PropDestroy(eid)` so the other peer drops its mirror. Plus a held-edge backstop. This is
     the robust replacement for the deleted position-consume.
   - **Dupe (symptom 1):** with the position-consume gone, the residual dupe is the
     un-removed grabbed-pile mirror (fixed by #grab-destroy above) + an eid race; enumerated
     in §6.

---

## 1. How our hook works (the observability test, exactly)

`ue_wrap::game_thread::Install()` MinHook-detours `UObject::ProcessEvent`
(`reflection::ProcessEventAddr()` → `0x141465930`); the detour fires PRE-observers, the
original, then POST-observers (`src/votv-coop/src/ue_wrap/game_thread.cpp`). Every observer
target is matched by UFunction pointer. **A UFunction whose call never reaches ProcessEvent
can have an observer registered but the observer never runs.** That is the failure we are
diagnosing.

ProcessEvent itself is virtual — invoked as `Obj->vtable[+0x220](self, func, params)`. The
detour catches every such virtual call (the data-xref scan shows `0x141465930` populated in
every UObject-derived vtable, e.g. `0x143a1af48` holds the qword `0x0000000141465930`).

### 1.1 BP→BP calls bypass ProcessEvent (IDA, decisive)

The BP-VM opcode handlers live in the `GNatives` table (`qword_144D8ECD0[opcode]`). Verified:

| Opcode | Handler (IDA) | What it does | ProcessEvent? |
|---|---|---|---|
| `EX_FinalFunction` (0x1B) | `sub_141474FB0` (`execFinalFunction`) | reads `UFunction*` from bytecode; `if (Flags&FUNC_Native 0x400) ProcessInternal(`sub_141302DC0`) else ProcessLocalScriptFunction(`sub_141453550`)` | **NO** |
| `EX_VirtualFunction` (0x1C) | `sub_1414751A0` (`execVirtualFunction`) | `FindFunction(obj, FName)` then same branch as above | **NO** |
| (shared) `UObject::CallFunction` | `sub_1414573C0` | events/scripts → `ProcessLocalScriptFunction` / `ProcessInternal`; never PE | **NO** |
| `ProcessInternal` | `sub_141302DC0` | sets the frame, calls `Func->Func` (the interpreter `sub_141465DF0`/`sub_141465CE0`) at UFunction+0xD8 | **NO** |

`UObject::ProcessEvent` (`0x141465930`) ALSO ends by calling `sub_141302DC0` (ProcessInternal) —
i.e. ProcessEvent is just "set up params + call ProcessInternal", and the BP-VM opcode
handlers call ProcessInternal **without** the ProcessEvent wrapper. So when a running BP
graph executes `CALL Foo()` / `CALLVIRT Foo()`, control goes straight into the interpreter —
our detour is never entered. This is the mechanism behind the already-shipped, already-proven
conclusion in `interactable_sync.cpp` (lines 110-118, 657-663): "IDA-PROVEN 2026-06-04 the
interaction verbs (`doorOpen`, `SetActive`, `Open`, `player_use`…) dispatch via `CallFunction
→ ProcessInternal (@0x141302dc0)` and bypass our `ProcessEvent` detour (@0x141465930), so a
POST observer never fires." That subsystem switched its SENDER to per-tick state polling for
exactly this reason. **Same rule applies to the whole clump/pile family.**

### 1.2 Multicast-delegate broadcasts DO go through ProcessEvent (IDA, decisive)

`UPrimitiveComponent::DispatchBlockingHit` (`sub_1428B8A30`): resolves the `OnComponentHit`
multicast delegate (`sub_1428CCC30` builds the "OnComponentHit" FName + reads the property),
packs the hit params (the `{MyComp, OtherActor, OtherComp, NormalImpulse}` tuple + the 136-B
FHitResult), and calls `ProcessMulticastDelegate` (`sub_1407F5710`).

`ProcessMulticastDelegate` (`sub_1407F5710`): copies the `InvocationList` (TArray of
`FScriptDelegate` {WeakObjPtr, FName}), and **for each live binding** resolves the object +
`FindFunctionChecked(FName)` and calls `(*(*obj + 0x220))(obj, func, params)` — i.e.
`obj->ProcessEvent(BoundFunc, Params)`. (Disasm: `call qword ptr [r9+220h]` with args
`(obj, func, params)`; slot +0x220 == ProcessEvent, confirmed against the vtable data
above.)

**Therefore the bound delegate handler runs through our detour.** This is the SAME path that
makes input-action events observable — `grab_observer.cpp` registers `InpActEvt_use` and it
demonstrably fires (and `interactable_sync.cpp:671-675` relies on that as "the ONE
ProcessEvent-observable use-edge"). Input actions and component-bound events are both
multicast-delegate broadcasts; both hit ProcessEvent.

> Engine builds normally compile DispatchBlockingHit's broadcast as
> `OnComponentHit.Broadcast(...)`. The clump's `bNotifyRigidBodyCollision=True` is the gate
> that makes the physics system *call* DispatchBlockingHit on contact; with it true (verified
> in the CDO), the owner's clump fires the handler on every qualifying ground hit.

---

## 2. THE OBSERVABILITY MAP (full clump/pile lifecycle)

Dispatch evidence: `research/bp_reflection/{actorChipPile,prop_garbageClump,mainPlayer}.json`
disassembled with `research/bp_reflection/_disasm.py` + a recursive opcode walk; IDA dispatch
proof in §1. "CALLVIRT/CALL" = the opcode at the call site (the caller). "Entry" = how the
UFunction is first entered (engine event vs delegate vs BP-internal verb call).

| # | UFunction | Class | How it's INVOKED (caller → opcode) [evidence] | Entry kind | Observable via our PE hook? | Evidence |
|---|---|---|---|---|---|---|
| 1 | `BndEvt__…StaticMesh_…ComponentHitSignature__DelegateSignature` (the convert trigger) | `prop_garbageClump_C` | physics blocking hit → `DispatchBlockingHit` → `OnComponentHit.Broadcast` → `ProcessMulticastDelegate` → `ProcessEvent(handler)` | **delegate broadcast** | **YES** (PRE + POST) | §1.2; CDO `ComponentDelegateBindings` binds `StaticMesh.OnComponentHit`; `bNotifyRigidBodyCollision=True`, `ECC_PhysicsBody` |
| 2 | `ExecuteUbergraph_prop_garbageClump` | `prop_garbageClump_C` | called from #1 (`CALL …Ubergraph(2702)`) | called inside an already-PE'd frame | **N/A** (one giant fn; can't gate on internal offset) | the bound handler #1 is the seam, not this |
| 3 | `K2_DestroyActor` (clump self-destroy on convert) | `prop_garbageClump_C` ubergraph | `CALLVIRT K2_DestroyActor()` (1× in clump ubergraph) | BP-internal | **NO** | opcode walk: 1× `EX_LocalVirtualFunction` |
| 4 | `BeginDeferredActorSpawnFromClass` / `FinishSpawningActor` (spawns the pile during convert) | UGameplayStatics (called from clump ubergraph) | `EX_CallMath` (native thunk) | native | **NO** | opcode walk: `CallMath` |
| 5 | `SetNotifyRigidBodyCollision` (clump silences its own further hits) | UPrimitiveComponent (clump ubergraph) | `CALLVIRT` | BP-internal | **NO** | opcode walk |
| 6 | `turnToPile` (chipPile→clump GRAB morph: spawns clump+destroys pile) | `actorChipPile_C` | `CALLVIRT turnToPile()` from chipPile ubergraph (1×) | BP-internal | **NO** | opcode walk: 1× CALLVIRT |
| 7 | `toClump` (alt morph entry; spawns clump, destroys self) | `actorChipPile_C` | `CALLVIRT toClump()` from chipPile ubergraph (1×) | BP-internal | **NO** | opcode walk; body: `K2_DestroyActor` CALLVIRT |
| 8 | `K2_DestroyActor` (chipPile self-destroy in turnToPile/toClump) | `actorChipPile_C` ubergraph | `CALLVIRT` (2×) | BP-internal | **NO** | opcode walk: 2× CALLVIRT |
| 9 | `playerGrabbed` (pile grab entry → ubergraph 3905 → morph) | `actorChipPile_C` | **`mainPlayer` ubergraph calls it `CALLVIRT playerGrabbed()` — 2×** | BP-internal (NOT a delegate) | **NO** ← the failed fix's target | walk of `ExecuteUbergraph_mainPlayer`: 2× `EX_LocalVirtualFunction` |
| 10 | `playerGrabbed_pre` | `actorChipPile_C` | `mainPlayer` ubergraph `CALLVIRT` (1×) | BP-internal | **NO** | walk: 1× CALLVIRT |
| 11 | `playerTryToCollect` (the trashBits/pile collect) | `actorChipPile_C` / `trashBitsPile_C` | `mainPlayer` ubergraph `CALLVIRT` (3×) | BP-internal | **NO** | walk: 3× CALLVIRT (matches the prior hands-on "POST never fired") |
| 12 | `playerTryToHold` / `playerTryToGrab` | `actorChipPile_C` | `mainPlayer` ubergraph `CALLVIRT` (2× / 1×) | BP-internal | **NO** | walk |
| 13 | `pickupObjectDirect` / `pickupObject` / `collectObject` (mainPlayer carry entries) | `AmainPlayer_C` | thin CustomEvent stubs → `CALL …Ubergraph(N)`; reached from the ubergraph itself after the `useArm` trace | BP-internal (driven from input, but the carry call itself is internal) | **NO** as a hook seam | stubs are `CALL ExecuteUbergraph_mainPlayer(...)` |
| 14 | `InpActEvt_use_…41/42`, `InpActEvt_LeftMouseButton_…`, etc. (the player INPUT that initiates a grab/collect) | `AmainPlayer_C` | engine input system broadcasts the input-action delegate → `ProcessEvent` | **input-action delegate** | **YES** | grab_observer + interactable_sync both observe `InpActEvt_use` and it fires |
| 15 | `Init` / `ReceiveBeginPlay` (chipPile + clump spawn) | both | engine dispatches via `ProcessEvent` | engine event | **YES** | `prop_lifecycle` Init POST observers fire (the late-load catch proves it) |
| 16 | `K2_DestroyActor` (generic, for the keyed Aprop_C path) | `AActor` | when called via `ProcessEvent` (e.g. our own `ConsumeLocalActor`, or engine teardown) it's seen; when called `CALLVIRT` from a BP ubergraph it is NOT | mixed | **PARTIAL** | the `K2_DestroyActor` PRE observer fires for engine/our dispatches but NOT for BP-internal self-destroys (#3/#8) — the documented blind spot |
| 17 | `ReceiveTick` (held-clump per-tick) | `prop_garbageClump_C` | engine tick → `ProcessEvent` | engine event | **YES** (but not needed; pose is polled) | — |

**Reading of the map:**
- The ONLY observable seams in the whole grab→carry→convert→re-grab lifecycle are: the
  **clump hit handler** (#1, the convert edge), the **player input actions** (#14), and
  **Init/BeginPlay/Tick** (#15/#17). Everything that actually mutates the entities
  (`playerGrabbed`, `turnToPile`, `toClump`, the convert spawn, every self-`K2_DestroyActor`)
  is BP-internal CALLVIRT/CallMath → **unobservable**.
- This is why polling (`grabbing_actor`/`holding_actor`, liveness death-watch) is the
  project-wide substrate for this family, mirroring `interactable_sync`'s state-poll
  conclusion.

---

## 3. Why the last fix failed (root cause, precisely)

`prop_lifecycle.cpp` added `GrabObserver_ChipPile_PlayerGrabbed_PRE` + `RegisterChipPileGrabObserver`
on `actorChipPile_C::playerGrabbed` (PRE), on the premise (doc comment lines 376-387 +
`votv-clump-pile-dupe-DECISIVE-RE-2026-06-08.md` §4) that `playerGrabbed` is "externally
dispatched, the ONLY observable hook." **That premise is false.** `mainPlayer`'s ubergraph
invokes `chipPile.playerGrabbed` with `EX_LocalVirtualFunction` (CALLVIRT) — proven by the
opcode walk (2 call sites). CALLVIRT → `execVirtualFunction (sub_1414751A0)` → `ProcessInternal
/ ProcessLocalScriptFunction` directly → **never `ProcessEvent`.** The observer registered
successfully but its callback was never invoked, so the re-grabbed pile's `PropDestroy(eid)`
was never sent → the peer's mirror pile never disappeared (symptom 3), and the convert still
lagged (symptom 2, untouched by that fix). The "no internal callers ⇒ observable" reasoning
was the exact trap the task warned about.

**Action (RULE 2):** delete `GrabObserver_ChipPile_PlayerGrabbed_PRE`,
`RegisterChipPileGrabObserver`, and its call from `RegisterExtraKeyedInitObservers`. Replace
with the mirror-pile death-watch (§5.2).

---

## 4. Sub-question 1 — INSTANT CONVERT (symptom 2): YES, the hit delegate is observable

**Answer: the clump's `OnComponentHit` bound handler IS observable; PRE-observe it to fire a
lag-free `PropConvert` at the exact convert edge.**

Verified chain (§1.2 + CDO): owner's real clump simulates with `bNotifyRigidBodyCollision=True`
→ a flat-static ground contact triggers `UPrimitiveComponent::DispatchBlockingHit` →
`OnComponentHit.Broadcast` → `ProcessMulticastDelegate` → `obj->ProcessEvent(BndEvt__…handler,
params)`. Our PRE observer on that UFunction runs **before** the BP handler body, i.e. before
the ubergraph spawns the pile and `K2_DestroyActor`s the clump. At PRE we have the live clump
(eid, current `GetActorLocation`, velocity, `chipType`) and the delegate's `Hit` param (the
resting location). We re-evaluate the BP's OWN convert gate (byte-exact, from the convert RE):

```
delayOnHit == false             (the 1st ground hit only arms; 2nd+ converts)
&& canConvert == true           (always true)
&& (holdPlayer invalid OR holdPlayer.grabbing_actor invalid)   (not still held)
&& Dot(Hit.Normal, Up) > 0.75   (flat-ish surface)
&& !IsSimulatingPhysics(Hit.Component)   (landed on static world, not a dynamic body)
```

If the gate passes, THIS call is the one that converts → broadcast `PropConvert` now. (PRE not
POST: by POST the clump actor + transform are torn down by its own `K2_DestroyActor`.)

The clump's fields are reflectable (`delayOnHit`, `canConvert`, `holdPlayer`, `chipType`); the
`Hit` is the observer's `params` frame (`FHitResult` at the param offset — resolve via
`FindParamOffset(fn, L"Hit")`). This is the same "read the real gate, don't guess" discipline
the door `CanOpen` uses.

**If we wanted to avoid re-implementing the gate:** a POST observer on the hit handler is ALSO
observable, but by POST the clump may be dead — we'd lose the eid/transform. So PRE + the gate
re-check is correct. The gate is small and fully RE'd; acceptable.

**Fallback if the PRE gate ever proves flaky:** the death-watch (§5.1) still detects the
convert (clump actor dies) — it just incurs the physical-settle latency (~1-3 s, the symptom-2
lag). The PRE observer is what removes that latency; keep the death-watch as the backstop.

---

## 5. Sub-question 2 — GRAB-DESTROY PROPAGATION (symptoms 1+3)

Goal: when peer A grabs pile X (eid) that peer B mirrors, B must drop its mirror X.
`actorChipPile_C::playerGrabbed` is unobservable (§3), so we CANNOT broadcast at the grab edge
directly. Two candidate observable mechanisms; one is robust.

### 5.1 The mirror-pile death-watch (ROBUST — recommended)

**A mirror pile, when grabbed locally, dies locally** — this is the key enabling fact. When B
(who is mirroring pile X) walks up and grabs X, B's `mainPlayer` runs the real pile interaction
(`useArm` SphereTrace hits the pile — the landed pile keeps BP-default QueryAndPhysics, proven
in `votv-clump-mirror-grab-RE`), which calls `chipPile.playerGrabbed → turnToPile`, which
`K2_DestroyActor`s the pile actor. So **the mirror pile's actor goes dead on B's machine the
moment B grabs it.** A liveness death-watch over mirror piles catches that:

- Register every pile this peer MIRRORS (the `kFreshLanded`/`PropConvert`-spawned piles, which
  carry a wire eid via `RegisterPropMirror`) into a `WatchedPile{actor, internalIdx, eid}`
  set, exactly like the existing clump death-watch (`trash_collect_sync`).
- Each tick, `IsLiveByIndex(actor, idx)`; when it goes dead → broadcast `PropDestroy{key=None,
  eid}` so the OTHER peers drop their mirror, then drop the watch entry.
- This is symmetric and identity-based: whoever grabs the shared pile, the grabber's machine
  detects the death and tells everyone else. It REPLACES the deleted position-consume with an
  exact-identity destroy (the one thing the consume was actually doing — §6 of the prior doc).

**Stream-out false-destroy mitigation (critical — the grime/clump precedent):** a level
sublevel STREAM-OUT also makes `IsLiveByIndex` go false, which would emit a spurious
`PropDestroy`. Guard exactly as the grime/clump death-watch already does:
1. **Connect-teleport / world-transition gate:** suppress the watch (or treat deaths as
   stream-out, not destroy) during the post-connect teleport + any active level transition
   window (the same `g_fleeing`/transition latches net_pump already exposes). The dirt
   death-watch learned this when the connect-teleport sublevel stream-out flooded false
   destroys.
2. **Proximity gate (the grime super-sponge precedent):** a grab-destroy happens at a pile the
   local player is interacting with → it is NEAR the local camera. A stream-out happens to far
   piles. Gate the destroy on "pile was within ~Ncm of the local player/camera when it died"
   (grime used 800 cm for the wipe). Far death → stream-out → ignore (the pile will re-stream
   and re-register). This cleanly separates "grabbed it" from "it streamed out."
3. Both gates compose: a destroy is broadcast only for a NEAR death OUTSIDE a transition
   window. Identical shape to the shipped grime wipe death-watch (which smoke-proved 0
   false-fire on the connect stream-out).

**Echo safety:** a wire-driven destroy on B applies via `ConsumeLocalActor` (K2_DestroyActor
with the incoming-destroy echo latch set), not via a grab; the death-watch must skip entries
torn down by `ConsumeLocalActor` (mark them consumed so their death isn't re-broadcast) — same
pattern the prop echo-suppress set already provides.

### 5.2 Held-edge backstop (cheap, complementary)

Independent of the death-watch, when net_pump's held-prop poll sees a NEW held clump
(`EnsureHeldItemBroadcast`), check whether that grab originated from a pile WE were mirroring:
the just-spawned held clump is the morph product of a source pile. If, the tick before the
new-held edge, the player's `lookAtActor` (or the nearest mirror pile within a small radius of
the player) was a mirror pile with an eid, emit `PropDestroy(thatEid)`. This catches the grab
slightly earlier than the death-watch and covers the (rare) case where the morph reuses/relocates
fast. It is a backstop, not the primary — the death-watch (§5.1) is the robust primary because
it keys off the actual local death, not a heuristic.

> Why NOT hook the grab input (`InpActEvt_use`) + read `lookAtActor` (the task's option (a)):
> it IS observable, but it fires on EVERY E-press (door, item, nothing) and the pile-vs-not and
> press-vs-release disambiguation is fiddly (the door code needed a 300 ms debounce for exactly
> this). It would also fire on the GRABBER, who must then map lookAtActor→eid anyway. The
> death-watch needs none of that: the local death is an unambiguous, already-happened fact. Use
> the input edge only as the §5.2 backstop refinement if the death-watch's latency (one tick)
> ever matters. Robust primary = death-watch.

### 5.3 Where the eid comes from (identity flow)

- A pile that exists because WE converted/landed it locally as the OWNER does NOT need a
  grab-destroy broadcast from us for our own grab — we ARE the one grabbing; the OTHER peer is
  mirroring it, and THAT peer's death-watch fires when ITS copy dies. Wait — no: the other peer
  only grabs if THEY walk up to it. The case is symmetric: **whoever grabs, their machine's
  death-watch fires.** So a pile needs a cross-peer eid on BOTH peers. The OWNER's pile carries
  a LOCAL Prop Element id (minted at convert/land); the MIRROR carries the wire eid
  (`RegisterPropMirror`). The death-watch resolves the eid via
  `GetPropElementIdForActor` (local) → fallback `ResolveMirrorEidByActor` (mirror) — exactly the
  resolution the deleted `playerGrabbed` observer already coded; reuse it verbatim.
- **The eid MUST be the SAME value on both peers.** This is satisfied by `PropConvert`/landed-pile
  carrying the authoritative `newEid` minted by the OWNER and bound on the receiver via
  `RegisterPropMirror`. (The old separate-eid landed path already did this for the mirror;
  PropConvert keeps it.)

---

## 6. Sub-question 3 — the residual DUPE (symptom 1), enumerated

With the position-consume deleted (`remote_prop_spawn.cpp:432-457`, done), the remaining ways
ONE clump can become TWO on the peer:

| Path | Mechanism | Fixed by |
|---|---|---|
| **(P1) Un-removed grabbed-pile mirror** (DOMINANT) | A grabs pile X; B was mirroring X. Nothing tells B to remove its X. B's player later grabs X → another clump. Net: piles accumulate. The deleted consume was (wrongly, by co-location) the only thing that removed it. | §5.1 mirror-pile death-watch → `PropDestroy(eid)` when A's grab kills A's copy of X **and** B's death-watch when B's copy dies. Identity-exact. |
| **(P2) Mirror ball self-converts on landing** | The thrown mirror ball runs its OWN hit handler → spawns a 2nd pile atop the owner's authoritative one. | ALREADY FIXED + KEEP: `SetActorRootNotifyRigidBodyCollision(false)` at clump-mirror spawn (`remote_prop_spawn.cpp:445`) silences the mirror's hit handler; `OnRelease` uses PhysicsOnly so it lands but can't be grabbed/queried (`remote_prop.cpp:519`). |
| **(P3) Convert + landed-pile double-spawn** | Old flow: death-watch's `BroadcastLandedPileNear` spawns a pile AND a separate clump mirror lingers; with PropConvert both are folded into one atomic swap. With BOTH the old landed-pile path AND PropConvert active you'd get two piles. | When PropConvert lands (§4), **DELETE** `BroadcastLandedPileNear` + the death-watch's pile-spawn (`trash_collect_sync.cpp:70-117,216`). Death-watch keeps ONLY the `PropDestroy` backstop. RULE 2: no two convert paths. |
| **(P4) eid reuse / landed-pile eid collision** | The prior smoke saw two landed piles broadcast with the same eid (rapid grab loop) → one mirror leaks. | PropConvert mints `newEid` from the authoritative owner allocator per convert; never reuses the dying clump's eid for the new pile. The receiver destroys `oldEid` and spawns `newEid` — distinct ids, no collision. |
| **(P5) Cross-peer non-co-location replay** | (Not a dupe path anymore.) chipPiles aren't co-located; the old consume guessed a co-located pile and destroyed the wrong one while the landed pile spawned anyway = multiplication. | Deleted with the consume. PropConvert/identity model never searches by position. |

**The dupe the user still sees = P1** (the re-grabbed pile that never disappears) plus the
P3-style accumulation when a pile is re-grabbed/re-thrown repeatedly. Both are closed by the
death-watch-on-mirror-piles (§5.1) + folding convert into the atomic PropConvert (§4) + deleting
the landed-pile/position paths.

---

## 7. The complete robust design (observable edges only)

### 7.1 Protocol (one new reliable kind)

`include/coop/net/protocol.h` — add to `ReliableKind` (the enum currently maxes at
`SkyState = 34`; id **32 is unused** between `GrimeState=31` and `GarageDoorState=33`, so use
`PropConvert = 35` for clarity, or reclaim `32`) + bump `kProtocolVersion` (currently 44 → 45):

```cpp
struct PropConvertPayload {           // ~56 B; one reliable datagram
    uint32_t oldEid;                  // the mirror BALL to destroy (the clump's broadcast eid)
    uint32_t newEid;                  // authoritative id for the new pile (owner allocator)
    WireClassName pileClass;          // read off self.pile / ClassNameOf(spawned pile)
    float locX, locY, locZ;           // resting transform (Hit.Location / clump GetActorLocation)
    float rotPitch, rotYaw, rotRoll;
    uint8_t  chipType;                // variant (clump.chipType)
    uint8_t  _pad[3];
    float velX, velY, velZ;           // landing velocity -> turnToPile impact dust+sound
};
```

Route in `event_feed` to `remote_prop::OnConvert`. (Alternative: reuse `PropSpawnPayload` +
`kFreshLanded` and pass `oldEid` in a spare field; a dedicated kind is cleaner and self-documenting.
Either is fine — both are one reliable datagram on the existing ordered lane.)

### 7.2 OWNER detection — PRE observer on the clump hit handler (instant convert)

New `coop::clump_convert` (its own `.cpp/.h`, per the modular rule — do NOT grow
`trash_collect_sync.cpp` which is already a dense single-feature file):

- Register a **PRE** observer on
  `prop_garbageClump_C::BndEvt__prop_garbageClump_StaticMesh_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature`
  (resolve via `FindClass("prop_garbageClump_C")` → `FindFunction(cls, L"BndEvt__…")`; the
  exact name is the single export in `prop_garbageClump.functions.txt`). Also register the
  subclass handlers if present (`prop_garbageClump_erie/leaves/wetConcrete_C`,
  `prop_dirtball_C`) — same late-load catch pattern as the Init observers.
- In the callback (game-thread; ProcessEvent observers may fire off-thread → `GT::Post` the
  body if `!IsGameThread()`, re-validate `IsLive`): only act if `self` is a clump WE own (it
  carries a local Prop Element eid / is in the held-broadcast set) — i.e. the OWNER. Read
  `delayOnHit`, `canConvert`, `holdPlayer`(+`grabbing_actor`), `chipType`, and the `Hit` param;
  evaluate the gate (§4). If it passes: mint `newEid`, read the pile class
  (`prop_garbageClump_C::pile` TSubclassOf → class name), broadcast `PropConvert{oldEid =
  this clump's eid, newEid, restingXform, chipType, vel}`. Mark the new eid as the
  authoritative pile id locally so the OWNER's own subsequently-spawned pile is bound to it
  (so a later re-grab on the owner side resolves the eid for §5.1).
- Do NOT suppress the owner's own conversion — let the BP convert the owner's clump normally;
  we only OBSERVE to time the broadcast.

### 7.3 RECEIVER — atomic ball→pile swap

`remote_prop::OnConvert(payload)` (game thread, from event_feed):
1. Destroy the mirror ball by `oldEid`: `ResolveLiveActorByEid(oldEid)` → `ConsumeLocalActor`
   (echo-suppressed) + `UnregisterPropMirror(oldEid)`. (Exactly the `OnDestroy` eid teardown,
   `remote_prop.cpp:722-745`.)
2. Spawn the authoritative pile via the existing `remote_prop_spawn::OnSpawn` fresh path with
   `kFreshLanded` set + `pileClass` + `newEid` + `chipType` + `initLinVel = vel` (so it fires
   `turnToPile(vel)` for the impact dust/sound and binds the mirror by `newEid` via
   `RegisterPropMirror`). Same OnSpawn code the old landed path called — no new spawn logic.
3. Both in one handler/tick → the ball vanishes the same frame the pile appears (no linger, no
   second pile). The pile keeps BP-default QueryAndPhysics → grabbable on both peers.

### 7.4 RE-GRAB propagation — mirror-pile death-watch (§5.1) + held backstop (§5.2)

`coop::clump_convert` (or extend the existing death-watch in `trash_collect_sync`, keeping
files under cap):
- Register every PILE this peer mirrors (spawned by OnConvert/`kFreshLanded` with a wire eid)
  AND every pile this peer OWNS (local eid) into a `WatchedPile{actor, idx, eid, lastPos}` set.
- Per-tick `IsLiveByIndex`; on death, if NOT a `ConsumeLocalActor` teardown AND the pile was
  NEAR the local player/camera at death AND no level transition is active → broadcast
  `PropDestroy{key=None, eid}`; drop the entry. (Stream-out far/transition deaths → silent.)
- Held-edge backstop: in `EnsureHeldItemBroadcast`'s new-held path, if the player's
  `lookAtActor` (or nearest mirror pile within ~Ncm) the prior tick was a tracked pile with an
  eid, emit `PropDestroy(thatEid)` immediately. Belt-and-suspenders for the one-tick latency.

### 7.5 DELETE (RULE 2 — no parallel paths)

- `prop_lifecycle.cpp`: `GrabObserver_ChipPile_PlayerGrabbed_PRE`, `RegisterChipPileGrabObserver`,
  and its call in `RegisterExtraKeyedInitObservers` (unobservable target — §3).
- `trash_collect_sync.cpp`: `BroadcastLandedPileNear` + its `FindNearestChipPile`-200 cm search
  (`:70-117`); the death-watch's pile-spawn responsibility (`:216` `madePile`). The clump
  death-watch keeps ONLY the `PropDestroy(eid)` backstop for a clump that dies WITHOUT
  converting (LifeSpan expiry / despawn) — so a non-converted clump mirror still gets cleaned.
- `ue_wrap::prop::FindNearestChipPile` / `TurnChipPileToPile`-as-landed-spawn are retired as
  the landed-pile mechanism (TurnChipPileToPile stays only as the OnConvert impact-sound call).

### 7.6 KEEP (already correct)

- `SetActorRootNotifyRigidBodyCollision(spawned, false)` on the clump-mirror spawn
  (`remote_prop_spawn.cpp:445`) — mirror never self-converts (P2).
- `OnRelease` PhysicsOnly (`2`) on the clump mirror (`remote_prop.cpp:519`) — lands but
  ungrabbable in the (now near-zero) flight window.
- net_pump's `grabbing_actor`/`holding_actor` poll + `EnsureHeldItemBroadcast` (the
  held-clump mirror + pose stream) — the observable grab-carry substrate.

---

## 8. Files / addresses (implementation map)

- **Hook substrate:** `src/votv-coop/src/ue_wrap/game_thread.cpp` (ProcessEvent detour;
  `RegisterPreObserver`/`RegisterPostObserver`).
- **IDA (dispatch proof):** ProcessEvent `0x141465930`; ProcessInternal `0x141302DC0`;
  ProcessLocalScriptFunction `0x141453550`; CallFunction `0x1414573C0`; execFinalFunction
  `0x141474FB0`; execVirtualFunction `0x1414751A0`; BP-VM interpreters `0x141465CE0` /
  `0x141465DF0`; DispatchBlockingHit `0x1428B8A30`; **ProcessMulticastDelegate `0x1407F5710`
  (`call qword ptr [r9+220h]` = ProcessEvent per binding)**; ProcessEvent-in-vtable sample
  `0x143a1af48` → `0x141465930`.
- **BP disasm:** `research/bp_reflection/{actorChipPile,prop_garbageClump,mainPlayer}.json` +
  `_disasm.py`; opcode walk = the recursive `EX_*VirtualFunction`/`EX_*FinalFunction`/`EX_CallMath`
  scan in this session.
- **Convert gate (byte-exact):** `votv-clump-ball-to-pile-conversion-RE-and-event-fix-2026-06-08.md`
  §2 (`prop_garbageClump` ubergraph @2702 arm → @2874 gate → @27 spawn → @2640 K2_DestroyActor).
- **CDO delegate binding + `bNotifyRigidBodyCollision=True`:** `prop_garbageClump.json`
  (`ComponentDelegateBindings`, StaticMesh `BodyInstance` ObjectType=ECC_PhysicsBody).
- **Failed fix to delete:** `prop_lifecycle.cpp:375-425` (`GrabObserver_ChipPile_PlayerGrabbed_PRE`
  + `RegisterChipPileGrabObserver`), `:550-552` (its call).
- **Reuse points:** `remote_prop.cpp` `ResolveLiveActorByEid:298`, `OnDestroy:722`,
  `ConsumeLocalActor:789`, `ResolveMirrorEidByActor:664`, `UnregisterPropMirror:681`,
  `OnRelease:505-535`; `remote_prop_spawn.cpp` `OnSpawn` fresh path `:315-504`, mirror-inert
  `:445`, `kFreshLanded` turnToPile `:463-468`; `trash_collect_sync.cpp` death-watch shape
  `:37-61,199-225`; `protocol.h` `ReliableKind:450`, `kFreshLanded:1208`, `PropSpawnPayload:1167`,
  `PropDestroyPayload:1218`, `kProtocolVersion:415`.
- **Precedents:** state-poll-not-observe conclusion `interactable_sync.cpp:110-118,657-675`;
  stream-out death-watch + proximity gate `coop/grime_sync` (the grime super-sponge wipe);
  clump death-watch `trash_collect_sync.cpp`.

---

## 9. Verification plan (before handoff)

1. **Confirm the hit handler fires:** register the PRE observer, log on every fire; hands-on
   throw a clump → expect log lines at the moment it lands (PROVES #1 observable end-to-end on
   THIS build — the one thing that must hold for the instant-convert design).
2. **Confirm `playerGrabbed` does NOT fire:** temporarily log in the (about-to-be-deleted)
   observer; grab a pile → expect ZERO lines (confirms §3 on this build; then delete).
3. **Convert latency:** with PropConvert, the peer's pile appears ~1 RTT after the owner's
   clump converts (no ~3 s linger).
4. **Re-grab disappear:** A throws → both see the pile; A re-grabs the pile → B's mirror pile
   vanishes (death-watch `PropDestroy`). B re-grabs → A's vanishes. No accumulation over 10
   grab/throw cycles (closes P1/P3/P4).
5. **No stream-out false-destroy:** connect-teleport + walk through a streaming sublevel →
   ZERO spurious `PropDestroy` for piles that merely streamed out (proximity + transition gate).
