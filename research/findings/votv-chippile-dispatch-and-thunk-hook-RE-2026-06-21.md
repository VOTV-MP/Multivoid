# VOTV chipPile/garbageClump dispatch model + the UFunction::Func thunk-hook design (durable RE) — 2026-06-21

**Tag: durable-RE** (bytecode/dispatch facts true until the GAME updates) + a DESIGN section (the thunk
hook, IDA-gated, NOT built). Companion to `docs/COOP_DISPATCH_VISIBILITY.md` +
`docs/COOP_ENTITY_EXPRESSION_MAP.md` + `docs/piles/08-HOST-AUTH-TRASH-CHANNEL.md`.

This doc closes the question the s35 "08" redesign got WRONG and a live hands-on (2026-06-21) settled:
**is the chipPile/clump `BeginDeferredActorSpawnFromClass` spawn observable to our ProcessEvent hook?**
**No.** It is dispatched `EX_CallMath` (a native thunk via `UFunction::Func`), one layer BELOW our sole
hook seam (`UObject::ProcessEvent`). The 2026-06-08 pass-2 RE already had this right
(`docs/piles/findings/votv-clump-lifecycle-observability-and-robust-design-2026-06-08-pass2.md` §1.1/§2
row 4: "EX_CallMath ... Observable: NO"); s35/08 contradicted our own earlier correct RE, registered a
POST observer on BeginDeferred, and it NEVER FIRED (live log 2026-06-21: 0 fires across 870 piles + every
re-pile). Committed in `0e56ca39`.

RULE 1: every "observable / not" verdict is from the binary's BP-VM dispatch path, not "has no internal
callers". Re-verify offsets against the current `CXXHeaderDump` (WP18 — memory decays, the dump/binary is
authority).

---

## 0. TL;DR

1. **`UObject::ProcessEvent` is only the OUTER entry** (engine/native/delegate → BP). Our MinHook detour
   sits there and ONLY there.
2. The BP-VM bytecode opcodes that issue calls — **`EX_CallMath`, `EX_FinalFunction`, `EX_VirtualFunction`,
   `EX_LocalFinalFunction`, `EX_LocalVirtualFunction`** — route via
   `UObject::CallFunction → UFunction::Invoke → (Context->*UFunction::Func)(...)`, i.e. for script funcs
   `ProcessInternal`, for native funcs the exec thunk — **ONE LAYER BELOW our ProcessEvent detour.** They
   never re-enter ProcessEvent. So our hook sees only OUTER entries: input action events, anim notifies,
   timer/latent resumes, multicast-delegate `Broadcast()`, engine lifecycle (BeginPlay/Tick), and our own
   `reflection::CallFunction`.
3. **`actorChipPile_C::playerGrabbed` and EVERYTHING it calls are INNER sub-calls → invisible.** The grab
   spawn (`BeginDeferredActorSpawnFromClass(clump)`), `FinishSpawningActor`, `pickupObjectDirect`, the
   pile's own `K2_DestroyActor(self)` — all dispatched from inside the `mainPlayer`/`actorChipPile`/
   `prop_garbageClump` ubergraphs by `EX_CallMath` / `EX_LocalVirtualFunction`. The re-pile spawn
   (clump→pile) is likewise the clump ubergraph's `EX_CallMath BeginDeferred`.
4. **The pinecone/ambient caller's BeginDeferred IS visible.** Same UFunction, different CALLER opcode:
   `garbagePileSpawner` and the ambient-prop spawners dispatch `BeginDeferredActorSpawnFromClass` such that
   it reaches ProcessEvent (host_spawn_watcher's POST fires on them in shipped code). **Visibility is a
   property of the CALLER's opcode, not of the callee UFunction.** [V — host_spawn_watcher fires for
   pinecone/spawner; 0 fires for chipPile/clump, live log 2026-06-21]
5. **`WorldContextObject` is `EX_Self`** (the source actor) for BOTH transitions — bytecode-confirmed:
   `actorChipPile_C::playerGrabbed`'s clump-spawn passes `self`(=the dying pile); `prop_garbageClump_C`'s
   ubergraph pile-spawn passes `self`(=the dying clump). So a hook that reads `WorldContextObject` at the
   spawn gets the SOURCE entity for free — the deterministic, zero-proximity link. The ONLY thing missing
   is a seam that fires on an `EX_CallMath` call: that requires patching `UFunction::Func` (the thunk),
   NOT a ProcessEvent observer.

