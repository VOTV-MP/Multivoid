# VOTV physics-object interaction — deep reverse-engineering

**Date:** 2026-05-23
**Game build:** Alpha 0.9.0-n (`VotV-Win64-Shipping.exe`, imagebase `0x140000000`)
**Trigger:** RULE 1 + "no infinite hopeless iteration cycles". The SDK-header UFunction guess for the grab observers proved wrong in hands-on test (commit `14d0787` debug build); this doc replaces it with ground truth from IDA decompile + runtime log diagnostic.

## TL;DR — the architecture, finally

```
   E-press
      │
      ▼
 InpActEvt_use_K2Node_InputActionEvent_41   <-- BP-emitted, ProcessEvent-dispatched
      │   (decides pickup vs drop based on mainPlayer.grabbing_actor state +
      │    line-trace against world, then plays one of two Timeline directions)
      ▼
 Timeline `grab` on mainPlayer_C  (UTimelineComponent instance)
      │
      ▼  UTimelineComponent::TickComponent  @ 0x142F1D9A0      (engine native, hot)
      │    └─ FTimeline::TickTimeline      @ 0x142F1DEE0       (advance playhead)
      │        ├─ FTimeline::TickTimelineEvents @ 0x142F1B550  (fire bound UFunctions)
      │        │     └─ OwningActor.ProcessEvent("grab__UpdateFunc", ...) <-- HOOKABLE
      │        │           └─ (BP-interpreted) reads curve, computes target,
      │        │              calls grabHandle.SetTargetLocation(targetLoc)
      │        │                    │
      │        │                    ▼
      │        │            UPhysicsHandleComponent::SetTargetLocation
      │        │              exec thunk @ 0x1430C6AD0  <-- HOOKABLE (universal)
      │        │              native     @ 0x142D7D3F0  writes FVector @ this+240
      │        │
      │        └─ FTimeline::FireFinishedEvent  @ 0x142F10E00  (on Timeline end)
      │              └─ OwningActor.ProcessEvent("grab__FinishedFunc") <-- HOOKABLE
      │                    └─ (BP-interpreted) calls grabHandle.ReleaseComponent()
      │                          │
      │                          ▼
      │                  UPhysicsHandleComponent::ReleaseComponent
      │                    exec thunk @ 0x142FEA9B0  <-- HOOKABLE (universal)
      │                    vtable[130] = PhysX-backend release
      │
      └── separately, every game tick:
          UPhysicsHandleComponent::TickComponent reads TargetLocation @+240
          and applies PhysX constraint forces to drag GrabbedComponent toward it.
```

The unknown wasn't "where is the BP", it was "**which** function does BP dispatch through ProcessEvent". The answer: **not the 8 SDK-header names** (smoothGrab, pickupObject, dropGrabObject, throwHoldingProp, switchToHeavyDrag, pickupObjectDirect, playerTryToGrab, canPickup — all of those are BP-pure inline functions and do **not** ProcessEvent-dispatch). The answer **is** the engine-native UPhysicsHandleComponent UFunctions, which are ProcessEvent-dispatched **and capture every grab/move/release universally** — light grab, heavy drag, doesn't matter.

## The log — empirical truth

Hands-on session 2026-05-23 22:40-22:43 with debug build `14d0787` (8 named observers + 4 name-prefix diagnostics: `pickup`, `grab`, `InpActEvt_use`, `holdObject`):

