// ue_wrap/engine.h -- high-level engine operations built on reflection.
//
// Engine-wrapper layer (principle 7): wraps specific engine UFunction calls
// behind a clean C++ API. No gameplay/network/coop state lives here -- just the
// marshaling to drive the engine through reflection::CallFunction (ProcessEvent).
//
// IMPORTANT: every call here invokes a UFunction, so it MUST run on the game
// thread (post via ue_wrap::game_thread::Post). Calling from another thread will
// crash the engine.

#pragma once

#include "ue_wrap/types.h"

namespace ue_wrap::engine {

// Run a console command (UKismetSystemLibrary::ExecuteConsoleCommand) -- the
// universal lever for "open <map>", "HighResShot ...", etc. Resolves the
// KismetSystemLibrary CDO + UFunction and a valid world context on first use
// (cached). Returns false if anything could not be resolved. Game thread only.
bool ExecuteConsoleCommand(const wchar_t* command);

// Load a VOTV save slot (e.g. a STORY save like "s_may2026") and enter gameplay
// via the game's own load entry: GameplayStatics::LoadGameFromSlot ->
// mainGameInstance_C::setSaveSlotObject + loadObjects=true -> open the gameplay
// map. The gameplay map is untitled_1 for every mode; the SAVE selects story vs
// sandbox. Returns false if the slot is missing/empty or load can't dispatch.
// Game thread only.
bool LoadStorySave(const wchar_t* slot);

// Spawn an actor of `actorClass` (a UClass*) at `location` via the deferred
// GameplayStatics pair (BeginDeferredActorSpawnFromClass + FinishSpawningActor)
// -- the same path the K2 SpawnActorFromClass node uses. Always spawns
// (CollisionHandlingOverride = AlwaysSpawn). Returns the spawned AActor* (a
// UObject*), or nullptr on failure. Game thread only.
// `inertPawn=true` makes a spawned APawn inert as a LOCAL player: in the deferred
// window (before BeginPlay) it zeroes AutoPossessPlayer / AutoPossessAI /
// AutoReceiveInput and sets bBlockInput, so the pawn never auto-acquires a
// PlayerController nor steals the local player's input/view. ROOT-CAUSE fix for
// the remote-pawn hijack (prevention, not post-hoc teardown).
void* SpawnActor(void* actorClass, const FVector& location, bool inertPawn = false);

// Split-API variant: call BeginDeferredSpawn -> patch class-specific fields on
// the returned actor (before Init/BeginPlay run) -> call FinishDeferredSpawn.
// Mirrors the K2 SpawnActorFromClass node's "Expose On Spawn" pattern. The
// transform passed to FinishDeferredSpawn MUST equal the one passed to
// BeginDeferredSpawn (the engine re-applies it post-construction).
// Phase 5G Inc 2: the wire-spawn path for non-prop garbage entities needs to
// stamp variantState (chipType, Shape, etc.) BEFORE Init rolls defaults --
// without this split, the Init POST observer would echo-broadcast a randomly-
// rolled state before the wire-supplied state lands.
void* BeginDeferredSpawn(void* actorClass, const FVector& location, const FRotator& rotation);
bool  FinishDeferredSpawn(void* actor, const FVector& location, const FRotator& rotation);

// AActor::K2_GetActorLocation on `actor`. Returns (0,0,0) if it cannot be called.
FVector GetActorLocation(void* actor);

// AActor::GetActorForwardVector on `actor` (unit facing vector). (0,0,0) on
// failure. Used to place something in front of the actor.
FVector GetActorForwardVector(void* actor);

// AActor::K2_GetActorRotation on `actor` (world rotation). Zero on failure. The
// pose-snapshot read for the network send path (we only wire yaw). Game thread only.
FRotator GetActorRotation(void* actor);

// AActor::GetVelocity on `actor` (world velocity, cm/s). Zero on failure. Its
// horizontal magnitude is the walk speed fed to the remote AnimBP locomotion
// blend (PoseSnapshot::speed -> RemotePlayer interp). Game thread only.
FVector GetActorVelocity(void* actor);

// AActor::K2_SetActorLocation on `actor` (teleport: bSweep=false, bTeleport=true
// -- snap to the absolute pose, the network pose-apply path). Returns the
// engine's success bool. Game thread only.
bool SetActorLocation(void* actor, const FVector& location);

// AActor::K2_SetActorRotation on `actor` (bTeleportPhysics=true). The network
// pose-apply path for orientation, and the diagnostic for whether the remote
// body follows its OWN rotation vs a global local-player read. Game thread only.
bool SetActorRotation(void* actor, const FRotator& rotation);

// AActor::SetActorTickEnabled. A remote pawn must NOT run the local-player
// per-frame BP EventTick (which re-applies view/post-process/exposure to the
// shared screen every frame -- the gamma stomp that survives a component
// destroy). Game thread only.
bool SetActorTickEnabled(void* actor, bool enabled);

// AActor::SetActorScale3D. World-space scale of the actor; used to shrink the
// nameplate world quad WITHOUT shrinking the WidgetComponent's render-target
// pixel count (bDrawAtDesiredSize=true couples RT pixels with quad cm at the
// widget's content desired size -- scaling the actor decouples visual size
// from texel density). Game thread only.
bool SetActorScale3D(void* actor, const FVector& scale);

// APawn::GetController on `pawn` -- returns the AController* (or nullptr).
void* GetController(void* pawn);

// AController::GetControlRotation on `controller` -- the view rotation the game
// drives from mouse-look. Reading it for a freecam gives smooth, game-native
// look with no raw-mouse handling. (0,0,0) on failure. Game thread only.
FRotator GetControlRotation(void* controller);

// Direct write to AController::ControlRotation (offset 0x0288). The engine reads
// this UPROPERTY next tick to derive view rotation; the BP-callable
// SetControlRotation UFunction does no extra clamping (just stores the value),
// so the direct write is equivalent and cheaper. Game thread only.
void SetControlRotation(void* controller, const FRotator& rot);

// K2_TeleportTo(DestLocation, DestRotation) -- the proper "teleport actor across
// the world" UFunction (returns bool: success). Unlike K2_SetActorLocation
// (which can be silently reverted by Character/CMC constraints when the move
// is far), K2_TeleportTo is designed to handle large teleports including
// across CMC's nav/floor checks. Returns the UFunction's bool return. Game
// thread only.
bool TeleportTo(void* actor, const FVector& location, const FRotator& rotation);

// GetActorBounds(bOnlyCollidingComponents, Origin, BoxExtent, bIncludeFromChildActors)
// -- the BP-callable that returns the actor's world-space axis-aligned bounding
// box (computed each tick from the rendered mesh's actual extent, NOT from
// bone hierarchy). Used for the puppet's "lowest visible point" Z derivation:
// bottom = Origin.Z - BoxExtent.Z. Returns false if unresolved. Game thread only.
bool GetActorBounds(void* actor, bool onlyColliding, FVector& outOrigin, FVector& outBoxExtent);

// APlayerController::SetViewTargetWithBlend(NewViewTarget, BlendTime) -- repoint
// the player's view to `newViewTarget` (e.g. a freecam ACameraActor), blending
// over `blendTime` seconds for a smooth cut. Game thread only.
bool SetViewTargetWithBlend(void* playerController, void* newViewTarget, float blendTime);

// Current view camera world location / rotation (APlayerCameraManager::
// GetCameraLocation / GetCameraRotation on the live manager). Used to seed a
// freecam at the player's eye. Zero on failure. Game thread only.
FVector GetCameraLocation();
FRotator GetCameraRotation();

// APawn::SpawnDefaultController on `pawn` -- spawns the pawn's AIControllerClass
// and possesses. Gives the body's AnimBP a valid controller (so it poses) with
// NO viewport/input/camera (AIController != PlayerController), so it does not
// hijack the local player. Game thread only.
bool SpawnDefaultController(void* pawn);

// APawn::DetachFromControllerPendingDestroy on `pawn` -- unpossess (the pawn
// keeps existing; only the controller link is severed). Game thread only.
bool DetachFromController(void* pawn);

// AActor::K2_DestroyActor on `actor` -- destroy it. Used to delete the orphan's
// stray 2nd PlayerController. Game thread only.
bool DestroyActor(void* actor);

// Find the UCharacterMovementComponent default subobject on a Character.
// Used by ue_wrap::puppet::SpawnPuppetMainPlayer to park the orphan's CMC
// tick (so the puppet doesn't fight our per-tick Velocity write) and by
// RemotePlayer::Spawn (via puppet.cpp) for the chain-measurement Z anchor.
// Returns nullptr if the pawn isn't a Character or its CMC isn't present.
void* GetCharacterMovementComponent(void* characterPawn);

// Write a FVector to a UMovementComponent's `Velocity` UPROPERTY (offset
// resolved once at startup via reflection: FindPropertyOffset on UMovementComponent).
// (Kept for potential future use; current v2 anim drive in RemotePlayer::
// ApplyToEngine writes Velocity via a direct memory store at the known
// 0xC4 offset rather than the reflection-resolved path.) Game thread only.
bool SetMovementVelocity(void* movementComp, const FVector& velocity);

// UActorComponent::SetComponentTickEnabled. Used to PARK the orphan
// puppet's CharacterMovementComponent in puppet::SpawnPuppetMainPlayer --
// the puppet's CMC would otherwise integrate gravity/walking + reset
// Velocity each tick, fighting our pose drive. Game thread only.
bool SetComponentTickEnabled(void* component, bool enabled);

// Direct field write of a UObject* slot at `byteOffset` inside `target`.
// Used for the runtime Aprop_C wire-key restore (remote_prop::OnSpawn
// rekey path) where a UFunction call would be heavyweight.
// Game thread only.
void WriteObjectField(void* target, size_t byteOffset, void* value);

// If `localPlayer` (a live AmainPlayer_C*) is currently grabbing `actor` via
// the LIGHT-grab path (mainPlayer.grabbing_actor @0x07D0 + grabHandle @0x0688),
// dispatch UPhysicsHandleComponent::ReleaseComponent on the PHC and clear
// the grabbing_actor slot. Returns true iff a release happened.
//
// 2026-05-25 (cross-peer destroy audit fix #3 -- moved here from
// coop::remote_prop per Principle 7: coop/ must not directly access engine
// struct offsets). The release-before-destroy ordering is critical:
// K2_DestroyActor leaves PHC.GrabbedComponent (@+0xB0) dangling, and
// UPhysicsHandleComponent::TickComponent dereferences that pointer next
// frame.
//
// Game thread only. No-op (returns false) if any input is null/dead.
bool ReleaseMainPlayerGrabIfHolding(void* localPlayer, void* actor);

// Eager-resolve the PHC.ReleaseComponent UFunction cache used by
// ReleaseMainPlayerGrabIfHolding. Call once during init when PhysicsHandle
// Component class is loaded (harness::InstallGrabObservers) so the first
// cross-peer PropDestroy receive doesn't hit a not-yet-resolved class and
// fall through to the warn-and-clear fallback. Idempotent. Returns true
// iff the cache is populated after the call. Game thread only.
bool WarmupPhcReleaseCache();

// UPhysicsHandleComponent::GrabbedComponent (the held UPrimitiveComponent*
// at the fixed offset documented in sdk_profile.h, IDA-confirmed against
// ReleaseComponent_Impl @0x142D7C670). Read once in PHC.ReleaseComponent
// PRE-observers (diagnostic logging of what's about to be released).
// Returns the component pointer, or nullptr if `phc` is null/dead or the
// slot is empty.
//
// Principle 7 (A-4, 2026-05-29): replaces the inline raw struct-offset
// deref in coop::grab_observer::GrabObserver_PHC_Release_PRE.
// Game thread only.
void* ReadPhysicsHandleGrabbedComponent(void* phc);

// Snapshot of AmainPlayer_C grab-state UPROPERTIES read in a single
// dispatch from adjacent reflected offsets. The 5 fields drive the
// pickup/drop diagnostic logs in coop::grab_observer's E-press / grab
// Timeline observers AND the net_pump held-prop replication path
// (grabbingActor + holdingActor cover the two morph paths: physics-
// handle props vs chipPile/clump-style "carry" props). Gameplay/coop
// code never touches these offsets directly (Principle 7). Returns
// false on null/dead pawn; `out` is then zero-initialised. `holdingActor`
// stays null if MainPlayer_holding_actor() isn't resolved in this build
// (the field was added in a later VOTV recook). Game thread only.
struct MainPlayerGrabState {
    void*  grabbingActor;  // AmainPlayer_C::grabbing_actor (PHC-held prop, or null)
    void*  holdingActor;   // AmainPlayer_C::holding_actor  (chipPile/clump morph carry, or null)
    bool   grabsHeavy;     // AmainPlayer_C::grabsHeavy     (PCC heavy-grab BP flag)
    bool   heavy;          // AmainPlayer_C::Heavy          (BP-side heavy state mirror)
    float  grabLen;        // AmainPlayer_C::grabLen        (Timeline current grab length)
};
bool ReadMainPlayerGrabState(void* mainPlayer, MainPlayerGrabState& out);

// Write both AmainPlayer_C::grabbing_actor + AmainPlayer_C::grabbing_component
// in a single dispatch. Used by the autotest harness's synthetic grab path
// where the test code drives UPhysicsHandleComponent.GrabComponentAtLocation
// + Release directly and must keep the BP-visible "what am I holding" mirror
// in sync (the BP layer reads these slots in attach-prop / drop-impulse
// timelines). Pass nullptr/nullptr for the release/clear case. Returns false
// on null/dead pawn. Game thread only.
//
// Principle 7 (post-A-4 follow-up, 2026-05-29): replaces 4 inline raw
// struct-offset writes in harness::autotest's grab-pickup + release-throw
// sequences.
bool WriteMainPlayerGrabbingPair(void* mainPlayer, void* actor, void* component);

// Diagnostic: log every FProperty on a UClass (name, offset, size). Used to
// verify a property offset we resolved via reflection (e.g. confirm we got
// the real UMovementComponent::Velocity offset and not a sibling property
// of the same FName). Walks the class's own properties only (no SuperStruct
// climb). Game thread only.
void LogClassProperties(const wchar_t* className);

// USceneComponent world location (K2_GetComponentLocation) and forward vector
// (GetForwardVector) -- e.g. the Camera component's eye point + look direction,
// to place something exactly in the player's view. (0,0,0) on failure.
FVector GetComponentLocation(void* component);
FVector GetComponentForwardVector(void* component);

// USceneComponent::RelativeLocation -- the BP-authored static relative offset
// to the attach-parent, read DIRECTLY via raw memory at +0x011C (not via
// UFunction). The "world" variant above (K2_GetComponentLocation) returns the
// computed world transform, which during BP construction / save-load init is
// transiently non-settled. This raw read returns the field's CURRENT stored
// value -- which is the BP-authored constant for the rest of the session once
// the construction script has completed. Used by RemotePlayer::Spawn to
// capture the mesh_playerVisible.RelLoc.Z shim once the local player is
// settled. (0,0,0) on null input -- but the offset itself is real; a true
// (0,0,0) RelLoc on a non-root component is unusual and worth logging.
FVector GetComponentRelativeLocation(void* component);

// Spawn a TRANSLUCENT world-space nameplate: an Actor carrying a UWidgetComponent
// that renders the reused uicomp_helpText_C UMG label. opacity (0..1) drives the
// whole plate's alpha (TintColorAndOpacity.A) -- real partial transparency, which
// a TextRender cannot do (its materials are Opaque/Masked). Returns the actor (a
// host you position/billboard each frame), or nullptr. Game thread only.
// `outTextBlock` (optional): receives the inner UTextBlock pointer so the caller
// can re-drive the visible label later (e.g. when the remote nickname arrives
// via the Join reliable message AFTER the nameplate was already spawned).
void* SpawnNameplateWidget(const FVector& location, const wchar_t* text, float opacity,
                           void** outTextBlock = nullptr);

// Build a SCREEN-SPACE HUD widget (a UUserWidget with a single multi-line UTextBlock
// root) and add it to the viewport. Unlike the world-space nameplate, this renders on
// the player's screen via UUserWidget::AddToViewport -- it does NOT need the stock HUD
// canvas (which VOTV doesn't run). Set to HitTestInvisible so it never steals input.
// `outer` should be a persistent object (the GameInstance) so it survives level loads.
//   alignment   -- pivot within the widget {0,0} = top-left, {1,0} = top-right, {0,.5} = left-middle, etc.
//   position    -- viewport pixel coords for the pivot (with bRemoveDPIScale=true).
//   justify     -- ETextJustify: 0=Left, 1=Center, 2=Right.
//   fontSize    -- pt.
//   color       -- text colour (use 1,1,1,1 for opaque white).
// Returns the root UUserWidget* (outRoot, for re-attach on level change) and the
// UTextBlock* (outText, to drive text via SetWidgetText). Game thread only.
bool SpawnScreenTextWidget(void* outer, int zOrder, FVector2D alignment, FVector2D position,
                           int justify, int fontSize, const FLinearColor& color,
                           void** outRoot, void** outText);

// Set a UTextBlock's text (Conv_StringToText -> UTextBlock::SetText). The HUD feed
// updates its line by rebuilding the whole multi-line string. Game thread only.
bool SetWidgetText(void* textBlock, const wchar_t* text);

// UUserWidget::AddToViewport(zOrder) / RemoveFromViewport -- re-attach the HUD feed
// after a level load (the widget object survives with a GameInstance outer, but its
// Slate viewport tree is torn down with the old world). Game thread only.
bool AddWidgetToViewport(void* userWidget, int zOrder);
bool RemoveWidgetFromViewport(void* userWidget);

// World Z of the LOWEST bone on this skeletal mesh's currently-evaluated pose.
// NOT suitable for "where the visible feet are" on a skeleton that mixes humanoid
// + non-humanoid bones (the VOTV kerfur skeleton has both humanoid foot bones
// AND non-humanoid 'wheels_R_end' for the non-upgraded variant -- a HUMAN player
// skin's visible feet are NOT at the lowest bone of this skeleton). Use
// GetBoneWorldZByName with a known foot bone name instead. Kept for diagnostics.
bool GetLowestBoneWorldZ(void* skelMeshComp, float& outZ);

// One-shot diagnostic: log every bone name + world location on a skeletal mesh
// component. Used to identify the correct foot bone for the rendered skin when
// the skeleton has hidden / non-visible bones at lower Z. Game thread only.
void DumpAllBonesWorldZ(void* skelMeshComp);

// World Z of a SPECIFIC bone (by FName string match) on the mesh component's
// currently-evaluated pose. Returns false if not found. Use this with a known
// foot-bone name to derive the visible-feet world Z (robust against
// authored-but-invisible bones in a multi-variant skeleton). Game thread only.
bool GetBoneWorldZByName(void* skelMeshComp, const wchar_t* boneName, float& outZ);

// ACharacter capsule half-height read (UCapsuleComponent::CapsuleHalfHeight at the
// fixed offset). 0.f if `mainPlayerPawn` is null or has no capsule. Used by
// RemotePlayer's foot-on-ground placement (puppet visible feet at world Z =
// source.actor.Z - halfH). Game thread only (memory read of an engine struct).
float GetActorCharacterHalfHeight(void* mainPlayerPawn);

// Set a USceneComponent's visibility: SetVisibility(visible, propagate) +
// SetHiddenInGame(!visible, propagate). visible=true shows a remote pawn's
// third-person body meshes (an unpossessed pawn never runs the gameplay code
// that unhides them); visible=false hides the orphan's editor-debug visualizers
// (ArrowComponent/BillboardComponent) that the unpossessed pawn leaves on.
// `propagate` -- pass to UE's SetVisibility/SetHiddenInGame bPropagateToChildren
// flag. DEFAULT true (the historical behavior); pass false when hiding ONE
// component that may have children we want visible (e.g. mainPlayer_C's
// ACharacter::Mesh native slot, which can be the AttachParent of the
// authoritative body mesh -- propagating hide there cascades to the body).
// Game thread only.
bool SetComponentVisible(void* component, bool visible = true, bool propagate = true);

// Force a SkeletalMeshComponent to ALWAYS tick its pose (VisibilityBasedAnimTick
// Option = AlwaysTickPoseAndRefreshBones). Without this a remote body that isn't
// the rendered viewpoint stops posing and collapses to its skeleton ("a stick").
// Direct byte write (no setter UFunction exists). Game thread only.
bool SetAnimTickAlways(void* skeletalMeshComponent);

// USkinnedMeshComponent::SetSkeletalMesh(NewMesh, bReinitPose=true) -- swap the
// skin asset onto a (puppet) SkeletalMeshComponent. Resolved from the
// SkinnedMeshComponent class (the function's OWNING class). Game thread only.
bool SetSkeletalMesh(void* skeletalMeshComponent, void* skeletalMeshAsset);

// USkeletalMeshComponent::SetAnimClass(NewClass) -- assign an AnimBP class to a
// SkeletalMeshComponent and (re)instantiate it (also flips AnimationMode to
// UseAnimBlueprint). Drives the puppet's pose. Game thread only.
bool SetAnimClass(void* skeletalMeshComponent, void* animBlueprintClass);

// Destroy an actor component (UActorComponent::K2_DestroyComponent). Used to
// strip a remote pawn's local-only systems (e.g. its unbound PostProcessComponent
// that hijacks the local screen's gamma/exposure). `contextObject` is the calling
// object for the engine's auth check (pass the owning actor). Game thread only.
bool DestroyComponent(void* component, void* contextObject);

// AmainPlayer_C::light_R -- the flashlight USpotLightComponent slot on a
// mainPlayer pawn. Returns the live component pointer, or nullptr if
// `mainPlayer` is null/dead OR the light_R slot is empty/dead. Engine-
// wrapper layer (principle 7): coop/item_activate uses this so the
// AmainPlayer_light_R offset stays out of gameplay code. Game thread only.
void* GetMainPlayerLightR(void* mainPlayer);

// Snapshot of a flashlight light's authoritative state -- everything the
// item_activate wire payload + the local probe diagnostic need.
struct FlashlightSnapshot {
    float intensity;
    float outerConeAngle;
    float innerConeAngle;
    bool  visible;
};

// Read the full snapshot off a USpotLightComponent (typically the
// AmainPlayer_C::light_R returned by GetMainPlayerLightR above). Returns
// false if `light` is null/dead. Game thread only (memory reads of an
// engine struct).
bool ReadFlashlightSnapshot(void* light, FlashlightSnapshot& out);

// AmainPlayer_C bookkeeping bools + mode byte that the flashlight wire
// payload mirrors. Adjacent fixed offsets on the pawn; one wrapper call
// replaces 4 raw struct-offset reads in coop/item_activate (Principle 7).
// Returns false on null/dead pawn; `out` is then zero-initialised.
// Game thread only.
struct MainPlayerFlashlightState {
    bool flashlight;       // AmainPlayer_C::flashlight     (canonical on/off)
    bool hasFlashlight;    // AmainPlayer_C::hasFlashlight  (equipped guard)
    bool crankFlashlight;  // AmainPlayer_C::crankFlashlight (_c variant marker)
    uint8_t mode;          // AmainPlayer_C::flashlightMode (focused/spread enum)
};
bool ReadMainPlayerFlashlightState(void* mainPlayer, MainPlayerFlashlightState& out);

// Direct write to AmainPlayer_C::flashlight (on/off bool). The DEV
// auto-toggle path (coop::item_activate::DebugForceToggle) flips the
// bool without going through the BP graph (the BP's input-guarded
// toggle is reflection-untouchable); the SetIntensity wire path drives
// the visible light separately. Returns false on null/dead pawn.
// Game thread only.
bool WriteMainPlayerFlashlight(void* mainPlayer, bool newState);

// ULightComponent::SetIntensity(NewIntensity). Internally marks the
// render state dirty so CreateSceneProxy refreshes brightness next
// frame. Cached UFunction resolved on first call. Returns false if
// `light` is null/dead OR the UFunction is not yet resolvable.
// Game thread only.
bool SetLightIntensity(void* light, float newIntensity);

// USceneComponent::SetVisibility(bNewVisibility, bPropagateToChildren).
// Distinct from SetComponentVisible above -- this is JUST the
// SetVisibility UFunction (no companion SetHiddenInGame call), which
// is what the puppet flashlight receive path needs: the SetVisibility
// virtual on ULightComponent calls MarkRenderStateDirty so the proxy
// is (re)built without forcing the hidden-in-game state on the whole
// component. Cached UFunction. Game thread only.
bool SetSceneComponentVisibility(void* sceneComponent, bool newVisibility, bool propagateToChildren);

// USpotLightComponent::SetOuterConeAngle(NewOuterConeAngle) /
// SetInnerConeAngle(NewInnerConeAngle). Both UFunctions internally
// MarkRenderStateDirty so the cone shape refreshes next frame. Used
// by the puppet flashlight receiver to mirror the sender's hold-F
// focused/spread mode. Cached UFunctions. Game thread only.
bool SetSpotLightOuterConeAngle(void* spotLight, float newAngle);
bool SetSpotLightInnerConeAngle(void* spotLight, float newAngle);

// A long-lived UObject suitable as a WorldContextObject for the deferred-spawn
// pair (BeginDeferredActorSpawnFromClass + FinishSpawningActor). Tries the
// GameInstance first (lives across map loads) and falls back to the World.
// Used by remote_prop + npc_sync receivers. M-3 (2026-05-29): extracted to
// kill the duplicate definitions in those two TUs.
void* GetWorldContext();

// USoundAttenuation config for SpawnSoundAttenuation below. Field meanings
// mirror Unreal's USoundAttenuation editor properties; the exact uint8 enum
// values follow UE4.27's EAttenuationShape / EAttenuationDistanceModel /
// EAttenuationFalloffMode. Defaults produce VOTV flashlight-click tuning
// (sphere, 20m full-volume zone, 200m falloff, inverse distance model).
struct SoundAttenuationConfig {
    uint8_t shape              = 0;        // 0=Sphere, 1=Box, 2=Capsule, 3=Cone
    uint8_t distanceAlgorithm  = 2;        // 0=Linear, 1=Logarithmic, 2=Inverse, ...
    uint8_t falloffMode        = 0;        // 0=Continues, 1=Hold
    float   extents[3]         = {2000.f, 0.f, 0.f};  // sphere: extents[0] = radius (cm)
    float   falloffDistance    = 20000.f;             // cm beyond extents until silence
    float   coneOffset         = 0.f;
    float   dBAttenuationAtMax = -60.f;
    bool    attenuate          = true;     // FlagsByte bit 0
    bool    spatialize         = true;     // FlagsByte bit 1
};

// Construct + configure a fresh USoundAttenuation via
// UGameplayStatics::SpawnObject + raw-field writes (Unreal exposes no
// setter UFunctions for the SoundAttenuationSettings fields; they are
// edit-time properties otherwise touched only via direct memory). The
// returned object is AddToRoot'd before return so UE GC never collects
// it (callers store the pointer in a C++ static which is invisible to
// UE's reachability scan -- without rooting, the object is reaped on
// the next GC pass and PlaySoundAtLocation crashes reading freed memory
// on the subsequent click; this was the 2026-05-26 F-spam crash root
// cause). Returns the live USoundAttenuation*, or nullptr if any
// resolution failed (SpawnObject UFunction missing / CDO missing /
// SpawnObject returns null). Game thread only.
//
// Principle 7 (A-3, 2026-05-29): replaces the inline SpawnObject +
// 8 raw att:: offset writes block that lived in
// coop::flashlight_click_sound::PlayIfStateChanged.
void* SpawnSoundAttenuation(const SoundAttenuationConfig& cfg);

// FRotator -> FQuat (UE4.27 stock formula, ZYX order, LEFT-HANDED coord
// system). Matches EXACTLY the body of FRotator::Quaternion() from
// Engine/Source/Runtime/Core/Public/Math/Rotator.h:
//
//   RotationQuat.X =  CR*SP*SY - SR*CP*CY;
//   RotationQuat.Y = -CR*SP*CY - SR*CP*SY;
//   RotationQuat.Z =  CR*CP*SY - SR*SP*CY;
//   RotationQuat.W =  CR*CP*CY + SR*SP*SY;
//
// Note the negative Y term -- this is UE4's left-handed-Z-up convention
// (NOT a bug). General right-handed ZYX Euler-to-quat references will show
// the opposite signs; do NOT "correct" against those without checking the
// UE4 source. Audit 2026-05-24 flagged this as a critical bug; verified
// false-positive by reading UE4.27 source directly. M-3 (2026-05-29):
// extracted to kill the duplicate definitions in remote_prop + npc_sync.
void RotatorToQuat(float pitchDeg, float yawDeg, float rollDeg,
                   float& qx, float& qy, float& qz, float& qw);

}  // namespace ue_wrap::engine
