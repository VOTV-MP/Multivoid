# VOTV remote-player PUPPET head-look — RE + fix design (2026-06-11)

> **STATUS 2026-06-11 PM: FIX IMPLEMENTED + BUILD CLEAN + smoke-confirmed firing.**
> `puppet.cpp::DriveHeadLookAtWorld` (replaced `DriveAnimBP`) + `remote_player.cpp`
> look-point reconstruction + the `sdk_profile.h:593` enum-comment fix are landed
> (§5). Smoke: "puppet: kerfur head-look drive active -- wrote lookAt + customLookAt
> =true (class-gate passed)", both peers stable, no faults.
>
> **STATUS 2026-06-11 EVENING — §5's "body turn comes free from the wire" claim
> FALSIFIED by hands-on; receiver-side turn-in-place SHIPPED.** Hands-on showed
> the puppet's WHOLE BODY snaps with every remote camera move and the head never
> leads. Root cause: VOTV's first-person pawn turns its body with the camera
> ~immediately, so the streamed actor yaw IS the camera yaw on foot
> (`headYawDelta` ~= 0 standing — confirmed in the smoke pos-diag:
> `actorYaw=-52.8 ctrl(Y=-52.8)`). The "source body lags the camera" premise
> (§5 / the old net_pump comment) only holds for transients, not the standing
> look-around. FIX (the §5 design's missing half, synthesized on the RECEIVER):
> `RemotePlayer::UpdateBodyYaw` presentation state machine — STANDING: `bodyYaw_`
> HOLDS, head leads via the world-space lookAt vs the body-relative LookAt
> clamps; camera lead >= 60 deg (head 45 + neck ~22 reach) -> body turns at
> 360 deg/s until within 5 deg (hysteresis). MOVING (>30 cm/s): rate-follows the
> STREAMED actor yaw at 1080 deg/s (strafe-facing stays wire-true — the
> controller-yaw-as-body regression guard). `ApplyToEngine` applies `bodyYaw_`,
> NOT `curYaw_`; the flashlight `lag_fl` gets relative yaw
> `OffsetDegrees(bodyYaw_+meshOffsetYaw_, curYaw_+headYawDelta)` so the cone
> still tracks the CAMERA while the body lags. Seeds: Spawn / first packet /
> teleport-SNAP / ragdoll-recover. MTA precedent: CClientPed.cpp:3450-3474
> (rotation = its own interpolated presentation state, never slammed from the
> wire). Smoke PASS (both peers stable, puppets bucket 0.00 ms/fr, logs clean).
>
> **STATUS 2026-06-11 ROUND 3 — "auto head follow still ticking" root-caused:
> the puppet renders TWO overlapped kel bodies, and the drive wrote only ONE.**
> Hands-on after round 2: body-turn clamp works, but the head still tracked the
> observer. Bytecode re-check first: the anim ubergraph gate is
> `@15 IFNOT(customLookAt) JUMP @1431`; `@1431 lookAt := PlayerCameraManager(0)`
> then rejoins `@29` (which only computes `lookingAtPlayer` + foot IK) — with
> customLookAt=true NOTHING in the anim graph rewrites lookAt, and customLookAt
> is written ONLY by the kerfur ACTOR ubergraph (@4617/@36158 — doesn't exist on
> a mainPlayer puppet). So the write was correct but landed on the WRONG body:
> mainPlayer_C renders mesh_playerVisible @0x04F8 ATTACHED TO the native
> ACharacter::Mesh slot @0x0280 — SAME skin asset, EACH ticking its OWN
> kerfurOmega AnimInstance (the v5 spawn note "both peers see overlapping bodies
> as ONE body"; the hurt-flash already paints BOTH meshes). In SP the two stay
> pixel-identical because both BUAs auto-aim at the same local camera; driving
> only mesh_playerVisible's instance broke that invariant — the Mesh slot's
> un-driven head kept auto-following the observer ON TOP of ours. FIX:
> `DriveHeadLookAtWorld` writes BOTH instances (dedupe-guarded, per-instance
> kerfur class gate) + the spawn seeds (removeArms/lookingAtPlayer/
> walkSpeedMultiplier) now seed BOTH (IsKerfurAnimBP-gated; previously
> mesh_playerVisible only — a latent two-body divergence of the same shape) +
> one-shot `puppet-MeshSlot` DumpAnimState proves the second instance's class in
> the smoke log. INVARIANT (this is the lesson): any per-instance anim write on
> the puppet MUST hit every rendered kel instance — mesh_playerVisible AND the
> ACharacter::Mesh slot.
>
> **STATUS 2026-06-11 ROUND 4 — "puppet looks headless" after round 3:
> `removeArms` is the FP SELF-VIEW recipe (removes arms AND HEAD), and the two
> bodies are intentionally ASYMMETRIC.** Round 3's "seed both instances
> identically" set `removeArms=true` on the Mesh-slot instance too — but
> `removeArms` gates `TwoWayBlend_1` (anim uber @3265
> `Conv_BoolToFloat(removeArms) -> AnimGraphNode_TwoWayBlend_1`) into a branch
> whose ModifyBone nodes SCALE AWAY the upper chain — the AnimBP's
> `BoneToModify` set includes `upperarm_R`, `upperarm_L`, `neck`, `head`
> (+center/belly; 5 BMM_Replace records in the CDO). The Mesh slot is the
> puppet's head+arms PROVIDER; beheading it = the symptom. Corollary:
> `mesh_playerVisible` (removeArms=true since the original spawn code) has been
> the de-headed/de-armed UNDERLAY all along — masked by the slot's full body;
> that's also why round 2's drive-on-mesh_playerVisible-only produced no
> visible head change. FIX: seeds are ROLE-AWARE — `removeArms = (comp ==
> mesh_playerVisible)`; the look drive still hits BOTH. REFINED INVARIANT:
> lookAt/customLookAt and other pose-input writes go to BOTH instances; the
> body-ROLE fields (removeArms) are per-body. The lookAt write itself is
> exonerated for the head scale (kerfur NPC mirrors receive the identical
> write and keep their heads).
>
> **STATUS 2026-06-11 FINAL — HANDS-ON CONFIRMED ("It's amazing, it works!").**
> Head present, head leads where the remote player looks, body turns at the
> clamp. The complete fix stack: (1) native lookAt/customLookAt drive (round 1),
> (2) receiver-side body-yaw turn-in-place `UpdateBodyYaw` (round 2), (3) drive
> BOTH overlapped kel instances (round 3), (4) role-aware seeds -- removeArms
> only on the mesh_playerVisible underlay (round 4). No pitch-sign complaint =
> the `+sin` convention stands. Tuning knobs if ever needed: kBodyTurn* in
> remote_player.h. UNCOMMITTED.**

**Goal (user):** the remote-player puppet's head must rotate to show **where the remote
player is actually looking** (their camera pitch + yaw), NOT auto-follow the nearest/local
(observer) player like a kerfur NPC. Desired feel = classic aim-offset + turn-in-place:
"head rotates first, then if threshold reached - body rotates."

**User symptom:** the puppet head STILL auto-follows the observer (host) — i.e. the current
`DriveAnimBP` head write is NOT winning.

**TL;DR — root cause is a two-part bug, both proven below:**
1. **The head ModifyBone write does nothing AND is clobbered.** The visible head twist on a
   kerfur AnimBP comes from two native `FAnimNode_LookAt` nodes aiming at the AnimBP `lookAt`
   FVector — NOT from the `ModifyBone` node `DriveAnimBP` writes. The current code writes
   `ModifyBone @0x2C60 .Rotation` with `RotationMode = 2`, believing `2 == BMM_Replace`. **It
   is not — `BMM_Replace = 1`, `BMM_Additive = 2`** (CXX dump, verified below). The sdk_profile
   comment "0=Ignore,1=Add,2=Replace" is **wrong** (Add/Replace swapped). So the write is
   Additive, not Replace. Worse: a native UE4.27 **PropertyAccess FastPath copy runs every tick
   AFTER `BlueprintUpdateAnimation`** and copies `headLookAt → ModifyBone @0x2C60 .Rotation`,
   overwriting our `.Rotation` write with `headLookAt`, which BUA derives from `lookAt`. So even
   the value we write is gone by anim-eval time.
2. **The head still tracks the observer because `lookAt` is recomputed each tick to the LOCAL
   camera.** `DriveAnimBP` zeroes the two LookAt-node Alphas — but the FastPath does NOT copy
   Alpha, so that part survives… yet on the *observer's* machine the AnimBP still recomputes
   `lookAt = local PlayerCameraManager(0).GetActorLocation()` (= the observer) every tick because
   `customLookAt == false`. Combined with the Alpha-zero being the only thing that "works", the
   net result the user sees is the head not pointing where the remote player looks.

**The fix is RULE-1 root-cause + uses the engine's own pipeline:** drive the head through the
**native `lookAt` (FVector @0x2D90) + `customLookAt=true` (bool @0x2E49)** path — exactly the
mechanism the kerfur-NPC mirror already uses (shipped v39) — feeding it a **world look-point
reconstructed from the streamed (body-yaw + headYawDelta + pitch)**. Stop poking ModifyBone and
stop zeroing the LookAt Alphas (RULE 2: that whole recipe is retired). The body-turn-at-threshold
is **already handled by the existing body-yaw stream** (the source body lags the camera natively),
so no new turn-in-place and **no new wire field** is needed. Details + exact code in §5.

---

## Sources (all byte-exact / reflection-exact; no guessing)

- **CXX SDK dump** (the standalone SDK, authority for struct layout/offsets):
  `Game_0.9.0n/.../CXXHeaderDump/AnimGraphRuntime.hpp` (FAnimNode_ModifyBone, FAnimNode_LookAt),
  `AnimGraphRuntime_enums.hpp` (EBoneModificationMode), `Engine_enums.hpp` (EBoneControlSpace),
  `AnimBlueprint_kerfurOmega_regular.hpp` (the AnimBP class members + offsets),
  `mainPlayer.hpp` (the puppet actor class).
- **BP kismet disassembly** (the authority for per-tick writes), produced earlier by
  `tools/bp_reflect.py` and cross-checked live this session:
  `research/bp_reflection/kerfuranim_cfg.txt` (offset-aware ubergraph CFG),
  `research/bp_reflection/AnimBlueprint_kerfurOmega_regular.functions.txt` (function index),
  `research/bp_reflection/_mainplayer_uber_full.txt` (the puppet actor ubergraph).
- **Prior RE this corroborates / extends** (both already byte-exact on the kerfur head pipeline):
  `research/findings/votv-kerfur-headlook-BP-disassembly-2026-06-07.md`,
  `research/findings/votv-kerfur-headlook-AnimBP-RE-and-coop-sync-2026-06-07.md`.
- **Live diagnostic** (smoke 2026-06-11, dumped at spawn before DriveAnimBP): the head-graph
  table in the task prompt (LookAt_1/LookAt disabled; ModifyBone @0x2C60 'head' rotMode=2).
- Current code: `src/votv-coop/src/ue_wrap/puppet.cpp` (`DriveAnimBP` @612, helpers @95-116),
  `src/votv-coop/src/coop/remote_player.cpp` (`ApplyToEngine` @ ~620-758),
  `src/votv-coop/src/coop/net_pump.cpp` (`BuildPose` @ ~200-233),
  `src/votv-coop/include/ue_wrap/sdk_profile.h::anim` (@581-603),
  `src/votv-coop/include/coop/net/protocol.h::PoseSnapshot` (@1162-1179).

> **No IDA was needed.** The determinant of "who wins each frame" is the AnimBP's serialized
> PropertyAccess copy table + the ubergraph bytecode, both fully captured in the cooked asset
> (the authority). The prior 2026-06-07 RE already drove IDA (`sub_1414E0A70`, the FastPath
> dispatcher) to confirm the LookAt/ModifyBone nodes are NOT in the per-node
> `EvaluateGraphExposedInputs` path — they are fed ONLY by the batched FastPath copies. The
> function index (`...functions.txt`, listing EvaluateGraphExposedInputs only for
> BlendSpacePlayer/TwoBoneIK/TwoWayBlend, none for LookAt) reproduces that conclusion from
> reflection alone.

---

## Q1 — WHO computes the head look each frame, and WHY does our write get overridden?

### The kerfur head pipeline (byte-exact)

`UAnimBlueprint_kerfurOmega_regular_C::BlueprintUpdateAnimation(DeltaTimeX)` is a trampoline:
it stashes DeltaTime and `CALL ExecuteUbergraph(3688)`. All logic is in the ubergraph. Three
facts from `research/bp_reflection/kerfuranim_cfg.txt` (verified this session):

```
@  15: IFNOT(customLookAt) JUMP @1431          ; the gate
@1431: CallFunc_GetPlayerCameraManager_ReturnValue := GetPlayerCameraManager(self, 0)
@1457: CallFunc_K2_GetActorLocation_ReturnValue   := <camMgr>.K2_GetActorLocation()
@1507: lookAt := CallFunc_K2_GetActorLocation_ReturnValue   ; *** the SOLE write to lookAt ***
@3870: ... := Subtract_VectorVector(lookAt, neckSocket+offset)
@3962: CallFunc_MakeRotFromZX_ReturnValue := MakeRotFromZX(<lookAt - neck>, Cross(...))
@4008: headLookAt := CallFunc_MakeRotFromZX_ReturnValue        ; headLookAt DERIVED from lookAt
```

- When `customLookAt == false` (the default — the field is absent from the CDO and is never
  written by either the AnimBP or the mainPlayer actor BP), BUA **overwrites `lookAt` every tick
  with the LOCAL `PlayerCameraManager(0)` world location** (@1507). On the observer's machine
  that camera is the OBSERVER → the head tracks the observer. **This is the user's symptom.**
- `headLookAt` (@0x2E3C) is **re-derived from `lookAt` every tick** (@4008).

### The head is moved by the LookAt nodes, fed by a native FastPath copy — NOT by BUA, NOT by ModifyBone

The two LookAt nodes have NO `EvaluateGraphExposedInputs` thunk (confirmed: the function index
`AnimBlueprint_kerfurOmega_regular.functions.txt` lists them only for BlendSpacePlayer/TwoBoneIK/
TwoWayBlend). They are fed exclusively by the AnimBP **PropertyAccessLibrary** "FastPath" copy
table, which the engine runs inside `FAnimInstanceProxy::UpdateAnimation` **after** the BP event
update (`BlueprintUpdateAnimation`) and **before** the AnimGraph evaluates the pose. The relevant
copies (decoded from the ClassExport's PropertyAccessLibrary in the 2026-06-07 RE):

| Copy | Type   | Source var  | → Dest node.field                                        | Absolute offset |
|------|--------|-------------|----------------------------------------------------------|-----------------|
| [16] | Struct | `lookAt`    | `AnimGraphNode_LookAt_1` (head @0x1730) `.LookAtLocation` | 0x1730+0x140 = **0x1870** |
| [17] | Struct | `lookAt`    | `AnimGraphNode_LookAt`   (neck @0x18E0) `.LookAtLocation` | 0x18E0+0x140 = **0x1A20** |
| [5]  | Struct | `headLookAt`| `AnimGraphNode_ModifyBone`   (head @0x2C60) `.Rotation`   | 0x2C60+0xE4 = **0x2D44** |
| [6]  | Struct | `headLookAt`| `AnimGraphNode_ModifyBone_1` (neck @0x2A28) `.Rotation`   | 0x2A28+0xE4 = **0x2B0C** |

**Consequences for the current `DriveAnimBP` (puppet.cpp:644-658):**

1. **Writing `ModifyBone @0x2C60 .Rotation` is futile.** Copy [5] runs every tick AFTER our write
   (BUA → FastPath → AnimGraph eval) and overwrites `.Rotation` with `headLookAt`. Our value is
   gone before the node evaluates. This is the same "graph overwrites our field each frame" class
   of bug as the doors-sent=0 case. → **our per-tick write IS stale by anim-eval time.**
2. **Even un-clobbered, the node is in the WRONG MODE to apply it.** The head ModifyBone defaults
   to `RotationMode = BMM_Ignore` (absent in CDO). With Ignore, `.Rotation` is ignored entirely.
   `DriveAnimBP` sets the mode to `2` thinking that's Replace — but `2 == BMM_Additive` (Q2). The
   `RotationMode` byte is NOT in the FastPath copy table, so the mode write survives — meaning the
   node ends up Additive-applying whatever `headLookAt` the FastPath last wrote, on TOP of the base
   pose. That `headLookAt` is a `MakeRotFromZX` world-Z-up rotation (not a pitch/yaw), so the
   contribution is nonsensical even when it does apply.
3. **Zeroing the LookAt Alphas (the only part that "works") does survive** — Alpha is NOT a
   FastPath dest — so the head holds the BlendSpace pose. But that just *disables* the native
   aim; it doesn't make the head point where the remote player looks. The user's complaint ("still
   follows the host") is the residual: in practice the head shows neither the remote look (we never
   feed it) nor a stable forward (the ModifyBone Additive garbage + the still-recomputed `lookAt`
   on any path that re-enables aim leaves it pointing at the observer).

**Verdict for Q1:** the head is computed by the two `FAnimNode_LookAt` nodes from the class member
`lookAt`, which BUA pins to the LOCAL camera every tick (customLookAt==false). Our ModifyBone
write is both (a) clobbered each tick by FastPath copy [5] and (b) in the wrong mode/space/
convention. The current approach fights the wrong node. **Use `lookAt` + `customLookAt`, not
ModifyBone.**

---

## Q2 — FAnimNode_ModifyBone mode + field layout (and the corrected enum)

From `CXXHeaderDump/AnimGraphRuntime.hpp` (struct `FAnimNode_ModifyBone : FAnimNode_SkeletalControlBase`,
Size 0x108) — offsets are **node-base-relative**:

| Field            | Offset  | Type                                   |
|------------------|---------|----------------------------------------|
| (base `Alpha`)   | 0x002C  | float (FAnimNode_SkeletalControlBase)  |
| (base `bAlphaBoolEnabled`) | 0x0029 | bool                         |
| BoneToModify     | 0x00C8  | FBoneReference (FName @+0)              |
| Translation      | 0x00D8  | FVector                                |
| **Rotation**     | **0x00E4** | FRotator                            |
| Scale            | 0x00F0  | FVector                                |
| TranslationMode  | 0x00FC  | TEnumAsByte<EBoneModificationMode>     |
| **RotationMode** | **0x00FD** | TEnumAsByte<EBoneModificationMode>  |
| ScaleMode        | 0x00FE  | TEnumAsByte<EBoneModificationMode>     |
| TranslationSpace | 0x00FF  | TEnumAsByte<EBoneControlSpace>         |
| **RotationSpace**| **0x0100** | TEnumAsByte<EBoneControlSpace>      |
| ScaleSpace       | 0x0101  | TEnumAsByte<EBoneControlSpace>         |

`EBoneModificationMode` (`AnimGraphRuntime_enums.hpp:34`):
```
BMM_Ignore = 0,  BMM_Replace = 1,  BMM_Additive = 2,  BMM_MAX = 3
```
**→ The sdk_profile.h:593 comment `(0=Ignore,1=Add,2=Replace)` is WRONG (Add/Replace swapped).
The code's `RotationMode = 2` is `BMM_Additive`, NOT Replace.** This mislabel is a real bug:
anyone reading the code believes it's an absolute override; it's actually additive.

`EBoneControlSpace` (`Engine_enums.hpp:496`):
```
BCS_WorldSpace = 0,  BCS_ComponentSpace = 1,  BCS_ParentBoneSpace = 2,  BCS_BoneSpace = 3
```

**WHICH SPACE is the head ModifyBone's rotation in?** The kerfur AnimBP's `AnimGraphNode_ModifyBone`
@0x2C60 has `RotationSpace` **absent in the CDO ⇒ `BCS_WorldSpace (0)`** (TEnumAsByte default 0;
the node was authored with default spaces because its mode is Ignore — it was never meant to drive).
`sdk_profile.h` does **not even define `ModBone_RotationSpace`** (it stops at RotationMode @0xFD),
so the current code never sets the space — it inherits whatever the CDO/FastPath left (world). A
FRotator written there is therefore interpreted in **world space**, meaning a raw `(pitch, yaw, 0)`
would NOT map to head-local "look up / look left"; it would rotate the head bone in world axes
(badly coupled to body facing). This is a third reason the ModifyBone path is the wrong tool here.

**Conclusion for Q2:** to use ModifyBone as an absolute override you would need `RotationMode = 1`
(BMM_Replace, not 2), a correctly chosen `RotationSpace` (component or parent-bone, NOT the default
world), AND you must defeat FastPath copy [5] that overwrites `.Rotation`. That is exactly the
"re-fight the whole battle" the 2026-06-07 RE warned against. **We do NOT use ModifyBone.** The
`lookAt`/`customLookAt` path (Q-fix §5) avoids all three problems because it feeds the *node the
kerfur natively uses*, with a *world point* (the LookAt node's native interface), via the *FastPath
copy that is on our side* (it copies our `lookAt` into LookAtLocation).

---

## Q3 — The clamp + head-then-body split (the "kerfur model")

### The clamp is on the LookAt nodes (`LookAtClamp`), per node, class-default

`FAnimNode_LookAt` (CXX dump): `LookAtClamp` @ node-base **+0x170** (float, degrees). From the
kerfur CDO (2026-06-07 RE): **`LookAtClamp = 45°`** on both the head node (@0x1730) and the neck
node (@0x18E0). So the engine itself clamps how far the head/neck can swing toward the look-target
to 45° each. There is also `InterpolationType = None` and `LookAtClamp` interplay, but the salient
number is **45° per node**.

### The head/neck split is two stacked LookAt nodes with different Alphas

- Head node `AnimGraphNode_LookAt_1` (@0x1730): `BoneToModify = 'head'`, `Alpha = 1.0`.
- Neck node `AnimGraphNode_LookAt`   (@0x18E0): `BoneToModify = 'neck'`, `Alpha = 0.5`.

Both aim at the SAME world point (`lookAt`). The neck contributes a partial (0.5) turn and the head
a full (1.0) turn, each clamped to 45°. That stack is the "natural look" distribution — the neck
leads slightly, the head finishes, both saturate at the clamp. **We do not need to author this
ourselves**; feeding `lookAt` drives both nodes exactly as the kerfur was authored.

### Head-then-body / turn-in-place is the SOURCE body-yaw lag, already streamed

The aim-offset-then-turn behaviour the user wants is **not in the AnimBP** — it is in how the
*source* `mainPlayer_C` body yaws. On the source (and confirmed in `net_pump.cpp`):
- `out.yaw = NormalizeAxis(actorRot.Yaw)` — the BODY (actor) yaw.
- `out.headYawDelta = NormalizeAxis(ctlRot.Yaw - actorRot.Yaw)` — camera-yaw LEAD over body-yaw.

VOTV's `mainPlayer_C` body **lags the camera**: when you free-look or sweep the camera, the body
does not instantly yaw — it follows after a threshold (the player's own turn-in-place). So on the
source, `headYawDelta` GROWS as the camera leads, then collapses toward 0 when the body catches up
and re-aligns. (The prior body-yaw RE — `votv-kerfur-bodyfacing-RE-2026-06-07.md` — and the
net_pump comment both confirm "the Character body LAGS the camera; head-leads-body is natural to
the source.") **We already stream BOTH the body yaw (drives `SetActorRotation`) and the lead
(`headYawDelta`).** So:
- The body turn-at-threshold is reproduced by the **existing body-yaw stream** (the puppet body
  yaws to `curYaw_` = source body yaw, which itself already lagged-then-snapped on the source).
- The head-leads-the-body within the clamp is reproduced by feeding a **look-target offset by
  `headYawDelta`** off the body forward (the head/neck LookAt nodes + their 45° clamp do the rest).

**Verdict Q3:** the clamp is the LookAt nodes' `LookAtClamp = 45°` (head + neck, @+0x170). The
head/neck split is the two stacked LookAt nodes (head Alpha 1.0 / neck Alpha 0.5). The
body-turn-at-threshold is NOT new work — it is the source's own body-yaw lag, already on the wire
as `yaw`; `headYawDelta` carries the head lead. The streamed (yaw + headYawDelta + pitch) is
**sufficient**; no separate neck field is required.

---

## Q4 — SOURCE compute correctness

`net_pump.cpp::BuildPose` (verified @200-233):
```
out.yaw          = NormalizeAxis(actorRot.Yaw);                 // BODY facing (actor yaw)
out.pitch        = NormalizeAxis(ctlRot.Pitch);                 // CONTROLLER view pitch
out.headYawDelta = NormalizeAxis(ctlRot.Yaw - actorRot.Yaw);    // camera-yaw lead over body
```
- **`pitch` is the controller/control pitch** (correct — the actor is always upright so actor
  pitch is 0; the controller carries the real view pitch). NormalizeAxis maps it into (-180,180]
  so e.g. "10° down" is -10, not 350 (this also keeps it inside `ValidatePose`'s pitch bound).
- **`headYawDelta = controller.Yaw − actor.Yaw`, normalized to (-180,180]** — a TRUE camera-vs-body
  lead. It is non-zero whenever the camera leads the body (free-look, or mid-turn before the body
  catches up) and ~0 when the body has aligned to the camera. So it is exactly the "head lead"
  signal we want, and it is correct as sent.
- Both are normalized before the wire and re-validated by `ValidatePose` (protocol.h:2158-2176).
  `headYawDelta` is range-checked to [-180,180]; `pitch` is checked to (-90,90) (`out.pitch` is
  normalized, so a downward look passes).

The receiver interpolates them into `curPitch_` / `curHeadYawDelta_` with wrap-aware error terms
(`OffsetDegrees`, remote_player.cpp:439-441) and currently passes `curPitch_, curHeadYawDelta_` to
`DriveAnimBP`. **The inputs are correct; only the consumption (Q1/Q2) is wrong.**

---

## §5 — FIX DESIGN (RULE 1 root-cause; uses the engine's native head pipeline)

### The shape

Drive the puppet head through the **kerfur native `lookAt`/`customLookAt` pipeline** (the same one
the NPC mirror uses, already plumbed: `ue_wrap::reflected_offset::AnimBP_kerfur_lookAt()` @0x2D90,
`AnimBP_kerfur_customLookAt()` @0x2E49, and the helper `WriteLookAtOnAnim()` in puppet.cpp:105).
Feed it a **world look-point reconstructed from the streamed direction**. Retire the ModifyBone /
Alpha-zero recipe entirely (RULE 2).

### Why this wins (timing + clobber)

- On the puppet (`mainPlayer_C` running the kerfur AnimBP), the ONLY per-tick writer of `lookAt`
  is the AnimBP's BUA @1507, **gated by `customLookAt`**. The mainPlayer actor BP does **NOT**
  write `anim.lookAt` or `anim.customLookAt` (verified: the entire `_mainplayer_uber_full.txt`
  has exactly one `anim.<field> :=`, namely `anim.rot :=` @58347; the `makeLookAt`/`isLookAt`/
  `lookAtActor` refs are the player's object-grab system, unrelated). So setting
  `customLookAt = true` kills the only competing writer → our `lookAt` survives every tick.
- The FastPath copies [16]/[17] (`lookAt → LookAt(_1).LookAtLocation`) run after BUA, before the
  AnimGraph evaluates — and their SOURCE is our `lookAt`. So they carry OUR value into the nodes
  the same frame. (We must NOT write `LookAtLocation` directly — that the FastPath would clobber;
  writing `lookAt` is the supported input.)
- No mode/space/convention problem: the LookAt node's native interface is a **world point**, which
  is unambiguous and peer-consistent.

### Reconstructing the world look-point on the puppet (the only new code)

We have, per tick on the receiver: the puppet actor (its body already yawed to `curYaw_`), and the
interpolated `curPitch_` + `curHeadYawDelta_`. Build a world point in front of the head:

```
// pseudo, in remote_player.cpp::ApplyToEngine, replacing the DriveAnimBP call:
const float lookYawDeg   = curYaw_ + curHeadYawDelta_;     // body facing + camera lead
const float lookPitchDeg = curPitch_;                      // controller view pitch (up = ?)
// Head anchor = puppet head world pos. Reuse the existing head-Z machinery
// (GetHeadPosition already maintains headZOffset_ from the 'head' bone). X/Y = actor pivot.
const ue_wrap::FVector headPos = GetHeadPosition();        // actor X/Y + head-Z offset
// Forward dir from yaw+pitch (UE: +X forward; pitch>0 looks UP in UE convention -> +Z uses +sin(pitch)).
const float yawRad = lookYawDeg * kDeg2Rad, pitchRad = lookPitchDeg * kDeg2Rad;
const float cp = cosf(pitchRad);
const ue_wrap::FVector dir{ cp * cosf(yawRad), cp * sinf(yawRad), sinf(pitchRad) };
const float kLookDist = 500.f;                            // any positive distance; LookAt uses direction
const ue_wrap::FVector worldLook{ headPos.X + dir.X*kLookDist,
                                  headPos.Y + dir.Y*kLookDist,
                                  headPos.Z + dir.Z*kLookDist };
Pup::DriveHeadLookAtWorld(actor_, worldLook);             // new wrapper (see below)
```

**UE pitch-sign caveat (verify hands-on, cheap to flip):** UE4 `FRotator` pitch is +up for the
control rotation, but the engine's "look up" can read as negative depending on the rig. The
`ValidatePose` normalize keeps `pitch` in (-90,90); pick the Z sign so "remote looks up → puppet
head tilts up". If inverted in the first hands-on, flip `sinf(pitchRad)` to `-sinf(pitchRad)`.
(This is a 1-char tuning, not a redesign.)

### New `ue_wrap` wrapper (Principle 7: engine memory stays in ue_wrap)

Add to `puppet.cpp` a public function that drives the head via the native path and (RULE 2)
**delete** `DriveAnimBP`'s LookAt-Alpha-zero + ModifyBone block:

```
// ue_wrap/puppet: drive the puppet head to a WORLD look-point via the kerfur
// native lookAt/customLookAt pipeline (the head/neck FAnimNode_LookAt nodes aim
// at lookAt; customLookAt=true stops BUA recomputing lookAt to the LOCAL camera).
void DriveHeadLookAtWorld(void* puppetActor, const FVector& worldTarget) {
    void* comp = GetSkeletalMeshComponent(puppetActor);
    if (!comp || !R::IsLive(comp)) return;
    void* anim = LiveAnimInstance(comp);
    if (!anim) return;
    WriteLookAtOnAnim(anim, worldTarget);   // writes lookAt @0x2D90 + customLookAt=true @0x2E49
}
```

`WriteLookAtOnAnim` already exists (puppet.cpp:105) and already class-gates on `IsKerfurAnimBP` and
sets `customLookAt=true`. The puppet AnimBP IS the kerfur AnimBP, so the gate passes.

### What to DELETE (RULE 2 — no parallel old + new path)

- **`DriveAnimBP` body (puppet.cpp:644-667):** the two LookAt-Alpha=0 writes, the
  `ModifyBone @0x2C60 .Rotation` + `.RotationMode = 2` writes, the `lookingAtPlayer = false` write,
  and the `headLookAt` write. All of it. Replace the call site in remote_player.cpp:758 with the
  `DriveHeadLookAtWorld` path above. (Keep `DriveAnimBP`'s signature only if other callers exist —
  grep shows the sole caller is remote_player.cpp:758, so rename/replace it cleanly.)
- **sdk_profile.h::anim ModifyBone/LookAt-Alpha drive constants** become unused for the puppet
  (`kKerfurModifyBone`, `ModBone_Rotation`, `ModBone_RotationMode`, the SkelCtl_Alpha writes).
  Keep the ones still used by `DumpKerfurHeadGraph` (diagnostic) but drop any left dead. **Fix the
  wrong comment regardless:** `ModBone_RotationMode` should read `(0=Ignore,1=Replace,2=Additive)`.
- `lookingAtPlayer = false` per-tick write (puppet.cpp:663) — **2026-07-02 CORRECTION: this
  paragraph's premise ("only gates a STATE, not the head aim", 2026-06-07 RE Q3) is WRONG — the
  LookAt nodes live INSIDE that state, so the flag IS the head-look on/off.** Forced/seeded false
  was actively harmful (pushes the machine to `lookStraight` = frozen head); the seed is now TRUE
  and a post-BUA Func hook re-asserts it every anim update, puppet-only (`5b2cb5ff`). Topology
  proof: `votv-puppet-head-freeze-backturned-RE-2026-06-24.md` top banner.

### Wire format: NO new field needed

The existing `PoseSnapshot.{yaw, pitch, headYawDelta}` is sufficient. We reconstruct the world
look-point on the receiver from (body yaw + headYawDelta + pitch + the puppet's own head world
position). **No protocol bump, no new payload.** This is strictly better than streaming a world
`lookAt` (which the NPC mirror does) because the puppet's look is a *direction relative to its own
body*, and we already have body yaw on the puppet — anchoring the ray to the puppet's head keeps it
correct regardless of the puppet's interpolated position.

> If a future requirement needs the head to track a *world object* (not the free-look direction),
> THEN stream a world `lookAt` like the NPC mirror. For "where the remote player is looking", the
> direction reconstruction is the right model and needs no new wire field.

### Clamp / head-then-body: nothing extra to build

- Clamp: inherited from the LookAt nodes' class-default `LookAtClamp = 45°` (head + neck). The
  head saturates at 45° off body-forward — exactly "head rotates within a limit". If the user wants
  a different clamp, it is a single per-node write (`*(float*)(anim + kKerfurLookAt_1 + 0x170) = N`),
  but the authored 45° already matches the requested feel.
- Body-turn-at-threshold: the existing `SetActorRotation(actor_, {0, curYaw_, 0})` (remote_player.cpp:672)
  already reproduces it, because `curYaw_` = the source body yaw, which on the source lagged the
  camera then snapped (turn-in-place). When the source body catches up, `headYawDelta → 0` and the
  head re-centres over the now-aligned body. **The "head first, then body" emerges for free** from
  streaming both signals and aiming the head off the (lagging) body.

### One-line verdict

Replace the puppet head drive: stop writing `ModifyBone @0x2C60` (clobbered by FastPath copy [5],
wrong mode `2`=Additive≠Replace, wrong/world space) and stop zeroing the LookAt Alphas. Instead,
each tick write the kerfur native **`lookAt` (FVector @0x2D90) = a world point along
(curYaw_+curHeadYawDelta_, curPitch_) from the puppet's head**, plus **`customLookAt = true`
(@0x2E49)**. The native FastPath copies [16]/[17] then aim the head (Alpha 1.0) + neck (Alpha 0.5)
LookAt nodes — clamped to 45° each — at where the remote player is looking, on every peer. The
body-turn-at-threshold is the existing body-yaw stream; no new wire field is needed.

---

## Corrections to land in the tree (independent of the head fix)

1. **sdk_profile.h:593** comment is wrong: `EBoneModificationMode` is
   `0=Ignore, 1=Replace, 2=Additive` (currently says `1=Add,2=Replace`). Fix the comment.
2. **sdk_profile.h::anim** is missing `ModBone_RotationSpace = 0x100` (and `ModBone_TranslationSpace
   = 0xFF`, `ModBone_ScaleSpace = 0x101`). Add if any ModifyBone-space code is ever written; not
   needed once the puppet stops using ModifyBone.
3. **puppet.cpp:657** comment `// BMM_Replace` next to `= 2` is wrong (it's Additive). Moot once the
   block is deleted per §5, but flag it so the mislabel doesn't get copy-pasted elsewhere.

## Modularity note

`puppet.cpp` is the touched file. Current size ~735 LOC (under the 800 soft cap). The §5 change is
net-negative (deletes the ModifyBone/Alpha block, adds a small `DriveHeadLookAtWorld`), so no
extraction is triggered. `remote_player.cpp` change is a few lines at the existing call site.
