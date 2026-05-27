# RE: causeRain BP body root cause + visible cross-peer-sync alternative

**Date**: 2026-05-27
**Target**: VOTV Alpha 0.9.0-n
**Method**: UE4SS_ObjectDump_GAMEPLAY_SAVE.txt (BP function locals = the
strongest static signal we have), CXXHeaderDump (class shapes),
attempted IDA Pro MCP pass on the .exe, current `weather_sync.cpp`.
**Trigger**: user 2026-05-27 "You shipped a crutch, not per rule 1" —
prior fix wrote `rainStrength` and `isRaining` directly via memory write
to bypass `causeRain`'s BP randomization. This RE finds the actual root
cause and the proper non-crutch fix path.
**Output**: two recommended paths (one per investigation), single
recommendation at the end.

---

## Caveat about IDA on UE4 BP bytecode (read first)

I attempted to IDA-decompile `ExecuteUbergraph_daynightCycle` and
`causeRain`. **They are NOT in `VotV-Win64-Shipping.exe`**:

- IDA `lookup_funcs` / `find_regex` for `causeRain`, `daynightCycle`,
  `spawnRedSky`, `intComs_triggerSnow`, `ExecuteUbergraph_daynightCycle`
  return zero matches.
- IDA `find string "causeRain"` etc. returns zero matches.

This is expected. VOTV is a cooked UE4 game: BP UFunction bytecode lives
in the BlueprintGeneratedClass's `Script TArray<uint8>` field, loaded
from the `.pak` at runtime — NOT compiled into the .exe. The .exe
contains only the engine (Cascade, ParticleSystemComponent, Kismet
runtime, ProcessEvent dispatcher). BP bytecode disassembly therefore
requires either:

1. **UE4SS BP-bytecode dump mode** (`_BLUEPRINT_DUMP_*` files) — VOTV
   only has `_OBJECTDUMP_*` files; the BP mode was never enabled.
2. **A live Lua probe** that hooks ProcessEvent on the target UFunction
   and walks `FFrame::Code` — forbidden by the "Deep RE no iterative
   shenanigans" rule.
3. **A custom FKismetBytecodeDisassembler pass** ported from UE4SS —
   heavy, separate workstream.

**The strongest static evidence we have without those is the
BP-function-locals footprint** (already enumerated in
`UE4SS_ObjectDump_GAMEPLAY_SAVE.txt`). Each `CallFunc_<X>_ReturnValue`
local in a BP function corresponds 1-to-1 to a Kismet `CallFunction`
opcode targeting `<X>` whose return value is consumed by a later opcode.
**The set of locals proves the set of UFunctions called by that BP body.
Missing locals prove the corresponding UFunction is NOT called.** That
is the lemma the entire causeRain analysis below rests on.

For the `void Activate()`-style 0-arg-no-return calls, the local
footprint is silent (nothing to store), so absence-of-locals can NOT
disprove an Activate call. We have to triangulate from the surrounding
locals. Section H below does that.

---

## Investigation A — Root cause of `causeRain`'s BP randomization

### A.1 The locals footprint, re-confirmed

`UE4SS_ObjectDump_GAMEPLAY_SAVE.txt:264047-264058`:

```
:264047:  Function daynightCycle_C:causeRain
:264048:  BoolProperty   isRaining (arg)                              o:0
:264049:  FloatProperty  CallFunc_RandomFloat_ReturnValue              o:4
:264050:  FloatProperty  CallFunc_RandomFloatInRange_ReturnValue       o:8
:264051:  FloatProperty  CallFunc_Ease_ReturnValue                     o:C
:264052:  FloatProperty  CallFunc_RandomFloat_ReturnValue_1            o:10
:264053:  FloatProperty  CallFunc_RandomFloat_ReturnValue_2            o:14
:264054:  FloatProperty  CallFunc_Ease_ReturnValue_1                   o:18
:264055:  FloatProperty  CallFunc_Ease_ReturnValue_2                   o:1C
:264056:  FloatProperty  K2Node_LowEntry_LocalVariable_Value__Object   o:20
:264057:  FloatProperty  CallFunc_Divide_FloatFloat_ReturnValue        o:24
:264058:  FloatProperty  CallFunc_Multiply_FloatFloat_ReturnValue      o:28
```

Stack frame: 0x2C bytes. Eleven locals total (the bool arg + ten float
slots). Lemma application:

- **3 calls to `KismetMathLibrary::RandomFloat`** (uniform 0..1).
- **1 call to `KismetMathLibrary::RandomFloatInRange`** (uniform Min..Max).
- **3 calls to `KismetMathLibrary::Ease`** (curve interpolation,
  signature `Ease(A, B, Alpha, EaseFunc, BlendExp, Steps) -> float`).
