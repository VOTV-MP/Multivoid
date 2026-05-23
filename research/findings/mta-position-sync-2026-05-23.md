# MTA:SA position sync — what MTA streams as a remote player's coordinate, and how it solves the "puppet floats during init" class of bug

Snapshot 2026-05-23. Per `[[feedback-check-mta-and-document]]`: before designing
a coop feature, read MTA's solution first.

Investigation prompted by the **puppet-floats-~10cm-after-init regression**:
we stream `source.mesh_playerVisible.GetComponentLocation().Z` and the
receiver writes `puppet.actor.Z = wire.z`, but during the source's first
1-5 s of life the visible mesh briefly sits above its eventual rest pose
(BP construction script + AnimBP touch `mesh.RelativeLocation.Z`). The
puppet faithfully replays those transients. Several iterations of "clamp
this on the wire" / "calibrate that on the receiver" have not fixed it,
because they are all crutches (per RULE №1).

This memo answers: **what coordinate does MTA stream, and why doesn't
MTA's remote player float?**

## TL;DR — MTA's pattern

| Concern | MTA's solution |
| --- | --- |
| What Z gets streamed | `m_pPlayerPed->GetPosition()->fZ` = the ped's `CEntitySAInterface::matrix->vPos.fZ` (the **entity transform root** — equivalent of UE's `AActor::GetActorLocation()` / `ACharacter::CapsuleComponent->GetComponentLocation()`). It is **NOT** the visible model's drawable world position; the model's bind-pose root offset is a render-time concern that lives below the entity transform and that the source never has to communicate. |
| Stable across init? | Yes. GTA:SA's ped entity transform is set once at `Spawn()` and only moves on physics steps after that. The model's root bone moves underneath it, the entity transform doesn't. |
| Crouch/duck | Streamed as a **separate boolean flag** in `SPlayerPuresyncFlags::data.bIsDucked`. Position field stays the same — the receiver runs a duck *task* (`CreateTaskSimpleDuck`) which produces the visual crouch posture. The wire never carries a different Z for crouching. |
| Vehicle / attached | Streamed as **relative position** under a contact-entity ID. Position is `actorPos - contactPos`, contact ID sent separately. |
| Settling at source | None at source — the source streams its current entity Z every tick from the moment puresync starts. There is no warm-up gate, no skip-first-N. |
| Settling at receiver | `SetFrozenWaitingForGroundToLoad(true)` on **local** player spawn only (waits for streamed geometry to load so the camera-owner doesn't fall through unloaded world). Remote players are not frozen — they just receive position + duck flag and run the same `SetTargetPosition` interpolator as steady-state. |
| Mesh-vs-entity drift | Solved entirely by *which coordinate is on the wire* — the entity transform doesn't have init transients, so the visible mesh (which is parented to it) doesn't either. There is no "filter out the source's transient mesh Z" code anywhere in MTA. The wire payload precludes the problem. |
| Under-floor "fix hack" | Exists (`CClientPed::UpdateUnderFloorFix`, `CClientPed.cpp:5490`) — but it is for a *different* failure mode: GTA's ped sometimes sinks through a floor that streamed in late. It detects "remote is stationary but local is ≥10 cm off in any axis" and snaps the local to the remote. It is NOT a transient-filter; it is a divergence-correction. |

## File map

### Sender (where MTA picks the position to put on the wire)

`Client/mods/deathmatch/logic/CNetAPI.cpp:1154-1171`:
```cpp
// Player position
CVector vecActualPosition;
pPlayerModel->GetPosition(vecActualPosition);    // <-- the entity transform
CVector vecPosition = vecActualPosition;

if (bInContact) {                                // attached to vehicle/object
    BitStream.Write(pContactEntity->GetID());
    CVector vecOrigin;
    pContactEntity->GetPosition(vecOrigin);
    vecPosition -= vecOrigin;                    // relative coord
}

SPositionSync position(false);
position.data.vecPosition = vecPosition;
BitStream.Write(&position);
```

`CClientPed::GetPosition` (`CClientPed.cpp:510-540`):
```cpp
void CClientPed::GetPosition(CVector& vecPosition) const
{
    if (IsFrozen())               vecPosition = m_matFrozen.vPos;
    else if (m_pPlayerPed)        vecPosition = *m_pPlayerPed->GetPosition();
    // ... vehicle / attach branches
    else                          vecPosition = m_Matrix.vPos;
}
```

`CPlayerPedSA`'s `GetPosition` (inherited from `CEntitySA`, `CEntitySA.cpp:289-309`):
```cpp
CVector* CEntitySA::GetPositionInternal()
{
    return &m_pInterface->matrix->vPos;   // the entity transform's vPos
}
```

So the chain is: `CNetAPI` -> `CClientPed::GetPosition` ->
`m_pPlayerPed (CPlayerPedSA)` -> `CEntitySAInterface::matrix->vPos`. This
is the GTA-engine equivalent of UE4's `AActor::GetActorLocation()` (the
actor root component's world position), NOT the skeletal mesh component's
world position.

### Flags (the rest of the body state the wire carries)

`Shared/sdk/net/SyncStructures.h:543-572`:
```cpp
struct SPlayerPuresyncFlags {
    bool bIsInWater, bIsOnGround, bHasJetPack,
         bIsDucked, bWearsGoogles, bHasContact,
         bIsChoking, bAkimboTargetUp, bIsOnFire,
         bHasAWeapon, bSyncingVelocity, bStealthAiming,
         isReloadingWeapon, animInterrupted, hangingDuringClimb;
};
```

`bIsDucked` is a **plain bit**, not a position offset. Receiver-side
`CNetAPI::ReadPlayerPuresync` (`CNetAPI.cpp:997`):
```cpp
pPlayer->Duck(flags.data.bIsDucked);
```
which (`CClientPed.cpp:4329-4355`) creates a `TaskSimpleDuck` on the
remote ped — the engine's own duck task produces the visual crouch and
adjusts the *visual* mesh accordingly. The entity transform doesn't move.

### Receiver (how the wire position lands on the remote ped)

`CNetAPI::ReadPlayerPuresync` (`CNetAPI.cpp:991`):
```cpp
pPlayer->SetTargetPosition(position.data.vecPosition, TICK_RATE, pContactEntity);
```

`CClientPed::SetTargetPosition` (`CClientPed.cpp:5370`) opens a 100 ms
LERP window (TICK_RATE = 100). `CClientPed::UpdateTargetPosition`
(`CClientPed.cpp:5430`) is called every frame and `SetPosition`s the ped
toward the target, with a snap threshold when the gap is too large. We
already mirror this on our side (see
`mta-pose-interpolation-2026-05-23.md`).

### What's NOT in MTA

There is **zero** code in MTA's sender path that:

- Skips the first N frames after spawn.
- Compares "mesh world Z" to "entity Z" and filters transients.
- Clamps Z to "be no higher than X".
- Waits for "settling" before sending.
- Reads anything off the model bone hierarchy as the streamed position.

Searched `CNetAPI.cpp`, `CClientPed.cpp` for: `first|skip|warmup|settle|
init|stabili[sz]e|mesh.*world|bone.*Z|HEAD_RADIUS|GROUND_OFFSET|PED.*HEIGHT`
— nothing of the kind exists on the position sender path. The wire
*coordinate choice* makes filters unnecessary.

### Local-player ground-load freeze (NOT the same problem)

`CClientPed::Spawn` (`CClientPed.cpp:746-750`):
```cpp
if (m_bIsLocalPlayer)
{
    SetFrozenWaitingForGroundToLoad(true);
    m_iLoadAllModelsCounter = 10;
}
```

This is a **camera-owner-only** behaviour: the local ped is frozen at the
spawn coordinate until streamed geometry finishes loading. It is not
applied to remote peds. It is also a different problem from ours — it
exists so the camera-owner doesn't fall through unloaded world, not to
hide a wire-side transient.

## Mapping to VOTV

| MTA concept | VOTV equivalent | Notes |
| --- | --- | --- |
| `CEntitySAInterface::matrix->vPos` | `AActor::GetActorLocation()` (we already use it via `ue_wrap::engine::GetActorLocation`) | The actor root's world location. For a `Character`, this is the capsule centre. **Stable across BP/AnimBP touching `mesh.RelLoc.Z`.** |
| `m_pPlayerPed->GetPosition()` returning entity-transform Z | `mainPlayerPawn.RootComponent->GetComponentLocation().Z` (= the capsule comp's world Z) | Identical semantics. |
| `SPositionSync::vecPosition` on the wire | our `PoseSnapshot{x,y,z}` | We already have x/y as the actor root. **Today we send mesh Z, not actor Z** — that's the bug. |
| `bIsDucked` flag | A new bit in our protocol (or a new optional field) | We don't currently sync crouch as a flag at all; the source's mesh.RelLoc.Z dropping during a crouch happens to encode it in the streamed Z. That's accidental encoding — fragile and incorrect (it bleeds into the floating bug). |
| `SetTargetPosition` interpolator | `coop::RemotePlayer::SetTargetPose` + tick LERP | Already MTA-shape; not changing. |
| `UpdateUnderFloorFix` | Not needed for us yet (no streamed-geometry late-load failure mode). Skip until we hit one. | Out of scope. |
| `SetFrozenWaitingForGroundToLoad` (local only) | Not relevant — no streamed open-world loading in VOTV (the map is pre-loaded). | Out of scope. |

## Why our current approach is fundamentally wrong (RULE №1)

Streaming `mesh_playerVisible.GetComponentLocation().Z` is asking the
source to send "where the rendered body draws right now", which is a
function of: actor.Z **plus** capsule halfH **plus** mesh.RelLoc.Z
**plus** any BP/AnimBP/crouch math that touches mesh.RelLoc.Z. **All four
inputs vary independently during the first 1-5 s** (the BP construction
script writes mesh.RelLoc.Z; AnimBP root-motion settles; UE's auto-
crouch shim plays; UE's floor snap nudges actor.Z). So the wire value
oscillates even though the source is conceptually "standing still".

The receiver then writes `puppet.actor.Z = wire.z`. There is no way to
unwrap this on the receiver — the receiver doesn't know which of the
four source-side inputs produced the current wire Z, so it can't subtract
the right offset to get a stable puppet actor.Z.

**The transient clamp we ship today**
(`harness.cpp:194: out.z = std::min(meshWorld.Z, loc.Z - 1.f)`) is a
crutch in the canonical sense:

- It only handles the "spike up above actor centre" subset of the
  transients (BP construction script briefly setting mesh world Z to
  `actor.Z + 2.57` cm). It doesn't handle the "spike down" or the
  small ~10cm oscillations during settling that the user sees.
- It is decided at the trust boundary by inspecting two source values
  that have no business being compared. It hides the underlying
  encoding choice rather than fixing it.
- It bakes "the visible mesh is below the actor centre" as a hard
  assumption — but a crouched character (capsule shrinks; mesh
  RelLoc.Z rises in proportion to keep the feet planted) can violate
  that; we'd silently clamp away a legitimate crouch transient.

**MTA never asks this question because it puts the entity transform on
the wire, not the rendered mesh world position.** Our equivalent is
`GetActorLocation()` — which we already have and which we already send
as x/y but not Z.

## Recommendation — mirror MTA's exact pattern

### Step 1. Send actor.Z, not mesh.world.Z

In `harness.cpp::ReadLocalPose`, replace:

```cpp
if (void* meshComp = ue_wrap::puppet::GetMeshPlayerVisibleComponent(local)) {
    const ue_wrap::FVector meshWorld = ue_wrap::engine::GetComponentLocation(meshComp);
    out.z = std::min(meshWorld.Z, loc.Z - 1.f);    // the clamp crutch
} else {
    out.z = loc.Z;
}
```

with simply:

```cpp
out.z = loc.Z;    // actor transform Z -- the MTA equivalent.
                  // Capsule centre on a Character; rock-stable across
                  // BP/AnimBP/crouch math, which only touch mesh.RelLoc.Z
                  // beneath the actor transform.
```

This deletes the clamp, the meshComp lookup, the transient-rejection
comment block — all of it. RULE №2 — no migration baggage.

### Step 2. Receiver derives the visible-body offset ONCE at puppet spawn

The receiver already has the LOCAL player's `mesh_playerVisible`
component (it spawned the puppet by skinning it from the local). At
spawn time it can measure:
```cpp
float visibleOffsetZ = localMesh.GetComponentLocation().Z
                     - localActor.GetActorLocation().Z;
```

This is **the BP-authored RelLoc.Z**, projected into world Z by the
local's current pose. It is the same number every standing
mainPlayer_C has, because it comes from the same blueprint.

Then on every wire packet:
```cpp
puppet.actor.SetZ(wire.z);  // puppet root IS its mesh, so puppet.mesh.world.Z = wire.z
                            // -- wait, our puppet's root is the mesh comp,
                            // so this comment changes...
```

…actually our puppet IS a SkeletalMeshActor whose root is its mesh
component (per memory entry "remote_player_hijack_and_pose"). So the
receiver needs to write `puppet.actor.Z = wire.z + visibleOffsetZ` —
i.e., the puppet's actor.Z = source's actor.Z + the BP-authored mesh
offset. Verify by capturing on the source: `meshWorld.Z - actor.Z` once
the source is settled — that number should be constant. (Memory entry
"remote_player_open_issues" says it's `-halfH` and user-verified;
that value, captured once at spawn from the LOCAL mesh, IS the offset.)

### Step 3. Sync crouch as a flag, not encoded into Z

Add a `flags : uint8` (or extend the unused `_pad` byte of the header)
to `PoseSnapshot`:

```cpp
struct PoseSnapshot {
    float x, y, z;
    float yaw, pitch, headYawDelta;
    float speed;
    uint8_t flags;        // bit 0: crouched
    uint8_t _pad[3];
};
```

Source: `flags |= isCrouched ? 1 : 0;` (read from `ACharacter::bIsCrouched`
or VOTV's own crouch state, whichever is canonical).

Receiver: when the bit flips, drive the puppet's `Crouch()` / `UnCrouch()`
UFunction (or write `bIsCrouched` + replicate it locally) so the puppet
plays the crouch animation. The Z offset for crouch comes out of the
puppet's *own* crouch behaviour — `ACharacter::Crouch()` reduces the
capsule half-height and raises `mesh.RelLoc.Z` proportionally to keep
the feet on the ground; we don't have to encode any of this on the wire.

(This bumps the protocol version. Per the comment block in `protocol.h`,
that's a clean bump — RULE №2; no back-compat.)

### Step 4. Delete the dead receiver-side calibration code

Memory entry "remote_player_open_issues" reports four iterations
(4407fa7 -> 9765ad2 -> bfa91d2 -> c8e192b) trying to calibrate the
floating offset on the receiver. With actor.Z on the wire and a one-
shot `visibleOffsetZ` measured at puppet spawn, all four iterations'
calibration paths become unnecessary. They go (RULE №2).

The 30-second continued correction against VOTV's late-init revert (per
the memory entry) is a *separate* concern (VOTV reverts the puppet's
position back to its initial pose after a delay) and stays.

### Step 5. Verify

After (1)-(4), run the two-instance LAN harness
(`tools/lan-test.ps1`) with both peers at known stable poses. The
puppet should land at exactly the source's feet on first packet and
stay there through the 1-5 s init window. No floating, no spikes.

If we still see ≤1 cm jitter, that's UE's own substep / floor-snap
noise on actor.Z — out of our control, ignorable at the visual
threshold. If we still see ≥1 cm consistent offset, the
`visibleOffsetZ` measurement at puppet spawn was wrong (likely the
local was crouching at that moment, or had a transient mesh.RelLoc.Z)
— measure later (after a known-settled grace) or re-measure each
N seconds against the local's own settled state.

## Should we adopt this verbatim?

**Yes.** MTA's pattern is the canonical solution to exactly this class
of bug, proven over a decade of shipped multiplayer. The shape ports
1:1 because UE4 has the same separation (actor transform vs skeletal
mesh component) as GTA:SA (entity matrix vs RpClump). Our current
approach — streaming the rendered-mesh world Z — is what MTA explicitly
does not do, and it explains every floating regression we've shipped.

The implementation plan above is ~50 lines of code change (delete the
clamp in harness.cpp, capture `visibleOffsetZ` at puppet spawn, apply
it on each wire packet, add `flags` to `PoseSnapshot`, route crouch
through it) and deletes several hundred lines of crutch / calibration
code.

## References

- `reference/mtasa-blue/Client/mods/deathmatch/logic/CNetAPI.cpp:1107-1270` — `WritePlayerPuresync`
- `reference/mtasa-blue/Client/mods/deathmatch/logic/CNetAPI.cpp:828-1014` — `ReadPlayerPuresync`
- `reference/mtasa-blue/Client/mods/deathmatch/logic/CClientPed.cpp:510-540` — `CClientPed::GetPosition`
- `reference/mtasa-blue/Client/mods/deathmatch/logic/CClientPed.cpp:739-791` — `CClientPed::Spawn` (local ground-load freeze)
- `reference/mtasa-blue/Client/mods/deathmatch/logic/CClientPed.cpp:4329-4370` — `CClientPed::Duck` / `IsDucked`
- `reference/mtasa-blue/Client/mods/deathmatch/logic/CClientPed.cpp:5370-5487` — `SetTargetPosition` / `UpdateTargetPosition` / `UpdateUnderFloorFix`
- `reference/mtasa-blue/Client/game_sa/CEntitySA.cpp:289-309` — `CEntitySA::GetPosition*` (matrix->vPos)
- `reference/mtasa-blue/Shared/sdk/net/SyncStructures.h:216-243,543-572` — `SPositionSync`, `SPlayerPuresyncFlags`
- `reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CPlayerSpawnPacket.cpp` — spawn-time position packet

VOTV files to touch:
- `src/votv-coop/src/harness/harness.cpp::ReadLocalPose` (delete clamp, send actor.Z)
- `src/votv-coop/include/coop/net/protocol.h` (bump version, add flags byte, document)
- `src/votv-coop/src/coop/remote_player.cpp` (capture `visibleOffsetZ` at spawn from local mesh, apply on every drive)
- `src/votv-coop/src/ue_wrap/puppet.cpp` (route crouch flag -> puppet Crouch/UnCrouch UFunction)
