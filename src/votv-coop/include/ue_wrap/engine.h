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

#include <vector>

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

// Self-test for the bug2 world-context staleness guard (2026-05-30). Forces the
// cached world context to look stale (corrupts the cached GUObjectArray index so
// IsLiveByIndex fails -- simulating a freed World after a level reload), then runs
// EnsureWorldContext and verifies it DROPPED the stale entry and re-resolved to a
// LIVE context. Returns true on recovery. Game-thread only; leaves the cache valid.
// Gated behind an autotest env flag -- not a shipping path.
bool DebugCheckWorldContextRecovery();

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

// AmainPlayer_C single-component-pointer accessors. Each returns the UObject*
// stored in a specific component slot on the local pawn:
//   - GrabHandle       -> UPhysicsHandleComponent driving normal-prop grab
//   - HeavyGrabPCC     -> UPhysicsConstraintComponent driving heavy-prop grab
//   - GrabTimeline     -> UTimelineComponent driving the grab animation curve
// All three are read by the autotest harness's synthetic grab scenario to
// dispatch UPhysicsHandleComponent.GrabComponentAtLocation / PCC.SetConstrained
// /Timeline.PlayFromStart against the real game-thread components. gameplay
// /test code uses these accessors so it never touches the mainPlayer_C struct
// layout directly (Principle 7). Returns nullptr on null/dead pawn OR if
// the slot itself is null. Game thread only.
//
// Principle 7 (autotest follow-up, 2026-05-29): replaces 3 inline raw struct-
// offset reads in harness::autotest's grab-resolve / heavy-grab-resolve /
// timeline-force-play scenarios.
void* ReadMainPlayerGrabHandle(void* mainPlayer);
void* ReadMainPlayerHeavyGrabPCC(void* mainPlayer);
void* ReadMainPlayerGrabTimeline(void* mainPlayer);

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
// that renders a UVerticalBox of TWO UTextBlocks -- line 1 = nick (nickColor),
// line 2 = health bar (barColor) -- so each line carries its OWN colour (a single
// UTextBlock is one colour). Each colour's alpha is the per-line opacity. Returns
// the actor (a host you position/billboard each frame), or nullptr. Game thread
// only. `outNickBlock` / `outBarBlock` (optional): receive the two inner
// UTextBlock pointers so the caller can re-drive each line independently (nick on
// the Join reliable; the bar on every vitals stream). In the graceful single-block
// fallback (VerticalBox class absent), outBarBlock receives nullptr and both lines
// render in nickColor on the nick block.
void* SpawnNameplateWidget(const FVector& location,
                           const wchar_t* nickText, const FLinearColor& nickColor,
                           const wchar_t* barText, const FLinearColor& barColor,
                           void** outNickBlock = nullptr, void** outBarBlock = nullptr);

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

// Set a UTextBlock's ColorAndOpacity (@UTextBlock_ColorAndOpacity, with the
// FSlateColor ColorUseRule forced to UseColor_Specified=0). The owning
// UWidgetComponent auto-redraws (bManuallyRedraw=0, RedrawTime=0), so the new
// color shows next frame -- no RequestRedraw needed; SetWidgetText (which only
// sets the text string) never clobbers it. Used by the nameplate damage-flash
// (vitals Inc3). Returns false on null textBlock. Game thread only.
bool SetTextBlockColor(void* textBlock, const FLinearColor& color);

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

// USkinnedMeshComponent::SetSkeletalMesh(null) -- clear the mesh asset so the
// component renders NOTHING, without touching its visibility flags / AttachParent
// (a bHiddenInGame hide on a parent whose child is a simulating ragdoll mesh would
// cascade and hide the child via IsVisible's parent-walk; clearing the asset does
// not). Separate fn because SetSkeletalMesh deliberately rejects null. Game thread.
bool ClearSkeletalMesh(void* skeletalMeshComponent);

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

// ---- Ragdoll / faint DISPLAY state (vitals pillar Inc2b) -----------------
// AmainPlayer_C drives every ragdoll cause (manual C-key, exhaustion faint,
// KO) through one UFunction + an AnimBP gate bool. These wrappers let the
// coop layer read the local player's ragdoll state (sender) and drive a
// puppet into/out of the faint pose (receiver) without touching engine
// memory or reflection directly (Principle 7). Offsets are name-resolved
// (recook-safe via reflected_offset); UFunctions are cached on first call.
// All game-thread only.

// Read AmainPlayer_C::isRagdoll (the AnimBP ragdoll gate, set by ANY ragdoll
// cause) + ::dead (death bool). Returns false (leaving out-params untouched)
// on a null/dead pawn or if either field offset can't be resolved yet.
bool ReadMainPlayerRagdollState(void* mainPlayer, bool& isRagdoll, bool& dead);

// AmainPlayer_C::ragdollMode(ragdoll, passOut, death). The receiver drives the
// puppet into the verified faint pose with (true, true, false) -- MUST-VERIFY
// #8 proved this flips an UNPOSSESSED puppet's isRagdoll 0->1 + spawns its
// ragdoll actor. Returns false on null/dead pawn or unresolved UFunction.
bool SetMainPlayerRagdollMode(void* mainPlayer, bool ragdoll, bool passOut, bool death);

// AmainPlayer_C::forceGetUp() -- begins the get-up. On a normal POSSESSED player
// (tick enabled) this fully recovers (clears isRagdoll + tears down the ragdoll
// actor over its timeline). Returns false on null/dead pawn or unresolved
// UFunction. POSSESSED-player only (e.g. the local player's own recover); for an
// orphan PUPPET use StopPuppetMeshRagdoll below (the tick-driven cleanup this
// relies on can't run on a tickless orphan).
bool ForceMainPlayerGetUp(void* mainPlayer);

