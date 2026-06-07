# VOTV kerfur HEAD-LOOK sync — AnimBP BP-disassembly ground truth + coop design (2026-06-07)

**Goal:** the kerfur mirror's head points where the *client's* local player is, not where the
*host* kerfur is actually looking (user hands-on: "kerfur was looking in another direction on
client, but on host kerfur looked on host"). This doc RE's the kerfur AnimBP head-look pipeline
byte-exact and recommends the authoritative sync.

**Method / sources (all byte-exact, no guessing):**
- AnimBP kismet disassembly via `tools/bp_reflect.py` (repak + kismet-analyzer) →
  `research/bp_reflection/AnimBlueprint_kerfurOmega_regular.json`. Offset-aware ubergraph CFG via
  `research/bp_reflection/_cfg.py` → `kerfuranim_cfg.txt` (134 stmts / 5070 bytes; every jump lands
  on a statement boundary).
- The AnimBP **PropertyAccessLibrary** (FastPath copy table) decoded from the ClassExport
  (`Exports[10]/Data[4]` = `PropertyAccessLibrary` → `PathSegments`/`SrcPaths`/`DestPaths`/`CopyBatches`).
- The AnimBP **CDO** node defaults (`Exports[11]` = `Default__AnimBlueprint_kerfurOmega_regular_C`).
- Actor BP `research/bp_reflection/kerfurOmega.json` (`ExecuteUbergraph_kerfurOmega`) for the external
  writers of `Anim.lookAt`/`Anim.customLookAt`.
- Offsets cross-checked vs `CXXHeaderDump/AnimBlueprint_kerfurOmega_regular.hpp` +
  `kerfurOmega.hpp`, and vs `src/votv-coop/include/ue_wrap/sdk_profile.h::anim`.
- IDA (`VotV-Win64-Shipping.exe.i64`): shipping build is symbol-stripped; the engine AnimNode
  evaluators are not named. Not needed — the AnimBP property-copy wiring is fully serialized in the
  cooked asset (the authority), and the world-vs-component question is proved from the BP math itself.

---

## TL;DR

The kerfur head-look is driven by **`lookAt` (FVector @0x2D90, a WORLD location)**, fed every tick by
two AnimBP FastPath copies into the two `FAnimNode_LookAt` nodes' `LookAtLocation`:
- copy #16: `lookAt` → `AnimGraphNode_LookAt_1.LookAtLocation` (`BoneToModify=head`, @0x1730, Alpha 1.0)
- copy #17: `lookAt` → `AnimGraphNode_LookAt.LookAtLocation`   (`BoneToModify=neck`, @0x18E0, Alpha 0.5)

`lookAt` is computed **per-peer from the LOCAL `PlayerCameraManager(0)`** (BUA block @1431 when
`customLookAt==false`), or written by the actor BP to a local-player-derived world loc (when
`customLookAt==true`). Either way each peer aims at ITS OWN local player → the desync.

`headLookAt` (@0x2E3C) copies into `AnimGraphNode_ModifyBone.Rotation` (@0x2C60) but that node's
`RotationMode=Ignore` natively → **inert in normal kerfur play.** `lookingAtPlayer` only gates two
state-machine transitions, not the head twist.

**RECOMMENDATION = Design C (location).** HOST reads `lookAt` @0x2D90 (3 floats, world). CLIENT
writes `lookAt` @0x2D90 on the mirror's AnimInstance each tick AND sets `customLookAt=true` @0x2E49
so the mirror's own BUA stops overwriting `lookAt` with the client's camera. The native FastPath
copy then aims both LookAt nodes at the host's world target. No node-poking, no ModifyBone override.

---

## Q1 — How is `lookAt` (@0x2D90) computed, and is it what the LookAt nodes consume?

**`lookAt` is a WORLD location.** Computed in the ubergraph (entry from `BlueprintUpdateAnimation`
which is just `CALL ExecuteUbergraph(3688)`):

- BUA enters at 3688, runs the head/locomotion math, then `JUMP @15`:
  - `@15: IFNOT(customLookAt) JUMP @1431`
  - `@1431` (customLookAt==FALSE branch):
    ```
    @1431 CallFunc_GetPlayerCameraManager_ReturnValue := GetPlayerCameraManager(self, 0)
    @1457 CallFunc_K2_GetActorLocation_ReturnValue   := <camMgr>.K2_GetActorLocation()
    @1507 lookAt := CallFunc_K2_GetActorLocation_ReturnValue     <-- LOCAL player-0 camera world loc
    @1534 JUMP @29
    ```
  - `@29` then computes `lookingAtPlayer` (a dot-product gate, see Q3) using `pawn` + the same camera.
  - When `customLookAt==TRUE`, block @29..@1431's `lookAt:=camera` is SKIPPED → `lookAt` keeps
    whatever an external writer last set (Q2/Q5).

`lookAt` IS what the LookAt nodes consume — via the AnimBP **FastPath property-copy table**
(`PropertyAccessLibrary.CopyBatches`, run by the engine each `UpdateAnimation`). Decoded copies
(`SrcPath[i] → DestPath[i]`, `Type` from the batch), the relevant two:

| # | type   | source (class var) | dest (AnimNode field written each tick) |
|---|--------|--------------------|------------------------------------------|
| 16| Struct | `lookAt`           | `AnimGraphNode_LookAt_1.LookAtLocation` (head, @0x1730) |
| 17| Struct | `lookAt`           | `AnimGraphNode_LookAt.LookAtLocation`   (neck, @0x18E0) |

There is **no `EvaluateGraphExposedInputs` function for either LookAt node** (the function index has
EvaluateGraphExposedInputs only for BlendSpacePlayer/TwoBoneIK/TwoWayBlend). The LookAt nodes are fed
**only** by these two batched FastPath copies — not by the ubergraph, not by a per-node exposed-input
thunk. So `lookAt` (the class member) is the single source of truth the nodes read.

**Proof `lookAt` is WORLD-space (no IDA needed):** BUA @3688–4008 derives `headLookAt` as
`MakeRotFromZX( lookAt − (Mesh.GetSocketLocation('neck') + (0,0,1)·k), … )`. `GetSocketLocation`
returns a WORLD location; subtracting it from `lookAt` to get a look direction is only meaningful if
`lookAt` is also WORLD. Combined with `@1457 lookAt := PlayerCameraManager.GetActorLocation()` (world),
`lookAt` is unambiguously a world coordinate. Hence the FastPath copy feeds a WORLD point into
`FAnimNode_LookAt.LookAtLocation`.

## Q2 — What does `customLookAt` (@0x2E49) gate?

**`customLookAt` is exactly the "don't auto-recompute lookAt" gate.** BUA @15:
`IFNOT(customLookAt) JUMP @1431`. So:
- `customLookAt == false` → BUA OVERWRITES `lookAt` with the local `PlayerCameraManager(0)` location
  every tick (block @1431).
- `customLookAt == true`  → BUA SKIPS that overwrite; `lookAt` retains whatever was last written into
  it (by the actor BP, or by us).

This is the lever that lets an external writer pin `lookAt`. **Confirmed it gates `lookAt` and nothing
else** (it is referenced nowhere else in the AnimBP except this single `@15` JumpIfNot).
`customLookAt` is NOT a FastPath copy source (it does not appear in the 19-entry copy table) — it is a
plain bool the BP graph branches on, so a direct memory write to @0x2E49 is honored by the very next
BUA tick.

## Q3 — What do `lookingAtPlayer` (@0x2E01) and `isFace` (@0x2E48) gate?

- **`lookingAtPlayer` does NOT gate the head twist.** BUA @515 computes it as a *dot-product proximity
  test* (`Dot(meshRight…, dirToCamera) >= −0.6` ⇒ roughly "the player is within ~127° of my facing")
  and writes it. It is consumed ONLY as two state-machine transition conditions, via FastPath copies:
  - copy #0: `lookingAtPlayer` → `AnimGraphNode_TransitionResult_7.bCanEnterTransition`
  - copy #2: `lookingAtPlayer` → `AnimGraphNode_TransitionResult_5.bCanEnterTransition`
  i.e. it picks an idle/look STATE (a body/blendspace anim), not the head-bone aim. (This matches the
  puppet live-diagnostic: head LookAt Alpha was 1.0/0.5 while `lookingAtPlayer==false`.)
- **`isFace` (@0x2E48):** copy #? n/a — `isFace` feeds the ubergraph at `@3475: TwoWayBlend.None :=
  Conv_BoolToFloat(isFace)` (block @3475), i.e. it's the **Alpha of a TwoWayBlend** (the `AnimNode_
  TwoWayBlend` @0x2B30) that blends in the "face" pose layer. It is NOT head-look related. (The actor's
  face is a separate `AkerfusFace_C` actor; `isFace` just toggles a body-pose blend.)

So neither is the head-look on/off; the head-look is *always on* (static node Alphas) and aims at
`lookAt`.

## Q4 — How is `headLookAt` (@0x2E3C) computed/used?

- **WRITTEN by BUA** every tick: `@4008 headLookAt := MakeRotFromZX(lookAt − neckAdj, Cross(...))`
  (block @3688). It is a **world-space FRotator** look-direction derived from `lookAt`.
- **Copied** to two ModifyBone nodes via FastPath:
  - copy #5: `headLookAt` → `AnimGraphNode_ModifyBone.Rotation`   (@0x2C60, `BoneToModify=head`)
  - copy #6: `headLookAt` → `AnimGraphNode_ModifyBone_1.Rotation` (@0x2A28, `BoneToModify=head`)
- **BUT inert in normal play:** CDO default for `AnimGraphNode_ModifyBone` (@0x2C60) is
  `RotationMode = None` (= `BMM_Ignore`). With RotationMode=Ignore the copied `Rotation` is not applied
  to the bone. So natively, `headLookAt` does nothing visible — the head aim comes entirely from the
  two `FAnimNode_LookAt` nodes (Q1). (The player-puppet path in `puppet.cpp::DriveAnimBP` force-sets
  RotationMode=Replace itself to *make* ModifyBone @0x2C60 drive the head — that is a puppet override,
  not the kerfur's native behavior. Do NOT assume the native kerfur uses ModifyBone for head-look.)

Convention: `headLookAt` is a world-space MakeRotFromZX rotation (pitch/yaw/roll where +Z axis points
at the target). It is a *derived read-only* by-product; streaming it is pointless because (a) it's
inert natively and (b) it's recomputed from `lookAt` each tick anyway.

## Q5 — Do the actor's `targetLoc`/`TargetActor` feed `lookAt`?

**No — the head-look target is independent of the AI movement target.** `AkerfurOmega_C::targetLoc`
(@0x0638) and `TargetActor` (@0x0648) are the *navigation/move* target; they are written/read only in
movement code (`targetLocation()`, `move()`, the ubergraph move logic) and never assigned into
`Anim.lookAt`. The only writers of `Anim.lookAt` / `Anim.customLookAt` (actor BP
`ExecuteUbergraph_kerfurOmega`):
- `[152] anim.customLookAt := false` — a reset (in a damaged/idle path; `spd:=400` nearby).
- `[1350] camMgr := GetPlayerCameraManager(self,0); [1351] loc := camMgr.GetActorLocation();
   [1352] anim.lookAt := loc; [1353] anim.customLookAt := true` — the "isSpooky" kill state
   (`isSpooky:=true; setFace(); … spd:=50; state:=1; move(false)`). Note even this CUSTOM path picks
   the LOCAL `PlayerCameraManager(0)` → still per-peer-local.

So: in **every** branch the kerfur's head-look resolves to the **local** player/camera. There is no
native path that makes `lookAt` consistent across peers. The desync is structural, not a missing flag.

---

## Why the mirror looks wrong (root cause, stated plainly)

The client mirror's AnimInstance ticks its own BUA. With `customLookAt==false` (the resting state),
BUA @1431 sets `mirror.lookAt = client's PlayerCameraManager(0).GetActorLocation()` every tick → the
FastPath copies aim both LookAt nodes at the CLIENT'S camera. The host kerfur's `lookAt` (the host
player, or its scripted target) is never transmitted, so the mirror cannot know it. Same mechanism for
the `customLookAt==true` spooky state (host picks host-camera, client picks client-camera).

---

## RECOMMENDATION — Design C (stream the world `lookAt`)

Stream the host kerfur's resolved world look-point and pin it on the mirror; let the kerfur's OWN
native `FAnimNode_LookAt` nodes do the aiming. This is minimal, matches the engine's own data flow,
and needs zero node-poking.

### Host (authoritative read) — per kerfur, per pose tick
- Read **`lookAt`** = AnimInstance FVector @ **0x2D90** (reflected name `lookAt` on
  `AnimBlueprint_kerfurOmega_regular_C`). World cm, 3×float32 = 12 bytes.
- (Optional, 1 byte) read `customLookAt` @0x2E49 to tell the client whether the host's look is the
  "auto camera-track" vs a scripted custom target — not required for correctness (the world point is
  the same kind of value either way), but lets the client skip the head-look entirely if you ever want
  "only sync custom looks." Recommended: **always sync** (simplest, always correct).

The AnimInstance pointer is the live one on the mirror/host mesh:
`SkeletalMeshComponent.AnimScriptInstance` (`USkeletalMesh_AnimScriptInstance`, the same accessor
`puppet.cpp::LiveAnimInstance` already uses). For a real host kerfur the mesh is
`AkerfurOmega_C` → its skeletal mesh comp; the AnimInstance is also reachable via
`AkerfurOmega_C::Anim` @0x0688 (`UAnimBlueprint_kerfurOmega_regular_C*`).

### Client (authoritative write) — per mirror, EACH tick (same cadence as the body pose drive)
1. Write **`lookAt`** = FVector @ **0x2D90** on the mirror's AnimInstance = the streamed world point.
2. Write **`customLookAt = true`** (bool @ **0x2E49**). **This is mandatory** — without it the mirror's
   BUA @1431 overwrites `lookAt` with the client camera the very next tick and the head snaps back.

That's it. The mirror's native FastPath copies (#16/#17) then push our `lookAt` into both
`FAnimNode_LookAt` nodes' `LookAtLocation` and the head/neck aim at the host's world target. The neck
node (Alpha 0.5) + head node (Alpha 1.0) + `LookAtClamp=45°` are class defaults — identical on both
peers — so the resulting head pose matches the host.

### Ordering / clobber analysis (why C is robust)
- BUA writes `lookAt` (only when `customLookAt==false`). We set `customLookAt=true`, so BUA's @1431
  branch is dead → BUA never touches `lookAt`. Our write survives.
- The FastPath copy `lookAt → LookAtLocation` runs each `UpdateAnimation` *after* the BP event update
  and *before* the AnimGraph evaluates (UE4.27 `EAnimPropertyAccessCallSite` batch). So as long as our
  `lookAt` write lands before the mirror's AnimInstance updates this frame (drive it in the same place
  the body pose is driven — the existing per-tick mirror drive), the copy carries our value into the
  nodes that same frame. Even if a frame races, `customLookAt=true` guarantees BUA can't fight us, so
  at worst the head is one frame stale — never wrong-direction.
- No need to disable the LookAt nodes or touch ModifyBone (unlike the *player* puppet). The player
  puppet disables LookAt + drives ModifyBone *because a player's head is a camera/controller view with
  no world look-target*; the kerfur's head IS a world look-target, so we use the node the kerfur
  already uses.

### Reflected offsets to add (currently missing)
`reflected_offset.cpp` already resolves `lookingAtPlayer`/`headLookAt`/`isFace` by name; ADD:
- `AnimBP_kerfur_lookAt`        → `VC_DEFINE_OFFSET(..., AnimBPKerfurRegularClass, L"lookAt")`   (FVector, 0x2D90)
- `AnimBP_kerfur_customLookAt`  → `VC_DEFINE_OFFSET(..., AnimBPKerfurRegularClass, L"customLookAt")` (bool, 0x2E49)
Name-based resolution (not the hard offset) keeps this recook-proof, consistent with the existing
kerfur AnimBP offsets.

### Wire format
Extend the kerfur EntityPose payload by 12 bytes (`float lookAtX/Y/Z`, world cm). 1 optional byte
(`customLookAt`) if you choose conditional sync. Unreliable lane is fine (it's a continuous value like
the body pose; a dropped frame just reuses the last `lookAt`). Bump protocol version.

### Why NOT Design R (rotation / ModifyBone)
- `headLookAt` is natively inert (`ModifyBone.RotationMode=Ignore`), so "stream the rotation" means
  *also* replicating the player-puppet's node surgery (zero LookAt Alphas + force ModifyBone Replace).
  That fights the kerfur's native graph for no benefit and loses the `LookAtClamp`/interp the design
  intends.
- The host's "resolved head rotation" would have to be `headLookAt` (world MakeRotFromZX) or a derived
  bone rotation; converting world→component→bone correctly on the mirror is strictly more fragile than
  shipping the world point the engine already knows how to consume.
- A world LOCATION is the engine's native interface here and is automatically peer-consistent (the host
  player and its client-side puppet share a world position via the existing player-pose sync), so the
  same `lookAt` value is correct on every peer with zero coordinate gymnastics.

---

## Exact answer (one line)

HOST reads `lookAt` (FVector world @0x2D90). CLIENT writes `lookAt` @0x2D90 **and** `customLookAt=true`
(bool @0x2E49) on the mirror's AnimInstance each tick. Native FastPath copies #16/#17 then aim
`FAnimNode_LookAt` (head @0x1730 / neck @0x18E0) `LookAtLocation` at the host's world point. Do not
touch `lookingAtPlayer`, `headLookAt`, or the ModifyBone nodes.
