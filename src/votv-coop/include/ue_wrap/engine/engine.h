// ue_wrap/engine.h -- engine operations built on reflection: each function marshals one UFunction
// call or one reflected field access. No gameplay, network or coop state. Nearly every call must
// run on the game thread (ue_wrap::game_thread::Post); the raw reads that are safe elsewhere say
// so.

#pragma once

#include "ue_wrap/core/types.h"

#include <string>
#include <vector>

namespace ue_wrap::engine {

// UKismetSystemLibrary::ExecuteConsoleCommand; the CDO, the UFunction and a world context resolve
// on first use. False on a resolution miss. Game thread.
bool ExecuteConsoleCommand(const wchar_t* command);

// UGameplayStatics::IsGamePaused / SetGamePaused on the cached world context; a paused peer stops
// ticking its world, so a session keeps every peer unpaused. IsGamePaused is false on a miss. Game
// thread.
bool IsGamePaused();
bool SetGamePaused(bool paused);

// AmainGamemode_C::transition("/Game/menu"), the game's own travel verb (the short name does not
// resolve); needs no pause and works for a dead player. Hold game_thread::SetTransparentBypass over
// the travel so the detour does not stall the world teardown. Game thread.
bool ReturnToMainMenu();

// Load a save slot through the game's own entry (LoadGameFromSlot, setSaveSlotObject with
// loadObjects=true, then untitled_1; the save selects the mode). `forceGameMode` >= 0 writes that
// enum_gamemode ordinal instead of deriving it from the slot-name prefix, which the coop slot
// `zcoop_<pid>` lacks. False if the slot is missing or the load cannot dispatch. Game thread.
bool LoadStorySave(const wchar_t* slot, int forceGameMode = -1);

// Drop the cached USaveGame* and the GameMode latch so a second in-process LoadStorySave re-loads
// from disk. Game thread.
void ResetCachedSave();

// A fresh New Game: LoadStorySave with a blank save object (CreateSaveGameObject(saveSlot_C)); a
// fresh client world holds only level-default props, so the host's snapshot mirrors onto it with
// nothing to reconcile away. Game thread.
bool StartFreshGame(bool storyMode);

// ---- Save-object-ready hook ----
// Fires once per LoadStorySave / StartFreshGame, on the game thread, with the USaveGame* about to
// be registered: the inventory arrays are present and the world not yet built from them, so the
// game's own load builds the live inventory from what the hook leaves. engine.cpp fires it
// unconditionally; the hook is a no-op unless an inventory is pending. Null disarms.
using SaveObjectReadyHook = void(*)(void* saveSlotObject);
void SetSaveObjectReadyHook(SaveObjectReadyHook hook);

// The game encodes a save's mode only in the slot-name prefix (story "s_", infinite "i_", sandbox
// "b_", halloween "SPOOKY_", ambience "a_", solar "l_"); both functions wrap
// Uui_saveSlots_C::getSavePrefix on the CDO, which resolves lazily (false / -1 until the menu or a
// gameplay transition loads it). enum_gamemode ordinals are not the submenu order (story=0,
// infinite=1, sandbox=4). Game thread.

// The prefix for `mode` (a TEnumAsByte<enum_gamemode::Type>) into `out`.
bool GetSavePrefix(uint8_t mode, std::wstring& out);

// The ordinal of `slot` from its prefix (the longest match wins): 0..7, or -1.
int DeriveModeFromSlot(const wchar_t* slot);

// Spawn `actorClass` at `location` through the deferred GameplayStatics pair (AlwaysSpawn); the
// AActor* or nullptr. `inertPawn` zeroes AutoPossessPlayer, AutoPossessAI and AutoReceiveInput and
// sets bBlockInput in the deferred window, so the pawn never takes a PlayerController or the local
// input. Game thread.
void* SpawnActor(void* actorClass, const FVector& location, bool inertPawn = false);

// The split form: BeginDeferredSpawn, write class-specific fields before Init and BeginPlay,
// FinishDeferredSpawn with the same transform (the engine re-applies it). A wire-spawned entity
// gets its variant state stamped in the window. Game thread.
void* BeginDeferredSpawn(void* actorClass, const FVector& location, const FRotator& rotation);
bool  FinishDeferredSpawn(void* actor, const FVector& location, const FRotator& rotation);

// Self-test of the world-context staleness guard: corrupts the cached GUObjectArray index, runs
// EnsureWorldContext and checks it re-resolved a live context. Behind an autotest env flag. Game
// thread.
bool DebugCheckWorldContextRecovery();

// AActor::K2_GetActorLocation; (0,0,0) if it cannot be called.
FVector GetActorLocation(void* actor);

// The checked read: false when the location could not be obtained. Use this wherever a wrong answer
// grants something: GetActorLocation returns (0,0,0), the world origin, on every failure. Game
// thread.
bool TryGetActorLocation(void* actor, FVector& out);

// AActor::GetActorScale3D (root world scale); unit scale on failure, since callers stamp it into
// spawn transforms. Game thread.
FVector GetActorScale3D(void* actor);

// True iff `actor` was spawned by a UChildActorComponent (a kerfur's eye camera, a console screen
// child): the parent's construction script spawns and destroys these on every peer (Aprop_C's
// ignoreSave = ignoreSav || IsChildActor()), so they carry no cross-peer identity. A raw read of
// the reflected ParentComponent weak pointer, safe off the game thread; a set pointer means child
// actor even mid-teardown.
bool IsChildActor(void* actor);

// The child actor's parent (the ParentComponent's Outer) and, in `outComponentName`, the
// UChildActorComponent's name: the half of a child actor's identity that is stable across processes
// (the `_CAT_<n>` tail of the actor's own name is a per-process counter). Null when not a child
// actor or the parent is gone. Raw reads; safe off the game thread.
void* ParentActorOf(void* actor, std::wstring* outComponentName = nullptr);

// AActor::SetActorScale3D (root relative scale); the trash proxy applies the host's per-form scale
// on every convert and spawn. Game thread.
bool SetActorScale3D(void* actor, const FVector& scale);

// UKismetSystemLibrary::CollectGarbage: a full purge at the end of the frame, paired with the
// adoption sweep's destruction burst so pending-kill garbage does not wait for UE's 61 s periodic
// purge. Game thread.
bool ForceGarbageCollection();

// AActor::GetActorForwardVector; (0,0,0) on failure.
FVector GetActorForwardVector(void* actor);

// AActor::K2_GetActorRotation (world); zero on failure. Game thread.
FRotator GetActorRotation(void* actor);

// AActor::GetVelocity (world, cm/s); zero on failure. Its horizontal magnitude is the remote
// locomotion blend's walk speed. Game thread.
FVector GetActorVelocity(void* actor);

// AActor::K2_SetActorLocation (bSweep=false, bTeleport=true: a snap). Game thread.
bool SetActorLocation(void* actor, const FVector& location);

// AActor::K2_SetActorRotation (bTeleportPhysics=true). Game thread.
bool SetActorRotation(void* actor, const FRotator& rotation);

// USceneComponent::K2_GetComponentRotation (world); zero on failure. Game thread.
FRotator GetComponentWorldRotation(void* component);

// USceneComponent::K2_SetWorldRotation (bSweep=false, bTeleport=true). Call after SetActorRotation:
// moving the root re-bases the child's world transform. Game thread.
bool SetComponentWorldRotation(void* component, const FRotator& rotation);

// The world rotation of `actor`'s visible StaticMesh component, or the actor rotation if it owns
// none. A chipPile's visual roll lives on the component (its UserConstructionScript), not the root,
// so the host captures this and the bare proxy reproduces it with SetActorRotation. Game thread.
FRotator GetVisibleMeshWorldRotation(void* actor);

// AActor::SetActorTickEnabled; a remote pawn must not run the local player's per-frame BP
// EventTick, which re-applies view and post-process to the shared screen. Game thread.
bool SetActorTickEnabled(void* actor, bool enabled);

// AActor::SetActorHiddenInGame: visual only (bHidden @0x58; collision is bActorEnableCollision
// @0x5C). Game thread.
bool SetActorHiddenInGame(void* actor, bool hidden);

// AActor::SetActorEnableCollision; pair it with a hide so an invisible mirror is neither grabbable
// nor physics-active. Game thread.
bool SetActorEnableCollision(void* actor, bool enabled);

// APawn::GetController; nullptr if none.
void* GetController(void* pawn);

// AController::GetControlRotation: the mouse-look view rotation; (0,0,0) on failure. Game thread.
FRotator GetControlRotation(void* controller);

// Direct write of AController::ControlRotation (offset 0x0288); the engine reads it next tick, and
// the setter UFunction does no more than store it. Game thread.
void SetControlRotation(void* controller, const FRotator& rot);

// K2_TeleportTo: the engine's own long-range teleport; K2_SetActorLocation can be silently reverted
// by Character constraints when the move is far. Game thread.
bool TeleportTo(void* actor, const FVector& location, const FRotator& rotation);

// GetActorBounds: the world-space AABB from the rendered mesh, not the bone hierarchy (the puppet's
// lowest visible point is Origin.Z - BoxExtent.Z). Game thread.
bool GetActorBounds(void* actor, bool onlyColliding, FVector& outOrigin, FVector& outBoxExtent);

// APlayerController::SetViewTargetWithBlend: repoint the view to `newViewTarget` over `blendTime`
// seconds. Game thread.
bool SetViewTargetWithBlend(void* playerController, void* newViewTarget, float blendTime);

// APlayerController::ProjectWorldLocationToScreen: a world point to viewport pixels through the
// local camera; false, `outScreen` untouched, behind the camera. The nameplates project here and
// snapshot the result for the render thread. Game thread.
bool ProjectWorldToScreen(void* playerController, const FVector& world,
                          FVector2D& outScreen, bool viewportRelative = false);

// The view camera's world location and rotation (APlayerCameraManager); zero on failure. Game
// thread.
FVector GetCameraLocation();
FRotator GetCameraRotation();

// APawn::SpawnDefaultController: an AIController possesses the pawn, so its AnimBP poses, with no
// viewport, input or camera. Game thread.
bool SpawnDefaultController(void* pawn);

// APawn::DetachFromControllerPendingDestroy: unpossess; the pawn keeps existing. Game thread.
bool DetachFromController(void* pawn);

// AActor::K2_DestroyActor. Game thread.
bool DestroyActor(void* actor);

// The UCharacterMovementComponent subobject of a Character; nullptr if none.
void* GetCharacterMovementComponent(void* characterPawn);

// The UStaticMeshComponent subobject of `actor` (a reflection child walk); nullptr if none. Game
// thread.
void* GetStaticMeshComponent(void* actor);

// UActorComponent::SetComponentTickEnabled; the puppet's CMC is parked at spawn so it does not
// fight the pose drive. Game thread.
bool SetComponentTickEnabled(void* component, bool enabled);

// Direct write of a UObject* slot at `byteOffset` inside `target`. Game thread.
void WriteObjectField(void* target, size_t byteOffset, void* value);

// If `localPlayer` is grabbing `actor` through the light-grab path (grabbing_actor @0x07D0,
// grabHandle @0x0688), dispatch UPhysicsHandleComponent::ReleaseComponent and clear the slot; true
// iff a release happened. Release before destroy: K2_DestroyActor leaves PHC.GrabbedComponent
// dangling and the handle's tick dereferences it. Game thread.
bool ReleaseMainPlayerGrabIfHolding(void* localPlayer, void* actor);

// True iff `localPlayer`'s grabbing_actor slot holds `actor`; no mutation. A held prop is skipped
// by the physics reconcile: forcing SimulatePhysics off mid-hold breaks the grab. Game thread.
bool IsMainPlayerGrabbing(void* localPlayer, void* actor);

// Eager-resolve the PHC.ReleaseComponent cache once the PhysicsHandleComponent class is loaded, so
// the first cross-peer PropDestroy does not fall through to the warn-and-clear fallback.
// Idempotent. Game thread.
bool WarmupPhcReleaseCache();

// UPhysicsHandleComponent::GrabbedComponent at the fixed offset sdk_profile.h names; nullptr if
// `phc` is null or dead or the slot is empty. Game thread.
void* ReadPhysicsHandleGrabbedComponent(void* phc);

// The AmainPlayer_C grab-state properties, read in one dispatch: grabbingActor and holdingActor
// cover the two carry paths (the physics handle vs the chipPile/clump carry). False on a null or
// dead pawn; `holdingActor` stays null when MainPlayer_holding_actor() is unresolved (a later
// recook added it). Game thread.
struct MainPlayerGrabState {
    void*  grabbingActor;  // AmainPlayer_C::grabbing_actor (PHC-held prop, or null)
    void*  holdingActor;   // AmainPlayer_C::holding_actor  (chipPile/clump morph carry, or null)
    bool   grabsHeavy;     // AmainPlayer_C::grabsHeavy     (PCC heavy-grab BP flag)
    bool   heavy;          // AmainPlayer_C::Heavy          (BP-side heavy state mirror)
    float  grabLen;        // AmainPlayer_C::grabLen        (Timeline current grab length)
};
bool ReadMainPlayerGrabState(void* mainPlayer, MainPlayerGrabState& out);

// AmainPlayer_C::lookAtActor @0x0AA0, the interactable under the crosshair; nullptr if unresolved
// or nothing is aimed at. Game thread.
void* ReadMainPlayerLookAtActor(void* mainPlayer);

// Direct write of lookAtActor, the interaction-trace result the BP re-derives each tick; lets a
// test aim the next InpActEvt_use dispatch at a chosen actor. Game thread.
bool WriteMainPlayerLookAtActor(void* mainPlayer, void* actor);

// The radial-menu confirm state: releaseEToUse @0x0E88 and actionIndex @0x0A98 (the index into
// getActionOptions). The kerfur radial verb is relayed from these because the actionName dispatch
// is EX_LocalVirtualFunction, invisible to ProcessEvent. Game thread.
bool ReadMainPlayerRadialSelect(void* mainPlayer, bool& releaseEToUse, int32_t& actionIndex);

// Write grabbing_actor and grabbing_component in one dispatch (the autotest's synthetic grab keeps
// the BP's "what am I holding" mirror in step); nullptr/nullptr clears. Game thread.
bool WriteMainPlayerGrabbingPair(void* mainPlayer, void* actor, void* component);

// AmainPlayer_C component slots: GrabHandle (UPhysicsHandleComponent), HeavyGrabPCC
// (UPhysicsConstraintComponent), GrabTimeline (UTimelineComponent); nullptr on a dead pawn or an
// empty slot. Game thread.
void* ReadMainPlayerGrabHandle(void* mainPlayer);
void* ReadMainPlayerHeavyGrabPCC(void* mainPlayer);
void* ReadMainPlayerGrabTimeline(void* mainPlayer);

// Diagnostic: log every FProperty (name, offset, size) of a UClass, its own properties only. Game
// thread.
void LogClassProperties(const wchar_t* className);

// USceneComponent world location (K2_GetComponentLocation) and forward vector; (0,0,0) on failure.
FVector GetComponentLocation(void* component);
FVector GetComponentForwardVector(void* component);

// USceneComponent::RelativeLocation read raw at +0x011C: the BP-authored offset, stable once
// construction has run, where K2_GetComponentLocation is unsettled mid-init. (0,0,0) on null.
FVector GetComponentRelativeLocation(void* component);

// A UParticleSystemComponent's `Template`, read raw at a reflection-resolved offset; the cue sync
// matches a live PSC to its cue by it, because the EX_CallMath spawn that created it is invisible
// to the detour. nullptr if unresolved.
void* GetParticleSystemTemplate(void* particleSystemComponent);

// Build a screen-space HUD widget (a UUserWidget with one multi-line UTextBlock) and add it to the
// viewport, HitTestInvisible; `outer` should be the GameInstance so it survives level loads.
// alignment = the pivot ({0,0} top-left, {1,0} top-right), position = viewport pixels, justify =
// ETextJustify (0 left, 1 centre, 2 right), fontSize in points; outRoot for a re-attach, outText
// for SetWidgetText. Game thread.
bool SpawnScreenTextWidget(void* outer, int zOrder, FVector2D alignment, FVector2D position,
                           int justify, int fontSize, const FLinearColor& color,
                           void** outRoot, void** outText);

// NewObject by class: the reflected UGameplayStatics::SpawnObject(objectClass, Outer), the one way
// this mod mints a UObject. `outer` must keep the result reachable: a widget's Outer does not root
// it, a panel's `Slots` UPROPERTY does, so an unattached widget is collectable at the next GC.
// nullptr until SpawnObject resolves. Game thread.
void* SpawnUObject(void* objectClass, void* outer);

// Set a UTextBlock's text (Conv_StringToText, then SetText). Game thread.
bool SetWidgetText(void* textBlock, const wchar_t* text);

// Raw write of a UTextBlock's ColorAndOpacity (ColorUseRule forced to UseColor_Specified). Only a
// UWidgetComponent-hosted block re-renders from its properties; a block in a constructed Slate tree
// ignores it (SetTextBlockColorDispatch). Game thread.
bool SetTextBlockColor(void* textBlock, const FLinearColor& color);

// UTextBlock::SetColorAndOpacity, the setter: required in a constructed UMG tree, where UMG bakes
// properties into Slate at attach. Game thread.
bool SetTextBlockColorDispatch(void* textBlock, const FLinearColor& color);

// UUserWidget::AddToViewport / RemoveFromViewport: re-attach the HUD feed after a level load (the
// object survives; its Slate tree does not). Game thread.
bool AddWidgetToViewport(void* userWidget, int zOrder);
bool RemoveWidgetFromViewport(void* userWidget);

// ---- Runtime UMG button injection ----
// Insert a UButton at the top of `refButton`'s UVerticalBox, cloning its FButtonStyle and slot
// layout so spacing and indent match, with the label styled as the native items (font_ui at 16,
// left-justified, cyan). `outButton` (may be null) gets the UButton* for the click poll. Game
// thread.
bool InjectCanvasButton(void* refButton, const wchar_t* label, void** outButton);

// Inject a UTextBlock as a new row above `refText`'s (a UHorizontalBox row inside a UVerticalBox of
// label rows), cloning the row's slot layout and refText's text style, so the coop version line
// reads as one more native label and shows with the menu. `outColor` gets the cloned colour so a
// temporary tint can be restored. Game thread.
bool InjectTextRowAbove(void* refText, const wchar_t* initial,
                        void** outText, FLinearColor* outColor);

// UWidget::IsHovered, the mouse-over test for the menu click poll. Game thread.
bool WidgetIsHovered(void* widget);

// UWidget::SetVisibility (0 Visible, 1 Collapsed, 2 Hidden, 3 HitTestInvisible, 4
// SelfHitTestInvisible). The loading state hides the game's menu with HitTestInvisible plus
// SetWidgetRenderOpacity(0): still a rendered state, so the menu keeps ticking and the same
// observer can restore it. Game thread.
bool SetWidgetVisibility(void* widget, uint8_t slateVis);

// UWidget::SetRenderOpacity: the visual half of the menu hide. Game thread.
bool SetWidgetRenderOpacity(void* widget, float opacity);

// World Z of the lowest bone of the evaluated pose. A diagnostic: the kerfur skeleton mixes
// humanoid foot bones with a 'wheels_R_end', so this is not where a human skin's feet are
// (GetBoneWorldZByName is).
bool GetLowestBoneWorldZ(void* skelMeshComp, float& outZ);

// Diagnostic: log every bone's name and world location. Game thread.
void DumpAllBonesWorldZ(void* skelMeshComp);

// World Z of one bone by name; false if not found. Game thread.
bool GetBoneWorldZByName(void* skelMeshComp, const wchar_t* boneName, float& outZ);

// World position of one bone: one GetSocketLocation dispatch per call (the FName cached per name),
// anchoring to the component transform when the bone is missing. False only on resolution failure.
// Game thread.
bool GetBoneWorldLocationByName(void* skelMeshComp, const wchar_t* boneName, FVector& outLoc);

// World rotation of a bone or socket (GetSocketRotation); false if not found. Game thread.
bool GetBoneWorldRotationByName(void* skelMeshComp, const wchar_t* boneName, FRotator& outRot);

// One bone of the evaluated pose: world position and the parent's index in the same array (-1 =
// root).
struct BonePoint {
    FVector world;
    int32_t parent;
};

// Every bone's world position and parent index; the bone graph is cached per component, the
// positions re-read each call (one dispatch per bone: a diagnostic budget, never a hot path).
// Returns the bone count. Game thread.
int CollectSkeletonBonePoints(void* skelMeshComp, std::vector<BonePoint>& out);

// ACharacter's capsule half-height (UCapsuleComponent::CapsuleHalfHeight); 0.f if none. Game
// thread.
float GetActorCharacterHalfHeight(void* mainPlayerPawn);

// SetVisibility(visible, propagate) + SetHiddenInGame(!visible, propagate): shows a remote pawn's
// body meshes (an unpossessed pawn never unhides them) or hides the orphan's editor visualisers.
// Pass propagate=false to hide a component whose children must stay visible (ACharacter::Mesh can
// be the body mesh's AttachParent). Game thread.
bool SetComponentVisible(void* component, bool visible = true, bool propagate = true);

// Force VisibilityBasedAnimTickOption = AlwaysTickPoseAndRefreshBones (a byte write; no setter
// exists): an unrendered remote body otherwise stops posing. Game thread.
bool SetAnimTickAlways(void* skeletalMeshComponent);

// USkinnedMeshComponent::SetSkeletalMesh(NewMesh, bReinitPose=true), resolved on the owning class.
// Game thread.
bool SetSkeletalMesh(void* skeletalMeshComponent, void* skeletalMeshAsset);

// SetSkeletalMesh(null): render nothing without touching visibility or AttachParent (a hide on a
// parent whose child is a simulating ragdoll would cascade). Game thread.
bool ClearSkeletalMesh(void* skeletalMeshComponent);

// USkeletalMeshComponent::SetAnimClass: assign and instantiate an AnimBP class. Game thread.
bool SetAnimClass(void* skeletalMeshComponent, void* animBlueprintClass);

// UPrimitiveComponent::CreateDynamicMaterialInstance: the slot's MID, parented to its current
// material; overriding a texture parameter re-skins the slot with no cooked material (the skins,
// docs/players.md). Game thread.
void* CreateDynamicMaterialInstance(void* component, int32_t elementIndex);

// UMaterialInstanceDynamic::SetTextureParameterValue; the kel body materials expose their diffuse
// as 'tex'. Game thread.
bool SetTextureParameterValue(void* materialInstanceDynamic, const wchar_t* paramName, void* texture);

// UStaticMeshComponent::SetStaticMesh, resolved on the owning class; recomputes bounds and
// collision. Rejects null. Game thread.
bool SetStaticMesh(void* staticMeshComponent, void* staticMeshAsset);

// USceneComponent::SetMobility (0 Static, 1 Stationary, 2 Movable). A runtime-spawned
// AStaticMeshActor defaults to Static, on which SetStaticMesh and SetActorLocation are silent
// no-ops, so a moving proxy must be Movable first. Game thread.
bool SetComponentMobility(void* sceneComponent, uint8_t mobility);

// UPrimitiveComponent::SetMaterial; null reverts the slot to the mesh asset's default (the trash
// proxy clears a stale clump override that way). Game thread.
bool SetComponentMaterial(void* primitiveComponent, int32_t elementIndex, void* material);

// UStaticMesh::GetMaterial(index); null on failure. Game thread.
void* GetStaticMeshMaterial(void* staticMeshAsset, int32_t materialIndex);

// UActorComponent::K2_DestroyComponent (`contextObject` = the owning actor, for the engine's auth
// check). Game thread.
bool DestroyComponent(void* component, void* contextObject);

// AmainPlayer_C::light_R, the flashlight USpotLightComponent; nullptr if the pawn or the slot is
// dead. Game thread.
void* GetMainPlayerLightR(void* mainPlayer);

// A flashlight light's authoritative state, for the wire payload.
struct FlashlightSnapshot {
    float intensity;
    float outerConeAngle;
    float innerConeAngle;
    bool  visible;
};

// Read the snapshot off a USpotLightComponent; false if `light` is null or dead. Game thread.
bool ReadFlashlightSnapshot(void* light, FlashlightSnapshot& out);

// The AmainPlayer_C flashlight bools and mode byte the wire payload mirrors, read in one call. Game
// thread.
struct MainPlayerFlashlightState {
    bool flashlight;       // AmainPlayer_C::flashlight     (canonical on/off)
    bool hasFlashlight;    // AmainPlayer_C::hasFlashlight  (equipped guard)
    bool crankFlashlight;  // AmainPlayer_C::crankFlashlight (_c variant marker)
    uint8_t mode;          // AmainPlayer_C::flashlightMode (focused/spread enum)
};
bool ReadMainPlayerFlashlightState(void* mainPlayer, MainPlayerFlashlightState& out);

// Direct write of AmainPlayer_C::flashlight (the dev auto-toggle; the BP's input-guarded toggle is
// out of reach). Game thread.
bool WriteMainPlayerFlashlight(void* mainPlayer, bool newState);

// ULightComponent::SetIntensity (marks the render state dirty). Game thread.
bool SetLightIntensity(void* light, float newIntensity);

// USceneComponent::SetVisibility alone, with no SetHiddenInGame: ULightComponent's override marks
// the render state dirty without forcing hidden-in-game onto the component. Game thread.
bool SetSceneComponentVisibility(void* sceneComponent, bool newVisibility, bool propagateToChildren);

// USpotLightComponent::SetOuterConeAngle / SetInnerConeAngle (both mark the render state dirty).
// Game thread.
bool SetSpotLightOuterConeAngle(void* spotLight, float newAngle);
bool SetSpotLightInnerConeAngle(void* spotLight, float newAngle);

// ---- Ragdoll / faint display state ----
// AmainPlayer_C drives every ragdoll cause through one UFunction and an AnimBP gate bool; offsets
// are name-resolved, UFunctions cached. Game thread.

// AmainPlayer_C::isRagdoll (the AnimBP gate) and ::dead; false, outs untouched, while unresolved.
bool ReadMainPlayerRagdollState(void* mainPlayer, bool& isRagdoll, bool& dead);

// AmainPlayer_C::ragdollMode(ragdoll, passOut, death); (true, true, false) is the faint pose, which
// works on an unpossessed puppet.
bool SetMainPlayerRagdollMode(void* mainPlayer, bool ragdoll, bool passOut, bool death);

// AmainPlayer_C::forceGetUp(): the get-up of a possessed player (its cleanup is tick-driven, so a
// puppet's flop ends by destroying its spawned body instead).
bool ForceMainPlayerGetUp(void* mainPlayer);

// AmainPlayer_C::forceWakeup(): the unconditional stand-up (the ubergraph block @25800 restores
// movement, capsule collision, the camera and input, reading no gate); the KO recovery's verb.
// Possessed players only.
bool ForceMainPlayerWakeup(void* mainPlayer);

// AmainPlayer_C::canRagdoll (@0x0D10), ragdollMode()'s precondition: false early-outs every ragdoll
// cause. The Killer Wisp false-grab window forces it false on the host, since an HP pin cannot stop
// a ragdoll death. A raw masked write. Game thread.
bool SetMainPlayerCanRagdoll(void* mainPlayer, bool allowed);

// Read the same bool back, so a test can assert the gate took.
bool ReadMainPlayerCanRagdoll(void* mainPlayer, bool& allowed);

// AmainPlayer_C::"Add Player Damage": the owning peer applies a host-relayed enemy hit on its own
// possessed pawn, so it runs through that peer's armour BP and drops saveSlot.health; early-outs on
// a puppet. `blood` is not cosmetic: it gates the bloodLoss effect (@2784), so a hit without it
// produces a death the game never produces. Game thread.
bool InvokeAddPlayerDamage(void* mainPlayer, float damage, bool blood = false);

// The "Add Player Damage" UFunction pointer, for the Killer Wisp PRE-interceptor that zeroes the
// wisp's damage to the host during a false grab; null until mainPlayer_C loads.
void* AddPlayerDamageFunctionPtr();

// ---- The puppet's faint display ----
// A puppet never runs ragdollMode: its playerRagdoll_C's lifecycle assumes a possessed, ticking
// player, and on a tickless orphan the actor is never reaped and its PhysX keeps simulating. The
// display spawns the ragdoll body directly (SpawnPlayerRagdollBody), hides the puppet's meshes and
// pelvis-attaches the puppet (RagdollDisplay in coop/player/remote_player_ragdoll).

// ---- Damage body pulse (material swap) ----
// SavedMaterial (types.h) is (component, slot, original); the puppet renders two body meshes (Mesh
// @0x280 and mesh_playerVisible), so the saved set spans both. The caller owns the vector.
using ue_wrap::SavedMaterial;

// Swap both body meshes' materials to the hurt-flash material (a skeletal gore skin, so it renders
// red), caching the originals into `saved`; Restore puts them back. False, with no effect, when
// anything does not resolve. Game thread.
bool ApplyHurtFlashMaterial(void* puppet, std::vector<SavedMaterial>& saved);
bool RestoreHurtFlashMaterial(void* puppet, std::vector<SavedMaterial>& saved);

// Eager-resolve the hurt material and the material UFunctions once per puppet spawn. Game thread.
void WarmupHurtFlashCache();

// A loaded UMaterialInterface by object name; nullptr if not loaded. Game thread.
void* ResolveMaterialByName(const wchar_t* name);

// Spawn the game's playerRagdoll_C for `ownerPlayer` as the visible flop body: Player @0x248
// stamped before Finish so BeginPlay configures the mesh from the owner, then the simulation
// started. Death-free (ragdollMode would kill the host). The AActor*, or nullptr. Game thread.
void* SpawnPlayerRagdollBody(void* ownerPlayer, const FVector& location, const FRotator& rotation);

// Attach `actor` to `body`'s mesh at the pelvis bone (KeepWorld), so it follows the flop; Detach
// reverses it. Game thread.
bool AttachActorToRagdollBody(void* actor, void* body);
bool DetachActorFromRagdollBody(void* actor);

// Attach `actor` to `component` at a socket, SnapToTarget (the Killer Wisp grab-hold);
// DetachActorFromParent reverses it. Game thread.
bool AttachActorToComponentSocket(void* actor, void* component, const wchar_t* socket);
bool DetachActorFromParent(void* actor);

// ---- Generic actor root-physics substrate ----
// Root-component primitives through K2_GetRootComponent, never the Aprop_C mesh offset, so they
// work on the non-Aprop_C trash clump. Game thread; each IsLive-gates its arguments.

// SetSimulatePhysics on the root primitive: freeze a held mirror, thaw it on release.
bool SetActorSimulatePhysics(void* actor, bool simulate);

// Force the root component Movable: a Static root silently ignores SetActorLocation (the call still
// returns true), and a save-loaded chipPile rests at Static.
bool SetActorRootMovable(void* actor);

// SetCollisionEnabled on the root (0 None, 1 QueryOnly, 2 PhysicsOnly, 3 QueryAndPhysics); a thrown
// mirror needs 3 to land.
bool SetActorRootCollisionEnabled(void* actor, uint8_t collisionType);

// SetNotifyRigidBodyCollision on the root; false keeps the collision but silences the
// OnComponentHit BP events (a mirror clump's landing handler would spawn a second pile).
bool SetActorRootNotifyRigidBodyCollision(void* actor, bool notify);

// The root's linear (cm/s) and angular (deg/s) velocity: the throw-energy transfer on release.
bool GetActorRootPhysicsVelocity(void* actor, FVector& outLin, FVector& outAng);
bool SetActorRootPhysicsVelocity(void* actor, const FVector& lin, const FVector& ang);
// Angular only, for a turning author: writing the linear component of a resting rig wakes it and it
// sinks.
bool SetActorRootPhysicsAngularVelocity(void* actor, const FVector& ang);

// The root's mass in kg (GetMass); 0 on failure. The native throw speed scales as
// 15000/max(mass,10).
float GetActorRootMass(void* actor);

// The root surface's UPhysicalMaterial (GetMaterial(0), then GetPhysicalMaterial()), the input
// lib_C::physSound takes; equivalent to the trace's answer for the single-material clump and pile.
// Null on failure.
void* GetActorRootPhysicalMaterial(void* actor);

// True only when the root body is positively confirmed asleep or not simulating; false on any
// failure. The host stamps kAtRest from it.
bool IsActorRootBodyAtRest(void* actor);

// PutRigidBodyToSleep on the root: a teleport wakes the body, and a thousand woken props that never
// re-sleep are a permanent physics scene, so the client sleeps a kAtRest prop right after
// converging it. The body stays dynamic and grabbable.
bool PutActorRootBodyToSleep(void* actor);

// The sender's ragdoll stream: the native ragdoll's (ragdollActor @0xC40) pelvis world transform
// and velocity (cm/s, deg/s); false while not ragdolling. Game thread.
bool ReadLocalRagdollPelvisPhysics(void* mainPlayer, FVector& outLoc, FRotator& outRot,
                                   FVector& outLinVel, FVector& outAngVel);

// The receiver's ragdoll stream: set the mirror body's pelvis linear and angular velocity so it
// tracks the sender's flop. Game thread.
void DriveRagdollBodyPelvisVelocity(void* body, const FVector& linVel, const FVector& angVel);

// A ragdoll actor's simulating SkeletalMesh (Aragdoll_C::SkeletalMesh @0x230); null if dead. Game
// thread.
void* GetRagdollBodyMesh(void* ragdollActor);

// The local player's native ragdoll mesh (ragdollActor @0xC40 is non-null exactly while
// ragdolling). Game thread.
void* GetLocalRagdollBodyMesh(void* mainPlayer);

// A long-lived WorldContextObject for the deferred-spawn pair: the GameInstance, else the World.
void* GetWorldContext();

// USoundAttenuation config; the enum values follow UE4.27's EAttenuationShape / DistanceModel /
// FalloffMode, and the defaults are the flashlight-click tuning.
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

// Construct a USoundAttenuation (SpawnObject plus raw field writes; the fields have no setters) and
// AddToRoot it: callers keep the pointer in a C++ static, invisible to UE's GC, and an unrooted
// object would be reaped under PlaySoundAtLocation. nullptr on failure. Game thread.
void* SpawnSoundAttenuation(const SoundAttenuationConfig& cfg);

// UGameplayStatics::PlaySoundAtLocation: a one-shot 3D sound at `location` owned by `worldContext`;
// `attenuation` may be null (2D). Game thread.
void PlaySoundAtLocation(void* worldContext, void* sound, const FVector& location,
                         void* attenuation, float volume = 1.f, float pitch = 1.f);

// FRotator to FQuat, UE4.27's own formula (ZYX, left-handed): the body of FRotator::Quaternion() in
// Runtime/Core/Public/Math/Rotator.h. The negative Y term is UE4's convention, not a defect; a
// right-handed reference shows the opposite signs.
void RotatorToQuat(float pitchDeg, float yawDeg, float rollDeg,
                   float& qx, float& qy, float& qz, float& qw);

// ---- Navigation and locomotion ----
// A baked-NavMesh path query and pawn movement input, for the bot director. Game thread.

// A point reachable within `radiusCm` of `origin` (K2_GetRandomReachablePointInRadius on the CDO);
// false when the navmesh has no polygon there.
bool RandomReachablePoint(void* worldContext, const FVector& origin, float radiusCm, FVector& out);

// A route from `start` to `end` (FindPathToLocationSynchronously on the CDO), its PathPoints into
// `outPts`; false on no route.
bool FindNavPath(void* worldContext, const FVector& start, const FVector& end,
                 std::vector<FVector>& outPts);

// APawn::AddMovementInput (resolved on Pawn): accumulates ControlInputVector, which the CMC
// consumes each tick, so re-issue it every frame.
void AddMovementInput(void* pawn, const FVector& worldDir, float scale, bool force);

}  // namespace ue_wrap::engine
