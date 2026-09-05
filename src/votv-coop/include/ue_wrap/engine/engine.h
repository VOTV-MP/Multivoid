// ue_wrap/engine.h -- engine operations built on reflection.
//
// The engine-wrapper layer (principle 7): each function marshals one engine UFunction call or one
// reflected field access behind a plain C++ signature. No gameplay, network or coop state lives
// here.
//
// Nearly every call dispatches a UFunction through reflection::CallFunction (ProcessEvent) and must
// run on the game thread (post through ue_wrap::game_thread::Post); a call from any other thread
// crashes the engine. The few raw reads that are safe off the game thread say so.

#pragma once

#include "ue_wrap/core/types.h"

#include <string>
#include <vector>

namespace ue_wrap::engine {

// Run a console command (UKismetSystemLibrary::ExecuteConsoleCommand): "open <map>", "HighResShot",
// any console verb. The library CDO, the UFunction and a world context resolve on first use and are
// cached. False when anything fails to resolve. Game thread only.
bool ExecuteConsoleCommand(const wchar_t* command);

// World pause through UGameplayStatics::IsGamePaused / SetGamePaused on the cached world context. A
// paused peer stops ticking its world, so a session keeps every peer unpaused. IsGamePaused returns
// false on any resolution miss (a caller that clears the pause never sees a false "paused");
// SetGamePaused returns the engine's own success bool. Game thread only.
bool IsGamePaused();
bool SetGamePaused(bool paused);

// Travel to the main menu through the game's own verb, AmainGamemode_C::transition("/Game/menu")
// (the full path; the short name does not resolve). A direct gamemode call: it needs no pause and
// works for a dead player. Hold game_thread::SetTransparentBypass over the travel and the time at
// the menu so the ProcessEvent detour neither stalls the world teardown nor churns at the menu.
// False when it cannot dispatch. Game thread only.
bool ReturnToMainMenu();

// Load a save slot (a story save such as "s_may2026") and enter gameplay through the game's own
// load entry: GameplayStatics::LoadGameFromSlot, mainGameInstance_C::setSaveSlotObject with
// loadObjects=true, then the gameplay map. The map is untitled_1 for every mode; the save selects
// story or sandbox. `forceGameMode` >= 0 writes that enum_gamemode ordinal instead of deriving it
// from the slot-name prefix (the coop slot `zcoop_<pid>` has no game-known prefix; its mode arrives
// with the save transfer); -1 derives it from the prefix. False if the slot is missing or empty or
// the load cannot dispatch. Game thread only.
bool LoadStorySave(const wchar_t* slot, int forceGameMode = -1);

// Drop the cached USaveGame* and the GameMode latch so a second in-process LoadStorySave (a rejoin
// with a fresh zcoop_ slot) re-loads from disk instead of re-registering the stale cached object.
// Game thread only.
void ResetCachedSave();

// A fresh New Game: like LoadStorySave, but with a blank save object
// (GameplayStatics::CreateSaveGameObject(saveSlot_C)) instead of a disk slot. A fresh client world
// holds only level-default props, so the host's connect snapshot mirrors the host's whole world
// onto it with nothing to reconcile away. `storyMode` picks the story or sandbox GameMode. Polled
// like LoadStorySave. Game thread only.
bool StartFreshGame(bool storyMode);

// ---- Save-object-ready hook ----
// A callback the inventory layer registers to overwrite a freshly loaded or created save object's
// player-scoped inventory before the game's own loadObjects() materialises it on BeginPlay. It
// fires exactly once per LoadStorySave or StartFreshGame, on the game thread, with the USaveGame*
// (a UsaveSlot_C*) about to be registered and travelled into: the inventory arrays are present and
// the world is not yet built from them, so the game's own load then builds the live inventory from
// whatever the hook leaves in the object. engine.cpp fires it unconditionally; the hook must be a
// no-op unless a per-player inventory is pending. Registering null disarms it. The substrate
// exposes the injection point and holds no inventory knowledge.
using SaveObjectReadyHook = void(*)(void* saveSlotObject);
void SetSaveObjectReadyHook(SaveObjectReadyHook hook);

// The game encodes a save's mode only in the slot-name prefix (story "s_", infinite "i_", sandbox
// "b_", halloween "SPOOKY_", ambience "a_", solar "l_"). Both functions below wrap
// Uui_saveSlots_C::getSavePrefix, a pure mode-to-prefix map on the CDO, so the load path and the
// save browser share one prefix source. enum_gamemode ordinals are not the submenu button order
// (story=0, infinite=1, sandbox=4, ...). The widget CDO resolves lazily: until the menu or a
// gameplay transition loads it, both return false / -1. Game thread only.

// The prefix string for `mode` (a TEnumAsByte<enum_gamemode::Type>) into `out`; false while the
// widget CDO or getSavePrefix is unresolved.
bool GetSavePrefix(uint8_t mode, std::wstring& out);

// The enum_gamemode ordinal of save slot `slot` from its name prefix (the longest matching prefix
// wins): 0..7, or -1 on no match or an unresolved widget.
int DeriveModeFromSlot(const wchar_t* slot);

// Spawn an actor of `actorClass` (a UClass*) at `location` through the deferred GameplayStatics
// pair (BeginDeferredActorSpawnFromClass + FinishSpawningActor), the path the K2
// SpawnActorFromClass node uses. Always spawns (CollisionHandlingOverride = AlwaysSpawn). Returns
// the AActor* (a UObject*) or nullptr. `inertPawn` makes a spawned APawn inert as a local player:
// in the deferred window, before BeginPlay, it zeroes AutoPossessPlayer, AutoPossessAI and
// AutoReceiveInput and sets bBlockInput, so the pawn never acquires a PlayerController and never
// takes the local player's input or view. Game thread only.
void* SpawnActor(void* actorClass, const FVector& location, bool inertPawn = false);

// The split form: BeginDeferredSpawn, then write class-specific fields on the returned actor
// before Init and BeginPlay run, then FinishDeferredSpawn (the K2 node's Expose On Spawn pattern).
// The transform given to FinishDeferredSpawn must equal the one given to BeginDeferredSpawn; the
// engine re-applies it after construction. A wire-spawned entity gets its variant state (chipType,
// Shape, ...) stamped in the window, so Init never rolls defaults that an observer would then
// broadcast. Game thread only.
void* BeginDeferredSpawn(void* actorClass, const FVector& location, const FRotator& rotation);
bool  FinishDeferredSpawn(void* actor, const FVector& location, const FRotator& rotation);

// Self-test of the world-context staleness guard: corrupts the cached GUObjectArray index so
// IsLiveByIndex fails (a freed World after a level reload), runs EnsureWorldContext and checks that
// it dropped the stale entry and re-resolved a live context. True on recovery; leaves the cache
// valid. Behind an autotest env flag, not a shipping path. Game thread only.
bool DebugCheckWorldContextRecovery();

// AActor::K2_GetActorLocation on `actor`. Returns (0,0,0) if it cannot be called.
FVector GetActorLocation(void* actor);

// The checked read: false when the location could not be obtained, `out` left zeroed. Use this
// wherever a wrong answer grants something. GetActorLocation returns a default FVector on every
// failure path, and (0,0,0) is the world origin, an ordinary position, so a failed read reports
// "this actor is at the origin": in a distance or authority test that is fail-open for anything
// near the origin and a false denial for everything else. Game thread.
bool TryGetActorLocation(void* actor, FVector& out);

// AActor::GetActorScale3D on `actor` (root component world scale). Unit scale (1,1,1) on failure:
// callers stamp it straight into spawn transforms, where a zero scale would collapse the mirror
// invisibly. The game saves a prop's scale inside its object transform, so a mirror carries it.
// Game thread only.
FVector GetActorScale3D(void* actor);

// True iff `actor` was spawned by a UChildActorComponent (a kerfur's eye camera prop_camera_good_C,
// a console screen child, ...). The game excludes these from its world-object universe
// (Aprop_C::ignoreSave = ignoreSav || IsChildActor(), in prop_base): the parent's construction
// script spawns, positions and destroys them on every peer, so they never carry an independent
// cross-peer identity. A raw read of the reflected AActor::ParentComponent weak pointer (offset
// resolved once), no dispatch: safe off the game thread. A set weak pointer (index >= 0,
// serial != 0) means child actor even while the parent is mid-teardown; the semantic is "owned by
// a parent", not "parent currently live".
bool IsChildActor(void* actor);

// The child-actor parent link, read from the same AActor::ParentComponent weak pointer. On success
// `outComponentName` receives the owning UChildActorComponent's own name, the half of a child
// actor's identity that is stable across processes: a child actor is named
// "<component>_GEN_VARIABLE_<class>_CAT_<n>", and for a runtime-created parent the trailing <n> is
// a per-process allocation counter that differs between host and client for the same object, so
// the component name plus the parent's identity is what two peers can agree on. Returns the parent
// actor (the component's Outer), or null when the actor is not a child actor or the parent is
// gone. Array-slot reads only, no dispatch: safe off the game thread.
void* ParentActorOf(void* actor, std::wstring* outComponentName = nullptr);

// AActor::SetActorScale3D(FVector) on `actor` (root component relative scale). Returns the dispatch
// success (the engine function is void). The trash proxy applies the host's per-form scale (a
// clump and a pile differ) on every convert and spawn so the mirror is host-sized. Game thread
// only.
bool SetActorScale3D(void* actor, const FVector& scale);

// UKismetSystemLibrary::CollectGarbage: schedules a full GC purge at the end of the current frame
// (the engine's own post-level-transition pattern). The adoption sweep pairs its destruction burst
// of about a thousand actors with this, so the pending-kill garbage does not sit at a
// multi-gigabyte plateau until UE's 61 s periodic purge. False if the library or the function is
// unresolved. Game thread only.
bool ForceGarbageCollection();

// AActor::GetActorForwardVector on `actor` (the unit facing vector); (0,0,0) on failure.
FVector GetActorForwardVector(void* actor);

// AActor::K2_GetActorRotation on `actor` (world rotation). Zero on failure. The pose-snapshot read
// of the send path, which wires only yaw. Game thread only.
FRotator GetActorRotation(void* actor);

// AActor::GetVelocity on `actor` (world velocity, cm/s). Zero on failure. Its horizontal magnitude
// is the walk speed the remote AnimBP locomotion blend takes. Game thread only.
FVector GetActorVelocity(void* actor);

// AActor::K2_SetActorLocation on `actor` (bSweep=false, bTeleport=true: a snap to the absolute
// pose, the pose-apply path). Returns the engine's success bool. Game thread only.
bool SetActorLocation(void* actor, const FVector& location);

// AActor::K2_SetActorRotation on `actor` (bTeleportPhysics=true): the pose-apply path for
// orientation. Game thread only.
bool SetActorRotation(void* actor, const FRotator& rotation);

// USceneComponent::K2_GetComponentRotation on `component` (the resolved world rotation). Zero on
// failure. Reads a kerfur's ACharacter::Mesh body facing, which the actor BP aims at the local
// player independently of the actor root. Game thread only.
FRotator GetComponentWorldRotation(void* component);

// USceneComponent::K2_SetWorldRotation on `component` (bSweep=false, bTeleport=true). Returns the
// dispatch success. Drives a mirror kerfur's ACharacter::Mesh world rotation to the host's streamed
// body facing (the mirror's actor tick is off, so nothing overwrites it). Call after
// SetActorRotation: moving the actor root re-bases the child mesh's world transform. Game thread
// only.
bool SetComponentWorldRotation(void* component, const FRotator& rotation);

// The world rotation of `actor`'s visible StaticMesh component, or the actor rotation if it owns
// none. A chipPile's per-instance visual variety lives on its StaticMesh component's relative
// rotation (a random roll applied in its UserConstructionScript), not on the actor root, which
// stays identity, so GetActorRotation reads identity for every pile and every mirror would render
// identically oriented. The host captures this so the bare AStaticMeshActor proxy (whose mesh
// sits on its own identity-relative root) reproduces the orientation through a plain
// SetActorRotation. Game thread.
FRotator GetVisibleMeshWorldRotation(void* actor);

// AActor::SetActorTickEnabled. A remote pawn must not run the local player's per-frame BP
// EventTick, which re-applies view, post-process and exposure to the shared screen every frame.
// Game thread only.
bool SetActorTickEnabled(void* actor, bool enabled);

// AActor::SetActorHiddenInGame: visual only (bHidden @0x58; collision is the separate
// bActorEnableCollision @0x5C). A deferred host mirror stays hidden until reconcile resolves it,
// then is revealed. Game thread.
bool SetActorHiddenInGame(void* actor, bool hidden);

// AActor::SetActorEnableCollision. Pair it with SetActorHiddenInGame(true) on a deferred-hidden
// mirror so it is neither grab-trace-hittable nor physics-active while invisible (hide alone
// leaves collision on). Game thread.
bool SetActorEnableCollision(void* actor, bool enabled);

// APawn::GetController on `pawn`: the AController*, or nullptr.
void* GetController(void* pawn);

// AController::GetControlRotation on `controller`: the view rotation the game drives from
// mouse-look. Reading it for a freecam gives smooth, game-native look with no raw-mouse handling.
// (0,0,0) on failure. Game thread only.
FRotator GetControlRotation(void* controller);

// Direct write to AController::ControlRotation (offset 0x0288). The engine reads this UPROPERTY
// next tick to derive the view rotation; the BP-callable SetControlRotation UFunction only stores
// the value, so the direct write is equivalent and cheaper. Game thread only.
void SetControlRotation(void* controller, const FRotator& rot);

// K2_TeleportTo(DestLocation, DestRotation): the engine's own "teleport an actor across the
// world". K2_SetActorLocation can be silently reverted by Character / CharacterMovement
// constraints when the move is far; K2_TeleportTo handles large teleports, including across the
// movement component's nav and floor checks. Returns the UFunction's bool. Game thread only.
bool TeleportTo(void* actor, const FVector& location, const FRotator& rotation);

// GetActorBounds(bOnlyCollidingComponents, Origin, BoxExtent, bIncludeFromChildActors): the
// actor's world-space axis-aligned bounding box, computed each tick from the rendered mesh's
// extent, not from the bone hierarchy. The puppet's lowest visible point is
// Origin.Z - BoxExtent.Z. False if unresolved. Game thread only.
bool GetActorBounds(void* actor, bool onlyColliding, FVector& outOrigin, FVector& outBoxExtent);

// APlayerController::SetViewTargetWithBlend(NewViewTarget, BlendTime): repoint the player's view
// to `newViewTarget` (a freecam ACameraActor, say), blending over `blendTime` seconds. Game thread
// only.
bool SetViewTargetWithBlend(void* playerController, void* newViewTarget, float blendTime);

// APlayerController::ProjectWorldLocationToScreen(World, Screen&, bViewportRelative): project a
// world point to viewport-pixel screen coordinates through the local player's camera. True, with
// `outScreen` filled, when the point is in front of the camera; false, `outScreen` untouched, when
// it is behind. `playerController` is the local player's controller (GetController(localPawn)).
// The screen-space nameplates project on the game thread (this is a UFunction) and snapshot the
// result for the render thread to draw. Game thread only.
bool ProjectWorldToScreen(void* playerController, const FVector& world,
                          FVector2D& outScreen, bool viewportRelative = false);

// The current view camera's world location and rotation (APlayerCameraManager::GetCameraLocation /
// GetCameraRotation on the live manager); a freecam seeds itself at the player's eye from these.
// Zero on failure. Game thread only.
FVector GetCameraLocation();
FRotator GetCameraRotation();

// APawn::SpawnDefaultController on `pawn`: spawns the pawn's AIControllerClass and possesses.
// Gives the body's AnimBP a valid controller (so it poses) with no viewport, input or camera (an
// AIController is not a PlayerController), so it cannot hijack the local player. Game thread only.
bool SpawnDefaultController(void* pawn);

// APawn::DetachFromControllerPendingDestroy on `pawn`: unpossess (the pawn keeps existing; only
// the controller link is severed). Game thread only.
bool DetachFromController(void* pawn);

// AActor::K2_DestroyActor on `actor`. Game thread only.
bool DestroyActor(void* actor);

// The UCharacterMovementComponent default subobject of a Character; nullptr if the pawn is not a
// Character or has no CMC. The puppet spawn parks it (SetComponentTickEnabled) so the orphan's CMC
// does not fight the per-tick pose drive.
void* GetCharacterMovementComponent(void* characterPawn);

// The UStaticMeshComponent subobject of `actor` (an AStaticMeshActor's root, or any actor that
// owns one), found by a reflection child walk; nullptr if none. Reaches the trash proxy's mesh
// component for SetStaticMesh and collision. Game thread.
void* GetStaticMeshComponent(void* actor);

// UActorComponent::SetComponentTickEnabled. Parks the orphan puppet's CharacterMovementComponent
// at spawn: ticking, it would integrate gravity and walking and reset Velocity every tick,
// fighting the pose drive. Game thread only.
bool SetComponentTickEnabled(void* component, bool enabled);

// Direct write of a UObject* slot at `byteOffset` inside `target` (the runtime Aprop_C wire-key
// restore on a remote prop's spawn, where a UFunction call would be heavyweight). Game thread
// only.
void WriteObjectField(void* target, size_t byteOffset, void* value);

// If `localPlayer` (a live AmainPlayer_C*) is grabbing `actor` through the light-grab path
// (mainPlayer.grabbing_actor @0x07D0 + grabHandle @0x0688), dispatch
// UPhysicsHandleComponent::ReleaseComponent on the handle and clear the grabbing_actor slot. True
// iff a release happened. Release before destroy: K2_DestroyActor leaves PHC.GrabbedComponent
// (@+0xB0) dangling, and UPhysicsHandleComponent::TickComponent dereferences it next frame.
// False, with no effect, when any input is null or dead. Game thread only.
bool ReleaseMainPlayerGrabIfHolding(void* localPlayer, void* actor);

// Read-only twin of ReleaseMainPlayerGrabIfHolding: true iff `localPlayer`'s grabbing_actor slot
// holds `actor`. No mutation, no dispatch. The snapshot bind paths skip the physics reconcile and
// the teleport-converge for a prop the local player is holding: forcing SimulatePhysics off
// mid-hold breaks the PhysicsHandle grab. False for null or dead inputs. Game thread only.
bool IsMainPlayerGrabbing(void* localPlayer, void* actor);

// Eager-resolve the PHC.ReleaseComponent UFunction cache ReleaseMainPlayerGrabIfHolding uses.
// Call once during init, when the PhysicsHandleComponent class is loaded, so the first cross-peer
// PropDestroy does not meet an unresolved class and fall through to the warn-and-clear fallback.
// Idempotent; true iff the cache is populated after the call. Game thread only.
bool WarmupPhcReleaseCache();

// UPhysicsHandleComponent::GrabbedComponent, the held UPrimitiveComponent* at the fixed offset
// sdk_profile.h documents (confirmed against ReleaseComponent_Impl in the disassembly).
// PHC.ReleaseComponent PRE-observers read it to log what is about to be released. The component
// pointer, or nullptr if `phc` is null or dead or the slot is empty. Game thread only.
void* ReadPhysicsHandleGrabbedComponent(void* phc);

// Snapshot of AmainPlayer_C grab-state UPROPERTIES, read in one dispatch from adjacent reflected
// offsets. They drive the pickup/drop diagnostics of the grab observers and the held-prop
// replication: grabbingActor and holdingActor cover the two carry paths (physics-handle props vs
// the chipPile/clump-style carry). False on a null or dead pawn, `out` then zero-initialised.
// `holdingActor` stays null when MainPlayer_holding_actor() is unresolved in this build (the field
// arrived in a later game recook). Game thread only.
struct MainPlayerGrabState {
    void*  grabbingActor;  // AmainPlayer_C::grabbing_actor (PHC-held prop, or null)
    void*  holdingActor;   // AmainPlayer_C::holding_actor  (chipPile/clump morph carry, or null)
    bool   grabsHeavy;     // AmainPlayer_C::grabsHeavy     (PCC heavy-grab BP flag)
    bool   heavy;          // AmainPlayer_C::Heavy          (BP-side heavy state mirror)
    float  grabLen;        // AmainPlayer_C::grabLen        (Timeline current grab length)
};
bool ReadMainPlayerGrabState(void* mainPlayer, MainPlayerGrabState& out);

// The actor the local player is aiming at (AmainPlayer_C::lookAtActor @0x0AA0); on an E-press, the
// door or interactable being used. nullptr if unresolved, aiming at nothing, or the pawn is dead.
// Game thread.
void* ReadMainPlayerLookAtActor(void* mainPlayer);

// Direct write of AmainPlayer_C::lookAtActor, the cached interaction-trace result the BP
// re-derives each tick (not a field a setter UFunction manages). Lets a test aim the BP's
// icast(lookAtActor) at a chosen actor for the single InpActEvt_use dispatch that immediately
// follows; the next tick's trace overwrites it. device_screen.cpp uses the same pattern
// (ClearAimForDispatch nulls and restores it around an enter dispatch). False on a null or dead
// pawn or an unresolved offset. Game thread only.
bool WriteMainPlayerLookAtActor(void* mainPlayer, void* actor);

// The local player's radial-menu confirm state: AmainPlayer_C::releaseEToUse @0x0E88 (true on the
// "release E to use" confirm) and actionIndex @0x0A98 (the highlighted option's index into the
// target's getActionOptions list). False, outs untouched, if the pawn is dead or the fields are
// unresolved. The kerfur radial verb is relayed from these because the actionName dispatch itself
// is EX_LocalVirtualFunction, invisible to ProcessEvent. Game thread.
bool ReadMainPlayerRadialSelect(void* mainPlayer, bool& releaseEToUse, int32_t& actionIndex);

// Write AmainPlayer_C::grabbing_actor and ::grabbing_component in one dispatch. The autotest's
// synthetic grab drives UPhysicsHandleComponent.GrabComponentAtLocation and Release directly and
// keeps the BP-visible "what am I holding" mirror in step (the BP reads these slots in its
// attach-prop and drop-impulse timelines). Pass nullptr/nullptr to clear. False on a null or dead
// pawn. Game thread only.
bool WriteMainPlayerGrabbingPair(void* mainPlayer, void* actor, void* component);

// AmainPlayer_C single-component-pointer accessors; each returns the UObject* in one component
// slot of the local pawn:
//   GrabHandle    the UPhysicsHandleComponent driving the normal-prop grab
//   HeavyGrabPCC  the UPhysicsConstraintComponent driving the heavy-prop grab
//   GrabTimeline  the UTimelineComponent driving the grab animation curve
// The autotest's synthetic grab dispatches UPhysicsHandleComponent.GrabComponentAtLocation,
// PCC.SetConstrained and Timeline.PlayFromStart against these real components without touching
// the mainPlayer_C layout itself. nullptr on a null or dead pawn or an empty slot. Game thread
// only.
void* ReadMainPlayerGrabHandle(void* mainPlayer);
void* ReadMainPlayerHeavyGrabPCC(void* mainPlayer);
void* ReadMainPlayerGrabTimeline(void* mainPlayer);

// Diagnostic: log every FProperty on a UClass (name, offset, size), to confirm that a
// reflection-resolved offset is the property meant and not a sibling of the same FName. Walks the
// class's own properties only (no SuperStruct climb). Game thread only.
void LogClassProperties(const wchar_t* className);

// USceneComponent world location (K2_GetComponentLocation) and forward vector (GetForwardVector):
// a Camera component's eye point and look direction, say. (0,0,0) on failure.
FVector GetComponentLocation(void* component);
FVector GetComponentForwardVector(void* component);

// USceneComponent::RelativeLocation, the BP-authored relative offset to the attach parent, read
// directly at +0x011C rather than through a UFunction. K2_GetComponentLocation returns the
// computed world transform, which is transiently unsettled during BP construction and save-load
// init; this raw read returns the field's stored value, the BP-authored constant once the
// construction script has completed. The puppet spawn captures the mesh_playerVisible
// RelativeLocation.Z from it once the local player is settled. (0,0,0) on null input; a true
// (0,0,0) on a non-root component is unusual and worth logging.
FVector GetComponentRelativeLocation(void* component);

// A UParticleSystemComponent's `Template` (the UParticleSystem* it renders), a raw pointer read at
// a reflection-resolved, cached offset. The cue sync identifies which cosmetic cue a live PSC
// belongs to from it: the EX_CallMath SpawnEmitterAtLocation that created the component is
// invisible to the ProcessEvent detour, so the component's template is matched instead. nullptr
// on null input, an unresolved offset or a non-PSC component.
void* GetParticleSystemTemplate(void* particleSystemComponent);

// Build a screen-space HUD widget (a UUserWidget with one multi-line UTextBlock root) and add it
// to the viewport. Unlike the world-space nameplate it renders on the player's screen through
// UUserWidget::AddToViewport, so it needs no stock HUD canvas (which the game does not run).
// HitTestInvisible, so it never steals input. `outer` should be a persistent object (the
// GameInstance) so it survives level loads.
//   alignment   the pivot within the widget: {0,0} top-left, {1,0} top-right, {0,.5} left-middle
//   position    viewport pixel coordinates of the pivot (bRemoveDPIScale=true)
//   justify     ETextJustify: 0=Left, 1=Center, 2=Right
//   fontSize    points
//   color       the text colour (1,1,1,1 is opaque white)
// Returns the root UUserWidget* (outRoot, for a re-attach on level change) and the UTextBlock*
// (outText, driven through SetWidgetText). Game thread only.
bool SpawnScreenTextWidget(void* outer, int zOrder, FVector2D alignment, FVector2D position,
                           int justify, int fontSize, const FLinearColor& color,
                           void** outRoot, void** outText);

// NewObject by class: the reflected UGameplayStatics::SpawnObject(objectClass, Outer). The one way
// this mod mints a UObject it owns; every runtime-built UMG widget comes from here. `outer` must
// be a live UObject that keeps the result reachable. A widget's Outer does not root it: a UMG
// widget stays alive because its panel's `Slots` array, a UPROPERTY, references it, so a widget
// built and never attached is collectable at the next GC.
// nullptr if the SpawnObject UFunction has not resolved yet (it resolves with the same lazy set the
// widget builders use) or if either argument is null. Game thread only.
void* SpawnUObject(void* objectClass, void* outer);

// Set a UTextBlock's text (Conv_StringToText, then UTextBlock::SetText). The HUD feed updates its
// line by rebuilding the whole multi-line string. Game thread only.
bool SetWidgetText(void* textBlock, const wchar_t* text);

// Raw write of a UTextBlock's ColorAndOpacity (@UTextBlock_ColorAndOpacity, the FSlateColor's
// ColorUseRule forced to UseColor_Specified=0). A block hosted by a UWidgetComponent re-renders
// from its properties (bManuallyRedraw=0, RedrawTime=0), so the colour shows next frame with no
// RequestRedraw, and SetWidgetText never clobbers it; a block inside a constructed UMG/Slate tree
// ignores this write (see SetTextBlockColorDispatch). False on a null textBlock. Game thread only.
bool SetTextBlockColor(void* textBlock, const FLinearColor& color);

// Set a UTextBlock's colour through the UTextBlock::SetColorAndOpacity(FSlateColor) setter
// dispatch. Required for a block living in a constructed UMG/Slate tree (the injected menu rows):
// UMG bakes properties into the Slate widget at attach, so the raw write above never reaches it.
// Game thread only.
bool SetTextBlockColorDispatch(void* textBlock, const FLinearColor& color);

// UUserWidget::AddToViewport(zOrder) / RemoveFromViewport: re-attach the HUD feed after a level
// load (the widget object survives with a GameInstance outer, but its Slate viewport tree is torn
// down with the old world). Game thread only.
bool AddWidgetToViewport(void* userWidget, int zOrder);
bool RemoveWidgetFromViewport(void* userWidget);

// ---- Runtime UMG button injection (the menu entry) ----
// Construct a UButton at runtime inside an existing UCanvasPanel, positioned relative to
// `refButton`, a menu UButton already in that canvas (button_start / NEW GAME): its list panel (a
// UVerticalBox) is derived, the new button inserted at the top, refButton's FButtonStyle cloned
// for the look and its VBox slot layout copied, so spacing and indent match. Engine substrate only
// (principle 7): the caller resolves the game menu's canvMenu / button_start.
// The label is styled to match the native menu items (font_ui at size 16, left-justified) and
// tinted cyan to mark the coop entry; tex_btnStart's style is not cloned because its pointer is
// null at some inject timings and the fallback is a Roboto, centred, white default.
// `outButton` receives the spawned UButton* for the click poll (may be null). True on success.
// Game thread only (SpawnObject + UFunction dispatch).
bool InjectCanvasButton(void* refButton, const wchar_t* label, void** outButton);

// Inject a native UTextBlock as a new row directly above `refText`'s row. Expects the game's label
// shape: refText sits in a row panel (a UHorizontalBox) whose slot lives in a UVerticalBox of label
// rows; the new block goes at the top of that VerticalBox with the row's slot layout (padding,
// alignment) and refText's text style (font, colour, shadow, justification) cloned, so it reads as
// one more native label line. The coop version line sits above the game's own txt_version this
// way: a child of the menu, so it shows and hides with the menu, with no viewport add/remove and
// no per-frame gating. `outText` receives the spawned UTextBlock* (driven through SetWidgetText);
// the cloned normal text colour goes to `outColor`, so a caller can restore it after a temporary
// "update available" tint. Either out-param may be null. Engine substrate only (principle 7).
// Game thread only.
bool InjectTextRowAbove(void* refText, const wchar_t* initial,
                        void** outText, FLinearColor* outColor);

// UWidget::IsHovered(): the mouse-over test for the menu click poll (paired with a global
// VK_LBUTTON edge). False if the widget or the UFunction is unresolved. Game thread only.
bool WidgetIsHovered(void* widget);

// UWidget::SetVisibility(ESlateVisibility). The client loading state pairs this
// (HitTestInvisible=3) with SetWidgetRenderOpacity(0) on the game's whole menu widget, so the menu
// disappears both visually (opacity 0) and functionally (HitTestInvisible: the widget and all its
// children stop receiving clicks), and the player cannot click invisible menu options.
// HitTestInvisible rather than Collapsed or Hidden is deliberate: it is still a rendered Slate
// state, so the menu keeps ticking and the same per-tick observer can restore it. slateVis:
// 0=Visible, 1=Collapsed, 2=Hidden, 3=HitTestInvisible, 4=SelfHitTestInvisible. False if the
// widget or the UFunction is unresolved. Game thread only.
bool SetWidgetVisibility(void* widget, uint8_t slateVis);

// UWidget::SetRenderOpacity(float): the visual half of the menu hide (SetWidgetVisibility above).
// Opacity 0 fades the whole menu widget out so only the 3D menu background remains, the canvas the
// connecting screen draws its progress over; 1 restores it. False if the widget or the UFunction
// is unresolved. Game thread only.
bool SetWidgetRenderOpacity(void* widget, float opacity);

// World Z of the lowest bone of this skeletal mesh's currently evaluated pose. Not a "where the
// visible feet are" read on a skeleton that mixes humanoid and non-humanoid bones: the game's
// kerfur skeleton has humanoid foot bones and a 'wheels_R_end' for the non-upgraded variant, so a
// human skin's visible feet are not at its lowest bone. GetBoneWorldZByName with a known foot bone
// is the placement read; this one is a diagnostic.
bool GetLowestBoneWorldZ(void* skelMeshComp, float& outZ);

// Diagnostic: log every bone name and world location on a skeletal mesh component, to find the
// foot bone of a rendered skin whose skeleton has hidden bones at a lower Z. Game thread only.
void DumpAllBonesWorldZ(void* skelMeshComp);

// World Z of one bone (by FName string match) of the mesh component's currently evaluated pose.
// False if not found. With a known foot-bone name this gives the visible-feet world Z, robust
// against authored-but-invisible bones in a multi-variant skeleton. Game thread only.
bool GetBoneWorldZByName(void* skelMeshComp, const wchar_t* boneName, float& outZ);

// World position of the bone named `boneName`, the hot-path-safe by-name read: one
// GetSocketLocation dispatch per call (the FName comes from the global name table, cached per
// distinct name; no skeleton enumeration). Silently anchors to the component transform when the
// mesh lacks the bone (UE's own fallback), which suits HUD and audio anchors. False only on
// outright resolution failure. Game thread only.
bool GetBoneWorldLocationByName(void* skelMeshComp, const wchar_t* boneName, FVector& outLoc);

// World rotation of a named bone or socket (SceneComponent::GetSocketRotation; a bone name is a
// valid socket name). False if the bone is not found or the function is unresolved. Game thread
// only.
bool GetBoneWorldRotationByName(void* skelMeshComp, const wchar_t* boneName, FRotator& outRot);

// One bone of a skeletal mesh's currently evaluated pose (the ragdoll bone visualiser): world
// position plus the index of its parent bone within the same output array (-1 = root).
struct BonePoint {
    FVector world;
    int32_t parent;
};

// Fill `out` (cleared first) with every bone's current world position and parent index on
// `skelMeshComp`. The bone graph (names and parent links through USkinnedMeshComponent::
// GetNumBones / GetBoneName / GetParentBone) is cached per component; each call re-reads only the
// positions (SceneComponent::GetSocketLocation). Returns the bone count (0 on a null component or
// unresolved functions). Cost: one UFunction call per bone per invocation, a diagnostic budget;
// never call it from an always-on hot path. Game thread only.
int CollectSkeletonBonePoints(void* skelMeshComp, std::vector<BonePoint>& out);

// ACharacter capsule half-height (UCapsuleComponent::CapsuleHalfHeight at the fixed offset). 0.f if
// `mainPlayerPawn` is null or has no capsule. The puppet's foot-on-ground placement puts the
// visible feet at source.actor.Z - halfH. Game thread only (a memory read of an engine struct).
float GetActorCharacterHalfHeight(void* mainPlayerPawn);

// Set a USceneComponent's visibility: SetVisibility(visible, propagate) + SetHiddenInGame(!visible,
// propagate). visible=true shows a remote pawn's third-person body meshes (an unpossessed pawn
// never runs the gameplay code that unhides them); visible=false hides the orphan's editor-debug
// visualisers (ArrowComponent, BillboardComponent), which the unpossessed pawn leaves on.
// `propagate` goes to UE's bPropagateToChildren flag, default true; pass false when hiding one
// component whose children must stay visible (mainPlayer_C's ACharacter::Mesh native slot can be
// the AttachParent of the authoritative body mesh, and a propagating hide there cascades to the
// body). Game thread only.
bool SetComponentVisible(void* component, bool visible = true, bool propagate = true);

// Force a SkeletalMeshComponent to always tick its pose (VisibilityBasedAnimTickOption =
// AlwaysTickPoseAndRefreshBones). Without it a remote body that is not the rendered viewpoint
// stops posing and collapses to its skeleton. A direct byte write; no setter UFunction exists.
// Game thread only.
bool SetAnimTickAlways(void* skeletalMeshComponent);

// USkinnedMeshComponent::SetSkeletalMesh(NewMesh, bReinitPose=true): swap the skin asset onto a
// (puppet) SkeletalMeshComponent. Resolved on the SkinnedMeshComponent class, the function's
// owning class. Game thread only.
bool SetSkeletalMesh(void* skeletalMeshComponent, void* skeletalMeshAsset);

// USkinnedMeshComponent::SetSkeletalMesh(null): clear the mesh asset so the component renders
// nothing, without touching its visibility flags or AttachParent. A bHiddenInGame hide on a parent
// whose child is a simulating ragdoll mesh would cascade to the child through IsVisible's parent
// walk; clearing the asset does not. A separate function because SetSkeletalMesh rejects null.
// Game thread.
bool ClearSkeletalMesh(void* skeletalMeshComponent);

// USkeletalMeshComponent::SetAnimClass(NewClass): assign an AnimBP class to a SkeletalMeshComponent
// and (re)instantiate it (also flips AnimationMode to UseAnimBlueprint). Drives the puppet's pose.
// Game thread only.
bool SetAnimClass(void* skeletalMeshComponent, void* animBlueprintClass);

// UPrimitiveComponent::CreateDynamicMaterialInstance(ElementIndex): create, or return the existing,
// MID for the slot, parented to the slot's current material (SourceMaterial null, OptionalName
// NAME_None). The MID inherits the parent's parameters; overriding a texture parameter re-skins
// the slot with no cooked material (the skins, docs/players.md). Returns the
// UMaterialInstanceDynamic*, or null. Game thread only.
void* CreateDynamicMaterialInstance(void* component, int32_t elementIndex);

// UMaterialInstanceDynamic::SetTextureParameterValue(ParameterName, Value). The kel body materials
// expose their diffuse as the texture parameter 'tex' (the inst_kel4_body instance). Game thread
// only.
bool SetTextureParameterValue(void* materialInstanceDynamic, const wchar_t* paramName, void* texture);

// UStaticMeshComponent::SetStaticMesh(NewMesh): swap the static-mesh asset on a
// UStaticMeshComponent (the trash-proxy mirror re-skinning pile<->clump; the client resolves
// NewMesh from chipType through getChipPileType). Resolved on the owning class
// UStaticMeshComponent (FindFunction does not climb to super). Recomputes bounds and the collision
// body from the new mesh in the same call. Rejects null. Game thread only.
bool SetStaticMesh(void* staticMeshComponent, void* staticMeshAsset);

// USceneComponent::SetMobility(NewMobility): set a component's mobility (Static=0, Stationary=1,
// Movable=2) and re-register its render state. A runtime-spawned AStaticMeshActor defaults to
// Static, on which SetStaticMesh and SetActorLocation are silent no-ops
// (AreDynamicDataChangesAllowed() is false); the trash proxy is a kinematic host-driven follower
// that is re-skinned and moved every frame, so its mesh component must be Movable before the
// first SetStaticMesh or it never renders and cannot follow. Game thread only.
bool SetComponentMobility(void* sceneComponent, uint8_t mobility);

// UPrimitiveComponent::SetMaterial(ElementIndex, Material): override one material slot on a
// component. `material` may be null: SetMaterial(0, null) reverts the slot to the mesh asset's own
// default material (the trash proxy clears a stale clump override this way when a shared component
// goes back to a pile). The clump form sets slot 0 to the pile mesh's material on the fixed
// dirtball mesh, as prop_garbageClump_C::setTex does. Game thread only.
bool SetComponentMaterial(void* primitiveComponent, int32_t elementIndex, void* material);

// UStaticMesh::GetMaterial(MaterialIndex): the UMaterialInterface* the mesh asset carries at
// `materialIndex`. Feeds the clump's material swap (getChipPileType(chipType).GetMaterial(0), per
// setTex). Null on failure. Game thread.
void* GetStaticMeshMaterial(void* staticMeshAsset, int32_t materialIndex);

// Destroy an actor component (UActorComponent::K2_DestroyComponent): strips a remote pawn's
// local-only systems, such as its unbound PostProcessComponent that hijacks the local screen's
// gamma and exposure. `contextObject` is the calling object for the engine's auth check (pass the
// owning actor). Game thread only.
bool DestroyComponent(void* component, void* contextObject);

// AmainPlayer_C::light_R, the flashlight USpotLightComponent slot on a mainPlayer pawn. The live
// component pointer, or nullptr if `mainPlayer` is null or dead or the slot is empty or dead.
// Keeps the light_R offset out of gameplay code (principle 7). Game thread only.
void* GetMainPlayerLightR(void* mainPlayer);

// Snapshot of a flashlight light's authoritative state: everything the flashlight wire payload and
// the local diagnostic need.
struct FlashlightSnapshot {
    float intensity;
    float outerConeAngle;
    float innerConeAngle;
    bool  visible;
};

// Read the full snapshot off a USpotLightComponent (typically the light_R that GetMainPlayerLightR
// returns). False if `light` is null or dead. Game thread only (memory reads of an engine struct).
bool ReadFlashlightSnapshot(void* light, FlashlightSnapshot& out);

// AmainPlayer_C bookkeeping bools and the mode byte the flashlight wire payload mirrors: adjacent
// fixed offsets on the pawn, read in one call. False on a null or dead pawn, `out` then
// zero-initialised. Game thread only.
struct MainPlayerFlashlightState {
    bool flashlight;       // AmainPlayer_C::flashlight     (canonical on/off)
    bool hasFlashlight;    // AmainPlayer_C::hasFlashlight  (equipped guard)
    bool crankFlashlight;  // AmainPlayer_C::crankFlashlight (_c variant marker)
    uint8_t mode;          // AmainPlayer_C::flashlightMode (focused/spread enum)
};
bool ReadMainPlayerFlashlightState(void* mainPlayer, MainPlayerFlashlightState& out);

// Direct write of AmainPlayer_C::flashlight (the on/off bool). The dev auto-toggle flips the bool
// without the BP graph (the BP's input-guarded toggle is out of reflection's reach); the
// SetIntensity wire path drives the visible light separately. False on a null or dead pawn. Game
// thread only.
bool WriteMainPlayerFlashlight(void* mainPlayer, bool newState);

// ULightComponent::SetIntensity(NewIntensity). Marks the render state dirty, so CreateSceneProxy
// refreshes the brightness next frame. The UFunction is cached on first call. False if `light` is
// null or dead or the UFunction is not yet resolvable. Game thread only.
bool SetLightIntensity(void* light, float newIntensity);

// USceneComponent::SetVisibility(bNewVisibility, bPropagateToChildren). Distinct from
// SetComponentVisible above: just the SetVisibility UFunction, with no companion SetHiddenInGame
// call. The puppet flashlight receive path needs exactly this: ULightComponent's SetVisibility
// override calls MarkRenderStateDirty, so the proxy is rebuilt without forcing the hidden-in-game
// state onto the whole component. Cached UFunction. Game thread only.
bool SetSceneComponentVisibility(void* sceneComponent, bool newVisibility, bool propagateToChildren);

// USpotLightComponent::SetOuterConeAngle(NewOuterConeAngle) / SetInnerConeAngle(NewInnerConeAngle).
// Both mark the render state dirty, so the cone shape refreshes next frame; the puppet flashlight
// receiver mirrors the sender's hold-F focused/spread mode with them. Cached UFunctions. Game
// thread only.
bool SetSpotLightOuterConeAngle(void* spotLight, float newAngle);
bool SetSpotLightInnerConeAngle(void* spotLight, float newAngle);

// ---- Ragdoll / faint display state ----
// AmainPlayer_C drives every ragdoll cause (the manual C key, the exhaustion faint, a KO) through
// one UFunction and an AnimBP gate bool. These wrappers let the coop layer read the local player's
// ragdoll state (the sender) and drive a puppet into and out of the faint pose (the receiver)
// without touching engine memory or reflection directly (principle 7). Offsets are name-resolved
// (recook-safe, through reflected_offset); UFunctions are cached on first call. All game thread
// only.

// Read AmainPlayer_C::isRagdoll (the AnimBP ragdoll gate, set by any ragdoll cause) and ::dead.
// False, out-params untouched, on a null or dead pawn or while either field offset is unresolved.
bool ReadMainPlayerRagdollState(void* mainPlayer, bool& isRagdoll, bool& dead);

// AmainPlayer_C::ragdollMode(ragdoll, passOut, death). (true, true, false) is the faint pose: on an
// unpossessed puppet it flips isRagdoll 0->1 and spawns the puppet's ragdoll actor. False on a
// null or dead pawn or an unresolved UFunction.
bool SetMainPlayerRagdollMode(void* mainPlayer, bool ragdoll, bool passOut, bool death);

// AmainPlayer_C::forceGetUp(): begins the get-up. On a possessed player (tick enabled) this fully
// recovers: clears isRagdoll and tears the ragdoll actor down over its timeline. Possessed players
// only; the tick-driven cleanup it relies on cannot run on a tickless orphan, so a puppet's flop
// ends by destroying its spawned ragdoll body instead (RagdollDisplay in coop/player). False on a
// null or dead pawn or an unresolved UFunction.
bool ForceMainPlayerGetUp(void* mainPlayer);

// AmainPlayer_C::forceWakeup(): the unconditional stand-up. It jumps straight to the ubergraph
// block (@25800) that restores the movement mode, capsule collision, camera attach and EnableInput
// and detaches the ragdoll mesh, reading no gate; wakeup(passOut) early-outs on
// dead || isWakingUp || noWakeup and forceGetUp() runs a latent delay first, so a KO recovery
// calls this one. Possessed players only. False on a null or dead pawn or an unresolved
// UFunction. Game thread.
bool ForceMainPlayerWakeup(void* mainPlayer);

// AmainPlayer_C::canRagdoll (@0x0D10), ragdollMode()'s own precondition: false early-outs every
// ragdoll cause on this pawn. The Killer Wisp false-grab window forces it false on the host (the
// grab montage's notifies write playerDamaged and fire ragdollMode(true,false,true) inside the
// bytecode; an HP pin cannot stop a ragdoll death, this native gate can), then restores true on
// the window's falling edge. A raw masked write of a plain BP bool with no setter. False on a
// null or dead pawn or an unresolved bool. Game thread.
bool SetMainPlayerCanRagdoll(void* mainPlayer, bool allowed);

// Read the same bool back, so a test can assert that the death gate took rather than assume the
// write landed: a gate you cannot observe is a gate you cannot prove. False on a null or dead
// pawn or an unresolved bool.
bool ReadMainPlayerCanRagdoll(void* mainPlayer, bool& allowed);

// AmainPlayer_C::"Add Player Damage"(Damage, damageLocation, fullBody, blood, Source), the primary
// player-damage entry. The owning peer invokes it on its own possessed mainPlayer_C when the host
// relays an enemy hit, so the damage runs through that peer's own armour and inventory BP and
// drops its saveSlot.health, and the health stream and the damage flash follow. Only `Damage` and
// `blood` are set; the other parameters zero-init. Safe on a puppet: the BP early-outs on an
// unpossessed pawn. `blood` is not cosmetic: the function gates a block on it (@2784,
// IFNOT(blood)) that ends in lib_C::addEffect('bloodLoss', ...), the screen-washing effect, so a
// synthetic hit with blood=false produces a death the game itself never produces. False on a
// null or dead pawn or an unresolved UFunction. Game thread only.
bool InvokeAddPlayerDamage(void* mainPlayer, float damage, bool blood = false);

// The mainPlayer_C "Add Player Damage" UFunction pointer, resolved on demand. The Killer Wisp
// host-neutralise installs a ProcessEvent PRE-interceptor on it that zeroes the wisp's limb-tear
// damage to the host while it false-grabs a client. null until mainPlayer_C is loaded. Game
// thread only.
void* AddPlayerDamageFunctionPtr();

// ---- The puppet's faint display ----
// A puppet never runs the game's ragdollMode. That spawns a separate playerRagdoll_C whose whole
// lifecycle (the get-up, GC, the PhysX teardown) is tick- and timeline-coupled and assumes a
// possessed player: on a tickless, controllerless orphan the actor self-invalidates to
// PendingKill, is never reaped, and its PhysX keeps simulating (gigabytes of growth). The display
// spawns the game's ragdoll body directly instead, death-free (SpawnPlayerRagdollBody below),
// hides the puppet's own body meshes and pelvis-attaches the puppet to the body so the nameplate
// and the recover hand-off follow the flop (RagdollDisplay in coop/player/remote_player_ragdoll).

// ---- Damage body pulse (puppet mesh material swap) ----
// The saved-material cache type (component, slot index, original) is ue_wrap::SavedMaterial in
// types.h. The puppet renders two visible body meshes (Mesh @0x280 and mesh_playerVisible), so
// the saved set spans both: a flat (component, index, original) list restores every slot exactly,
// however many components or slots were swapped. The caller (RemotePlayer) owns the vector.
using ue_wrap::SavedMaterial;

// Swap both visible body meshes' materials to the hurt-flash material
// (PlayerHurtFlashMaterialName, a skeletal gore skin, so it renders red rather than the default
// grey), caching every original into `saved`; RestoreHurtFlashMaterial with the same vector
// restores them. The Minecraft-style whole-body red pulse; it shares the nameplate hurt-flash
// trigger. Materials are render state independent of animation and physics, so it composes with
// the pose drive and the ragdoll. False, with no effect, when the puppet, mesh or material does
// not resolve; the nameplate flash still fires then. Game thread only.
bool ApplyHurtFlashMaterial(void* puppet, std::vector<SavedMaterial>& saved);
bool RestoreHurtFlashMaterial(void* puppet, std::vector<SavedMaterial>& saved);

// Eager-resolve the hurt material and the material UFunctions (once per puppet spawn), so the
// first damage flash does no GUObjectArray name walks. Game thread.
void WarmupHurtFlashCache();

// Resolve a loaded UMaterialInterface by object name (MaterialInstanceConstant first, then
// Material, then any class). nullptr if not loaded. Game thread.
void* ResolveMaterialByName(const wchar_t* name);

// Spawn the game's playerRagdoll_C for `ownerPlayer` (a puppet) at `location`/`rotation` as the
// visible flop body: a deferred spawn with Player @0x248 (Expose On Spawn) stamped before Finish,
// so BeginPlay self-configures the body mesh from the owner, then the body simulation started
// (BeginPlay builds the rigid bodies frozen). Death-free: it never touches the owner's dead or
// isRagdoll fields, unlike ragdollMode, which is globally scoped and kills the host. Returns the
// spawned AActor* (DestroyActor it on recover), or nullptr. engine_playerragdoll.cpp. Game thread
// only.
void* SpawnPlayerRagdollBody(void* ownerPlayer, const FVector& location, const FRotator& rotation);

// Attach `actor` (its RootComponent) to ragdoll `body`'s mesh at the pelvis bone, so it rigidly
// follows the flopping pelvis every frame. KeepWorld rules: it keeps the spawn-time co-location,
// then follows the deltas. False on failure. DetachActorFromRagdollBody detaches it (KeepWorld).
// Game thread only.
bool AttachActorToRagdollBody(void* actor, void* body);
bool DetachActorFromRagdollBody(void* actor);

// Generic socket attach (engine_attach.cpp): attach `actor` to `component` at the named socket,
// SnapToTarget, so it sits at and follows the socket each frame (the Killer Wisp grab-hold: the
// victim puppet on the wisp body mesh's 'playerGrab'). KeepWorld scale. False on unresolved.
// DetachActorFromParent reverses it (KeepWorld). Game thread only.
bool AttachActorToComponentSocket(void* actor, void* component, const wchar_t* socket);
bool DetachActorFromParent(void* actor);

// ---- Generic actor root-physics substrate (engine_attach.cpp) ----
// Root-component physics primitives for the held-clump mirror: they operate through
// K2_GetRootComponent, never the Aprop_C StaticMesh offset, so they work on the non-Aprop_C trash
// clump (kinematic while held, physics plus a throw velocity on release). Game thread only; each
// IsLive-gates its arguments.

// SetSimulatePhysics(simulate) on `actor`'s root primitive component (through K2_GetRootComponent):
// freezes the clump mirror while attached (kinematic follow) and thaws it on release (free fall).
// No-op if the root is not a primitive. Game thread only.
bool SetActorSimulatePhysics(void* actor, bool simulate);

// Force `actor`'s root component to Movable (EComponentMobility::Movable=2) so a following
// SetActorLocation / SetActorRotation relocates it. A Static root silently ignores
// SetActorLocation (the K2 call still returns true), and a save-loaded chipPile native rests at
// Static mobility, so a position correction calls this before the teleport or the snap does
// nothing. Game thread only.
bool SetActorRootMovable(void* actor);

// SetCollisionEnabled(collisionType) on `actor`'s root primitive component. collisionType:
// 0=NoCollision 1=QueryOnly 2=PhysicsOnly 3=QueryAndPhysics. The thrown clump mirror needs 3 to
// collide and land instead of sinking through the floor (the bare-spawned mirror lacks the
// collision a real grabbed clump has). Generic (the root component, not the Aprop_C mesh offset).
// Game thread only.
bool SetActorRootCollisionEnabled(void* actor, uint8_t collisionType);

// SetNotifyRigidBodyCollision(notify) on `actor`'s root primitive component. `false` stops the
// root's OnComponentHit-bound BP events from firing without disabling physical collision, so a
// kinematic or physics body still lands, it just runs no ComponentHit handler. Silences a mirror
// trash clump's own ground-hit turn-to-pile handler: the client mirror is spawned through
// FinishSpawningActor, which auto-binds that handler, and on landing it would
// BeginDeferredActorSpawnFromClass a second pile atop the host's authoritative one. Generic (the
// root component). Game thread.
bool SetActorRootNotifyRigidBodyCollision(void* actor, bool notify);

// Read or overwrite `actor`'s root primitive linear (cm/s) and angular (deg/s) velocity: the
// throw-energy transfer on clump release. Generic (not the Aprop_C mesh offset, so it works on the
// non-Aprop_C clump). False on failure. Game thread only.
bool GetActorRootPhysicsVelocity(void* actor, FVector& outLin, FVector& outAng);
bool SetActorRootPhysicsVelocity(void* actor, const FVector& lin, const FVector& ang);
// Angular only, for a mirror whose author is turning but not translating: writing the linear
// component of a resting rig wakes it and it sinks.
bool SetActorRootPhysicsAngularVelocity(void* actor, const FVector& ang);

// `actor`'s root primitive mass in kg (UPrimitiveComponent::GetMass on the root); 0 on failure or
// a non-simulating body. The native LMB throw formula scales speed as 15000/max(mass,10). Generic
// (the root component). Game thread only.
float GetActorRootMass(void* actor);

// The UPhysicalMaterial governing `actor`'s root surface: root primitive component,
// GetMaterial(0), UMaterialInterface::GetPhysicalMaterial() (all reflected UFunctions; the
// virtuals resolve through the native thunks). Feeds lib_C::physSound the way the native grab
// chain's hit.PhysMat input does (mainPlayer ubergraph @100003). Equivalent to the trace's
// resolution for a single-material actor without a BodyInstance.PhysMaterialOverride (the trace
// honours the override and the hit face's slot; the game's grabbable plain-Actor family, clump and
// pile, is single-material with no authored override). Null if the root is not a primitive, has
// no material, or the material has no physical material. Game thread only.
void* GetActorRootPhysicalMaterial(void* actor);

// True only when `actor`'s root rigid body is positively confirmed at rest (no body awake: asleep
// while simulating, or not simulating). False on any resolution failure; an unverifiable rest is
// never claimed. The host stamps the kAtRest physFlag from this so the client can mirror the rest
// state. Generic root component. Game thread only.
bool IsActorRootBodyAtRest(void* actor);

// Force `actor`'s root rigid body to sleep (PutRigidBodyToSleep, BoneName=None). The client calls
// it right after teleport-converging a kAtRest prop at connect, so the prop lands at host
// authority and returns to rest without a transient physics settle: a teleport wakes the body,
// and a thousand woken props that never re-sleep are a permanent physics scene. Leaves the body a
// dynamic, grabbable physics body (not SetSimulatePhysics(false)). False on failure. Game thread.
bool PutActorRootBodyToSleep(void* actor);

// The ragdoll physics sync, sender side: read the local player's native ragdoll
// (AmainPlayer_C::ragdollActor @0xC40, the AplayerRagdoll_C the C key or a faint spawned) pelvis
// bone world transform plus the pelvis linear and angular velocity, for streaming to peers
// (RagdollPoseSnapshot). False if the player is not ragdolling (no ragdollActor) or the pelvis or
// mesh does not resolve. Velocities in cm/s and deg/s (the PropRelease units). Game thread only.
bool ReadLocalRagdollPelvisPhysics(void* mainPlayer, FVector& outLoc, FRotator& outRot,
                                   FVector& outLinVel, FVector& outAngVel);

// The ragdoll physics sync, receiver side: slave the spawned mirror `body`'s pelvis rigid body to
// a peer's streamed velocity (SetPhysicsLinearVelocity + SetPhysicsAngularVelocityInDegrees on the
// pelvis bone, bAddToCurrent=false, the PropRelease apply pair), so the mirror tumbles to track
// the sender's real ragdoll instead of free-simulating. No effect if the body, mesh or pelvis does
// not resolve. Game thread only.
void DriveRagdollBodyPelvisVelocity(void* body, const FVector& linVel, const FVector& angVel);

// The simulating SkeletalMesh component (Aragdoll_C::SkeletalMesh @0x230) of a ragdoll actor: the
// spawned mirror body or the game's native AplayerRagdoll_C. Null if the actor is null or dead or
// the mesh is unresolved. Game thread only (the ragdoll bone visualiser).
void* GetRagdollBodyMesh(void* ragdollActor);

// The local player's native ragdoll body mesh: mainPlayer.ragdollActor @0xC40 (spawned by the C
// key, faint or trip ragdollMode, destroyed on wakeup, so non-null is the "is ragdolling"
// signal), then its SkeletalMesh. Null while not ragdolling. Game thread only.
void* GetLocalRagdollBodyMesh(void* mainPlayer);

// A long-lived UObject suitable as the WorldContextObject of the deferred-spawn pair
// (BeginDeferredActorSpawnFromClass + FinishSpawningActor): the GameInstance when it resolves (it
// lives across map loads), else the World.
void* GetWorldContext();

// USoundAttenuation config for SpawnSoundAttenuation below. Field meanings mirror Unreal's
// USoundAttenuation editor properties; the uint8 enum values follow UE4.27's EAttenuationShape /
// EAttenuationDistanceModel / EAttenuationFalloffMode. The defaults are the flashlight-click tuning
// (a sphere, a 20 m full-volume zone, 200 m of falloff, the inverse distance model).
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

// Construct and configure a fresh USoundAttenuation through UGameplayStatics::SpawnObject plus raw
// field writes (Unreal exposes no setter UFunctions for the SoundAttenuationSettings fields; they
// are edit-time properties). The returned object is AddToRoot'd before return, so UE's GC never
// collects it: callers keep the pointer in a C++ static, which UE's reachability scan cannot see,
// and an unrooted object would be reaped at the next GC pass and PlaySoundAtLocation would read
// freed memory on the following click. Returns the live USoundAttenuation*, or nullptr if the
// SpawnObject UFunction or the CDO is missing or SpawnObject returns null. Game thread only.
void* SpawnSoundAttenuation(const SoundAttenuationConfig& cfg);

// UGameplayStatics::PlaySoundAtLocation: a one-shot 3D positional sound at `location`, owned by
// `worldContext` (the actor whose world it plays in, also the OwningActor for concurrency and
// occlusion). `sound` is a USoundBase (a SoundWave or SoundCue). `attenuation` may be null (the
// sound then plays 2D). The per-call transient UAudioComponent is engine-managed. Shared by the
// flashlight click and the prop grab/throw sounds. Game thread only.
void PlaySoundAtLocation(void* worldContext, void* sound, const FVector& location,
                         void* attenuation, float volume = 1.f, float pitch = 1.f);

// FRotator to FQuat (UE4.27's own formula, ZYX order, left-handed coordinate system), matching the
// body of FRotator::Quaternion() in Engine/Source/Runtime/Core/Public/Math/Rotator.h exactly:
//
//   RotationQuat.X =  CR*SP*SY - SR*CP*CY;
//   RotationQuat.Y = -CR*SP*CY - SR*CP*SY;
//   RotationQuat.Z =  CR*CP*SY - SR*SP*CY;
//   RotationQuat.W =  CR*CP*CY + SR*SP*SY;
//
// The negative Y term is UE4's left-handed, Z-up convention, not a defect: a general right-handed
// ZYX Euler-to-quaternion reference shows the opposite signs, so do not "correct" it against one
// without reading the UE4 source.
void RotatorToQuat(float pitchDeg, float yawDeg, float rollDeg,
                   float& qx, float& qy, float& qz, float& qw);

// ---- Navigation and locomotion (engine_nav.cpp) ----
// A baked-NavMesh path query and pawn movement input: what the bot director drives the possessed
// player with. Game thread only.

// A point guaranteed reachable within `radiusCm` of `origin` over the baked NavMesh
// (UNavigationSystemV1::K2_GetRandomReachablePointInRadius, static, on the CDO). False if the
// navmesh has no reachable polygon there (also the navmesh-not-built signal); `out` untouched on
// false. `worldContext` is any live actor (the possessed player works).
bool RandomReachablePoint(void* worldContext, const FVector& origin, float radiusCm, FVector& out);

// A traversable route from `start` to `end` over the baked NavMesh
// (UNavigationSystemV1::FindPathToLocationSynchronously, static, on the CDO), read from the
// resulting UNavigationPath::PathPoints. True, `outPts` filled (two or more points), on a real
// route; false on no route, a call failure or a bad points read (`outPts` cleared).
bool FindNavPath(void* worldContext, const FVector& start, const FVector& end,
                 std::vector<FVector>& outPts);

// APawn::AddMovementInput on `pawn` (resolved on the Pawn declaring class, not the leaf).
// Accumulates ControlInputVector; the CharacterMovementComponent consumes and clears it on its own
// tick, so it must be re-issued each frame to sustain movement. `force`=true applies even when
// input is nominally disabled. No effect on null or unresolved.
void AddMovementInput(void* pawn, const FVector& worldDir, float scale, bool force);

}  // namespace ue_wrap::engine