// TEST DRIVER ONLY: write mainPlayer.isRagdoll @0x87F directly. The pose stream's
// kStateBitRagdoll is derived from this field, so a direct write makes a REMOTE
// peer's puppet flop WITHOUT invoking VOTV's real ragdollMode -- i.e. without
// spawning the leaky AplayerRagdoll_C on this possessed player. NOT the production
// faint/KO/C-key path (that is VOTV's own BP). Used by the autonomous ragdoll test
// so the host-puppet visual smoke doesn't OOM the driving client. Game thread.
bool WriteMainPlayerIsRagdoll(void* mainPlayer, bool isRagdoll);

// AmainPlayer_C::"Add Player Damage"(Damage, damageLocation, fullBody, blood, Source)
// -- the primary player-damage entry. vitals Inc3-WIRE: the OWNER peer invokes this on
// its OWN POSSESSED mainPlayer_C when the host relays an enemy hit, so the damage runs
// through that peer's private armor/inventory BP and drops its shared saveSlot.health
// (then the existing Inc1 stream + Inc3 flash fire). Only `Damage` is set; the
// location/fullBody/blood/Source params zero-init. SAFE on a puppet by accident -- the
// BP early-outs on an unpossessed pawn (#6 probe). Returns false on null/dead pawn or
// unresolved UFunction. Game thread only.
bool InvokeAddPlayerDamage(void* mainPlayer, float damage);

// ---- Orphan-puppet faint visual via the puppet's OWN mesh -----------------
// vitals Inc2b receiver. We do NOT use VOTV's ragdollMode on the puppet: that
// spawns a separate playerRagdoll_C physics actor whose entire lifecycle
// (get-up, GC, PhysX teardown) is tick/timeline-coupled and assumes a POSSESSED
// player. On our deliberately tickless, controllerless orphan puppet that actor
// self-invalidates to PendingKill but is never GC-reaped, so its PhysX keeps
// simulating -> +5 GB RSS runaway (root-caused 2026-05-31, IDA/SDK RE + probe).
// Instead we ragdoll the puppet's OWN mesh_playerVisible -- a component WE own,
// so enable/disable is clean and leak-free (user decision 2026-05-31).

// ---- Inc3 damage body-pulse (puppet mesh material swap) -------------------
// The saved-material cache type (which component / slot index / original) is
// ue_wrap::SavedMaterial in types.h. The puppet renders TWO visible body meshes
// (Mesh@0x280 + mesh_playerVisible), so the saved set spans both -- a flat
// (component,index,original) list restores every slot exactly regardless of how
// many components or slots were swapped. The caller (RemotePlayer) owns the vector.
using ue_wrap::SavedMaterial;

// Swap BOTH visible body meshes' materials to the hurt-flash material
// (PlayerHurtFlashMaterialName -- a SKELETAL gore skin so it renders red, not the
// default grey), CACHING every original into `saved`. Restore with
// RestoreHurtFlashMaterial passing the same vector. This is the Minecraft-style
// "whole body flashes red" damage pulse; it shares the nameplate hurt-flash
// trigger. Materials are render state independent of animation/physics, so it
// composes with the pose drive + ragdoll. No-op-safe (returns false) if the
// puppet/mesh/material won't resolve; then the nameplate flash still fires
// (graceful). Game thread only.
bool ApplyHurtFlashMaterial(void* puppet, std::vector<SavedMaterial>& saved);
bool RestoreHurtFlashMaterial(void* puppet, std::vector<SavedMaterial>& saved);

// Eager-resolve the hurt material + material UFunctions (call once per puppet
// spawn) so the first damage flash does no GUObjectArray name walks. Game thread.
void WarmupHurtFlashCache();

// Resolve a loaded UMaterialInterface by object name (tries MaterialInstanceConstant,
// then base Material, then any class). Returns nullptr if not loaded. Game thread.
void* ResolveMaterialByName(const wchar_t* name);

// Make the puppet flop: SetAllBodiesSimulatePhysics(true) on mesh_playerVisible
// (@0x04F8) so the skin's physics asset simulates from its current pose, + set
// isRagdoll=true as a state marker. ALSO clears the native ACharacter::Mesh
// @0x0280's mesh asset (saved into outSavedCharMeshAsset) so ONLY the limp
// simulating @0x4F8 renders during the flop -- @0x0280 is a SECOND visible body
// sharing the skin and, left animated/upright, double-images over the ragdoll.
// The caller pauses pose-driving while active so physics owns the body, and must
// pass outSavedCharMeshAsset back to StopPuppetMeshRagdoll. Clears @0x0280 only
// AFTER the sim engages, so a failed flop leaves the body visible (graceful).
// Returns false on null/dead pawn or if the sim couldn't engage.
bool StartPuppetMeshRagdoll(void* puppet, void*& outSavedCharMeshAsset);

// Recover: SetAllBodiesSimulatePhysics(false) on mesh_playerVisible so the mesh
// snaps back to AnimBP-driven animation, restore the native ACharacter::Mesh
// @0x0280 body (savedCharMeshAsset from StartPuppetMeshRagdoll), + clear isRagdoll.
// The caller resumes pose-driving. Returns false on null/dead pawn. Game thread.
bool StopPuppetMeshRagdoll(void* puppet, void* savedCharMeshAsset);

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