```
[22:40:00] grab_hook: registered POST observer for 'smoothGrab' @ 0000028715033260
[22:40:00] grab_hook: registered POST observer for 'pickupObject' @ 0000028715033420
[22:40:00] grab_hook: registered POST observer for 'pickupObjectDirect' @ 0000028715032FC0
[22:40:00] grab_hook: registered PRE observer for 'dropGrabObject' @ 0000028715033340
[22:40:00] grab_hook: registered PRE observer for 'throwHoldingProp' @ 000002871503FC80
[22:40:00] grab_hook: registered POST observer for 'switchToHeavyDrag' @ 00000286F29D1200
[22:40:00] grab_hook: registered POST observer for 'playerTryToGrab' @ 00000286F29D0080
[22:40:00] grab_hook: registered POST observer for 'canPickup' @ 00000286F29D0A20

(user picks up + drops + picks up + drops + picks up + drops a prop)

[22:41:07] grab_diag: dispatched UFunc 'InpActEvt_use_K2Node_InputActionEvent_41' self=0000028708079070
[22:41:07] grab_diag: dispatched UFunc 'grab__UpdateFunc' self=0000028708079070
[22:41:08] grab_diag: dispatched UFunc 'grab__UpdateFunc'  (×3)
[22:41:08] grab_diag: dispatched UFunc 'grab__FinishedFunc'
[22:41:11] grab_diag: dispatched UFunc 'grab__UpdateFunc'  (×5 across 22:41:11-12)

(quiet for ~1 minute)

[22:42:13] grab_diag: dispatched UFunc 'grab__UpdateFunc'  (×9 across 22:42:13-15)

(quiet, then second E-press)

[22:43:09] grab_diag: dispatched UFunc 'InpActEvt_use_K2Node_InputActionEvent_41'
[22:43:17] grab_diag: dispatched UFunc 'grab__UpdateFunc'  (×9 across 22:43:17-21)
```

**Statistics:**
- All 8 SDK-header observers: **0 fires** in ~3 minutes of active grabbing.
- `InpActEvt_use_K2Node_InputActionEvent_41`: 2 fires (corresponding to 2 E-presses).
- `grab__UpdateFunc`: 27 fires (Timeline updates during grab animation).
- `grab__FinishedFunc`: 1 fire (Timeline end).
- Zero hits on the `pickup` prefix or `holdObject` prefix — confirming `pickupObject*` and `holdObject*` UFunctions never dispatch.

## Why the SDK-header functions don't dispatch

VOTV's mainPlayer_C BP graph defines functions like `smoothGrab`, `pickupObject`, etc. as **"Pure Functions"** (no exec pin). In Blueprint, pure functions are:
- Inlined at call sites by the BP compiler (`EX_CallMath`, `EX_LocalVirtualFunction` resolved inline)
- Re-evaluated every time any output is read
- **NOT dispatched through `UObject::ProcessEvent`**

UE4SS-style reflection iteration sees them in `UClass::Children` and they have valid UFunction pointers (which is why our `reflection::FindFunction` lookup succeeds), but they're **never the second argument to ProcessEvent**. Our hook is a ProcessEvent detour, so we never see them.

The exec UFunctions that **do** ProcessEvent-dispatch on mainPlayer_C BP:
- BP-event functions (`InpActEvt_*`, `BeginPlay`, `Tick`, anim notifies)
- Timeline auto-functions (`<TimelineName>__UpdateFunc`, `<TimelineName>__FinishedFunc`, `<TimelineName>__<EventTrackName>_Track`)
- Functions with exec pins (impure) bound via Custom Events or BP nodes that emit `EX_FinalFunction`/`EX_VirtualFunction` opcodes

## Native PhysicsHandle — full mapping

### Exec thunks (UFunction.Func pointers — what ProcessEvent's `function->Func` points at)

| Address | Renamed (IDB) | UE4 UFunction name (FName) | What it reads from FFrame |
|---|---|---|---|
| `0x1430C64B0` | `execUPhysicsHandleComponent_GrabComponentAtLocation` | `GrabComponentAtLocation` | UPrimitiveComponent* comp, FName bone, FVector location |
| `0x1430C65D0` | `execUPhysicsHandleComponent_GrabComponentAtLocationWithRotation` | `GrabComponentAtLocationWithRotation` | + FRotator rotation |
| `0x1430C6AD0` | `execUPhysicsHandleComponent_SetTargetLocation` | `SetTargetLocation` | FVector targetLoc |
| `0x1430C6B60` | `execUPhysicsHandleComponent_SetTargetLocationAndRotation` | `SetTargetLocationAndRotation` | FVector targetLoc, FRotator targetRot |
| `0x142FEA9B0` | `execUPhysicsHandleComponent_ReleaseComponent` | `ReleaseComponent` | (no params) |

