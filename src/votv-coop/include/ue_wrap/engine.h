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

#include <string>
#include <vector>

namespace ue_wrap::engine {

// Run a console command (UKismetSystemLibrary::ExecuteConsoleCommand) -- the
// universal lever for "open <map>", "HighResShot ...", etc. Resolves the
// KismetSystemLibrary CDO + UFunction and a valid world context on first use
// (cached). Returns false if anything could not be resolved. Game thread only.
bool ExecuteConsoleCommand(const wchar_t* command);

// Travel to VOTV's MAIN MENU via the game's own verb, AmainGamemode_C::transition(
// "/Game/menu") (full path -- the short name does not resolve). Works regardless of
// player state (direct gamemode call, NO pause needed -> dead-player-safe). Pair with
// game_thread::SetTransparentBypass held over the travel + the time at the menu, so our
// ProcessEvent detour neither hangs the gameplay-world teardown nor churns at the menu.
// Used by the local-death flee. Game thread only. Returns false if it can't dispatch.
bool ReturnToMainMenu();

// Load a VOTV save slot (e.g. a STORY save like "s_may2026") and enter gameplay
// via the game's own load entry: GameplayStatics::LoadGameFromSlot ->
// mainGameInstance_C::setSaveSlotObject + loadObjects=true -> open the gameplay
// map. The gameplay map is untitled_1 for every mode; the SAVE selects story vs
// sandbox. Returns false if the slot is missing/empty or load can't dispatch.
// `forceGameMode` (v56 save-transfer): >=0 writes that enum_gamemode ordinal
// directly instead of deriving it from the slot-name prefix -- the coop slot
// `zcoop_<pid>` has no game-known prefix; the mode comes over the wire
// (SaveTransferBeginPayload.gameMode). -1 = derive from the prefix (default).
// Game thread only.
bool LoadStorySave(const wchar_t* slot, int forceGameMode = -1);

// v56 rejoin (design-workflow B7): drop the cached USaveGame* + the GameMode
// latch so a SECOND in-process LoadStorySave (browser rejoin with a fresh
// zcoop_ slot) re-loads from disk instead of re-registering the stale cached
// object. Game thread only.
void ResetCachedSave();

// FRESH New-Game boot: like LoadStorySave but with a BLANK saveSlot
// (GameplayStatics::CreateSaveGameObject(saveSlot_C)) instead of a disk slot -> a fresh New
// Game in untitled_1. The deterministic baseline for the ephemeral-client world snapshot: a
// fresh client has only level-default props, so the host's existing connect-snapshot mirrors
// the host's whole world onto it with no reconcile-remove. `storyMode` picks story vs sandbox
// GameMode (the coop target is story). Polled like LoadStorySave. Game thread only.
bool StartFreshGame(bool storyMode);

// ---- Save-object-ready hook (inventory Inc 4) ---------------------------------
// A callback the coop inventory layer registers to OVERWRITE a freshly loaded/created saveSlot
// object's player-scoped inventory BEFORE the game's native loadObjects() materializes it on
// BeginPlay. It fires exactly ONCE per disk-load (LoadStorySave) / creation (StartFreshGame),
// on the game thread, with the USaveGame* (a UsaveSlot_C*) about to be registered + traveled
// into -- i.e. the moment the inventory arrays are present but the world has not been built
// from them yet. This is the RULE-1 apply injection point: the game's own load builds the live
// inventory from whatever we leave in the save object (no live obj_11 poke, no second reload).
// The hook MUST self-gate to a no-op unless the coop layer has a pending per-player inventory
// (armed only on a CLIENT join); engine.cpp fires it unconditionally. Registering null disarms.
// Principle 7: the substrate exposes the injection point, holding no inventory knowledge.
using SaveObjectReadyHook = void(*)(void* saveSlotObject);
void SetSaveObjectReadyHook(SaveObjectReadyHook hook);

// VOTV encodes a save's game MODE only in its slot-name PREFIX (story "s_", infinite
// "i_", sandbox "b_", halloween "SPOOKY_", ambience "a_", solar "l_"). These wrap
// Uui_saveSlots_C::getSavePrefix (a pure mode->prefix map on the CDO) so the GameMode-
// on-load fix AND the save browser share ONE prefix source (RULE 2). enum_gamemode
// ordinals are NOT the submenu button order (story=0, infinite=1, sandbox=4, ...).
// Both resolve the widget CDO lazily and return false / -1 until it loads (the menu or
// a gameplay transition loads it). Game thread only.

// Write VOTV's prefix string for `mode` (a TEnumAsByte<enum_gamemode::Type>) into `out`.
// Returns false if the save-slots widget CDO / getSavePrefix isn't resolvable yet.
bool GetSavePrefix(uint8_t mode, std::wstring& out);

// Derive the enum_gamemode ordinal of save slot `slot` from its name prefix (the
// longest matching getSavePrefix wins). Returns the mode (0..7), or -1 on no match /
// unresolved widget.
int DeriveModeFromSlot(const wchar_t* slot);

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

// AActor::GetActorScale3D on `actor` (root component world scale). Returns
// UNIT scale (1,1,1) on failure -- callers stamp it straight into spawn
// transforms, where a zero scale would collapse the mirror invisibly. v54
// PropSpawn real-scale stamp (SP saves scale inside the object transform;
// pre-v54 senders hardcoded 1,1,1). Game thread only.
FVector GetActorScale3D(void* actor);

// AActor::SetActorScale3D(FVector NewScale3D) on `actor` (root component relative
// scale). Returns the success of the UFunction call (the engine fn itself is void).
// v83: the host-authoritative trash proxy applies the host's real per-form scale
// (clump vs pile differ) on every convert/spawn so the mirror is host-sized -- the
// proxy-looks-smaller bug (scale was never sent on PropConvert nor applied). Game thread only.
bool SetActorScale3D(void* actor, const FVector& scale);

// UKismetSystemLibrary::CollectGarbage -- schedules a full GC purge at the
// end of the current frame (the engine's own post-level-transition pattern).
// The adoption sweep pairs its ~1k-actor destruction burst with this so the
// pending-kill garbage doesn't sit at a multi-GB plateau until UE's default
// 61 s periodic purge (smoke-measured 10.6 GB client plateau grazing the
// process commit cap). Returns false if the library/function is unresolved.
// Game thread only.
bool ForceGarbageCollection();

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

// USceneComponent::K2_GetComponentRotation on `component` (resolved WORLD rotation).
// Zero on failure. v40: reads the kerfur's ACharacter::Mesh visible body facing (which the
// actor BP aims at the local player, decoupled from the actor root). Game thread only.
FRotator GetComponentWorldRotation(void* component);

// USceneComponent::K2_SetWorldRotation on `component` (bSweep=false, bTeleport=true). Returns
// the dispatch success. v40: drives a mirror kerfur's ACharacter::Mesh WORLD rotation to the
// host's streamed body facing (the mirror's actor tick is OFF, so nothing overwrites it).
// Call AFTER SetActorRotation -- moving the actor root re-bases the child mesh's world transform.
// Game thread only.
bool SetComponentWorldRotation(void* component, const FRotator& rotation);

// The WORLD rotation of `actor`'s visible StaticMesh component, or the actor
// rotation if it owns none. A chipPile's per-instance visual variety lives on
// its StaticMesh COMPONENT's relative rotation (a random roll applied in its
// UserConstructionScript), NOT the actor root (which stays identity) -- so
// GetActorRotation captures identity for every pile and every mirror proxy
// would render identically oriented. Capture THIS on the host so the bare-
// AStaticMeshActor proxy (whose mesh sits on its own identity-relative root)
// reproduces the same orientation via a plain SetActorRotation. Game thread.
FRotator GetVisibleMeshWorldRotation(void* actor);

// AActor::SetActorTickEnabled. A remote pawn must NOT run the local-player
// per-frame BP EventTick (which re-applies view/post-process/exposure to the
// shared screen every frame -- the gamma stomp that survives a component
// destroy). Game thread only.
bool SetActorTickEnabled(void* actor, bool enabled);

// AActor::SetActorHiddenInGame -- VISUAL ONLY (bHidden @0x58; collision is the
// separate bActorEnableCollision @0x5C). The instant-world deferred-spawn upper
// layer hides a host mirror until reconcile resolves, then reveals it. Game thread.
bool SetActorHiddenInGame(void* actor, bool hidden);

// AActor::SetActorEnableCollision -- pair with SetActorHiddenInGame(true) on a
// deferred-hidden mirror so it is not grab-trace-hittable / physics-active while
// invisible (hide alone leaves collision on). Game thread.
bool SetActorEnableCollision(void* actor, bool enabled);

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

// APlayerController::ProjectWorldLocationToScreen(World, Screen&, bViewportRelative)
// -- project a WORLD point to VIEWPORT-PIXEL screen coords through the local
// player's camera. Returns true if the point is in front of the camera (and fills
// `outScreen`); false if behind it (outScreen left untouched). `playerController`
// is the LOCAL player's controller (ue_wrap::engine::GetController(localPawn)).
// Used by the ImGui screen-space nameplates: the projection runs on the game
// thread (this is a UFunction) and the result is snapshotted for the render
// thread to draw. Game thread only.
bool ProjectWorldToScreen(void* playerController, const FVector& world,
                          FVector2D& outScreen, bool viewportRelative = false);

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

// The UStaticMeshComponent subobject of `actor` (an AStaticMeshActor's root, or
// any actor that owns one) via a reflection child-walk. nullptr if none. Used to
// reach the host-authoritative trash proxy's mesh component (SetStaticMesh /
// collision). Game thread.
void* GetStaticMeshComponent(void* actor);

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

// Read-only twin of ReleaseMainPlayerGrabIfHolding: true iff `localPlayer`'s
// grabbing_actor slot currently holds `actor`. No mutation, no PHC dispatch.
// Fork B 2d (2026-06-10): the snapshot bind paths skip physics-reconcile +
// teleport-converge for a prop the LOCAL player is holding (forcing
// SimulatePhysics off mid-hold breaks the PhysicsHandle grab at a
// re-bracket). Game thread only; false for null/dead inputs.
bool IsMainPlayerGrabbing(void* localPlayer, void* actor);

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

// The actor the local player is aiming at (AmainPlayer_C::lookAtActor @0x0AA0). On an
// E-press this is the door/interactable being used. Returns nullptr if unresolved / not
// aiming at anything / mainPlayer dead. Game thread.
void* ReadMainPlayerLookAtActor(void* mainPlayer);

// Direct write of AmainPlayer_C::lookAtActor (the cached interaction-trace result, re-derived
// each tick -- NOT a UFunction-setter-managed field). Lets a test drive the BP's
// icast(lookAtActor) to a chosen actor for the SINGLE InpActEvt_use dispatch that immediately
// follows (the next tick's trace overwrites it). Same in-tree pattern device_screen.cpp uses
// (ClearAimForDispatch nulls + restores it around an enter dispatch). Returns false on
// null/dead pawn or unresolved offset. Game thread only.
bool WriteMainPlayerLookAtActor(void* mainPlayer, void* actor);

// The local player's radial-menu confirm state: AmainPlayer_C::releaseEToUse @0x0E88 (true on the
// "release E to use" radial confirm) + actionIndex @0x0A98 (the highlighted option's index into the
// target's getActionOptions list). Returns false (outs untouched) if mainPlayer is dead or the
// fields are unresolved (recook). Game thread. Used by coop/kerfur_menu_input to relay the client's
// kerfur radial verb (the actionName dispatch itself is EX_LocalVirtualFunction / PE-invisible).
bool ReadMainPlayerRadialSelect(void* mainPlayer, bool& releaseEToUse, int32_t& actionIndex);

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

// Read a UParticleSystemComponent's `Template` (the UParticleSystem* it renders) via a
// reflection-resolved property offset (raw pointer read, cached). Used by event_cue_sync to
// identify which cosmetic cue a live PSC belongs to (the EX_CallMath SpawnEmitterAtLocation that
// created it is invisible to our ProcessEvent detour, so we match the resulting component's
// template instead). nullptr on null input / unresolved offset / a non-PSC component.
void* GetParticleSystemTemplate(void* particleSystemComponent);

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

// ---- Runtime UMG button injection (MULTIPLAYER menu, P1 2026-06-04) ------------
// Construct a UButton at runtime inside an existing UCanvasPanel and position it
// relative to a reference widget already in that canvas. Engine substrate only
// (principle 7): no coop/menu knowledge -- the caller (coop::multiplayer_menu)
// resolves the VOTV menu's canvMenu/button_start/tex_btnStart and passes them in.
//
//   refButton   the menu UButton to sit ABOVE (e.g. button_start / NEW GAME). We
//               derive its list panel (a UVerticalBox), insert our button into it at
//               the TOP (above refButton), and clone refButton's FButtonStyle for the
//               look. The VerticalBox owns layout, so spacing matches the other items.
//   label       button text (e.g. L"MULTIPLAYER")
//   refText     UTextBlock whose FSlateFontInfo + colour we clone for the label
//               (e.g. tex_btnStart). May be null (then the nameplate's default style).
//   outButton   receives the spawned UButton* (for the click poll). May be null.
//
// Returns true on success. Game thread only (SpawnObject + UFunction dispatch).
bool InjectCanvasButton(void* refButton, const wchar_t* label, void* refText,
                        void** outButton);

// UWidget::IsHovered() -> bool. The mouse-over test for the menu click poll
// (paired with a global VK_LBUTTON edge in coop::multiplayer_menu). Returns false
// if the widget/UFunction is unresolved. Game thread only.
bool WidgetIsHovered(void* widget);

// UWidget::SetVisibility(ESlateVisibility). The client loading state pairs this (set to
// HitTestInvisible=3) WITH SetWidgetRenderOpacity(0) on VOTV's whole menu widget so the menu
// disappears both VISUALLY (opacity 0) and FUNCTIONALLY (HitTestInvisible -> the widget and
// all its children stop receiving clicks) -- the user can't click invisible menu options.
// HitTestInvisible (rather than Collapsed/Hidden) is deliberate: it is still a "rendered"
// Slate state, so the menu KEEPS TICKING and the same per-tick observer can restore it.
// slateVis: 0=Visible, 1=Collapsed, 2=Hidden, 3=HitTestInvisible, 4=SelfHitTestInvisible.
// Returns false if the widget / UFunction is unresolved. Game thread only.
bool SetWidgetVisibility(void* widget, uint8_t slateVis);

// UWidget::SetRenderOpacity(float). The visual half of the menu hide (see SetWidgetVisibility
// above): opacity 0 fades the whole menu widget out so only the 3D menu background remains --
// the "clean menu canvas" the connecting screen draws its progress over -- then 1 restores it.
// Returns false if the widget / UFunction is unresolved. Game thread only.
bool SetWidgetRenderOpacity(void* widget, float opacity);

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

// World POSITION of the bone named `boneName` -- the hot-path-safe by-name variant:
// ONE GetSocketLocation dispatch per call (the FName comes from the global name table,
// cached per distinct name -- no skeleton enumeration). Silently anchors to the
// COMPONENT transform when the mesh lacks the bone (UE's own fallback) -- desirable
// for HUD/audio anchors. False only on outright resolution failure. Game thread only.
bool GetBoneWorldLocationByName(void* skelMeshComp, const wchar_t* boneName, FVector& outLoc);

// WORLD rotation of a named bone/socket (SceneComponent::GetSocketRotation -- a bone
// name is a valid socket name). Returns false if the bone is not found or the function
// is unresolved. Game thread only. Used by the puppet-head-freeze probe to measure the
// head/neck twist against the native LookAt clamp.
bool GetBoneWorldRotationByName(void* skelMeshComp, const wchar_t* boneName, FRotator& outRot);

// One bone of a skeletal mesh's CURRENTLY-EVALUATED pose (ragdoll bone visualizer):
// world position + the index of its PARENT bone within the same output array (-1 = root).
struct BonePoint {
    FVector world;
    int32_t parent;
};

// Fill `out` (cleared first) with every bone's current world position + parent index on
// `skelMeshComp`. The bone GRAPH (names + parent links via USkinnedMeshComponent::
// GetNumBones/GetBoneName/GetParentBone) is cached per component; each call re-reads only
// the positions (SceneComponent::GetSocketLocation -- a bone name is a valid socket name).
// Returns the bone count (0 on null comp / unresolved functions). COST: one UFunction call
// PER BONE per invocation -- a dev-diagnostic budget; never call from an always-on hot
// path. Game thread only.
int CollectSkeletonBonePoints(void* skelMeshComp, std::vector<BonePoint>& out);

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

// UPrimitiveComponent::CreateDynamicMaterialInstance(ElementIndex) -- create (or
// return the existing) MID for the slot, parented to the slot's CURRENT material
// (SourceMaterial null, OptionalName zeroed = NAME_None). The MID inherits the
// parent's parameters; overriding a texture parameter re-skins the slot with NO
// cooked material needed (client-model textures, COOP_CLIENT_MODEL.md 7).
// Returns the UMaterialInstanceDynamic*, or null. Game thread only.
void* CreateDynamicMaterialInstance(void* component, int32_t elementIndex);

// UMaterialInstanceDynamic::SetTextureParameterValue(ParameterName, Value). The
// kel body materials expose their diffuse as texture parameter 'tex'
// (inst_kel4_body MIC RE 2026-07-02). Game thread only.
bool SetTextureParameterValue(void* materialInstanceDynamic, const wchar_t* paramName, void* texture);

// UStaticMeshComponent::SetStaticMesh(NewMesh) -- swap the static-mesh asset onto
// a UStaticMeshComponent (the host-authoritative trash-proxy mirror re-skinning
// pile<->clump; the client resolves NewMesh from chipType via getChipPileType).
// Resolved from the OWNING class UStaticMeshComponent (FindFunction does not climb
// to super). Recomputes bounds + collision body from the new mesh in the same
// call. Rejects null. Game thread only.
bool SetStaticMesh(void* staticMeshComponent, void* staticMeshAsset);

// USceneComponent::SetMobility(NewMobility) -- set a component's mobility (Static=0,
// Stationary=1, Movable=2) and re-register its render state. A runtime-spawned
// AStaticMeshActor defaults to STATIC, on which SetStaticMesh AND SetActorLocation are
// silently no-ops (AreDynamicDataChangesAllowed()==false) -- the trash proxy is a
// kinematic host-driven follower we re-skin + move every frame, so its mesh component
// MUST be Movable BEFORE the first SetStaticMesh or it never renders + can't follow.
// Game thread only.
bool SetComponentMobility(void* sceneComponent, uint8_t mobility);

// UPrimitiveComponent::SetMaterial(ElementIndex, Material) -- override one material
// slot on a component. `material` MAY be null: SetMaterial(0, null) reverts that
// slot to the mesh asset's own default material (the trash proxy uses this to clear
// a stale clump material override when re-skinning a shared component back to a
// pile). The clump form sets slot 0 to the pile mesh's material on the fixed
// dirtball mesh (verified prop_garbageClump_C::setTex). Game thread only.
bool SetComponentMaterial(void* primitiveComponent, int32_t elementIndex, void* material);

// UStaticMesh::GetMaterial(MaterialIndex) -> UMaterialInterface* -- the material the
// mesh asset carries at `materialIndex`. Feeds the clump's material swap
// (getChipPileType(chipType).GetMaterial(0), per setTex). Null on failure. Game thread.
void* GetStaticMeshMaterial(void* staticMeshAsset, int32_t materialIndex);

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

// AmainPlayer_C::"Add Player Damage"(Damage, damageLocation, fullBody, blood, Source)
// -- the primary player-damage entry. vitals Inc3-WIRE: the OWNER peer invokes this on
// its OWN POSSESSED mainPlayer_C when the host relays an enemy hit, so the damage runs
// through that peer's private armor/inventory BP and drops its shared saveSlot.health
// (then the existing Inc1 stream + Inc3 flash fire). Only `Damage` is set; the
// location/fullBody/blood/Source params zero-init. SAFE on a puppet by accident -- the
// BP early-outs on an unpossessed pawn (#6 probe). Returns false on null/dead pawn or
// unresolved UFunction. Game thread only.
bool InvokeAddPlayerDamage(void* mainPlayer, float damage);

// The mainPlayer_C "Add Player Damage" UFunction pointer (resolved on demand). Exposed so
// the Killer Wisp host-neutralize can install a ProcessEvent PRE-interceptor that zeroes the
// wisp's limb-tear damage to the HOST while it false-grabs a client. null until mainPlayer_C
// is loaded. Game thread only.
void* AddPlayerDamageFunctionPtr();

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

// Spawn VOTV's playerRagdoll_C ragdoll body for `ownerPlayer` (the puppet orphan) at
// `location`/`rotation` as an INVISIBLE PHYSICS DRIVER: deferred spawn -> stamp Player
// @0x248 (Expose-On-Spawn) -> Finish (BeginPlay self-configures the ragdoll skeleton)
// -> StartBodySim -> HIDE its own mesh (the "plushy" the user rejected). DEATH-FREE:
// never touches the owner's dead/isRagdoll (unlike ragdollMode, which is globally
// scoped and kills the host). The visible Dr. Kel puppet is then pelvis-attached to it
// (AttachActorToRagdollBody) so it tumbles along. Returns the spawned AActor*
// (DestroyActor it on recover), or nullptr. See ue_wrap/engine_playerragdoll.cpp +
// [[project-ragdoll-sync]]. Recipe proven by harness/autotest_ragdoll_spawn_probe.cpp.
// Game thread only.
void* SpawnPlayerRagdollBody(void* ownerPlayer, const FVector& location, const FRotator& rotation);

// Attach `actor` (its RootComponent) to ragdoll `body`'s mesh at the PELVIS bone so it
// rigidly follows the flopping pelvis per-frame ("pelvis to pelvis"). KeepWorld rules
// (keeps the spawn-time co-location, then follows deltas). Returns false on failure.
// DetachActorFromRagdollBody detaches it (KeepWorld). Game thread only.
bool AttachActorToRagdollBody(void* actor, void* body);
bool DetachActorFromRagdollBody(void* actor);

// Generic socket-attach (engine_attach.cpp): attach `actor` to `component` at the named
// SOCKET, SnapToTarget so it sits at + follows the socket each frame (the Killer Wisp
// grab-hold: victim puppet -> wisp body mesh 'playerGrab'). KeepWorld scale. False on
// unresolved. DetachActorFromParent reverses it (KeepWorld). Game thread only.
bool AttachActorToComponentSocket(void* actor, void* component, const wchar_t* socket);
bool DetachActorFromParent(void* actor);

// ---- Generic actor root-physics substrate (ue_wrap/engine_attach.cpp) ----
// Root-component physics primitives used by the held-clump mirror (coop/remote_prop):
// they operate via K2_GetRootComponent, NEVER the Aprop_C StaticMesh offset, so they
// work on the non-Aprop_C trash clump (the mannequin model -- kinematic while held,
// physics + throw velocity on release). Game-thread only; each IsLive-gates its args.
// [[project-bug-trash-chippile-uaf-crash]]

// SetSimulatePhysics(simulate) on `actor`'s root primitive component (via
// K2_GetRootComponent). Used to freeze the clump mirror while attached (kinematic
// follow) and thaw it on release (free-fall). No-op if the root isn't a primitive.
// Game thread only.
bool SetActorSimulatePhysics(void* actor, bool simulate);

// Force `actor`'s ROOT component to Movable (EComponentMobility::Movable=2) so a subsequent
// SetActorLocation/SetActorRotation actually relocates it. A Static-mobility root silently NO-OPs
// SetActorLocation (the K2 call still returns true) -- so teleporting a Static native does nothing.
// A save-loaded chipPile native rests at Static mobility; the b3 pos-correction must call this before
// the teleport or the snap is a no-op (the "applied but drift unchanged" bug). Game thread only.
// [[lesson-runtime-staticmeshactor-must-be-movable]]
bool SetActorRootMovable(void* actor);

// SetCollisionEnabled(collisionType) on `actor`'s root primitive component.
// collisionType: 0=NoCollision 1=QueryOnly 2=PhysicsOnly 3=QueryAndPhysics. The
// thrown clump MIRROR needs 3 so it collides + lands instead of sinking through the
// floor (the bare-spawned mirror lacks the collision a real grabbed clump has). Generic
// (root component, not the Aprop_C mesh offset). Game thread only.
bool SetActorRootCollisionEnabled(void* actor, uint8_t collisionType);

// SetNotifyRigidBodyCollision(notify) on `actor`'s root primitive component. `false`
// stops the root's OnComponentHit-bound BP events from firing WITHOUT disabling physical
// collision -- so a kinematic/physics body still lands, it just doesn't run its ComponentHit
// handler. Used to silence a MIRROR trash-clump's own ground-hit -> turn-to-pile handler (the
// client mirror is spawned via FinishSpawningActor, which auto-binds that handler; on landing
// it would BeginDeferredActorSpawnFromClass a SECOND pile atop the host's authoritative one --
// the user's clump-dup bug). Generic (root component, not the Aprop_C mesh offset). Game thread.
bool SetActorRootNotifyRigidBodyCollision(void* actor, bool notify);

// Read/overwrite `actor`'s root primitive linear (cm/s) + angular (deg/s) velocity --
// the throw-energy transfer on clump release ("physics like the mannequin"). Generic
// (does NOT use the Aprop_C mesh offset, so it works on the non-Aprop_C clump). Returns
// false on failure. Game thread only.
bool GetActorRootPhysicsVelocity(void* actor, FVector& outLin, FVector& outAng);
bool SetActorRootPhysicsVelocity(void* actor, const FVector& lin, const FVector& ang);

// `actor`'s root primitive mass in kg (UPrimitiveComponent::GetMass on the root). 0 on failure /
// non-simulating body. Used by the native LMB throw formula (speed scales as 15000/max(mass,10)).
// Generic (root component, not the Aprop_C mesh offset). Game thread only.
float GetActorRootMass(void* actor);

// The UPhysicalMaterial governing `actor`'s root surface: root primitive component
// -> GetMaterial(0) -> UMaterialInterface::GetPhysicalMaterial() (all reflected
// UFunctions; virtuals resolve through the native thunks). Feeds lib_C::physSound
// like the native grab chain's hit.PhysMat input (mainPlayer uber @100003).
// Equivalent to the trace's resolution for single-material actors WITHOUT a
// BodyInstance.PhysMaterialOverride (the trace honors the override + the hit
// face's slot; VOTV's grabbable plain-Actor family -- clump/pile -- is
// single-material with no authored override, audit M-2 2026-06-11). Null if the
// root isn't a primitive, has no material, or the material has no physical
// material assigned. Game thread only.
void* GetActorRootPhysicalMaterial(void* actor);

// True ONLY if positively confirmed `actor`'s root rigid body is at rest (no body
// awake -- asleep simulating OR non-simulating). Returns false on any resolution
// failure (NEVER claim an unverifiable rest). The host stamps the kAtRest physFlag
// from this so the client can mirror the host's rest-state. Generic root component.
// Game thread only. [[project-fps-physics-wake-kAtRest-2026-06-09]]
bool IsActorRootBodyAtRest(void* actor);

// Force `actor`'s root rigid body to sleep (PutRigidBodyToSleep, BoneName=None). The
// client calls this right after teleport-converging a kAtRest prop at connect so it
// lands at host authority and returns to rest WITHOUT a transient physics settle (the
// root cause of the client connect-FPS lock: ~1388 RNG-divergent props were teleport-
// WOKEN and never re-slept -> permanent physics scene). Leaves the body a DYNAMIC,
// grabbable physics body (NOT SetSimulatePhysics(false)). False on failure. Game thread.
bool PutActorRootBodyToSleep(void* actor);

// v22 ragdoll physics sync -- SENDER side. Read the LOCAL player's NATIVE ragdoll
// (AmainPlayer_C::ragdollActor @0xC40, the AplayerRagdoll_C the C-key/faint spawned)
// pelvis bone WORLD transform + pelvis linear + angular velocity, for streaming to
// peers (RagdollPoseSnapshot). Returns false if the player isn't ragdolling (no
// ragdollActor) or the pelvis/mesh can't resolve. Velocities are cm/s + deg/s (the
// PropRelease units). Game thread only.
bool ReadLocalRagdollPelvisPhysics(void* mainPlayer, FVector& outLoc, FRotator& outRot,
                                   FVector& outLinVel, FVector& outAngVel);

// v22 ragdoll physics sync -- RECEIVER side. Slave our spawned mirror `body`'s pelvis
// rigid body to a peer's streamed velocity: SetPhysicsLinearVelocity +
// SetPhysicsAngularVelocityInDegrees on the pelvis bone (bAddToCurrent=false, the same
// PropRelease apply pair) so the mirror body tumbles to TRACK the sender's real ragdoll
// instead of free-simulating. No-op-safe if body/mesh/pelvis won't resolve. GT only.
void DriveRagdollBodyPelvisVelocity(void* body, const FVector& linVel, const FVector& angVel);

// The simulating SkeletalMesh component (Aragdoll_C::SkeletalMesh @0x230) of a ragdoll
// ACTOR -- our spawned mirror body OR the game's native AplayerRagdoll_C. Null if the
// actor is null/dead or the mesh is unresolved. Game thread only (ragdoll bone visualizer).
void* GetRagdollBodyMesh(void* ragdollActor);

// The LOCAL player's native ragdoll body mesh: mainPlayer.ragdollActor @0xC40 (spawned by
// the C-key / faint / trip ragdollMode, destroyed on wakeup -- non-null IS the "is
// ragdolling" signal) -> its SkeletalMesh. Null while not ragdolling. GT only.
void* GetLocalRagdollBodyMesh(void* mainPlayer);

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

// Fire UGameplayStatics::PlaySoundAtLocation -- a one-shot 3D positional sound at
// `location`, owned by `worldContext` (the actor whose world it plays in; also the
// OwningActor for concurrency/occlusion). `sound` is a USoundBase (SoundWave or
// SoundCue). `attenuation` may be null (plays 2D then). The per-call transient
// UAudioComponent is engine-managed. Game thread only. Shared by the flashlight
// click (coop::flashlight_click_sound) and the prop grab/throw sounds
// (coop::prop_sound) -- the dispatch lived inline in the former (RULE 2).
void PlaySoundAtLocation(void* worldContext, void* sound, const FVector& location,
                         void* attenuation, float volume = 1.f, float pitch = 1.f);

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
