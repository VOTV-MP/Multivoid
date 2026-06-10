# VOTV ATV Phase 1 -- pose-stream implementation blueprint (2026-06-08)

Companion to `votv-ATV-quadbike-RE-and-coop-sync-design-2026-06-08.md` (the RE / offsets).
This doc is the BUILD blueprint: the exact files, the transport decision, the flows to copy
(with file:line), and the divergences from the NPC pose stream. Produced by a
feature-dev:code-explorer map of the NPC/kerfur pose-stream subsystem.

## Status (2026-06-08, this session)
- **ue_wrap/atv.{h,cpp} BUILT** (the engine substrate -- Principle-7, no net deps). The
  foundation: IsAtv/GetKeyString(Key@0x0618)/GetRootTransform/GetRootVelocity/GetOccupantPlayer
  (Player@0x05B0)/IsDriven(isDriven@0x05F7)/GetFuel/GetHealth/GetBrake + DriveMirrorTransform +
  PrepareMirror (sim-physics off + tick off + notifyRigidBody false). Uses actor-level engine
  helpers (Mesh@0x0570 IS the actor root, so GetActorLocation == the physics body).
- **REMAINING (next increment):** protocol structs + element::Atv + atv_sync + the session-layer
  pose transport + net_pump/event_feed wiring + audit + smoke. All specified below.

## The element/pose-stream pattern (what to copy)
- **element::Npc** (`include/coop/element/npc.h`) is the twin: subclass of `element::Element`,
  owns a `coop::LerpWindow` (the shared interp TIMING -- `include/coop/lerp_window.h`, reuse
  VERBATIM) + cur/target/error fields. `SetTargetNpcPose(snap)` (advance-before-rebase: Advance
  FIRST, then write target+error=(target-cur), then window_.Open(now,75ms)); `Tick()` per frame
  -> AdvanceInterp (apply dAlpha to errors) + ApplyToEngine (when dirty). HOST never interpolates
  (reads the live actor). Drive impl in `src/coop/npc_pose_drive.cpp`.
- **element::Element** base (`include/coop/element/element.h`): m_id/m_type/m_actor/m_internalIdx/
  m_mirror/m_ownerSlot. SetActor(void*,int32) sets BOTH ptr+idx. Validity = R::IsLiveByIndex(
  actor, GetInternalIdx()) NOT IsLive. **ADD `Atv` to the ElementType enum.**
- **Registry** (`include/coop/element/registry.h`): host `AllocHostId(e)` (range [1,32768)),
  client `RegisterMirror(id,e)`, `FreeId`/`UnregisterMirror` in ~Element, trust helpers
  `IsAllowedHostAllocatedEid(id)` / `IsAllowedSenderEid(senderIsHost,id)` -- call at every
  receiver boundary.
- **MirrorManager<T>** (`include/coop/element/mirror_manager.h`): host `AllocAndInstall(uptr,
  isHost=true)`, client `Install(wireEid, uptr, ownerSlot=-1)` (host-auth -> -1), `Snapshot(vec&)`
  (copy ptrs under lock, iterate without), `DrainAll()` (teardown). MirrorManager<Atv> is a pure
  host-auth manager (like Npc) -> DrainAll on disconnect.

## TRANSPORT DECISION (Phase 1)
The ATV body is ONE actor, full transform (it tips/flips -> needs pitch/yaw/roll, unlike the NPC
yaw-only EntityPoseSnapshot) + velocity. Two options:
1. **A new UNRELIABLE `AtvPose` datagram** (the NPC/prop pattern -- newest-wins, correct for
   continuous motion). Needs new Session methods: `SetLocalAtvPose` (GT write under localMutex_),
   `TakeRemoteAtvPose` (GT drain under remoteMutex_), `StoreRemoteAtvPose` (net thread, newest-
   wins) + a `MsgType::AtvPose` case in `Session::HandleMessage` + the per-peer send-loop stamp
   (`session.cpp:719-725` pattern). This is the heavy part (net-thread plumbing) but is the
   architecturally-correct transport (matches PropPose/EntityPose). **RECOMMENDED.**
2. A reliable AtvState at ~15-20Hz (simpler, no net-thread plumbing, rides SendReliable +
   event_feed) -- but reliable in-order ARQ LAGS under loss for continuous motion. Acceptable
   only as a stopgap; option 1 is the right shape.