---

## 1. The dispatch model (durable — the thing s35 got wrong)

Our hook: a MinHook detour on `UObject::ProcessEvent` (AOB-resolved; IDA `0x141465930`), installed in
`ue_wrap/game_thread.cpp`. The detour fires PRE observers → original → POST observers, each matched by
UFunction pointer.

**ProcessEvent is the OUTER door, not the dispatcher.** It is "set up the param frame + call
`ProcessInternal`". The BP-VM never calls back out through it for an inner call. Concretely (IDA-proven in
the 2026-06-08 pass-2 RE §1.1, addresses below):

```
ENGINE / NATIVE / DELEGATE  ──►  UObject::ProcessEvent (0x141465930)   ◄── OUR DETOUR (the only seam)
                                        │  sets FFrame, copies params
                                        ▼
                                 ProcessInternal (0x141302DC0)
                                        │  runs the bytecode interpreter (0x141465DF0 / ...CE0)
                                        ▼
   ┌─────────────  the BP-VM executes opcodes  ─────────────┐
   │  EX_CallMath / EX_FinalFunction / EX_VirtualFunction / │
   │  EX_LocalFinalFunction / EX_LocalVirtualFunction       │
   │      ──►  UObject::CallFunction (0x1414573C0)           │   ← one layer BELOW ProcessEvent
   │      ──►  UFunction::Invoke / execFinalFunction (0x141474FB0) /
   │              execVirtualFunction (0x1414751A0)          │
   │      ──►  (Context->*UFunction::Func)(Context, Stack, Result)
   │              • script func  → ProcessInternal           │   ← never re-enters ProcessEvent
   │              • native func  → the C++ exec thunk        │   ← the EX_CallMath / native path
   └────────────────────────────────────────────────────────┘
```

`UFunction::Func` is the per-UFunction function pointer at `UFunction + 0xD8` (the field
`ProcessInternal` installs the frame for, and the field the native exec thunk lives in for a native
UFunction). **Every inner BP call dereferences `Func` directly — our ProcessEvent detour is bypassed.**

**Multicast-delegate `Broadcast()` is the exception that re-enters ProcessEvent**:
`UPrimitiveComponent::DispatchBlockingHit → OnComponentHit.Broadcast → ProcessMulticastDelegate
(0x1407F5710)` calls, per live binding, `obj->ProcessEvent(BoundFunc, params)` (vtable +0x220 == ProcessEvent).
This is WHY input-action events (`InpActEvt_use`) and `BndEvt__…ComponentHit` handlers ARE visible — they
are delegate broadcasts, an OUTER entry. [V/RD: pass-2 §1.2, IDA]

### 1.1 What this means per chipPile/clump UFunction

| UFunction | Caller → opcode | Outer or inner? | Visible to our PE hook? |
|---|---|---|---|
| `AmainPlayer_C::InpActEvt_use_…41/42` (the E-press) | engine input → delegate Broadcast → PE | OUTER | **YES** (this is the seam we use) |
| `actorChipPile_C::playerGrabbed` | `mainPlayer` ubergraph `EX_LocalVirtualFunction` (2×) | inner | **NO** |
| `BeginDeferredActorSpawnFromClass`(clump) inside `playerGrabbed` | `EX_CallMath` (native thunk) | inner | **NO** ← s35's false "POST is observable" |
| `FinishSpawningActor`(clump) | `EX_CallMath` | inner | **NO** |
| `pickupObjectDirect(clump)` | `mainPlayer` ubergraph internal | inner | **NO** |
| `actorChipPile_C::K2_DestroyActor(self)` (grab-destroy) | `EX_VirtualFunction` BP self-call | inner | **NO** |
| `prop_garbageClump_C` `BndEvt__…ComponentHit` (the land trigger) | physics → delegate Broadcast → PE | OUTER | **YES** (candidate — the fallback trigger §3.3) |
| `BeginDeferredActorSpawnFromClass`(pile) inside the clump ubergraph (re-pile) | `EX_CallMath` | inner | **NO** |
| `prop_garbageClump_C::K2_DestroyActor(self)` (post-re-pile) | `EX_VirtualFunction` BP self-call | inner | **NO** |
| `BeginDeferredActorSpawnFromClass` from `garbagePileSpawner` / ambient (pinecone/stick) | engine/spawner path → PE | OUTER | **YES** (host_spawn_watcher fires) |

