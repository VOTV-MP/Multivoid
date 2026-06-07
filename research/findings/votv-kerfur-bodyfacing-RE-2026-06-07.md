# Kerfur BODY-facing desync — RE + coop sync (2026-06-07)

Sibling of the head-look RE (`votv-kerfur-headlook-AnimBP-RE-and-coop-sync-2026-06-07.md`
+ `votv-kerfur-headlook-BP-disassembly-2026-06-07.md`). Two agents reverse-engineered this
INDEPENDENTLY and CONVERGED byte-for-byte. Sources: `tools/bp_reflect.py` (repak +
kismet-analyzer) disassembly of the kerfur ACTOR BP + AnimBP, the CXXHeaderDump, IDA.

## The bug (user, fixed-landmark reference)
We sync kerfur NPCs host->client. Already synced: the actor TRANSFORM (location + body yaw via
`GetActorRotation`->`SetActorRotation`) AND the HEAD look (AnimBP `lookAt` FVector @0x2D90 ->
the two FAnimNode_LookAt head/neck nodes, gated by `customLookAt`@0x2E49 — v39). User reported a
VISUAL BODY desync with a KEYPAD (fixed world prop) reference: the kerfur's BACK faces the keypad
on the HOST while its FRONT faces the same keypad on the CLIENT — the body points OPPOSITE world
directions in the two instances, "from spawn." Yet we measured the actor's transform yaw and it
was BYTE-IDENTICAL on both (160.2 deg both, same loc). So the visible body facing is driven by
something OTHER than the actor root rotation we sync.

## Root cause (byte-exact, both agents)
The kerfur ACTOR BP — NOT the AnimBP — aims the visible body. `ExecuteUbergraph_kerfurOmega @28614`
(driven by `ReceiveTick`) does, once per tick:
```
@28564: ComposeRotators_ReturnValue := ComposeRotators(floor_lerp, rot{0, -90, 0})
@28614: Mesh.K2_SetWorldRotation(ComposeRotators_ReturnValue, false, _, false)
```
- `Mesh` = the inherited `ACharacter::Mesh` skeletal-mesh COMPONENT @0x0280 (CDO `CharacterMesh0`,
  RelativeRotation Yaw=-90). This is a WORLD rotation of a CHILD component — decoupled from the
  actor ROOT/capsule rotation that `K2_GetActorRotation`/our sync reads.
- `floor_lerp` (FRotator @0x07B8) RInterpTo-chases `floor` (FRotator @0x066C) at speed 10.
- `floor`'s IDLE/look branch (@29507 gate: `VSize(Velocity)<=0 && state in {0,1,2} && !isOnAtv &&
  !remoteControl && Dot(dirToPlayer, meshForw.Forward) < -0.6`) sets
  `floor := MakeRotator(0,0, FindLookAtRotation(self, GetPlayerPawn(self,0)).Yaw)` — i.e. TURN THE
  BODY TO FACE THE LOCAL PLAYER, engaging only when the player is roughly BEHIND the mesh (the head
  can't turn far enough = the head-rotation CLAMP is reached). `GetPlayerPawn(self,0)` = the LOCAL
  player per-peer -> divergent. (The moving branch @28499 sets `floor` from the `Forward` arrow,
  which rigidly follows the actor root -> peer-consistent once root yaw is synced.)
- The AnimBP does NOT rotate the body/root/spine: its only player-tracking nodes are the head/neck
  FAnimNode_LookAt (the v39 path); all ModifyBone nodes default `RotationMode=Ignore`.

## Why the mirror is wrong
The client mirror is parked via `puppet::DisableCharacterTicks` (`SetActorTickEnabled(false)`), so
`ExecuteUbergraph_kerfurOmega` — and thus `Mesh.K2_SetWorldRotation` — NEVER runs on it. The mirror
mesh therefore inherits the actor-root yaw we stream (`SetActorRotation`) * the static -90 default,
while the host mesh faces `floor_lerp - 90` (= the host player when idle). The -90 cancels; the
visible delta is `floor_lerp.yaw` (host, faces host) vs `actorYaw` (mirror). When the idle kerfur
faces ~away from the host, that's ~180 deg = "back-to-keypad on host, front-to-keypad on client,"
wrong from spawn. This is the BODY twin of the head-look bug (a per-peer "face the local player"),
but a SEPARATE field from `lookAt` — syncing `customLookAt` does NOT cover it.

## The fix (v40, shipped 2026-06-07)
Stream the host's RESOLVED mesh world yaw; write it onto the mirror's mesh component (mirror tick
is off -> nothing fights it, NO gate flag needed). Mirrors the v39 head design (read host-resolved
value -> write on mirror).
- New `ue_wrap::engine::GetComponentWorldRotation` (`K2_GetComponentRotation`) +
  `SetComponentWorldRotation` (`K2_SetWorldRotation`) — generic USceneComponent world-rotation
  thunks (Engine.hpp:17941/17950), reflected like Get/SetActorRotation.
- New `ue_wrap::puppet::ReadKerfurBodyYaw`/`DriveKerfurBodyYaw` — resolve `ACharacter::Mesh`@0x0280,
  class-gate on the kerfur AnimBP, read/write the mesh world yaw. `DriveKerfurBodyYaw` MUST run AFTER
  `SetActorRotation` (moving the actor root re-bases the child mesh world transform).
- protocol v40: `EntityPoseSnapshot` += `float bodyYaw` (40->44 B) + `kEntityPoseBitHasBodyYaw`.
  Host reads it in `TickPoseStream` (gated -> only kerfur-family set the bit); client interpolates
  it (shortest-arc, shared LerpWindow) + drives it in `Npc::ApplyToEngine` after SetActorRotation.
- Audit: SHIP (no CRIT/HIGH; the 2 extra UFunction calls/kerfur/tick are kerfur-only via the gate).

## Verify status
build CLEAN + npctest PASS (host streams `bodyYaw`, client drive class-gate passed, no crash). The
idle dev-spawned kerfur stays at its default mesh yaw (the floor-face-player branch needs an ACTIVE
kerfur), so the DYNAMIC body-turn is HANDS-ON-PENDING (load up, use the keypad/fixed-landmark check).

## Files
include/ue_wrap/sdk_profile.h (GetComponentRotationFn/SetWorldRotationFn), include/ue_wrap/engine.h
+ src/ue_wrap/engine.cpp (Get/SetComponentWorldRotation), include/ue_wrap/puppet.h + src/ue_wrap/
puppet.cpp (NpcBodyMesh extract + Read/DriveKerfurBodyYaw), include/coop/net/protocol.h (v40 +
bodyYaw + bit), include/coop/element/npc.h (interp members), src/coop/npc_pose_host.cpp (host read),
src/coop/npc_pose_drive.cpp (store+interp+drive), src/coop/npc_mirror.cpp (trust validation).
