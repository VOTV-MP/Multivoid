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
// blend (RemotePlayer::Drive's `speed`). Game thread only.
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

// APawn::GetController on `pawn` -- returns the AController* (or nullptr).
void* GetController(void* pawn);

// AController::GetControlRotation on `controller` -- the view rotation the game
// drives from mouse-look. Reading it for a freecam gives smooth, game-native
// look with no raw-mouse handling. (0,0,0) on failure. Game thread only.
FRotator GetControlRotation(void* controller);

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

// USceneComponent world location (K2_GetComponentLocation) and forward vector
// (GetForwardVector) -- e.g. the Camera component's eye point + look direction,
// to place something exactly in the player's view. (0,0,0) on failure.
FVector GetComponentLocation(void* component);
FVector GetComponentForwardVector(void* component);

// Spawn a TRANSLUCENT world-space nameplate: an Actor carrying a UWidgetComponent
// that renders the reused uicomp_helpText_C UMG label. opacity (0..1) drives the
// whole plate's alpha (TintColorAndOpacity.A) -- real partial transparency, which
// a TextRender cannot do (its materials are Opaque/Masked). Returns the actor (a
// host you position/billboard each frame), or nullptr. Game thread only.
void* SpawnNameplateWidget(const FVector& location, const wchar_t* text, float opacity);

// Build a SCREEN-SPACE HUD widget (a UUserWidget with a single multi-line UTextBlock
// root) and add it to the viewport. Unlike the world-space nameplate, this renders on
// the player's screen via UUserWidget::AddToViewport -- it does NOT need the stock HUD
// canvas (which VOTV doesn't run). Set to HitTestInvisible so it never steals input.
// `outer` should be a persistent object (the GameInstance) so it survives level loads.
// Returns the root UUserWidget* (outRoot, for re-attach on level change) and the
// UTextBlock* (outText, to drive text via SetWidgetText). Game thread only.
bool SpawnHudFeedWidget(void* outer, int zOrder, void** outRoot, void** outText);

// Set a UTextBlock's text (Conv_StringToText -> UTextBlock::SetText). The HUD feed
// updates its line by rebuilding the whole multi-line string. Game thread only.
bool SetWidgetText(void* textBlock, const wchar_t* text);

// UUserWidget::AddToViewport(zOrder) / RemoveFromViewport -- re-attach the HUD feed
// after a level load (the widget object survives with a GameInstance outer, but its
// Slate viewport tree is torn down with the old world). Game thread only.
bool AddWidgetToViewport(void* userWidget, int zOrder);
bool RemoveWidgetFromViewport(void* userWidget);

// World location of the skeletal mesh's "head" bone (USceneComponent::GetSocketLocation;
// the head bone FName is enumerated once + cached). Used to anchor the nameplate to the
// visible head instead of the actor origin. Returns false if unresolved. Game thread only.
bool GetHeadWorldLocation(void* skelMeshComp, FVector& out);

// Set a USceneComponent's visibility: SetVisibility(visible, propagate) +
// SetHiddenInGame(!visible, propagate). visible=true shows a remote pawn's
// third-person body meshes (an unpossessed pawn never runs the gameplay code
// that unhides them); visible=false hides the orphan's editor-debug visualizers
// (ArrowComponent/BillboardComponent) that the unpossessed pawn leaves on.
// Game thread only.
bool SetComponentVisible(void* component, bool visible = true);

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

}  // namespace ue_wrap::engine
