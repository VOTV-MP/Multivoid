# Physics-object pickup — implementation architecture blueprint (2026-05-23)

The feature-dev:code-architect agent's verbatim blueprint after consuming
the converged deep-research deliverables. Sources:
- `research/findings/physics-object-pickup-coop-plan-2026-05-23.md` (consolidated)
- `research/findings/mta-object-pickup-sync-2026-05-23.md`
- `research/findings/votv-physics-interaction-surface-2026-05-23.md`
- IDA agent report (in conversation): RVA-renamed PhysicsHandle natives + ProcessEvent hook strategy
- COOP_SCOPE entry: commit `1c620be`

## Architecture decision — 4 clean layers (one per project pattern)

| Layer | Files | Responsibility |
|---|---|---|
| **Engine wrap** | `ue_wrap/prop_wrap.{h,cpp}` (new) | Aprop_C offset reads (Key, IsHeavy, GetStaticMesh) + UFunction calls (SetSimulatePhysics, SetPhysicsLinearVelocity). NO coop state. Game thread only for UFunction paths. |
| **Source observer** | `coop/grab_tracker.{h,cpp}` (new) | Reads mainPlayer_C grab state after each `smoothGrab` ProcessEvent + on grab/drop events. Produces `GrabEvent`, `HeldPropPose`, `ReleaseEvent` structs. NO network logic. |
| **Coop orchestrator** | `coop/prop_sync.{h,cpp}` (new) | PropKeyRegistry (Key→Aprop_C* map), PropInterpolator (MTA linear LERP, twin of RemotePlayer), PropSyncManager (ownership state, packet routing). Game thread only. |
| **Protocol v4** | `coop/net/protocol.h` (modify), `session.{h,cpp}` (modify) | Bump version to 4. New `ReliableKind` values: GrabRequest/GrabAck/GrabDeny/Release. New `PosePacketV4` = PoseSnapshot + optional HeldPropRecord (gated by `hasProp` byte). Includes `bCrouched` bit reservation for the Option G follow-up. |

## Reuse of existing patterns

- `ue_wrap::game_thread::ProcessEventDetour` (already installed) — extended with a multi-observer fixed-array (8 entries max), pointer-compare hot path (same cost as existing interceptor). Pre-observers fire BEFORE original (used by `dropGrabObject`/`throwHoldingProp` to snapshot state before engine clears it); post-observers fire AFTER (used by `pickupObject`/`smoothGrab`/`switchToHeavyDrag`).
- `ReliableChannel` (stop-and-wait ARQ) — extended via new `ReliableKind` values; no structural changes.
- `RemotePlayer` linear LERP interpolation (`kInterpWindowMs=75`) — `PropInterpolator` is a structural twin applied to a prop transform.
- Satellite-ACharacter pattern from Bug 2 (Plan B2) — extended for Stage 7 puppet drag-mode bool.

## Wire protocol v4 (additions)

```cpp
inline constexpr uint16_t kProtocolVersion = 4;  // bump from 3

enum class ReliableKind : uint8_t {
    Join         = 1,  // existing
    GrabRequest  = 2,  // client→host: "I want to grab propKey"
    GrabAck      = 3,  // host→all: "propKey is now held by holderPeer"
    GrabDeny     = 4,  // host→requester: "propKey already held or invalid"
    Release      = 5,  // holder→host (then host→all): "propKey released"
};

// Reliable payloads (full layout in the architect's blueprint):
struct GrabRequestPayload  { uint32_t propKey; uint8_t grabMode; uint8_t grabSeq; FVector localAnchor; float grabDist; };  // 26B
struct GrabAckPayload      { uint32_t propKey; uint16_t holderPeer; uint8_t grabMode; uint8_t grabSeq; FVector localAnchor; float grabDist; };  // 28B
struct GrabDenyPayload     { uint32_t propKey; uint16_t currentHolder; uint8_t grabSeq; uint8_t _pad; };  // 8B
struct ReleasePayload      { uint32_t propKey; uint8_t releaseSeq; uint8_t isThrow; uint8_t _pad[2]; FVector releaseLoc; float[4] releaseRot; FVector linearVel; FVector angularVel; };  // 54B

// Unreliable per-tick held-prop pose (piggybacked on PosePacketV4):
struct HeldPropRecord {
    uint32_t propKey;
    FVector  worldLoc;
    float    worldRotX, worldRotY, worldRotZ, worldRotW;  // full quat (16B)
    FVector  linearVel;
    uint8_t  poseSeq;     // RFC1982 stale-drop
    uint8_t  postThrow;   // 1 = post-throw low-rate stream
    uint8_t  _pad[2];
};  // 44B, static_assert checked

struct PosePacketV4 {
    PacketHeader  header;
    PoseSnapshot  pose;
    uint8_t       hasProp;   // 0 = no held prop (52B total), 1 = HeldPropRecord follows (93B)
    uint8_t       _pad[3];
    HeldPropRecord prop;     // valid only when hasProp == 1
};
```