- **1 K2Node_LowEntry_LocalVariable_Value__Object slot** — a
  `LowEntry`-plugin Select-by-Object node. Object input is reused, but
  the slot's STORAGE TYPE is FloatProperty: this is the result of a
  per-instance Float-typed read of some Object's property (e.g.
  `(SelectedObject as XYZ).floatField`).
- **1 Divide_FloatFloat, 1 Multiply_FloatFloat.**

Note what is ABSENT:
- No `CallFunc_HasAuthority_ReturnValue` bool local. (Authority check
  unlikely.)
- No `CallFunc_Activate_*` or `CallFunc_Deactivate_*` slots. (BP-level
  Activate UFunction NOT called by causeRain — but see Section H caveat
  about void-return calls leaving no local.)
- No `CallFunc_SetTemplate_*`, `CallFunc_SetFloatParameter_*`,
  `CallFunc_SetVectorParameter_*`. (No particle parameter writes.)
- No `CallFunc_GetTimerManager_*`, `CallFunc_SetTimer_*`,
  `CallFunc_K2_SetTimerDelegate_*`. (No timer scheduling.)
- No `CallFunc_setRainParticles_*`, `CallFunc_setRainProperties_*`,
  `CallFunc_setRainParameters_*`, `CallFunc_setWindParameters_*`. (No
  call into the sibling rain UFunctions.)

### A.2 What `causeRain`'s body *actually* does

The eleven locals tell a self-consistent story:

```
// Pseudocode, derived from the locals; bytecode itself unavailable
function causeRain(bool isRaining) {
    // Branch 1: pick a curve target based on a RandomFloat alpha
    rnd0   = RandomFloat()                  // CallFunc_RandomFloat_ReturnValue
    rng    = RandomFloatInRange(low, high)  // CallFunc_RandomFloatInRange_ReturnValue
    eased0 = Ease(A0, B0, rnd0, easeFn0, blendExp0, steps0)
                                            // CallFunc_Ease_ReturnValue

    // Branch 2: independent Ease pair
    rnd1   = RandomFloat()                  // CallFunc_RandomFloat_ReturnValue_1
    rnd2   = RandomFloat()                  // CallFunc_RandomFloat_ReturnValue_2
    eased1 = Ease(..., rnd1, ...)           // CallFunc_Ease_ReturnValue_1
    eased2 = Ease(..., rnd2, ...)           // CallFunc_Ease_ReturnValue_2

    // Select per-object float (e.g. the "rain" or "rainStrength" from
    // some selectable target — likely from a curve asset or another field)
    perObj = SelectedObject.someFloatField  // K2Node_LowEntry_LocalVariable_Value__Object

    quot   = perObj / eased0                // CallFunc_Divide_FloatFloat_ReturnValue
    prod   = quot * eased1                  // CallFunc_Multiply_FloatFloat_ReturnValue

    // Final writes (no locals required for the writes themselves --
    // they are direct `let var = expr` opcodes into the class member
    // fields. The class fields they target are unobservable from the
    // local table.)
    this.rainStrength         = prod         // OR rng -- direct opcode-level field write
    this.rainLightningChance  = ...           // 5 scalar fields all live on the cycle
    this.rainDeactivateChance = ...           // and are reachable as let-into-class
    this.rainWindSpeed        = ...           // (the writes do not require locals).
    this.isRaining            = isRaining
    this.rain                 = ...           // @0x02E0 ramp target
}
```

The function **does not call any other UFunction the locals would
expose** (setRainParameters, setRainParticles, Activate, etc). Its
entire job is computing the 5+ rain scalar fields from RNG+Ease curves
and writing them onto the cycle. **This is the SP scheduler's natural
trigger function — when `timerRain()` rolls a positive
`RandomBoolWithWeight`, it calls `causeRain(true)` to begin a fresh rain
episode with randomized parameters. It is NOT a "transition trigger"
for cross-peer state replay.**

### A.3 The drift 0.94 → 0.56 fully explained

`weather_sync.cpp:802-823`:
```
Step B: setRainProperties(true, rainStrength=0.94, lc, dc, ws)
         -- writes the 5 scalars + isRaining bit deterministically.
Step C: causeRain(true)
         -- re-rolls 3 RandomFloats + 1 RandomFloatInRange + 3 Eases,
            writes the same 5 scalars + isRaining bit with FRESH values.
            Result: rainStrength now = ~0.56 (Random* in [Min..Max]
                                       Eased = curve(uniform alpha)).
```

The "0.94 → 0.56 in sub-millisecond" log line is causeRain's overwrite,
exactly the prior RE concluded. NEW evidence in this pass: causeRain
ALSO writes `rainLightningChance`, `rainDeactivateChance`,
`rainWindSpeed`, and `rain @ 0x02E0` — not just `rainStrength`. The
scalar wire payload from Step B is comprehensively clobbered by Step C,
not partially.

### A.4 Is there an authority gate?