Each exec thunk reads its params from `FFrame` (the BP interpreter state), then tail-calls the native class method below.

### Class methods (native bodies)

| Address | Name | Behavior |
|---|---|---|
| `0x142D7A580` | `UPhysicsHandleComponent::GrabComponentAtLocation` | Thin forwarder → `vtable[1064/8=133]` PhysX-backend grab |
| `0x142D7A5B0` | `UPhysicsHandleComponent::GrabComponentAtLocationWithRotation` | Same `vtable[133]` (unified backend; no-rot variant pre-fills identity quat in caller) |
| `0x142D7D3F0` | `UPhysicsHandleComponent::SetTargetLocation` | Writes `FVector` at `this+240` (TargetLocation). No PhysX work — read by TickComponent. |
| `0x142D7D420` | `UPhysicsHandleComponent::SetTargetLocationAndRotation` | Writes `FQuat` at `+224`, `FVector` at `+240`, derived transform at `+256` (from default-transform globals `0x14491FC38`/`0x14491FC40`) |
| (vtable[130], via thunk `0x142FEA9B0`) | `UPhysicsHandleComponent::ReleaseComponent` | PhysX constraint release; clears `GrabbedComponent` |

### Field offsets (UPhysicsHandleComponent)

| Offset | Type | Meaning |
|---|---|---|
| `+224` (0xE0) | `FQuat` (16B) | `TargetRotation` |
| `+240` (0xF0) | `FVector` (12B padded to 16) | `TargetLocation` ← **this is the per-tick driver** |
| `+256` (0x100) | `FTransform`-like block | Derived/cached transform |

These are stock UE4 4.27 field offsets; the names are taken from the public engine source. Field offset for `GrabbedComponent` (UPrimitiveComponent*) is earlier in the struct (~+0x60); not yet pinned in IDB.

### Timeline pipeline (engine native)

| Address | Renamed (IDB) | Behavior |
|---|---|---|
| `0x142F1D9A0` | `UTimelineComponent_TickComponent` | Per-frame entry; stat-scopes, parents AActorComponent::TickComponent, calls FTimeline::TickTimeline |
| `0x142F1DEE0` | `FTimeline_TickTimeline` | Advances `Position` by `Rate*dt`; handles wrap (loop) and clamp (non-loop with FireFinishedEvent) |
| `0x142F1B550` | `FTimeline_TickTimelineEvents` | Fires bound update + event UFunctions via `OwningActor.ProcessEvent(fn, params)` |
| `0x142F10E00` | `FTimeline_FireFinishedEvent` | Fires bound finished UFunction once at end |

Timeline state struct (at `UTimelineComponent + 176`):
- `+0`: flags byte (`bit 0` = looping, `bit 1` = reversed, `bit 2` = playing, `bit 4` = idk)
- `+8`: `Rate` (float, play speed)
- `+12`: `Position` (float, playhead 0..Length)
- `+96`: FinishedFunc binding region (the FName-resolved UFunction* that `FireFinishedEvent` calls)
- `+128`: `OwningActor` pointer (the actor on which to ProcessEvent)
- `+136`: count of bound event funcs

## What this means for the coop mod

### The right hook surface

Hook the **engine-native** UPhysicsHandleComponent UFunctions, not VOTV's BP-pure helpers:

