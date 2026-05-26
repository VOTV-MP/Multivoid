# VOTV flashlight cone — DEEP RE findings 2026-05-26

Phase 5F follow-up: wire works (sent=4 applied=4 cross-peer), Intensity
field flips correctly via SetIntensity reflection (5.0/0.0), but no
visible flashlight cone renders on the puppet. This document
consolidates the deep-RE evidence so the next iteration is
evidence-backed rather than iterative-guess (per
`feedback-deep-re-no-iteration`).

## Tool inventory used

- IDA Pro MCP on `VotV-Win64-Shipping.exe` (uptime 4+ days; full
  hexrays + strings cache ready)
- CXX header dumps at `Game_0.9.0n/WindowsNoEditor/VotV/Binaries/Win64/CXXHeaderDump/`
  - Engine.hpp lines 13501-13700 (ULightComponent family)
  - mainPlayer.hpp (VOTV-specific player blueprint)
- 3 parallel agents (light registration RE / orphan audit /
  force-register paths) — synthesized
- Diagnostic readbacks added to `ApplyToPuppet` — ran LAN
  autonomous test, captured field values on BOTH puppet AND local
  light_R for differential analysis

## IDA-confirmed facts

### ULightComponent::SetIntensity native = `sub_142A930E0`

```c
__int64 SetIntensity(this, float NewIntensity) {
    bool isCDO = IsCDO_or_template(this);  // sub_1429EF470
    if ((isCDO || !(this+0x88 & 1) || this+0x14F) && NewIntensity != this+0x20C) {
        this+0x20C = NewIntensity;
        return PushColorAndBrightnessToProxy(this);  // sub_142A98430
    }
    return isCDO;
}
```

Guard = "AreDynamicDataChangesAllowed": `IsCDO || !bRegistered || Mobility != Static`. In our case both lights are at runtime (not CDO), `bRegistered`=1 (bit 0 of byte +0x88), Mobility=2 (Movable). So `Mobility != Static` → write proceeds.

### `PushColorAndBrightnessToProxy` = `sub_142A98430`

```c
void Push(this) {
    proxy = *(QWORD*)(this + 0x3F8);  // FLightSceneProxy*
    if (!proxy) return;  // <-- HARD NO-OP if proxy is null
    // Compute LightColor * Intensity → m128
    // Write proxy+0x120, proxy+0x28, proxy+0x2C
    // Enqueue render command via ENQUEUE_RENDER_COMMAND macro
}
```

`+0x3F8` IS the SceneProxy in this build. Confirmed via the constructor
`sub_142AF2AB0` which zero-inits 130 qwords (= 0x410 bytes); the
write at `qword[127] = 0x3F8` is the proxy slot being zeroed.

### Proxy creator = `sub_142A7B590` (the `CreateRenderState_Concurrent` virtual)

Two vtable xrefs (`0x144069488`, `0x145126588`) — virtual, not directly callable. Body:

```c
char CreateRenderState_Concurrent(this) {
    sub_1429E7950(this);  // sets byte+0x88 = (current & 0x1D) | 2 → bit 1 ("bRenderStateCreated")
    v2 = 0;
    if (this+0x229 != 1 || this+0x230 != 0 || sub_142A8EB10(this)) v2 = 1;
    if (this+0x214 & 1) {  // bAffectsWorld
        if (v2) {
            if (sub_142A6CCB0(this)) {  // ShadowQuality <= ScalabilityLimit
                if (sub_142A6CD30(this)) {  // VISIBILITY CASCADE (see below)
                    // ... permissive: if any of [this+0x288 != 0, IsServer-fast,
                    // Mobility-in-range && fast-path] → alloc + construct proxy ...
                    v6 = needsProxy ? FLightSceneProxy::Construct(Malloc(0x140), this) : 0;
                    this+0x3F8 = v6;  // <-- WRITE PROXY (possibly null)
                    if (v6) {
                        scene = this+0xA8;  // World
                        scene->Scene->AddLight(v6);
                    }
                }
            }
        }
    }
    return v2;
}
```

### Visibility cascade `sub_142A6CD30`

```c
bool sub_142A6CD30(this) {
    parent = this+0xC0;  // AttachParent
    owner = this+0xA8;   // Owner cache
    if (!owner) owner = sub_1429EE5A0(this);  // GetOwner() lazy
    if (!parent || (gp = sub_1428CC620(parent)) == 0 || sub_142A6CD30(gp)) {  // recurse up attach
        v5 = owner && sub_142F8AEB0(owner);
        v6 = vtable[+0x468](this) && (!parent || (parent+0x58 & 0x20) == 0);
        return v5 && v6 && (this+0x14C & 0x20) != 0;  // <-- visByte bit 5
    }
    return false;
}
```

**Critical:** the final gate is `this+0x14C & 0x20 != 0` — visByte bit 5.

## Observed diagnostic readings (autonomous LAN test 2026-05-26)

After 4 iterations of DebugForceToggle (which bypasses BP — directly
writes `flashlight` bool + calls SetIntensity), the diagnostic dump
shows:

| field | puppet | local |
|---|---|---|
| `light` ptr | 0x000001A6C72ABD80 | 0x000001A607D32E00 |
| `SceneProxy@+0x3F8` | **NULL** | **NULL** |
| `Intensity@+0x20C` | 5.00 (latest write) | 0.00 / 5.00 (toggle in flight) |
| `bRegistered@+0x88 bit 0` | 1 | 1 |
| `bVisible@+0x14C bit 4` | 0 | 0 |
| `bHiddenInGame@+0x14D bit 2` | 0 | 0 |
| `Mobility@+0x14F` | 2 (Movable) | 2 (Movable) |
| `bAffectsWorld@+0x214 bit 0` | 1 | 1 |
| `regByte@+0x88` | 0x2B | 0x0B |
| `visByte@+0x14C` | 0x63 | 0x43 |
| `worldByte@+0x214` | 0x1F | 0x1F |
| `AttachParent@+0xC0` | 0x000001A5E1B5D800 | 0x000001A5F87DB780 |