The same `BeginDeferredActorSpawnFromClass` UFunction appears in BOTH a VISIBLE row (pinecone) and an
INVISIBLE row (chipPile grab). **That is the whole correction.** A POST observer keyed on the UFunction
pointer fires for the pinecone caller and is silent for the chipPile caller, because the chipPile caller
issued it as `EX_CallMath` — below the hook.

### 1.2 WorldContextObject == EX_Self (bytecode-confirmed, both transitions)

"Spawn Actor from Class" compiles `WorldContextObject` as an explicit pin. In both chipPile/clump spawn
sites the bytecode supplies `EX_Self` for it:
- **Grab (pile→clump):** `actorChipPile_C::playerGrabbed`'s `BeginDeferred(clump)` — WorldContextObject =
  `self` = the dying CHIPPILE. [RD: bp_reflection opcode walk]
- **Re-pile (clump→pile):** `prop_garbageClump_C` ubergraph's `BeginDeferred(pile, restingXform)` —
  WorldContextObject = `self` = the dying CLUMP. [RD: pass-2 §2 row 4 + ubergraph walk]

So reading `WorldContextObject` at the spawn yields the SOURCE entity directly — no proximity, no
position search. The new pile/clump is the `ReturnValue`. The two together are the exact `(source, product)`
pair the host-auth channel needs. The blocker was never the data; it was the SEAM.

---

## 2. AS-BUILT today (committed `0e56ca39`, deployed `872ca8ee`, proto v82): visible-seam sync

Because the spawn is invisible, the shipped sync drives off the VISIBLE seams only:

**GRAB (pile→clump) — VERIFIED hands-on** (`[SYNC-MIRROR OK]` in the client log):
1. `trash_collect_sync::OnPileGrabPre` — a PRE observer on `InpActEvt_use` (a REAL input event → PE
   → VISIBLE, which is why it works). On a host E-press aimed at a chipPile (the pile is still alive at
   PRE), it records that pile's eid via `trash_channel::NotePendingGrab(eid, chipType)`.
2. `local_streams`' new-held edge (the existing `grabbing_actor`/held-prop poll) sees the spawned clump
   enter the hand and calls `trash_channel::AdoptPendingGrabClump` → `trash_channel::OnHostConvert(E,
   kToClump, clump)`: bump ctx, `RegisterPropMirror(E, clump, rebindInPlace)` (re-skin E onto the clump
   locally), broadcast `PropConvert{oldEid=newEid=E, kToClump, ctx}`. Clients re-skin their ONE mirror of E.
   Identity is the host eid end-to-end; NO proximity.

**RE-PILE (clump→pile) — AS-BUILT, hands-on NO-DUPE** (user confirmed no dupes; one known minor glitch):
1. At adopt, `trash_collect_sync::WatchClumpForRepile(E, clump)` enrolls the adopted clump in a host-tick
   death-watch (`g_watchedClumps`; `lastPos` follows it through carry + flight).
2. `trash_collect_sync::Tick` (host net-pump): the tick the clump dies (`!IsLiveByIndex`) — the BP just
   re-piled it (spawned a fresh chipPile at the clump's sphere-traced resting point, then destroyed the
   clump, both EX_CallMath/BP-internal → invisible) — `FindNearestUntrackedChipPile_(lastPos, 250cm)`
   finds the fresh pile and `trash_channel::OnHostConvert(E, kToPile, newPile)` converts E onto it in
   place → the client re-skins its single mirror (no destroy+spawn dupe).
3. **Gated to UNTRACKED piles only** (`GetPropElementIdForActor(o) != kInvalidId ⇒ skip`): a pre-existing
   tracked neighbour is excluded, so a dense cluster cannot mis-bind — this is NOT the s35 proximity
   mis-bind (which matched ANY nearby pile incl. tracked neighbours).
4. The rebind re-points E onto the live new pile, so the reaper sees E alive and won't PropDestroy it.
   If NO fresh pile is near the resting spot → the clump was consumed → `PropDestroy(eid)` now.