| UFunction | When fires | What we learn |
|---|---|---|
| `PhysicsHandleComponent.GrabComponentAtLocation` | Light-grab starts (Timeline's UpdateFunc first calls it OR a one-shot setup) | which UPrimitiveComponent was grabbed + initial location |
| `PhysicsHandleComponent.GrabComponentAtLocationWithRotation` | Same as above but if BP supplies a rotation | + rotation |
| `PhysicsHandleComponent.SetTargetLocation` | **Every tick** during light grab | per-tick target FVector — the held-prop drive signal |
| `PhysicsHandleComponent.SetTargetLocationAndRotation` | Every tick if heavy drag uses the with-rot variant | + rotation |
| `PhysicsHandleComponent.ReleaseComponent` | Grab ends (drop or throw) | nothing useful in params, but signals end |

This is **universal** — it captures both light grab AND heavy drag (they both go through the same PhysicsHandle component). It avoids guessing which VOTV-specific BP function will be the entry point.

### What we still need (next research)

1. **GrabbedComponent offset** in UPhysicsHandleComponent — for reading the held UPrimitiveComponent* in the SetTargetLocation observer (alternative: read from `mainPlayer_C.grabbing_actor` @0x07D0).
2. **Which UPhysicsHandleComponent instance is `grabHandle`** on mainPlayer_C — read from `mainPlayer_C.grabHandle` (component property). Offset TBD; find via reflection at runtime.
3. **Heavy drag path** — does it also go through PhysicsHandle, or does it use AttachToActor + custom drag math? The log showed `grab__UpdateFunc` regardless (Timeline is shared), but no `SetTargetLocation` log entry since the diagnostic was only firing on functions starting with `pickup`/`grab`/`InpActEvt_use`/`holdObject` — none catch `SetTargetLocation`. **Next probe: add a `SetTargetLocation` prefix to the diagnostic to confirm heavy drag also uses PhysicsHandle.**
4. **Throw path** — `throwHoldingProp` is BP-pure, so it doesn't dispatch. The throw likely (a) reads camera forward, (b) calls `ReleaseComponent`, (c) calls `AddImpulse` on the released UPrimitiveComponent. We'd capture step (b); step (c) is `UPrimitiveComponent::AddImpulse` exec thunk (also ProcessEvent-dispatched). **TBD probe.**

### Receiver-side strategy (unchanged)

Same as before, MTA-shape:
1. `SetSimulatePhysics(false)` on the prop on remotes (prop now kinematic).
2. Stream prop world transform from grabber → other peers via UNRELIABLE_SEQUENCED piggyback packet.
3. `SetActorLocation`/`SetActorRotation` on remotes each packet.
4. `SetSimulatePhysics(true)` on release; if `throwHoldingProp` sent a velocity, `AddImpulse`.

The hook surface change doesn't affect the wire protocol or receiver — only the trigger point on the host. The host **reads** the grabbed prop from `mainPlayer_C.grabbing_actor` after the first `SetTargetLocation`/`GrabComponentAtLocation` observer fires, and **drives** the prop sync packet from there.

## Code changes triggered by this finding

### Drop (RULE 2 — no migration baggage)

The 8 SDK-header observers in `harness.cpp` and the 6 UFunction-name constants in `sdk_profile.h` (`smoothGrabFn`, `pickupObjectFn`, `pickupObjectDirectFn`, `dropGrabObjectFn`, `throwHoldingPropFn`, `switchToHeavyDragFn`) — all proven non-dispatching, all dead code. Delete.

`playerTryToGrab` and `canPickup` — also BP-pure (none dispatched). Delete.

### Add

In `sdk_profile.h`:
```cpp
constexpr const wchar_t* PhysicsHandleClass = L"PhysicsHandleComponent";
constexpr const wchar_t* GrabComponentAtLocationFn = L"GrabComponentAtLocation";
constexpr const wchar_t* GrabComponentAtLocationWithRotationFn = L"GrabComponentAtLocationWithRotation";
constexpr const wchar_t* SetTargetLocationFn = L"SetTargetLocation";
constexpr const wchar_t* SetTargetLocationAndRotationFn = L"SetTargetLocationAndRotation";
constexpr const wchar_t* ReleaseComponentFn = L"ReleaseComponent";

// Timeline-driven grab on mainPlayer_C BP
constexpr const wchar_t* MainPlayerGrabUpdateFn = L"grab__UpdateFunc";
constexpr const wchar_t* MainPlayerGrabFinishedFn = L"grab__FinishedFunc";

// Input event for E (use)
constexpr const wchar_t* MainPlayerUseInputEventFn = L"InpActEvt_use_K2Node_InputActionEvent_41";
```

In `harness.cpp`, register observers in this order of preference:

**Primary (universal, captures any physics-handle grab):**
1. `PhysicsHandleComponent.GrabComponentAtLocation` (post) — pickup
2. `PhysicsHandleComponent.GrabComponentAtLocationWithRotation` (post) — pickup w/ rot
3. `PhysicsHandleComponent.SetTargetLocation` (post) — per-tick target update
4. `PhysicsHandleComponent.SetTargetLocationAndRotation` (post) — per-tick w/ rot
5. `PhysicsHandleComponent.ReleaseComponent` (pre) — release (read state before clear)

**Secondary (BP-Timeline level — informational / triangulates with primary):**
6. `mainPlayer_C.grab__UpdateFunc` (post) — confirms Timeline tick
7. `mainPlayer_C.grab__FinishedFunc` (pre) — confirms Timeline end
8. `mainPlayer_C.InpActEvt_use_K2Node_InputActionEvent_41` (post) — confirms E press

### Retire the diagnostic mode (RULE 2)

The name-prefix diagnostic served its purpose. Strip after the new observers are confirmed firing in hands-on retest.

## Stage 1 next iteration

1. Drop the 8 SDK-header observers + delete the 6 UFunction-name constants (RULE 2).
2. Add the 5 PhysicsHandle observers + 3 mainPlayer_C observers (primary + secondary).
3. Keep diagnostic temporarily, add `SetTargetLocation` prefix to detect heavy-drag path coverage.
4. Build, deploy, hands-on retest: pick up small prop (light grab), drag heavy desk (heavy drag), throw small prop. Confirm:
   - `SetTargetLocation` fires per tick during light grab → host captures held prop
   - `GrabComponentAtLocation` fires once at pickup
   - `ReleaseComponent` fires once at release
   - Whether heavy drag also goes through `SetTargetLocation` or a different path
5. Once primary observers confirmed firing, retire diagnostic + Timeline secondary observers (or keep one as fallback diagnostic).

## IDB renames applied this session

```
0x142F1D9A0  sub_142F1D9A0  →  UTimelineComponent_TickComponent
0x142F1DEE0  sub_142F1DEE0  →  FTimeline_TickTimeline
0x142F1B550  sub_142F1B550  →  FTimeline_TickTimelineEvents
0x142F10E00  sub_142F10E00  →  FTimeline_FireFinishedEvent
```

Comments added to the 5 PhysicsHandle exec thunks + 2 class methods + 2 Timeline functions, with vtable slots, field offsets, and behavior summaries.

IDB saved: `D:\Projects\Programming\VOTV_MP\Game_0.9.0n\WindowsNoEditor\VotV\Binaries\Win64\VotV-Win64-Shipping.exe.i64`

## Cross-refs

- Previous (now-obsolete) plan: `research/findings/physics-object-pickup-coop-plan-2026-05-23.md` — the 8-UFunction observer plan, superseded by this doc.
- Architect blueprint: `research/findings/physics-object-pickup-architecture-2026-05-23.md` — wire protocol + 7-stage build sequence remain valid; only the observer-registration step (Stage 1) changes.
- MTA precedent: `research/findings/mta-object-pickup-sync-2026-05-23.md` — unchanged.
- Log: `Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/votv-coop.log` (session 22:39:56–22:43:21).