Both regByte+visByte have **bit 1** set (= bRenderStateCreated) — so
`sub_1429E7950` ran on both → `sub_142A7B590` executed on both → ran
through the gating logic. But the proxy is NULL on both.

**Crucial diff: puppet has visByte bit 5 (0x20) SET; local has it CLEAR.** The visibility cascade gate `this+0x14C & 0x20 != 0`:
- Puppet PASSES → would proceed to proxy creation
- Local FAILS → returns 0 → proxy creation skipped

Yet puppet's proxy is still NULL → some OTHER gate inside the
proxy-creation block failed (probably the permissive `v2` from the
top OR-condition, or one of the inner net-mode / scene-allocation
checks).

## VOTV-specific: `flashlightStateChanged` delegate

mainPlayer.hpp:184-185 declares:

```cpp
FmainPlayer_CFlashlightStateChanged flashlightStateChanged;  // 0x0AC8
void flashlightStateChanged(USpotLightComponent* Light, bool Visible);  // signature
```

This is a **multicast dynamic delegate** broadcast on flashlight
toggle. Subscribers (likely cooked `prop_equipment_flashlight_C`
instances + possibly map-side props) receive `(Light, Visible)` and
do whatever their BP graph does — could include spawning a beam
actor, activating a particle, lighting a separate actor, etc.

The CXX dump shows VOTV's mainPlayer.hpp has 2 candidates for the
toggle path:
- `updateFlashlight()` (line 435)
- `Flashlight Update()` (line 488, literal space — IDA exec thunk verified earlier)

Plus the input events (lines 600-601) and an internal
`flashlightStateChanged__DelegateSignature` (line 819).

## Key uncertainty (the question for the user)

The user previously stated the local flashlight cone renders
visually during hands-on F-press. Our autonomous diagnostic shows
local.SceneProxy = NULL during DebugForceToggle (which bypasses BP).
Three explanations:

A. **The flashlight cone is rendered by a separate actor, not
   USpotLightComponent.** The BP broadcasts `flashlightStateChanged`,
   and a subscriber spawns/activates a beam. The SpotLightComponent
   contributes scene LIGHTING (illumination of nearby surfaces) but
   not the visible cone. Our puppet doesn't broadcast → no subscriber
   → no beam.

B. **The local cone DOES render via SpotLightComponent, but the
   proxy is created during the BP's first F-press path (which we
   bypass with DebugForceToggle).** A normal hands-on F-press fires
   `InpActEvt_flashlight_K2Node_InputActionEvent_13`, which the BP
   compiles to a path that involves SetVisibility +
   SetIntensity + broadcast — and one of those calls triggers
   `MarkRenderStateDirty` which schedules `RecreateRenderState_Concurrent`
   at end-of-frame, creating the proxy. Our DebugForceToggle only
   does the field write, never goes through that dirty-mark path.

C. **The `+0x3F8` offset is wrong for this build.** The IDA-decompile
   of `sub_142A98430` clearly reads `+0x3F8` as the SceneProxy, but
   maybe the actual rendered field is at a different non-reflected
   offset. (Unlikely given the constructor zeros qword[127] at the
   same offset — but worth eliminating.)

## Targeted fix candidates (NOT yet applied — awaiting verification)

If (A): broadcast `flashlightStateChanged(puppet.light_R, on)` from
ApplyToPuppet via reflection. This invokes whatever the BP-side
subscribers do. Need to find the multicast `__DelegateSignature`
UFunction.

If (B): call `Flashlight Update` UFunction on the puppet via
reflection (we have its address — `0x000001A6122D3880` from
diagnostic). The BP graph fires SetVisibility (which calls
MarkRenderStateDirty → proxy creation). Risk: BP might early-return
on puppet (no Controller, no input state). Spoof `input_flashlight`
+ `hasFlashlight` if needed.

If (C): inspect the cxx dump's USkeletalMeshActor / actor field
offsets via reflection to confirm proxy offset by reading multiple
offsets after +0x3F8 looking for non-null heap pointers.

## What we are NOT doing yet (per RULE feedback-deep-re-no-iteration)

- NOT applying any fix yet
- NOT iterating on multiple candidates
- WAITING for user verification of the key uncertainty before
  selecting (A) / (B) / (C) and making ONE targeted change

## Commits and code state at finding

- HEAD at investigation start: `0941433`
- Local edits (uncommitted): diagnostic readbacks in
  `src/votv-coop/src/coop/item_activate.cpp` + offset additions in
  `src/votv-coop/include/ue_wrap/sdk_profile.h`. NO mutating
  "fix" code present.
- Wire layer + identity Registry: unchanged, still passes LAN test.

## Cross-references

- `[[project-flashlight-cone-deep-re-plan]]` — the plan that
  triggered this investigation
- `[[project-session-2026-05-26-state]]` — session checkpoint
- `[[feedback-deep-re-no-iteration]]` — the rule that gated the
  switch from iterative to evidence-first
- `research/findings/votv-flashlight-RE-2026-05-25.md` — original
  Inc1 RE (Case-b verdict, intensity-vs-visibility, save persistence)
