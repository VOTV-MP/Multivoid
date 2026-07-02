# VOTV kerfur AnimBP head-look — BP DISASSEMBLY + PropertyAccess ground truth (2026-06-07)

> **2026-07-02 CORRECTION — §2 Q3's "gates state-machine transitions ... NOT the LookAt nodes'
> on/off" is WRONG in its consequence:** the LookAt nodes live INSIDE the very state those
> transitions select (`lookAtPlayer` in "New State Machine_1"), so the transition gate IS the
> head-look on/off. Topology proof + fix: `votv-puppet-head-freeze-backturned-RE-2026-06-24.md`
> top banner (`5b2cb5ff`). The data-flow facts and Design C in this doc stand.

**Question:** how does the kerfur head-look animate, and what is the cleanest way for a CLIENT
mirror (AI suppressed, AnimBP still ticking) to make its head point where the HOST kerfur's head
points?

**Method (reusable):** `tools/bp_reflect.py` extracted + disassembled
`AnimBlueprint_kerfurOmega_regular` from the cooked `.pak` (repak + kismet-analyzer). The
offset-aware CFG (`research/bp_reflection/_cfg.py` → `kerfur_anim_uber_blocks.txt`) traced
`ExecuteUbergraph` byte-exactly (134 stmts, 5070 bytes, computed offsets land on statement
boundaries). The **`PropertyAccessLibrary`** copy table was read from the AnimBP ClassExport's
serialized properties (the UE4.27 AnimBP "fast-path" copies, which are NOT bytecode). IDA
(`VotV-Win64-Shipping.exe.i64`) confirmed the FastPath dispatcher excludes the LookAt nodes from
the per-node EvaluateGraphExposedInputs path. Sources cited inline.

> **This corrects the prior project understanding** (puppet.cpp:566-575 + sdk_profile.h:542-552),
> which said the FAnimNode_LookAt nodes are driven by an opaque target and the ONLY way to decouple
> the head is to zero their Alpha + drive ModifyBone @0x2C60. In fact **the LookAt nodes aim at the
> AnimBP `lookAt` FVector** (copied into their `LookAtLocation` every tick via PropertyAccess), and
> `lookAt` is overwritten by BUA each tick ONLY when `customLookAt == false`. So writing `lookAt` +
> `customLookAt = true` cleanly redirects the head — no Alpha-zeroing, no ModifyBone override needed.

---

## 1 — The data flow that aims the head (byte-exact)

### 1a. `BlueprintUpdateAnimation` is a trampoline
`BlueprintUpdateAnimation` (CXX dump AnimBlueprint_kerfurOmega_regular.hpp:120) →
`UBERFRAME[K2Node_Event_DeltaTimeX] = DeltaTimeX; ExecuteUbergraph(3688); RETURN`. All logic is in
`ExecuteUbergraph_AnimBlueprint_kerfurOmega_regular`.

### 1b. The `customLookAt` gate (ubergraph @15 — the decisive branch)
Entry 3688 runs the foot-IK / animWalk math, then `@4689: JUMP @15`:
```
@  15: IFNOT(customLookAt) JUMP @1431      ; customLookAt==FALSE -> @1431
@1431: GetPlayerCameraManager(0).K2_GetActorLocation()   -> CallFunc_K2_GetActorLocation_ReturnValue
@1507: lookAt := CallFunc_K2_GetActorLocation_ReturnValue ; *** the ONLY write to `lookAt` ***
@1534: JUMP @29
@  29: ... (camera-relative dot-product) ...
@ 515: lookingAtPlayer := (Dot(headDir, camDir) >= -0.6)
```
- **`customLookAt == FALSE`** → BUA jumps to @1431 and **overwrites `lookAt` (@0x2D90) with the
  LOCAL player camera's world location** (`GetPlayerCameraManager(self,0).K2_GetActorLocation()`),
  then falls into @29.
- **`customLookAt == TRUE`** → the @15 jump is NOT taken → execution **falls straight to @29,
  SKIPPING the @1507 `lookAt := camera` write.** `lookAt` is left at whatever was written
  externally. `lookingAtPlayer` is still recomputed at @515 in both paths (it gates state-machine
  transitions, not the head — see §3), but it does not touch `lookAt`.