`AtvPoseSnapshot` (sibling of PropPoseSnapshot, NOT EntityPoseSnapshot):
```
uint32_t elementId;                     // Atv Element id (host range)
float    x, y, z;                       // world cm (root mesh centre == actor loc)
float    pitch, yaw, roll;              // full rotation
float    velX, velY, velZ;              // linear velocity cm/s (mirror inherits on release)
uint8_t  occupantSlot;                  // 0xFF = unoccupied
uint8_t  stateBits;                     // bit0=isDriven, bit1=brake, bit2=broken
uint8_t  _pad[2];
```
`AtvPosePacket { PacketHeader header; AtvPoseSnapshot pose; }` (one ATV/datagram, no batch).
Reliable `AtvStatePayload { WireKey key; uint32 elementId; float fuel; float health; uint8 brake;
uint8 occupantSlot; uint8 broken; uint8 adopt; }` for Phase 2 fuel/health/brake on-change +
connect-snapshot (adopt=1, trust-gated senderSlot==0). Identity = Key@0x0618 (save-placed, stable
both peers -- verify with a keysHash diagnostic). Bump kProtocolVersion at impl time.

## Host stream (copy `npc_pose_host.cpp:79-149` TickPoseStream)
role==Host && connected -> AtvMirrors().Snapshot(elems) -> for each: IsLiveByIndex guard, read
ue_wrap::atv::GetRootTransform + GetRootVelocity + GetOccupantPlayer(->slot via players registry)
+ IsDriven/IsBrakeOn -> fill AtvPoseSnapshot -> s->SetLocalAtvPose(snap). (One ATV -> single snap,
not a batch.)

## Client apply (copy `npc_mirror.cpp:433-473` TickClientNpcs)
role!=Host -> s->TakeRemoteAtvPose(snap) (drain) -> float-validate -> AtvMirrors().Get(eid) ->
el->SetTargetAtvPose(snap); then Snapshot+el->Tick() each mirror. The mirror is materialised
(client) by resolving the AATV_C by Key (always-present world prop -- like base_window/grime) OR
on the first pose for a new eid; on materialise call ue_wrap::atv::PrepareMirror(actor) (sim
physics off + tick off + notifyRigidBody false) so the custom rig can't fight the streamed
transform.

## element::Atv divergences from element::Npc
- ctor `Atv() : Element(ElementType::Atv) {}`
- full rotation: cur/target/error Pitch_+Yaw_+Roll_ (replace yaw-only)
- add curVel_ (FVector, snapped) for release-inherit (Phase 3); add curOccupantSlot_ (uint8,
  0xFF) -- on change call the seat/unseat primitive
- DROP curLookAt_/curBodyYaw_ (kerfur-specific)
- ApplyToEngine: ue_wrap::atv::DriveMirrorTransform(actor, curPos_, {pitch,yaw,roll}) instead of
  SetActorLocation+DriveCharacterMovement (no CMC on the ATV)

## Occupant seating (Phase 1.5 -- new code, primitive exists)
On occupantSlot 0xFF->N: get RemotePlayer slot N's puppet, K2_AttachToActor to the mirror ATV's
playerHit@0x04F0 seat + SetActorHiddenInGame(puppet,true) (RE doc 3.1). On N->0xFF: detach +
unhide. Model on `engine_playerragdoll.cpp:211` AttachActorToRagdollBody (K2_Attach via
ParamFrame). NO existing seat code -- first user.

## Wiring sites
- net_pump.cpp: Install ~308 (after npc_sync::Install); Tick ~680-681 (TickAtvStream +
  TickClientAtvs after the npc ticks); connect-edge ~574 (RegisterExistingWorldAtvs +
  QueueConnectBroadcastForSlot); OnDisconnect (next to npc_sync::OnDisconnect).
- session.cpp/session_npc.cpp: SetLocalAtvPose / TakeRemoteAtvPose / StoreRemoteAtvPose +
  HandleMessage MsgType::AtvPose case + send-loop stamp.
- CMake: +ue_wrap/atv.cpp (DONE) +coop/element/atv.cpp +coop/atv_sync.cpp.

## File list (sizes all < 800)
- include/coop/element/element.h  -- add `Atv` to ElementType (trivial)
- ue_wrap/atv.{h,cpp}             -- engine substrate [BUILT this session]
- coop/element/atv.{h,cpp}        -- element::Atv twin (~80h/150cpp)
- coop/atv_sync.{h,cpp}           -- host stream + client apply + register + connect-snap (~40h/300cpp)
- coop/net/protocol.h             -- AtvPoseSnapshot/AtvPosePacket/MsgType::AtvPose (+AtvStatePayload Ph2)
- session_npc.cpp / session.h     -- the 3 Session pose methods + HandleMessage routing

## Gotchas (from the RE doc)
- Mounting Possess(ATV) unpossesses mainPlayer_C -> the mirror must NOT be possessed (kinematic
  orphan); host TickAtvStream guards by GetController()!=nullptr discrimination already used.
- health<=0 && speed>4 -> explode() is a HARD transition -> replicate as a discrete reliable edge
  (Phase 2), not by each mirror crossing zero independently (double-explode). Mirror MTA
  LastSyncedHealth.
- Key@0x0618 cross-peer stable (save-placed) -- verify keysHash host==client at install (a
  crafted ATV via crafted() gets a per-peer NewGuid -> Phase 2 EntitySpawn).