**KNOWN minor [?]:** a ~5 s vanish-return on SOME re-piles (the host reaper racing the convert rebind:
the clump dies, the reaper's death-watch can fire a PropDestroy before — or alongside — the convert's
rebind lands the new pile under E). Cosmetic, self-corrects. To be removed by the deterministic thunk
hook (§3), which converts the SAME tick the new pile is constructed (no death-watch window).

**`ctx` (MTA sync-time-context, proto v82) — KEPT, holds:** `trash_channel` bumps a per-eid uint8
generation on every transition; `PropConvert`/`PropPose`/`PropRelease` carry it; receivers drop a stale-ctx
packet (`AdoptInboundConvertCtx` / `IsInboundStreamCtxFresh`, wrap-aware int8 compare, 0 = no-enforcement
sentinel). This is what kills the "unreliable carry pose beats the reliable convert → pre-convert pile-jump
+ double grab-sound" race (the 2026-06-21 glitch): a carry pose for E is dropped until E's ToClump convert
has arrived. Port of `CElement::GenerateSyncTimeContext`/`CanUpdateSync` (MTA `CElement.cpp:1281/1300`).

---

## 3. PLANNED (DESIGN, NOT built — gated on an IDA pass): the deterministic re-pile via a UFunction::Func thunk

The clean, latency-free, proximity-free re-pile (and grab): catch the `EX_CallMath BeginDeferred` itself
by patching the callee's thunk, since a ProcessEvent observer provably can't.

### 3.1 The patch

Patch `UFunction::Func` (the `+0xD8` pointer) of `BeginDeferredActorSpawnFromClass` to our own native
thunk that:
1. Reads `WorldContextObject` (= source clump, `EX_Self`) from the incoming `FFrame.Locals + worldCtxOff`.
2. Forwards to the original `Func` (transparent — we are a forwarder, the spawn proceeds normally).
3. Reads `ReturnValue` (= the new pile) from the thunk's `Result` argument — **NOT** `Locals + returnOff`;
   the deferred-spawn's return is delivered via the exec-thunk `Result`/`RESULT_PARAM`, not the locals
   frame.
4. Filters: `IsGarbageClump(worldCtx) && IsChipPile(actorCls) && GetPropElementIdForActor(worldCtx) !=
   invalid` (only OUR tracked clump re-piling; ignore every other BeginDeferred caller, incl. the pinecone
   path that host_spawn_watcher already owns).
5. On a match → `trash_channel::OnHostConvert(BoundEidOf(worldCtx), kToPile, ReturnValue, restingXform)`.
   **Zero proximity, zero death-watch, same tick the pile is constructed.**

The SAME thunk catches the GRAB direction (worldCtx = a tracked chipPile, actorCls = clump → kToClump),
making the InpActEvt-PRE + held-edge adopt redundant for grab too — a future tightening, not required for
Increment 1.

The patch is **process-lifetime, on the game thread** (BeginDeferred is GT-only, the spawn path runs on
the game thread), a transparent forwarder, **no unpatch** (RULE 2 — it replaces the death-watch convert
wholesale once proven; the death-watch convert + its `FindNearestUntrackedChipPile_` then DELETE).

### 3.2 The IDA gate (must pass on OUR binary before any patch)

Pin two layouts on `VotV-Win64-Shipping.exe` (the install target), from the AOB-resolved anchors:

- **`FFrame::Locals` offset.** Anchor: the `Locals = Frame` (the `Stack.Locals` store) inside the
  AOB-resolved `UObject::ProcessEvent` — ProcessEvent builds the FFrame and stores the locals pointer;
  read that store's displacement. Cross-check it against a known native exec thunk that reads
  `Stack.Locals + offset` for its first param (any `exec*` native that takes a UObject param). Both must
  agree on the `Locals` field offset within FFrame and on how a param is addressed off it.
- **`UFunction::Func` offset.** Anchor: the `mov rax,[rFn + FuncOff]; call rax` inside ProcessEvent /
  `UFunction::Invoke` (`Func` is read then called). The resolved value (the function pointer we are about
  to patch) **must land inside `.text`** (a sanity gate — a bad offset yields a non-code pointer → reject,
  do not patch).

Per the escalation ladder (CLAUDE.md): reflection can't see FFrame/Func internals → this is exactly the
"drop to IDA" case. IDA Pro MCP is available.