`lookAt` is written in EXACTLY ONE place in the whole ubergraph (grep of the block dump:
`@1507: lookAt :=` is the sole `lookAt :=`). `customLookAt` appears only at the @15 read.
`BlueprintBeginPlay` (@4781→@4694) writes only `pawn`/`skeletalMesh`/`controller`; it does NOT
write `lookAt` or `customLookAt`. **So an externally-written `lookAt` survives the whole tick iff
`customLookAt == true`.**

### 1c. `lookAt` → the LookAt nodes (PropertyAccessLibrary, runs every tick, NOT bytecode)
The AnimBP ClassExport carries a `PropertyAccessLibrary` (UE4.27 AnimBP property-access fast-path).
Its single `CopyBatches[0].Copies` (19 copies) executes inside `FAnimInstanceProxy::UpdateAnimation`
each frame. The relevant entries (SrcPath segment → DestPath segments, from `PathSegments` +
`SrcPaths`/`DestPaths`):

| Copy | Type | Source var | → Dest node.pin |
|------|------|-----------|-----------------|
| **[16]** | **Struct** | **`lookAt`** | **`AnimGraphNode_LookAt_1` (head node @0x1730) `.LookAtLocation`** |
| **[17]** | **Struct** | **`lookAt`** | **`AnimGraphNode_LookAt`   (neck node @0x18E0) `.LookAtLocation`** |
| [5] | Struct | `headLookAt` | `AnimGraphNode_ModifyBone`   (head @0x2C60) `.Rotation` |
| [6] | Struct | `headLookAt` | `AnimGraphNode_ModifyBone_1` (neck @0x2A28) `.Rotation` |
| [0..4,7..15,18] | — | rise/lookingAtPlayer/footFoot_*/torsoOffset/offsetCenter/grabAlpha/animWalkAlpha/walk_blend/isOnAtv | transition-results, foot-IK, blendspace, etc. |

`FAnimNode_LookAt::LookAtLocation` is at node-base **+0x140** (sdk_profile.h:561). So the copies hit
absolute instance offsets **0x1730+0x140 = 0x1870** (head) and **0x18E0+0x140 = 0x1A20** (neck).

IDA (`sub_1414E0A70`, the FastPath dispatcher, RVA 0x1414E0A70) confirms the LookAt nodes are NOT
in the per-node `EvaluateGraphExposedInputs` path — only TwoWayBlend/TwoBoneIK/BlendSpacePlayer have
those (matches the function list: there is no `EvaluateGraphExposedInputs_*_LookAt_*`). The LookAt
inputs come exclusively from the PropertyAccess batch above.

### 1d. The LookAt node static defaults (from the AnimBP CDO)
Both `AnimGraphNode_LookAt_1` (head) and `AnimGraphNode_LookAt` (neck):
- `BoneToModify` = **'head'** / **'neck'** respectively.
- `LookAtTarget.bUseSocket = false`, Bone/Socket = 'None' → **no socket target → the node uses
  `LookAtLocation`** (a WORLD-space target point; the `LookAt_Axis = +Z, bInLocalSpace=true` is the
  *bone aim axis*, not the location frame).
- `LookAtLocation` default = (X=100,Y=0,Z=0) — overwritten each tick by the `lookAt` copy.
- `Alpha = 1.0` (head) / `0.5` (neck), `bAlphaBoolEnabled = true`, `AlphaInputType = Float`,
  `InterpolationType = None`, `LookAtClamp = 45°`, `LODThreshold = -1`.

**The two ModifyBone copies ([5],[6]) are effectively inert on a real NPC:** all 7 ModifyBone nodes
have `RotationMode` defaulting to `BMM_Ignore` (serialized as absent/None in the CDO) and default
`Rotation`=(0,0,0). With mode=Ignore the node ignores its `.Rotation` pin. So even though `headLookAt`
is copied into `ModifyBone.Rotation`, it does nothing unless something sets `RotationMode != Ignore`.
**The head actually rotates via the two FAnimNode_LookAt nodes aiming at `lookAt`.** (`headLookAt` is
a by-product the AnimBP computes — see §4 — kept wired to the inert ModifyBone slots.)

---

## 2 — Answers to the five questions