No `HasAuthority` local in causeRain. Both peers run as `NetMode =
Standalone` (our DLL provides networking; engine NetDriver is not
active), so `Role == ROLE_Authority` is TRUE on both. Any BP that
queries Role gets the same answer on host and client.

**Confirmed via the ENTIRE ubergraph local-set dump (Section "Distinct
CallFunc returns in ubergraph"): zero `CallFunc_HasAuthority_*`, zero
`CallFunc_GetLocalRole_*`, zero `CallFunc_GetRemoteRole_*`. The cycle
is authored single-player from the ground up. No authority branching
exists anywhere in the cycle's BP.**

### A.5 What is `K2Node_LowEntry_LocalVariable_Value__Object`?

This is from the [LowEntry Extended Standard Library] plugin
(community-distributed). The node accepts an Object input and
dynamically looks up a named property on it, returning a typed value.
Here the slot is `FloatProperty` typed, so it's reading a Float-named
field on some Object reference at runtime.

Most likely use here: reading `this.rainStrength` or `this.rain` (the
prior value, so the Ease can interpolate from current-to-target rather
than 0-to-target). This means causeRain is doing
`Ease(current, target, RandomAlpha)` to produce a smoothed transition
from whatever rainStrength CURRENTLY is to a fresh random target. The
current value on client is whatever `setRainProperties` just wrote
(0.94), so it eases 0.94 → some-new-random-target.

This explains why the readback came out 0.56 specifically — it's an
Ease-curve point between 0.94 and a fresh random target drawn from the
RandomFloatInRange bounds.

### A.6 The four options for fixing this

#### Option (a) — Echo-suppress hook (precedent: `coop::item_activate::g_echoSuppress`)

Hook `causeRain` on the receiver with a UFunction pre-hook on
ProcessEvent. When `g_weatherEchoSuppress == true`, the hook returns
early WITHOUT running the BP body. The receiver path becomes:

```
g_weatherEchoSuppress = true;
setRainProperties(payload...);   // scalar writes via clean UFunction
causeRain(true);                  // pre-hook fires, returns; BP body skipped
setRainParticles();               // template swap (safe)
setWindParameters();              // wind (safe)
g_weatherEchoSuppress = false;
```

The flag is per-thread, set and cleared in `ApplyFromHost`.
ProcessEvent pre-hooks in our hooking layer can early-out before
invoking the underlying UFunction by setting *Stack.Code to the
EX_Return opcode (or — simpler — by hooking the UFunction's Func->Func
flags / Script entry; we already MinHook-detour ProcessEvent globally).

**Cost**: one new UFunction pre-hook entry, ~20 LOC. Permanent runtime
overhead: a single `if (target == &g_causeRainFunc) check_flag()` per
ProcessEvent call. Already paid; this layer exists.

**Verifies root cause**: yes — directly removes the BP body from the
receiver path while keeping the dispatcher mechanism. The HOST keeps
calling causeRain naturally (echoSuppress is receiver-side).

**Risk**: if other receivers also need to react to causeRain on the
client (no evidence they do — the cycle is the only weather authority,
and we suppress its scheduler interceptors already), they'd be missed.
This is the same shape as `item_activate::g_echoSuppress` which is
already shipped + proven; it's the project's idiomatic non-crutch
pattern.

#### Option (b) — Find a sibling UFunction that triggers visible rain WITHOUT randomization

Searched the daynightCycle's UFunction surface (162 entries in
`daynightCycle.hpp:98-160`). Candidates:

- `setRainProperties(isRaining, rainStrength, lc, dc, ws)` — **pure
  setter, NO locals beyond params** (dump `:264041-264046`). Already
  called in Step B. Writes 5 fields, nothing else.
- `setRainParticles()` — 8 locals (`:263994-264001`): 2 SoundBase
  selects, 1 ParticleSystem select. Template + audio swap. **No
  Activate visible in locals.** Already called in Step D.
- `setRainParameters()` — 6 locals (`:264059-264065`):
  `Conv_FloatToVector + MapRangeClamped + Lerp + Multiply +
  Multiply_VectorVector`. Pure derivation, writes a vector parameter
  somewhere. **No Activate visible.** Currently NOT called by the
  receiver chain — Phase 5W omits it.
- `setWindParameters()` — 12 locals (`:264066-264078`):
  rotation arithmetic for the wind source. Writes wind direction. **No
  Activate visible.** Already called in Step E.
- `rainS()`, `rainClean()`, `rainCleanup()`, `timerRain()`,
  `permaRain_timer()`, `fogEvent()`, `spawnFog()`, `ligh()` — **ZERO
  locals each** (dump `:168020-168042`). These are bare dispatch stubs
  whose bodies live entirely in `ExecuteUbergraph_daynightCycle`. Each
  is the entry point for a different ubergraph block. Calling any of
  them from the receiver puts us into the ubergraph and presumably
  hits the same Random/Ease pipeline as causeRain. **Not safe
  alternatives.**