Total: 52 B without held prop, 93 B with — well within `kMaxPacketBytes = 256`.

## Conflict resolution

- **Host-authoritative grab ownership.** Client sends `GrabRequest`; host either broadcasts `GrabAck` (prop ungrabbed) or sends `GrabDeny` to requester only.
- **Per-prop monotonic `grabSeq`** (8 bits, wraps) rejects stale `GrabRequest`/`Release` after ownership change — mirrors MTA's `ucSyncTimeContext`.
- **Host writes its own ack immediately** (no round trip on host's local grab).
- **Same-frame double-grab**: first-to-host wins; loser receives reliable `GrabDeny` and rolls back local prediction.

## Receiver-side strategy

On `GrabAck`:
1. `PropKeyRegistry::Find(propKey) → Aprop_C*`
2. `engine::SetSimulatePhysics(prop->StaticMesh, false)` (prop becomes kinematic on the puppet's machine)
3. `PropInterpolator::Init()` snaps to current world transform
4. `RemotePlayer::SetHeldPropState(propKey, isDragging)` drives puppet AnimBP drag-mode bool (Stage 7)

Per `HeldPropRecord` tick:
- `interp.SetTarget(loc, rot, poseSeq)` opens a 75 ms LERP window (twin of RemotePlayer pose path)
- `PropSyncManager::Tick` advances all active interps + writes to engine via `SetActorLocationAndRotation`

On `Release`:
1. Validate `releaseSeq == grabSeq + 1` (stale reject)
2. `SetActorLocationAndRotation(propActor, releaseLoc, releaseRot)`
3. `SetSimulatePhysics(true)` + `SetPhysicsLinearVelocity` + `SetPhysicsAngularVelocityInDegrees`
4. Clear interpolator + RemotePlayer state
5. Host enters low-rate post-release streaming (3 s window with rest-detection — mirrors MTA's syncer post-push behavior)

## Build sequence (7 stages)

### Stage 1: Hook infrastructure (log only, ZERO wire)

1. Add `sdk_profile.h` prop-namespace offsets + function name constants
2. Add `RegisterObserver`/`RegisterPreObserver` to `game_thread.{h,cpp}` (fixed array of 8, pointer compare)
3. In `harness.cpp`: register observers for `smoothGrab`, `pickupObject`, `pickupObjectDirect`, `dropGrabObject`, `throwHoldingProp`, `switchToHeavyDrag` against `mainPlayer_C`. Body = log line with `self` pointer.
4. Build, deploy, run LAN test → verify NO regressions on existing pose/chat tests.
5. Gate: hands-on E-press → hook lines visible in log; per-frame `smoothGrab` log while holding.

### Stage 2: State snapshot at hook fire

1. Create `prop_wrap.{h,cpp}` with `GetKey`, `IsHeavy`, `IsStatic`, `GetStaticMesh`
2. In `pickupObject` post-observer: read `grabbing_actor@+0x07D0` → resolve Key → read mode/anchor/distance. Log: `"GRAB HOOK: propKey=X grabMode=Y anchor=(a,b,c) dist=D"`
3. In `smoothGrab` post-observer: read held prop world transform. Log: `"HELD_POSE propKey=X loc=(x,y,z)"`
4. In `dropGrabObject` PRE-observer: snapshot grab state BEFORE engine clears it
5. Gate: propKey reads non-zero; held pose changes each frame; finalLoc near visible drop point.

### Stage 3: Protocol v4 bump (wire only, no payload yet)

1. Add v4 structs + ReliableKind values to `protocol.h`. Bump version.
2. Add `SetLocalPoseV4`/`TryGetRemotePoseV4` to session
3. Net thread serializes conditionally (52 B or 93 B)
4. `harness.cpp` calls SetLocalPoseV4 with `hasProp=false`
5. Build, LAN test → existing pose/chat/nameplate still works
6. Gate: LAN test PASS, no regression on prior behaviour.

### Stage 4: Reliable GRAB/RELEASE messages

1. Create `grab_tracker.{h,cpp}` (the source-side observer pipeline)
2. Create `prop_sync.{h,cpp}`: PropKeyRegistry, PropSyncManager, packet routing
3. Wire `Session::SendReliable` for the new ReliableKind values
4. Host: on local grab → broadcast GrabAck immediately. Client: send GrabRequest → wait for GrabAck.
5. Gate: LAN test logs show GRAB/RELEASE exchange end-to-end.

### Stage 5: Receiver kinematic toggle + transform drive

1. Add `engine::SetSimulatePhysics`/`SetPhysicsLinearVelocity`/`SetPhysicsAngularVelocityInDegrees`/`SetActorLocationAndRotation`
2. Implement `PropSyncManager::OnRemoteGrab` (kinematic toggle + interp init)
3. Implement `PropInterpolator` (copy RemotePlayer LERP path, adjust for PropPose)
4. Per-tick: advance interp + write to engine
5. Gate: bag follows holder visually on remote side.

### Stage 6: Release velocity apply + post-throw streaming

1. Implement `PropSyncManager::OnRemoteRelease` (physics on + velocity apply)
2. `GrabTracker::OnThrow` reads velocity from prop transform delta or `GetActorVelocity`
3. Post-release streaming: host streams briefly with `postThrow=1` bit until rest detected (3 s window, rest epsilon)
4. Gate: throw arc matches between source and receiver; landing within ~50 cm after 3 s.

### Stage 7: Puppet AnimBP drag-mode bool

1. PREREQ: UE4SS or IDA probe of `AnimBlueprint_kerfurOmega_regular_C` to find the drag/carry bool variable name + offset
2. Extend `RemotePlayer::ApplyToEngine` to write the bool from `SetHeldPropState`
3. Gate: puppet body posture leans forward while remote peer drags a heavy prop.

## Open prerequisites (surfaced by the architect)

1. **Prop-Key cross-peer agreement** (HARD blocker for Stages 4-6 on REAL two-machine sessions). Spawn paths must route through host so both peers see the same Key.
2. **bCrouched coordination** — naturally folds into the v4 bump in Stage 3.
3. **Ragdoll sync coordination** — also folds into v4.
4. **AnimBP drag-mode field research** for Stage 7 (research prereq).
5. **`K2_SetActorLocationAndRotation` existence check** for Stage 2/5 (might be 2 separate calls instead — `FindFunction` verify).

## Files to create / modify (summary)

New:
- `src/votv-coop/include/ue_wrap/prop_wrap.h`
- `src/votv-coop/src/ue_wrap/prop_wrap.cpp`
- `src/votv-coop/include/coop/grab_tracker.h`
- `src/votv-coop/src/coop/grab_tracker.cpp`
- `src/votv-coop/include/coop/prop_sync.h`
- `src/votv-coop/src/coop/prop_sync.cpp`

Modified:
- `src/votv-coop/include/coop/net/protocol.h`
- `src/votv-coop/include/ue_wrap/sdk_profile.h`
- `src/votv-coop/include/ue_wrap/engine.h`, `src/ue_wrap/engine.cpp`
- `src/votv-coop/include/ue_wrap/game_thread.h`, `src/ue_wrap/game_thread.cpp`
- `src/votv-coop/include/coop/net/session.h`, `src/coop/net/session.cpp`
- `src/votv-coop/include/coop/remote_player.h`, `src/coop/remote_player.cpp`
- `src/votv-coop/src/harness/harness.cpp`
- `src/votv-coop/CMakeLists.txt`
- `tools/lan-test.ps1`

## Test strategy

- Per-stage LAN test gate (see Stages 1-7 above). Stage 1+2 require hands-on E-press; Stage 3 onward verified by autonomous LAN test.
- Add `grab` scenario to `lan-test.ps1` for Stage 4+ (autonomous pickup via `CallFunction` dispatched `pickupObjectDirect` against a hardcoded prop).
- Post-ship audit checklist (per [[feedback-post-ship-audit]]):
  1. Performance: `PropSyncManager::Tick` does NOT scan registry per frame; observer table fires O(N=8) pointer compares per ProcessEvent (acceptable).
  2. Thread safety: `HeldPropRecord` net-thread→game-thread handoff uses the same mutex pattern as `remotePose_`.
  3. MTA fidelity: confirm grab-seq stale rejection mirrors `ucSyncTimeContext`; release velocity apply mirrors `SET_ELEMENT_VELOCITY` RPC.
  4. RULE 1 crutch hunt: no broad suppression; `SetSimulatePhysics(false)` at specific grab-ack site only.
  5. RULE 2 migration: existing `ReliableKind::Join` path still works with new kinds; switch has graceful default branch.