### 3.3 The validation plan (READ-ONLY first — never enable convert blind)

1. **READ-ONLY log pass:** install the thunk as a pure forwarder that LOGS `worldCtx` class, `actorCls`,
   and `ReturnValue` class on every BeginDeferred — NO convert, NO state change.
2. Hands-on a grab + re-pile in a cluster. **Enable the convert ONLY once a re-pile prints
   `clump + chipPile`** (worldCtx == a garbageClump, ReturnValue == a chipPile) — i.e. the offsets are
   proven to read the right objects on a real re-pile. If the log shows garbage/null → an offset is wrong
   → fix the IDA pin, do not enable.
3. Only after the read-only log is correct: flip on `OnHostConvert`. Then DELETE the death-watch convert
   (RULE 2).

### 3.4 Fallback (ONLY if `FFrame::Locals` is genuinely unreachable)

If — and only if — `Locals` cannot be pinned at all (genuinely unreachable, **NOT merely an unusual
offset**): use the `prop_garbageClump_C` `BndEvt__…ComponentHit` delegate handler (an OUTER delegate
broadcast → ProcessEvent-VISIBLE; pass-2 §1.2/§4 proved it fires) as the trigger for the SOURCE clump, +
temporal correlation to the freshly-spawned pile. This is strictly worse (it's the morph's
re-implement-the-gate path + a correlation window) and is the fallback of last resort, not a co-equal
option. The death-watch convert (§2, AS-BUILT) is the current interim; the thunk hook (§3.1) is the
target.

---

## 4. Addresses / files (implementation map)

- **Hook substrate:** `src/votv-coop/src/ue_wrap/game_thread.cpp` (ProcessEvent detour, AOB-resolved).
- **As-built grab/re-pile:** `coop/trash_collect_sync.cpp` (`OnPileGrabPre` PRE observer :293;
  `WatchClumpForRepile` :351; `Tick` death-watch convert :364; `FindNearestUntrackedChipPile_` :73),
  `coop/trash_channel.cpp` (`NotePendingGrab` :92, `AdoptPendingGrabClump` :100, `OnHostConvert` :56,
  the ctx map + stale guards), `coop/local_streams.cpp` (the new-held edge adopt :291-328).
- **Host_spawn_watcher** (`coop/host_spawn_watcher.cpp`) owns the AMBIENT/pinecone BeginDeferred POST
  ONLY — the chipPile/clump path was removed from it (:118-122 comment records why: EX_CallMath, invisible).
- **Protocol v82:** `include/coop/net/protocol.h` `kProtocolVersion=82` :697; `PropConvertPayload.ctx`
  :3167; `PropPoseSnapshot.ctx` :1894; `PropReleasePayload.ctx` :2155.
- **IDA dispatch addresses (from pass-2):** ProcessEvent `0x141465930`; ProcessInternal `0x141302DC0`;
  ProcessLocalScriptFunction `0x141453550`; CallFunction `0x1414573C0`; execFinalFunction `0x141474FB0`;
  execVirtualFunction `0x1414751A0`; BP-VM interpreters `0x141465DF0`/`0x141465CE0`; ProcessMulticastDelegate
  `0x1407F5710` (`call qword [r9+220h]` = ProcessEvent per binding). `UFunction::Func` @ `UFunction+0xD8`.
- **BP disasm:** `research/bp_reflection/{actorChipPile,prop_garbageClump,mainPlayer}.json` + `_disasm.py`
  (the opcode walk: chipPile grab spawn + clump re-pile spawn both `EX_CallMath`; WorldContextObject = `EX_Self`).
- **The earlier-correct RE (the one s35 contradicted):**
  `docs/piles/findings/votv-clump-lifecycle-observability-and-robust-design-2026-06-08-pass2.md` §1.1, §2
  row 4. **It was right; 08's "observability reversal" was the regression.**