**Q1. How does BUA compute `lookAt`? Is it a WORLD location, and does it feed the FAnimNode_LookAt
target?**
Yes. When `customLookAt==false`, BUA sets `lookAt = GetPlayerCameraManager(self,0).K2_GetActorLocation()`
(@1507) — a **world location** (the local player's camera position). `lookAt` is then copied into BOTH
LookAt nodes' `LookAtLocation` by PropertyAccess copies [16]/[17] every tick. So `lookAt` IS the world
point the head/neck aim at. (Default `lookAt` = (0,500,0); CDO.)

**Q2. Does `customLookAt==true` make BUA SKIP auto-computing `lookAt`?**
**Yes — exactly.** `@15: IFNOT(customLookAt) JUMP @1431`. customLookAt==true ⇒ the @1507
`lookAt := camera` write is skipped (fall-through to @29). customLookAt==false ⇒ BUA overwrites
`lookAt` with the camera location each tick. `customLookAt` is the externally-respected gate. It
defaults to **false** (absent from CDO) and is never written by the graph, so writing it true is
stable across ticks.

**Q3. What do `lookingAtPlayer` and `isFace` gate?**
- `lookingAtPlayer` (@0x2E01): computed at @515 = `Dot(headDir, cameraDir) >= -0.6` (is the player
  roughly in front). It is a **PropertyAccess source for two `FAnimNode_TransitionResult.bCanEnterTransition`
  inputs** (PathSegments [0]/[6] → DestPaths [0]/[6]) — i.e. it gates **state-machine transitions**
  (idle/look state selection), NOT the LookAt nodes' on/off. (Confirms the old live-diag note: LookAt
  Alpha stayed 1.0/0.5 with lookingAtPlayer=false.) It does NOT change the look *target* — the target
  is always `lookAt`. Default true (CDO).
- `isFace` (@0x2E48): feeds `AnimGraphNode_TwoWayBlend.None` (the face-blend alpha, ubergraph @3512
  via `Conv_BoolToFloat(isFace)`). It is a **facial-expression blend** toggle, unrelated to head
  aiming. Default false.

**Q4. Is `headLookAt` written by BUA, and what is its convention?**
**Written every tick** at @4008: `headLookAt = MakeRotFromZX( (lookAt - (neckSocket + offset)), Cross((lookAt-neckSocket), (0,0,1)) )`
— i.e. a rotation whose **+Z (up) axis points from the neck toward `lookAt`** (MakeRotFromZX builds a
rotator from a Z basis + an X hint). It is a *derived* FRotator (a world-space rotation aiming the
neck→target direction along Z). It is copied into the (inert, mode=Ignore) ModifyBone.Rotation slots.
**It is a readable resolved rotation, but it is NOT what moves the head** (the LookAt nodes do that
from `lookAt`). Using it for Design R would require us to flip the ModifyBone mode to Replace AND
account for its Z-up (not standard pitch/yaw) convention — strictly worse than Design C.

**Q5. Is the head-look target the same as actor `targetLoc` / `TargetActor`?**
**Independent.** The head target is the AnimBP `lookAt` FVector, set by BUA to the **player camera**
location (`GetPlayerCameraManager(0)`), with no read of the owning `AkerfurOmega_C`'s `targetLoc`
(@0x638) or `TargetActor` (@0x648). The kerfur AnimBP head always tracks the (local) player camera by
default; it does not follow the NPC's AI move/target. (This is *why* the bug exists: on the client the
camera is the CLIENT's local player, so the mirror's head turns to the wrong person.)

---

## 3 — Root cause of the bug (precise)

`customLookAt` defaults to **false** on every kerfur AnimInstance. With customLookAt==false, BUA sets
`lookAt = local-player-camera-location` each tick (@1507), and the LookAt nodes aim the head/neck at
it (copies [16]/[17]). On the HOST the local camera is the host player → head tracks host player. On
the CLIENT mirror the local camera is the CLIENT player → the mirror's head tracks the *client's*
player, not the host's target. Nothing about the head target is networked today, so it diverges.

---

## 4 — RECOMMENDED DESIGN: **Design C (location)** — stream `lookAt`, write `lookAt` + `customLookAt=true`

Design C is strictly cleaner than Design R here and is the natural fit because the engine already
wires `lookAt → LookAtLocation` for us. World locations are peer-consistent (same world origin), so
the head will aim identically on every peer.