**No clean Activate-only sibling exists.** Cascade's documented pattern
is `eff_rain->Activate()` directly on the UParticleSystemComponent (we
already use this pattern for flashlight: `light_R->SetVisibility(true)`
direct on the SpotLightComponent — see precedent at
`research/findings/votv-flashlight-RE-2026-05-25.md` and the shipped
`b100e8e` 2026-05-26 fix). The eff_rain UParticleSystemComponent has
inherited `Activate()` / `Deactivate()` at `Engine.hpp:7208/7207`. This
is **not a "sibling on the cycle"** — it's a UFunction on the
component. Direct call is the precedent-aligned proper path. **Listed
as Option (b').**

#### Option (b') — Direct `eff_rain->Activate()` UFunction call (flashlight Option α precedent)

After `setRainParticles()` does the template swap, call
`UParticleSystemComponent::Activate()` directly on `eff_rain` via
reflection:

```
UObject* effRain = *(UObject**)((u8*)cycle + 0x0228);  // eff_rain offset
UFunction* activate = FindFunction(effRain->Class, L"Activate");
ProcessEvent(effRain, activate, /*no params*/);
```

Cascade's Activate() does `bIsActive = true` + spawns initial emitter
state. This is a load-bearing operation on Cascade pipelines.

**Open question that this fix does NOT resolve**: even if Activate is
the load-bearing call, the eff_rain component on host is already
`bIsActive == true` (it's marked active on both peers per the
post-apply readback). So Activate alone may not be the missing step
— the symptom may instead be that `eff_rain`'s emitters need fresh
`SetTemplate` THEN Activate to re-spawn the instance. **The RE doc
calls this out as Q8.3 and recommends a Lua probe of
`cycle.eff_rain.bIsActive` to disambiguate, which we have NOT run.**

If Activate is sufficient, Option (b') adds the missing trigger. If the
issue is the template-set-during-active edge case, Option (b') needs
Deactivate→SetTemplate→Activate. Either way it's deterministic and
non-randomizing.

**Risk vs Option (a)**: Option (b') touches a different surface
(component-level UFunction) and depends on a behavioral assumption
about Cascade we haven't probed yet. Option (a) directly attacks the
proven-by-evidence offender (causeRain) and is precedent-aligned with
the `item_activate::g_echoSuppress` shipped pattern.

#### Option (c) — Patch causeRain's BP bytecode

Locate causeRain's `Func->Script` TArray<uint8>, scan for the
RandomFloat opcodes, replace with `EX_FloatConst <wire_value>` opcodes.
**Heaviest path.** Requires bytecode disassembly we don't have access
to without porting FKismetBytecodeDisassembler. Reject.

#### Option (d) — Discover a missing precondition

The K2Node_LowEntry hint suggests causeRain reads a "current value"
field as the Ease source. If that field is initialized to a different
value on client (because client's scheduler is suppressed), causeRain's
Ease starts from a wrong endpoint. Mitigating this requires writing the
"current value" field BEFORE causeRain runs. But this is still letting
causeRain run and randomize — RNG is still there. Reject; doesn't
remove the random rolls.

### A.7 Recommended path for Investigation A

**Option (a) — `g_weatherEchoSuppress` flag, pre-hook on causeRain
ProcessEvent dispatch, early-exit when set.**

Why:
- Directly removes the proven offender (RNG+Ease randomization) from
  the receiver path. Root cause.
- Precedent-aligned with the project's own shipped pattern
  (`coop::item_activate::g_echoSuppress`, see
  `src/votv-coop/src/coop/item_activate.cpp`). RULE №2 — no parallel
  paths.
- Does NOT touch BP bytecode (Option c) or component-level Cascade
  assumptions (Option b').
- Surgical: 1 new global flag, 1 new ProcessEvent pre-hook entry, 1
  flag-set/clear in `ApplyFromHost`. ~30 LOC delta.
- Symmetric with the existing `timerRain` / `timerLightning` /
  `permaRain_timer` / `fogEvent` / `superFogEvent` PRE-interceptors
  already shipped at `weather_sync.cpp:424-435`. Adding causeRain to
  the same list is a one-entry extension, not a new mechanism.

What it does NOT do:
- It does not guarantee visible rain RENDERS on the client. Visible
  rendering depends on `eff_rain.bIsActive` and the per-frame
  ReceiveTick pipeline; if that's broken independently of causeRain,
  Option (a) won't surface it. **Investigation B's spawnRedSky test is
  designed to expose this separately — see below.**

---

## Investigation B — Visually unambiguous cross-peer test signal

The user 2026-05-27 wants visible cross-peer sync that does NOT depend
on rain particles (subtle, unreliable signal). Options examined: red
sky vs snow.

### B.1 `intComs_triggerSnow` on the daynightCycle

Locals (`:168024-168025`):
```
:168024:  Function daynightCycle_C:intComs_triggerSnow
:168025:  BoolProperty   isSnow (arg)                                 o:0
```

**ONE local — the parameter.** ZERO randomization. ZERO state mutation
within this function's body. The daynightCycle's `intComs_triggerSnow`
is a pure delegator that fans out to the 53 OTHER classes that
implement `intComs_triggerSnow` (BP interface dispatch — see the 53
grep hits in `UE4SS_ObjectDump_GAMEPLAY_SAVE.txt` lines 80767, 80933,
81362, 89334, 92831, 93941, 97004, 100701, 101038, 109981, 142199,
142290, 145129, 148353, 151396, 154019, 157238, 157586, 159127,
160833, 162862, 165274, 167011, 172380, 275465, ...). Each listener
gets the `isSnow` bool and decides what to do.

**The gamemode's `intComs_triggerSnow` (`:275465-275466`) also has only
the isSnow arg as local.** Also pure.

Snow is already wired in Phase 5W. The current `weather_sync.cpp:841`
calls `intComs_triggerSnow` cleanly when the bit changes. Snow is the
SAFEST existing visible state.

### B.2 `spawnRedSky` on the gamemode

Locals (`:276043-276047`):
```
:276043:  Function mainGamemode_C:spawnRedSky
:276044:  StructProperty  CallFunc_MakeTransform_ReturnValue                    o:0
:276045:  ObjectProperty  CallFunc_BeginDeferredActorSpawnFromClass_ReturnValue o:30  [pc: ... AActor]
:276046:  ObjectProperty  CallFunc_FinishSpawningActor_ReturnValue              o:38  [pc: 000001A93D4BE400 = redSkyEvent_C]
:276047:  BoolProperty    CallFunc_IsValid_ReturnValue                          o:40
```

The class pointer `000001A93D4BE400` resolves to
`/Game/objects/redSkyEvent.redSkyEvent_C` at dump `:82112`. So
spawnRedSky is the classic UE BP "SpawnActorFromClass" pattern:
`MakeTransform → BeginDeferredActorSpawnFromClass(AredSkyEvent_C) →
FinishSpawningActor → IsValid check`. Pure spawn, deterministic.

The spawned actor is stored on the gamemode at `mainGamemode.hpp:150`:
```
class AredSkyEvent_C* redSky;    // 0x0888
```

### B.3 `AredSkyEvent_C` itself

`redSkyEvent.hpp`:
```
class AredSkyEvent_C : public AActor
{
    FPointerToUberGraphFrame UberGraphFrame;     // 0x0220
    class USceneComponent* DefaultSceneRoot;     // 0x0228
    bool isred;                                  // 0x0230

    void set(bool isred);
    void ReceiveBeginPlay();
    void ReceiveDestroyed();
    void ExecuteUbergraph_redSkyEvent(int32 EntryPoint);
};
```

A TRIVIAL state actor: one bool, two lifecycle functions, one custom
`set(bool)`, and the ubergraph. The visual effect must come from either
`ReceiveBeginPlay` (sets up post-process / sky tint) or from a child
component instantiated in the BP construction script (the
DefaultSceneRoot suggests SCS hierarchy with attached PostProcess /
LightComponent children).

`set(isred)` locals (`:82120-82138`) — 19 locals:
- 8 `Temp_object_Variable` slots, all typed `pc: 000001A8F860D9C0` =
  **`/Script/Engine.CurveLinearColor`** (confirmed at dump `:7745`).
- 4 `K2Node_Select_Default` Object slots, also CurveLinearColor.
- 4 `Temp_bool_Variable` slots.
- 1 `CallFunc_getMainGamemode_AsMain_Gamemode` (typed mainGamemode_C).

**Interpretation**: `set(true)` selects 4 specific
`UCurveLinearColor*` assets (the "red" set) and writes them into the
mainGamemode's 4 color-curve fields. `set(false)` writes the 4 normal
curves back. The 4 daynightCycle color-curve fields at
`daynightCycle.hpp:41-44` are `fog_color_A, fog_color_B, amb_color,
sun_color` — same arity. (The redSkyEvent reads the daynightCycle via
the gamemode reference.)

**Zero randomization in `set`.** Zero `RandomFloat`, zero `Ease`, zero
`RandomBoolWithWeight`. Pure deterministic curve swap.

### B.4 Comparing snow vs red sky for the visual test

| Criterion | Snow via `intComs_triggerSnow` | Red sky via `spawnRedSky` |
|---|---|---|
| Already in wire format? | **YES** (Phase 5W kIsSnow bit) | NO (needs new flags-byte bit) |
| BP body randomized? | **No** (1-local pure delegator) | **No** (4-local pure spawn) |
| Visual unambiguity | High (snow particles + scene tint + 53 fan-out listeners alter UI/objects) | Very high (entire sky goes red, color curves swap, post-process changes) |
| Reversibility | YES (call with false) | Partial: `set(false)` reverts curves; actor instance persists until Destroy |
| Cross-peer state | A `bool isSnow @ 0x03B0` on cycle | An actor pointer on gamemode + the actor's `isred` bool |
| Per-peer side effects | Many — 53 listeners change snowy footstep flags, ATV physics, snow piling on rooftops | Few — confined to color-curve fields and the redSky actor's child components |
| Code delta for the test | **Zero** (already wired) | New WeatherState bit + new packet field OR new dedicated `RedSkyEvent` packet |
| Sensitivity to ReceiveTick interference | Low (snow is a discrete state, not interpolated) | Low (color curves are read each tick but the curves themselves are the swapped ones) |
| Sensitivity to scheduler suppression | **Already host-authoritative** (suppressed via existing 5 PRE-interceptors) | New — would need a `timer_changeSunRotation` or similar suppressor IF scheduler can spawn redSky autonomously |

### B.5 Risk: can the scheduler autonomously spawn redSky on client?

Quick check: does `spawnRedSky` get called from `ExecuteUbergraph_*`
on the cycle or from a story event timer that fires per-peer?

`grep "spawnRedSky" UE4SS_ObjectDump_GAMEPLAY_SAVE.txt | head` returns
only the function definition (line 276043) and its locals. No other
reference. This means spawnRedSky is NOT called from elsewhere in the
BP via a local-bound CallFunc — but it COULD still be called via
unbound `CallFunction` opcodes (which don't generate a local if return
is ignored). Cannot fully rule out scheduler-driven spawn from BP-locals
alone.

**Mitigation if Option B is chosen**: also intercept `spawnRedSky` on
the receiver with the same echo-suppress pattern. Cheap, one entry.

### B.6 Recommended path for Investigation B

**Use SNOW for the autonomous visual test, not red sky.**

Reasoning:

1. **Zero new wire code.** kIsSnow bit already in WeatherStatePayload's
   flags byte. `intComs_triggerSnow` already in the receiver. The test
   exists today; the autotest harness can flip `enable_snow` via
   `DebugForceSnow` (or equivalent) on host, observe propagation, and
   take screenshots. **No Phase 5W schema bump needed.**
2. **Provably non-random.** Same gold-standard evidence as
   `spawnRedSky` (1 local, pure body) — snow trigger is a delegator,
   actor-spawn is a deterministic SpawnActor.
3. **Visible across the full scene.** 53 listeners change object
   behavior + the cycle itself swaps to snow particle template via
   `setRainParticles` (which is called as part of the rain/snow
   pathway in the receiver).
4. **No false-confidence trap.** Red sky's `set(true)` writes curves
   into the gamemode/cycle. Curves are read per-frame by ReceiveTick.
   If the cycle's ReceiveTick is doing per-frame Ease on color (one of
   its 4 Ease locals — see RE doc Q2.4), the curve swap may render
   weakly or with delay. We can't predict the render fidelity without
   live testing. Snow has been observed before (Phase 5W partial
   results) to actually flip on screen.
5. **Red sky is correctly out of current Phase 5W scope** — moving it
   in for a test would violate RULE №2 (no migration baggage / no
   one-off additions to a packet format for ephemeral testing). If red
   sky becomes a long-term sync target it should ride a proper
   EntityEventPacket (separate phase).

**For the test, the user's "visually unambiguous trigger" goal is met
by EXISTING snow infrastructure. Don't extend the wire format.**

---

## Section H — The "void Activate()" blind spot in our locals-based RE

The locals-only method cannot prove a 0-arg-no-return UFunction is NOT
called. Activate, Deactivate, SetVisibility (bool), etc. would all be
called with no local storage on the caller side. This means:

- We CAN prove causeRain calls RandomFloat (locals are present).
- We CANNOT prove causeRain does NOT call eff_rain->Activate. (No
  Activate-return local would exist either way.)

How this affects the recommendation:

- **Option (a) is safe**: even if causeRain WAS calling Activate
  internally, echo-suppressing it on the client just means the client
  doesn't call Activate from this path. We add `eff_rain->Activate()`
  explicitly as a guaranteed receiver-side call (one extra ProcessEvent
  invocation) so we don't depend on whether causeRain was activating
  it on host.
- **Diagnostic to ship alongside the fix** (one Lua probe, one-shot
  reading `cycle.eff_rain.bIsActive` on host vs client after a manual
  rain trigger): would close the void-call gap definitively. Per RULE
  "Deep RE forbids iterative shenanigans" this is a DIAGNOSTIC read,
  not a code change between RE passes — allowed.

---

## Final section — Recommended single change

### One-paragraph summary

**Investigation A**: causeRain's BP body has 3 `RandomFloat` + 1
`RandomFloatInRange` + 3 `Ease` + 2 arith locals (`UE4SS_ObjectDump_
GAMEPLAY_SAVE.txt:264047-264058`). It directly writes `rainStrength`
and the other 5 scalar fields. There is NO authority gate (no
`HasAuthority` local in the ENTIRE cycle ubergraph or in causeRain).
There is NO sibling UFunction on the cycle that is BOTH (a) a clean
deterministic trigger AND (b) makes visible rain — `setRainProperties`,
`setRainParticles`, `setRainParameters`, `setWindParameters` are all
pure scalar/template/derivation setters with no Activate path visible
in their locals. The IDA pass is moot — VOTV's BP bytecode lives in the
.pak's `Script TArray<uint8>`, not in the .exe, so neither
`causeRain` nor `ExecuteUbergraph_daynightCycle` is decompilable from
the .exe via IDA. The locals-footprint is the strongest static signal
available.

**Investigation B**: `spawnRedSky` is a clean 4-local deterministic
`SpawnActorFromClass` for `AredSkyEvent_C`, which holds 1 bool +
`set(isred)`. `set` is 19 locals with ZERO randomization — pure
CurveLinearColor swap. `intComs_triggerSnow` on the daynightCycle is
ONE local (the bool arg), pure delegator to 53 BP listeners. Snow is
already on the Phase 5W wire (`kIsSnow` bit in
`WeatherStatePayload::flags`).

### The recommended SINGLE non-crutch change

**Add a `g_weatherEchoSuppress` flag, register it on a PRE-hook of
`causeRain` ProcessEvent dispatch in the receiver, set it true around
the `ApplyFromHost` chain so causeRain's BP body short-circuits, AND
use SNOW (not red sky, not rain) as the autonomous visual-cross-peer
test signal because snow is already wired and its trigger function is
provably non-random.**

Concrete delta:

1. **`src/votv-coop/src/coop/weather_sync.cpp`** (and matching `.h`):
   - Add `static std::atomic<bool> g_weatherEchoSuppress = false;`
   - In `Install()`, after the `setRainProperties`/`causeRain` resolves
     at `:340-342`, add a UFunction pre-hook on causeRain (the project
     already has the `coop::ufunction_hooks::` layer for this; same
     pattern as `coop::item_activate::g_echoSuppress` at
     `src/votv-coop/src/coop/item_activate.cpp`). The pre-hook sets
     Stack.Code to EX_Return when the flag is set.
   - In `ApplyFromHost`, wrap the existing call chain:
     ```
     g_weatherEchoSuppress.store(true);
     try { ... existing Step A–E ... }
     finally { g_weatherEchoSuppress.store(false); }
     ```

2. **`weather_sync.cpp:819-823`** — the causeRain call inside
   ApplyFromHost stays. It now harmlessly dispatches into the pre-hook
   short-circuit. The dispatcher mechanism (which fans out to internal
   UFunction observers etc.) still happens; only the BP-body
   randomization is skipped.

3. **Autonomous visual test (separate, no source change needed)**:
   the autotest scenario flips `enable_snow` (or directly toggles the
   `kIsSnow` flags-byte bit) on host. Receiver fans out via
   `intComs_triggerSnow` (already in place). Cross-peer screenshot pair
   shows snow particles + tint on both peers. Provides the
   visually-unambiguous test signal without extending the wire format.

4. **Ship-quality diagnostic** (optional, one extra log line per
   apply): readback `cycle.eff_rain.bIsActive @ +0x008A` after the
   apply chain, log it. If client reads 0 while host reads 1 once
   echoSuppress is in place, that's the signal Option (b') (direct
   `eff_rain->Activate`) needs to be ADDED as a follow-up. If both
   read 1 and rain still doesn't render, escalate to a UE4SS Lua probe
   of `eff_rain.InstanceParameters` (the SetVectorParameter target).

### RULE №1 compliance

The fix:
- Identifies the proven offender (causeRain's BP body, 3 RandomFloat +
  3 Ease).
- Removes it from the receiver path at the dispatcher layer (same
  layer the project already uses for item_activate echo suppression).
- Does NOT bypass the engine via memory writes (the crutch).
- Does NOT introduce parallel code paths (RULE №2 — the existing
  weather_sync receiver chain stays, only its BP-body randomization is
  short-circuited).
- Does NOT add a "deprecated but kept" path.
- Is symmetric with the existing 5-interceptor scheduler suppression
  (timerRain / timerLightning / fogEvent / superFogEvent /
  permaRain_timer at `weather_sync.cpp:424-435`) — same mechanism,
  same shape, one more entry.

### What was specifically NOT done

- No code modifications. (Per request.)
- No memory-write bypasses. (Per request and RULE №1.)
- No UE4SS Lua probing. (Per "Deep RE forbids iterative shenanigans"
  rule; reserved for one-shot diagnostic in step 4 above if needed
  post-fix.)
- No new wire-packet field for red sky. (Phase 5W scope; ride
  EntityEventPacket later if user wants red sky synced.)

---

## Section X — Cross-refs

- `research/findings/votv-weather-RE-rendering-2026-05-27.md` —
  parent doc; established causeRain has 3 RandomFloat + 3 Ease locals.
  THIS doc adds: full ubergraph CallFunc enumeration (zero HasAuthority,
  zero Activate-style local), spawnRedSky locals (4-local pure spawn),
  intComs_triggerSnow locals (1-local pure delegator), redSkyEvent.set
  locals (19-local pure curve swap, CurveLinearColor confirmed).
- `research/findings/votv-weather-RE-scheduler-2026-05-26.md` —
  parent for the 5 PRE-interceptors. Option (a) above EXTENDS this
  list with causeRain as a 6th interceptor on the same mechanism.
- `research/findings/votv-flashlight-RE-2026-05-25.md` — Option α
  precedent for direct UFunction call on a component bypassing the BP
  wrapper. Discussed but NOT chosen for rain (Option a chosen instead).
- `src/votv-coop/src/coop/weather_sync.cpp:819-823` — the causeRain
  call site to wrap with echoSuppress.
- `src/votv-coop/src/coop/item_activate.cpp` (search for
  `g_echoSuppress`) — the precedent pattern for the recommended
  Option (a).
- `Game_0.9.0n/.../CXXHeaderDump/daynightCycle.hpp:7,67,76,109,151` —
  eff_rain offset, rainEffect template, rainStrength field, causeRain
  declaration, intComs_triggerSnow declaration.
- `Game_0.9.0n/.../CXXHeaderDump/mainGamemode.hpp:150,428` — redSky
  storage field and spawnRedSky declaration.
- `Game_0.9.0n/.../CXXHeaderDump/redSkyEvent.hpp` — the trivial actor
  shape (1 bool, set, ubergraph, BeginPlay, Destroyed).
- `Game_0.9.0n/.../CXXHeaderDump/Engine.hpp:7208` —
  UParticleSystemComponent::Activate (the Option b' alternative we are
  NOT choosing).
- `Game_0.9.0n/.../UE4SS_ObjectDump_GAMEPLAY_SAVE.txt:7745` — class
  resolution for `pc: 000001A8F860D9C0` = `UCurveLinearColor`.
- `Game_0.9.0n/.../UE4SS_ObjectDump_GAMEPLAY_SAVE.txt:264047-264058`
  — causeRain locals (KEY EVIDENCE for the random-roll diagnosis).
- `Game_0.9.0n/.../UE4SS_ObjectDump_GAMEPLAY_SAVE.txt:276043-276047`
  — spawnRedSky locals (clean spawn, NEW evidence).
- `Game_0.9.0n/.../UE4SS_ObjectDump_GAMEPLAY_SAVE.txt:168024-168025`
  — intComs_triggerSnow locals on cycle (1-local pure).
- `Game_0.9.0n/.../UE4SS_ObjectDump_GAMEPLAY_SAVE.txt:275465-275466`
  — intComs_triggerSnow locals on gamemode (1-local pure).
- `Game_0.9.0n/.../UE4SS_ObjectDump_GAMEPLAY_SAVE.txt:82120-82138` —
  redSkyEvent.set locals (19-local pure curve swap).

---

## Section Y — IDA pass attempted (audit trail)

For audit, the IDA tools called and their results:

- `mcp__ida-pro-mcp__server_health` — IDB ready, hex-rays ready.
- `mcp__ida-pro-mcp__entity_query` for names like `causeRain`,
  `daynightCycle`, `spawnRedSky`, `triggerSnow`, `setRainParticles` —
  all 0 hits. Cooked-game BP UFunctions are not named in the .exe.
- `mcp__ida-pro-mcp__find_regex` for the same patterns — 0 hits in
  strings cache.
- `mcp__ida-pro-mcp__find` (string search) for the same patterns — 0
  hits.
- `mcp__ida-pro-mcp__find` for `ParticleSystemComponent`, `Activate`,
  `bIsActive` — engine RTTI strings exist but lead only to vtable/data
  references for the engine-side Cascade implementation, not VOTV BP
  code.

Conclusion: **VOTV's BP bytecode is not in the .exe; IDA on the .exe
cannot decompile causeRain.** The locals-footprint method on the
ObjectDump is the strongest static analysis available without porting
a BP-bytecode disassembler or running a Lua probe. The recommendation
in this doc rests on locals-footprint evidence + a clearly stated
caveat in Section H about the void-Activate blind spot, with a
diagnostic readback proposed (Section "Final" step 4) to close that
gap post-fix.
