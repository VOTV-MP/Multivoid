# Remote player visibility: body meshes + 3D text marker — 2026-05-22

**Goal** (user): make the second player (the orphan `mainPlayer_C`) VISIBLE so a
player can see the other in-world, and stick a 3D text label on top so its
location is obvious. Plus a one-click `.bat` for hands-on testing.

## What we learned

### Player body is fully present (user confirmed)
`mainPlayer_C` is first-person but HAS a full animated body skeletal mesh that the
owner sees when looking down, and which casts shadows. Component enumeration
(`reflection::ChildObjectsOf`, Outer == actor) shows the spawned orphan has the
SAME 72 components as the local player, including the body meshes:
`SkeletalMeshComponent` named **playermodel**, **mesh_playerVisible**, **arms**,
**CharacterMesh0**. So the skin is NOT missing on the orphan — it comes from the
class defaults on `SpawnActor`.

### SuperStruct offset CONFIRMED = 0x40
A safe pointer-compare probe (`reflection::DebugProbeSuperStructOffset`) found the
Actor class's qword at **0x40** equals the Object class pointer (Actor's super).
So `UStruct::SuperStruct = 0x40` (NOT the 0x30 I'd have guessed). Recorded in
`sdk_profile::off::UStruct_SuperStruct`.

### ShowBody — force body meshes visible (WORKS)
An unpossessed orphan never runs the gameplay code that unhides/sets up the
third-person body, so we force it: `coop::RemotePlayer::ShowBody()` enumerates the
orphan's `SkeletalMeshComponent`s and calls `engine::SetComponentVisible` =
`USceneComponent::SetVisibility(true,true)` + `SetHiddenInGame(false,true)` on
each. Live: "shown mesh_playerVisible / playermodel / arms / CharacterMesh0"
(shown=4), no crash. NOTE param name: `SetHiddenInGame`'s bool is **`NewHidden`**
(no `b` prefix); `SetVisibility`'s is `bNewVisibility`.

### NOT yet visually confirmed — placement vs camera direction
Screenshots after placing the orphan along the player's ACTOR forward did not show
the body. The likely cause: the first-person CAMERA look direction (control
rotation) != the pawn's actor-forward, so the orphan landed off-screen. Placing it
via the actual **Camera component** eye+forward (`engine::GetComponentLocation` /
`GetComponentForwardVector` on the `CameraComponent` named "Camera") is the fix —
those calls return correct values; they're implemented but the `show` scenario
currently uses actor-forward (a camera-placement attempt earlier appeared to stall
and was reverted to keep `play` robust; re-investigate). Whether the body ALSO
needs deeper BeginPlay mesh setup (asset/pose) beyond visibility is still open.

### 3D text marker (ATextRenderActor) — implemented, NOT yet visually verified
`engine::SpawnTextMarker(location, text, worldSize)` spawns an `ATextRenderActor`
(renders as real geometry -> works in SHIPPING, unlike debug-draw text which is
stripped), finds its `TextRenderComponent`, and sets the text. Param names/sizes
confirmed by a runtime FProperty dump:
- `UKismetTextLibrary::Conv_StringToText`: input **`inString`** (FString, 16),
  ReturnValue **FText (24 bytes)**.
- `UTextRenderComponent::SetText`: **`Value`** (FText, **16 bytes** — the
  TSharedRef core; we copy 16 of the 24 captured).
- `UTextRenderComponent::SetWorldSize`: **`Value`** (float).
First attempt used wrong names (`InString`, `World Size`) -> empty/default marker.
Corrected. Still TODO: verify it renders; set a bright color (night scene) via
`SetTextRenderColor(FColor)`; face it at the camera (`SetActorRotation`, FRotator);
place via camera ray; make it follow a moving orphan.

## New capabilities this turn (all in `ue_wrap`, principle 7)

- `reflection::ChildObjectsOf(outer)` — enumerate an actor's components.
- `reflection::DebugProbeSuperStructOffset()` — safe offset probe.
- `sdk_profile::off::UStruct_SuperStruct = 0x40`.
- `engine::GetActorForwardVector`, `GetComponentLocation`, `GetComponentForwardVector`.
- `engine::SetComponentVisible` (SetVisibility + SetHiddenInGame).
- `engine::SpawnTextMarker` (ATextRenderActor + Conv_StringToText + SetText/SetWorldSize).
- `ue_wrap/types.h`: `FRotator`, `FColor` (for upcoming facing/color).
- `coop::RemotePlayer::ShowBody()`.
- Harness scenarios: `skin` (component dump + super probe), `show` (place in front
  + body + marker + screenshot), **`play`** (spawn orphan beside player + body +
  marker, then hand control to the user).

## Hands-on testing — the .bat files

- `tools/play-coop.bat [scenario]` — deploys the standalone mod (UE4SS disabled),
  sets the scenario (default `play`), launches windowed. `play` skips into
  gameplay, spawns the 2nd `mainPlayer_C` ~2 m beside you with body forced visible
  and a `P2` label above; you keep control to walk around and look. VALIDATED:
  spawn + ShowBody=4, no hang, no crash.
- `tools/stop-coop.bat` — restores UE4SS, removes the standalone mod.

## Next

1. Verify the marker renders; bright color + face-camera; camera-ray placement.
2. Confirm the body renders in view (camera-accurate placement); if still hidden,
   investigate deeper player BeginPlay mesh setup (asset assignment / anim).
3. Make the marker + body follow a moving (network-driven) orphan.
4. Then Phase 3 UDP transport -> drive RemotePlayer from snapshots.