### HOST reads (per tick, from the host kerfur's live AnimInstance):
- **`lookAt` : `FVector` @ instance offset `0x2D90`** (AnimBlueprint_kerfurOmega_regular.hpp:88).
  This is the resolved world look-target BUA already computed this frame (= host player camera loc,
  or whatever any future customLookAt producer set). Stream the 3 floats (X,Y,Z).
  - Reuse the existing reflected accessor pattern; add `AnimBP_kerfur_lookAt()` alongside the
    `AnimBP_kerfur_*` accessors in `ue_wrap/reflected_offset.{h,cpp}` (the project resolves these by
    reflection rather than hardcoding 0x2D90 — matches RULE 3 / recook-safety).

### CLIENT writes (per tick, on the mirror's live AnimInstance, AFTER its BUA ran or any time before
the anim worker evaluates — simplest is in the same per-tick mirror drive that already sets CMC):
1. **`lookAt` (FVector @0x2D90) = streamed value.**
2. **`customLookAt` (bool @0x2E49) = `true`.**  ← the load-bearing flag: it makes the mirror's BUA
   SKIP the @1507 `lookAt := localCamera` overwrite, so our written `lookAt` survives the tick and the
   PropertyAccess copies [16]/[17] carry it into both LookAt nodes' LookAtLocation.

That is the entire client-side requirement. The native `lookAt → LookAtLocation` copies + the LookAt
nodes (Alpha 1.0/0.5, already enabled by class default) do the rest. No Alpha writes, no ModifyBone
writes.

### BUA-clobber analysis (the critical risk + why it's mitigated):
- The ONLY per-tick writer of `lookAt` is @1507, and it is **gated behind `customLookAt==false`**.
  Setting `customLookAt=true` removes that writer entirely. ⇒ **no clobber of `lookAt`.** Verified:
  the block dump has exactly one `lookAt :=`, inside the @1431 block reached only when customLookAt is
  false.
- `customLookAt` itself is **never written by the graph** (only read at @15) and **never written by
  BeginPlay** ⇒ once we set it true it stays true; we can re-assert it each tick for safety at zero
  cost.
- The `LookAtLocation` node pins ARE overwritten every tick by PropertyAccess copies [16]/[17] — but
  their SOURCE is our `lookAt`, so that is exactly what we want (we must NOT write `LookAtLocation`
  directly; write `lookAt` and let the copy propagate). Writing `LookAtLocation` directly would be
  clobbered by the copy; writing `lookAt` is not.
- Ordering: the PropertyAccess batch + the @1507 gate both run inside the mirror's own BUA/proxy
  update. As long as our `lookAt`+`customLookAt=true` write lands BEFORE the mirror's anim ticks that
  frame (or we write every game-thread tick, which the existing mirror drive already does), the value
  is correct. If a 1-frame ordering race is ever observed, set both fields in the same place the
  mirror's CMC.Velocity is written (already proven game-thread, pre-anim-eval, in the mirror drive).

### Why NOT Design R (rotation / ModifyBone):
- The head's motion is produced by the FAnimNode_LookAt nodes from `lookAt`, NOT by ModifyBone (all
  ModifyBone nodes are `RotationMode=Ignore` by default). To use Design R we would have to (a) zero
  both LookAt Alphas, (b) flip head ModifyBone @0x2C60 to Replace, (c) convert `headLookAt`'s Z-up
  `MakeRotFromZX` convention into a head-bone-local pitch/yaw — i.e. re-fight the exact battle the old
  puppet code fought. Design C uses one native pipeline the engine already runs. `headLookAt` is a
  derived rotation that does not drive the visible head; streaming it would be the wrong field.
- (The existing PLAYER-PUPPET head drive in `puppet.cpp::DriveAnimBP` uses the ModifyBone/Alpha-zero
  recipe because a player puppet has NO meaningful world look-target to stream — it streams the
  remote player's view pitch + body-relative yaw delta. For an NPC mirror we DO have a world target
  (`lookAt`), so Design C is available and superior. These can coexist: player puppets keep
  DriveAnimBP; NPC mirrors use the lookAt/customLookAt path.)

### One-line verdict
**Host reads `lookAt` (FVector @0x2D90); client writes `lookAt` (@0x2D90) + `customLookAt = true`
(bool @0x2E49) each tick on the mirror's AnimInstance.** The native PropertyAccess copy
(`lookAt → AnimGraphNode_LookAt(_1).LookAtLocation @+0x140`) + the two LookAt nodes then aim the
head/neck at the host's world look-point on every peer. `customLookAt=true` is the load-bearing flag
that stops the mirror's BUA from overwriting `lookAt` with the client's own camera (@1507, gated at
ubergraph @15).