- **Commit:** `0e56ca39` ("Re-pile via visible seams: InpActEvt grab (VALIDATED) + death-watch convert
  (hands-on no-dupe); the BeginDeferred-POST linchpin DISPROVEN"). Live log evidence: 0 host_spawn_watcher
  fires on the chipPile/clump convert across 870 piles + every re-pile, 2026-06-21.

---

## 5. The durable lesson

**Visibility is a property of the CALLER's opcode, not the callee UFunction.** A UFunction reachable
through ProcessEvent from one caller (pinecone spawner) can be wholly invisible from another (a BP
ubergraph's `EX_CallMath`). "host_spawn_watcher catches BeginDeferred for the spawner, therefore it catches
BeginDeferred for the clump" is a category error — it conflates the UFunction with its dispatch path. To
catch an `EX_CallMath` call you must patch `UFunction::Func` (the thunk), one layer below ProcessEvent; a
ProcessEvent observer can never see it. The 2026-06-08 pass-2 RE had this exactly right; trust the dated RE
over a later redesign's optimism, and re-read it before contradicting it.

---

## 6. PINNED OFFSETS — IDA pass 2026-06-21 (the thunk-hook gate, NOW CLOSED)

**Bound to this exe ONLY** (a game update silently invalidates them -- our `reflection.cpp:695` already
hard-checks exe size against the build the sigs target; add the two new offsets to the same `sdk_profile.h`
guard when the thunk lands):
- `VotV-Win64-Shipping.exe` SHA-256 `ad478218ec5513cc4c1682937db3214cbf2694d1dcd0583eb423447c049dd3ae`,
  size `84751360`, PE TimeDateStamp `0x6A08597D`. IDB imagebase `0x140000000`.

| symbol | value | RVA | evidence (decompiled) |
|---|---|---|---|
| `UObject::ProcessEvent` | — | `0x1465930` | matched by our `kSigProcessEvent` AOB -> single hit (same fn our resolver binds at runtime) |
| **`FFrame::Locals`** | **`0x28`** | — | OBS#1 ProcessEvent: the alloca'd param frame (params memcpy'd in) stored at `FFrame+0x28`. OBS#2 `FFrame::StepExplicitProperty` (`0x146fa2x`): `MostRecentPropertyAddress = *(FFrame+0x28) + prop->Offset`. **Two sightings AGREE byte-for-byte.** |
| `FFrame::Code` | `0x20` | — | ProcessEvent (Node@0x10/Object@0x18/Code@0x20/Locals@0x28) + the thunk's `Stack[+0x20]` Step-vs-StepExplicit gate (corroboration) |
| **`UFunction::Func`** | **`0xD8`** | — | `UFunction::Invoke` (`0x1302DC0`): `(*(Function+0xD8))(Object, &Frame, Result)` -- the FNativeFuncPtr indirect call (= `(Obj->*Func)(Stack, Result)`). Value lands in `.text`. (RE-UE4SS table 0xB0-0xE0 corroborates 0xD8; RE'd from OUR exe, not the table.) |
| `execBeginDeferredActorSpawnFromClass` | — | `0x300b270` | the BeginDeferred UFunction's `Func` (StaticRegisterNatives pair @ `0x14419b810`). Reads the 5 sig params via `StepExplicitProperty` from Locals; calls the impl `sub_142B3C120`; **`*Result = newActor`**. |

**Q2 EMPIRICALLY CONFIRMED in the disasm:** the thunk writes the spawned actor to **`Result`** (`*a3 =
result`), NOT back to `Locals+returnOff` -> read the new actor from the thunk's `Result` (3rd) arg.
**Param offsets need NO IDA:** `worldCtxOff` (WorldContextObject = param[0], read FIRST in the thunk) and
`actorClassOff` come from our runtime `reflection::FindParamOffset` (walks the UFunction FProperty chain ->
`Offset_Internal`), exactly as host_spawn_watcher already does. Bonus layout: `PropertyChainForCompiledIn`
@ `FFrame+0x80`, `MostRecentPropertyAddress` @ `FFrame+0x38`.

**STATUS:** the gate is CLOSED -- all offsets pinned + cross-checked. NEXT: write the thunk (a READ-ONLY
log pass FIRST -- log `worldCtx`/`actorClass`/`Result` class names, gated to trash-class spawns only, NO
convert; enable the convert only once a host re-pile prints `clump + chipPile`), then retire the proximity
death-watch (RULE 2). The patch: resolve the UFunction via `FindFunction`, read `Func` @ 0xD8 (save the
original = `0x300b270`+slide), write our thunk; game-thread, process-lifetime, transparent forwarder.
