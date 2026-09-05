// ue_wrap/sdk_profile_names.h -- the version surface, part 2: content names.
//
// Everything here is a game-content identity (blueprint class names, UFunction names, level names,
// save-landmark coordinates): it changes when the game's blueprints or maps change, not when the
// engine recompiles. Re-derive from the CXX header dump or bp_reflect on a game-version bump.
// sdk_profile.h includes this header, so consumers keep one include and the P::name:: spelling.

#pragma once

namespace ue_wrap::profile {

// ---- content names (change with game content, not the engine) ------------
namespace name {
inline constexpr const wchar_t* MainPlayerClass = L"mainPlayer_C";
inline constexpr const wchar_t* GamemodeClass = L"mainGamemode_C";
// AmainGamemode_C::transition(FName LevelName): the game's own level-travel verb;
// ReturnToMainMenu calls it with the full path "/Game/menu" (the short name does not resolve).
inline constexpr const wchar_t* MainGamemodeTransitionFn = L"transition";
inline constexpr const wchar_t* ActorClass = L"Actor";
inline constexpr const wchar_t* WorldClass = L"World";
inline constexpr const wchar_t* SetActorLocationFn = L"K2_SetActorLocation";
inline constexpr const wchar_t* GameplayLevel = L"untitled_1";

// The checkpoint spawn point at the base entrance (the actor centre, world cm; re-derive per game
// version): every client appearance is teleported here (coop/session/teleport_client).
inline constexpr float kKPPSpawnX = -37695.f;
inline constexpr float kKPPSpawnY =  69978.f;
inline constexpr float kKPPSpawnZ =   6420.f;

// Engine classes and functions dispatched through (stable engine names, kept here so the porting
// surface is one file). The persistent GameInstance subclass is game content (the world context
// that survives a level load).
inline constexpr const wchar_t* KismetSystemLibraryClass = L"KismetSystemLibrary";
inline constexpr const wchar_t* ExecuteConsoleCommandFn = L"ExecuteConsoleCommand";
inline constexpr const wchar_t* GameInstanceClass = L"mainGameInstance_C";

// The live UsaveSlot_C instance holds the player's current runtime vitals (food, sleep, health,
// coffeePower), which the BP mutates in place and SaveGameToSlot serialises whole; its field
// offsets are resolved at runtime through FindPropertyOffset (a BP-cooked class).
inline constexpr const wchar_t* SaveSlotClass = L"saveSlot_C";

// Actor spawning (the BlueprintCallable deferred-spawn pair the K2 SpawnActorFromClass node uses)
// and transform get/set.
inline constexpr const wchar_t* GameplayStaticsClass = L"GameplayStatics";

// UGameplayStatics::BeginDeferredActorSpawnFromClass is the deferred-spawn entry every BP "spawn
// actor of class" graph compiles to; intercepting it is one engine-level hook for every NPC and
// spawner class (the constant is BeginDeferredSpawnFn below).

// The NPC class allowlist. On a client every BeginDeferredActorSpawnFromClass for a class in this
// list is suppressed (the interceptor returns true with ActorClass zeroed, so the BP graph
// receives nullptr and bails); the host runs its spawners and the client's NPCs arrive through
// the EntityPose batch. All are targeting-compatible with the mainPlayer_C orphan puppet
// (AIPerception, PawnSensing or a Cast to mainPlayer_C reach a real Pawn of the same class).
inline constexpr const wchar_t* NpcClass_Zombie       = L"npc_zombie_C";
inline constexpr const wchar_t* NpcClass_KerfurOmega  = L"kerfurOmega_C";
inline constexpr const wchar_t* NpcClass_Krampus      = L"npc_krampus_C";
inline constexpr const wchar_t* NpcClass_Funguy       = L"npc_funguy_C";
inline constexpr const wchar_t* NpcClass_GoreSlither  = L"npc_goreSlither_C";
inline constexpr const wchar_t* NpcClass_Insomniac    = L"insomniac_C";
inline constexpr const wchar_t* NpcClass_Fossilhound  = L"fossilhound_C";
inline constexpr const wchar_t* NpcClass_Antibreather = L"antibreather_C";
inline constexpr const wchar_t* NpcClass_Orborb       = L"npc_orborb_C";
inline constexpr const wchar_t* NpcClass_ArirFollower = L"npc_arirFollower_C";
inline constexpr const wchar_t* NpcClass_AriralShooter   = L"npc_ariral_shooter_C";
inline constexpr const wchar_t* NpcClass_AriralPigBeater = L"npc_ariral_pigBeater_C";
// Event and late-game creatures in the same layer (verified against the CXX header dump):
// killerwisp_C (Akillerwisp_C : ACharacter, the lethal yellow Killer Wisp, not the base wisp_C
// swarm nor the coloured per-player wisps; AIPerception plus a Target APawn; spawned by
// ticker_yellowWispSpawner_C late in the game) and ventCrawler_C (AventCrawler_C : ACharacter, the
// ventCrawler event). Both ride the BeginDeferred interceptor and the pose stream; the vent
// crawler's wall crawl is a known yaw-only gap.
inline constexpr const wchar_t* NpcClass_KillerWisp   = L"killerwisp_C";
inline constexpr const wchar_t* NpcClass_VentCrawler  = L"ventCrawler_C";
// wisp_C (Awisp_C : ACharacter): the wispSwarm-event wisp and the ambient sky wisp
// (ticker_wispSpawner_C spawns at absolute map coordinates, world-anchored, so host-authoritative
// too). Both spawn paths are EX_CallMath, caught by the ufunction_hook Func thunk on
// BeginDeferred, source-gated by FFrame::Object's class. A mirror spawns invisible and fades in
// at the tick-driven landing edge, so it keeps its actor tick (CMC parked) and the pose lane
// drives the landing (ue_wrap::wisp::DriveWispLanding). The 8 colour variants are direct
// ACharacter subclasses, not wisp_C children, so each is listed.
inline constexpr const wchar_t* NpcClass_Wisp         = L"wisp_C";
inline constexpr const wchar_t* NpcClass_WispB        = L"wisp_b_C";
inline constexpr const wchar_t* NpcClass_WispRed      = L"wisp_red_C";
inline constexpr const wchar_t* NpcClass_WispBl       = L"wisp_bl_C";
inline constexpr const wchar_t* NpcClass_WispW        = L"wisp_w_C";
inline constexpr const wchar_t* NpcClass_WispG        = L"wisp_g_C";
inline constexpr const wchar_t* NpcClass_WispO        = L"wisp_o_C";
inline constexpr const wchar_t* NpcClass_WispP        = L"wisp_p_C";
inline constexpr const wchar_t* NpcClass_WispBlu      = L"wisp_blu_C";

// The allowlist array: resolved once at install through R::FindClass and cached. The classes load
// on the first gameplay-level transition, not at the menu, so the install retries until every
// entry resolves. sizeof-derived, so a new entry binds everywhere.
inline constexpr const wchar_t* kNpcAllowlist[] = {
    NpcClass_Zombie,
    NpcClass_KerfurOmega,
    NpcClass_Krampus,
    NpcClass_Funguy,
    NpcClass_GoreSlither,
    NpcClass_Insomniac,
    NpcClass_Fossilhound,
    NpcClass_Antibreather,
    NpcClass_Orborb,
    NpcClass_ArirFollower,
    NpcClass_AriralShooter,
    NpcClass_AriralPigBeater,
    NpcClass_KillerWisp,   // yellow Killer Wisp (late-game; AIPerception like the 12)
    NpcClass_VentCrawler,  // the ventCrawler event (its wall crawl is a yaw-only gap)
    NpcClass_Wisp,         // wispSwarm-event and ambient sky wisp (both host-authoritative)
    NpcClass_WispB,        // sky-wisp color variants (ticker_wispSpawner map; direct ACharacter
    NpcClass_WispRed,      //   subclasses, NOT wisp_C children -- each needs its own row)
    NpcClass_WispBl,
    NpcClass_WispW,
    NpcClass_WispG,
    NpcClass_WispO,
    NpcClass_WispP,
    NpcClass_WispBlu,
};
inline constexpr size_t kNpcAllowlistSize = sizeof(kNpcAllowlist) / sizeof(kNpcAllowlist[0]);

// The non-Character event actors the Character-only NPC mirror cannot replicate: npc_pose_drive
// drives position, yaw and the CMC, so these AActor/APawn ships and saucers get a transform-only
// full-rotation mirror (coop::element::WorldActor, coop/world_actor_sync). A second BeginDeferred
// interceptor consumes this list, disjoint from kNpcAllowlist. Verified AActor/APawn in the CXX
// header dump; matched by name and kept curated.
inline constexpr const wchar_t* kWorldActorAllowlist[] = {
    L"ufoDropper_body_C",          // ufoDropper delivery saucer body (the gray "drop pod")
    L"ufoDropper_car_C",           // ufoDropper car variant
    L"ufoDropper_tank_C",          // ufoDropper tank variant
    L"rozitBorg_C",                // Rozital mothership (borgRozital event)
    L"arirShip_C",                 // ariral ship
    L"skyUfo_C",                   // high-altitude sky UFO
    L"jellyfish_C",                // space jellyfish
    L"morningUfo_C",               // morningGay event UFO
    L"tentacleBallsFollower_C",    // tentacle-balls follower
    L"soltomiaCleaning_C",         // soltomia cleaning craft
    L"kocker_C",                   // kocker
    L"skyFallingEvent_C",          // "Sky Falling" event body (AskyFallingEvent_C; distinct from skyFallingEvent's emitter cue)
    L"superEgger_C",               // super egger
    L"igetis_C",                   // igetis
    L"firetank_C",                 // firetank (APawn -- transform-only mirror is layout-agnostic)
    L"piramidSubpawn_C",           // pyramid sub-pawn (APawn; the SANDBOX piramidTest_C child, not the event)
    L"piramid2_C",                 // walking pyramid event actor (a plain AActor; brain suppression and the
                                   // gather relay: coop/creatures/piramid_sync)
    L"baocoin_C",                  // the sell gun's coin: Abaocoin_C : AActor, not Aprop_C, so it rides no prop
                                   // lane; host-minted and mirrored (coop/items/coingun_sync). The mint is
                                   // EX_CallMath, so this entry alone does nothing: prop_coingun_C in
                                   // kExSpawnSourceClasses lets the Func-thunk drain reach
                                   // world_actor_sync::HostEnrollExSpawn, which allocates the eid.
};
inline constexpr size_t kWorldActorAllowlistSize =
    sizeof(kWorldActorAllowlist) / sizeof(kWorldActorAllowlist[0]);

// The transient physics props the host's ambient spawners materialise through
// BeginDeferredActorSpawnFromClass whose own Init is BP-internal (EX_LocalVirtualFunction, past
// the ProcessEvent detour). The spawn call is observable, so coop/host_spawn_watcher catches it at
// the BeginDeferred POST seam, mirrors the prop host-to-client through the PropSpawn pipeline
// (keyless, eid-routed) and death-watches its SetLifeSpan(600) expiry. Exact-pointer matched leaf
// classes. Local fall physics diverges per peer by design.
inline constexpr const wchar_t* kAmbientPropSpawnMirrorClasses[] = {
    L"prop_food_pinecone_C",  // the scare: spawns high, drops, bounces, rolls / lies
    L"prop_stick_C",          // pineconeSpawner forage branch
    L"prop_crystal_C",        // pineconeSpawner forage branch
};
inline constexpr size_t kAmbientPropSpawnMirrorClassesSize =
    sizeof(kAmbientPropSpawnMirrorClasses) / sizeof(kAmbientPropSpawnMirrorClasses[0]);

inline constexpr const wchar_t* BeginDeferredSpawnFn = L"BeginDeferredActorSpawnFromClass";
inline constexpr const wchar_t* FinishSpawningActorFn = L"FinishSpawningActor";

// The story-save load: the game's own entry (GameplayStatics::LoadGameFromSlot,
// mainGameInstance_C::setSaveSlotObject with loadObjects=true, then untitled_1; the save selects
// the mode).
inline constexpr const wchar_t* LoadGameFromSlotFn = L"LoadGameFromSlot";
inline constexpr const wchar_t* SetSaveSlotObjectFn = L"setSaveSlotObject";

// The live host-world capture (save_capture.cpp): repopulate the world save from live actors, then
// serialise it to a scratch slot for a joiner (a kerfur the host turned on after its last
// autosave is captured live). All on AmainGamemode_C except SaveGameToSlot.
inline constexpr const wchar_t* MainGamemodeSaveObjectsFn  = L"saveObjects";   // saveObjects(bool quicksave)
inline constexpr const wchar_t* MainGamemodeSaveTriggersFn = L"saveTriggers";  // saveTriggers()
inline constexpr const wchar_t* SaveGameToSlotFn           = L"SaveGameToSlot";  // (USaveGame*, FString slot, int32 idx) -> bool
inline constexpr const wchar_t* ActorClassName = L"Actor";  // owns K2_Get/SetActorLocation
inline constexpr const wchar_t* GetActorLocationFn = L"K2_GetActorLocation";
inline constexpr const wchar_t* GetActorRotationFn = L"K2_GetActorRotation";
inline constexpr const wchar_t* GetActorVelocityFn = L"GetVelocity";  // AActor::GetVelocity -> FVector (cm/s)
inline constexpr const wchar_t* GetActorScale3DFn = L"GetActorScale3D";  // AActor -> FVector
inline constexpr const wchar_t* CollectGarbageFn = L"CollectGarbage";  // KismetSystemLibrary static; schedules a full GC purge end-of-frame (post-sweep plateau collapse). Class constant @ the existing KismetSystemLibraryClass above.
inline constexpr const wchar_t* PropInventoryContainerPlayerClass = L"prop_inventoryContainer_player_C";  // PER-PLAYER inventory container -- never snapshot-expressed/broadcast/swept (prop_lifecycle::IsPerPlayerPropClass)
inline constexpr const wchar_t* GetActorForwardVectorFn = L"GetActorForwardVector";
inline constexpr const wchar_t* SetActorRotationFn = L"K2_SetActorRotation";
inline constexpr const wchar_t* SetActorScale3DFn = L"SetActorScale3D";  // AActor::SetActorScale3D(FVector) -> void
inline constexpr const wchar_t* SetActorTickEnabledFn = L"SetActorTickEnabled";
inline constexpr const wchar_t* SetActorHiddenInGameFn = L"SetActorHiddenInGame";        // AActor::SetActorHiddenInGame(bool bNewHidden) -- visual-only (instant-world deferred-hide)
inline constexpr const wchar_t* SetActorEnableCollisionFn = L"SetActorEnableCollision";  // AActor::SetActorEnableCollision(bool bNewActorEnableCollision) -- paired with hide so a hidden mirror is not grabbable
inline constexpr const wchar_t* DestroyActorFn = L"K2_DestroyActor";
inline constexpr const wchar_t* TeleportToFn = L"K2_TeleportTo";  // bool(FVector, FRotator); large-distance teleport that survives Character/CMC constraints
inline constexpr const wchar_t* GetActorBoundsFn = L"GetActorBounds";   // (bool,FVector&,FVector&,bool); world-space AABB of the actor's mesh

// Controller removal: an auto-possessing remote pawn's second PlayerController fights the local
// player's input and view; GetController, Detach, destroy.
inline constexpr const wchar_t* PawnClassName = L"Pawn";
inline constexpr const wchar_t* GetControllerFn = L"GetController";
inline constexpr const wchar_t* DetachFromControllerFn = L"DetachFromControllerPendingDestroy";
// An AIController poses the body through the possession path with no viewport, input or camera.
inline constexpr const wchar_t* SpawnDefaultControllerFn = L"SpawnDefaultController";

// SetSkeletalMesh is owned by USkinnedMeshComponent and SetAnimClass by USkeletalMeshComponent;
// FindFunction does not climb to super, so each resolves from its own class.
inline constexpr const wchar_t* SkeletalMeshComponentClass = L"SkeletalMeshComponent";
inline constexpr const wchar_t* SkinnedMeshComponentClass = L"SkinnedMeshComponent";
inline constexpr const wchar_t* SetSkeletalMeshFn = L"SetSkeletalMesh";   // USkinnedMeshComponent
inline constexpr const wchar_t* SetAnimClassFn = L"SetAnimClass";         // USkeletalMeshComponent

// UStaticMeshComponent::SetStaticMesh: the trash-proxy mirror's mesh swap (pile <-> clump, the
// mesh resolved from chipType through getChipPileType); resolved from its own class.
inline constexpr const wchar_t* StaticMeshComponentClass = L"StaticMeshComponent";
inline constexpr const wchar_t* SetStaticMeshFn = L"SetStaticMesh";       // UStaticMeshComponent

// The kerfur AnimBP generated class (the asset AnimBlueprint_kerfurOmega_regular plus the '_C'
// suffix of BP-generated classes).
inline constexpr const wchar_t* AnimBPKerfurRegularClass = L"AnimBlueprint_kerfurOmega_regular_C";

// The dev freecam: a spawned ACameraActor the view is pointed at (SetViewTargetWithBlend), look
// from the game's own control rotation, moved by WASD per frame.
inline constexpr const wchar_t* CameraActorClass = L"CameraActor";
inline constexpr const wchar_t* ControllerClassName = L"Controller";
inline constexpr const wchar_t* GetControlRotationFn = L"GetControlRotation";
// The BlueprintCallable setter, not a direct field write: K2_SetControlRotation also runs
// ProcessViewRotation and UpdateRotation, and skipping them jitters the view per tick.
inline constexpr const wchar_t* SetControlRotationFn = L"K2_SetControlRotation";
inline constexpr const wchar_t* PlayerControllerClassName = L"PlayerController";
// APlayerController::ProjectWorldLocationToScreen(FVector, FVector2D&, bool
// bPlayerViewportRelative) -> bool: the world-to-viewport projection of the screen-space nameplates
// (false behind the camera).
inline constexpr const wchar_t* ProjectWorldToScreenFn = L"ProjectWorldLocationToScreen";
inline constexpr const wchar_t* SetViewTargetWithBlendFn = L"SetViewTargetWithBlend";
inline constexpr const wchar_t* PlayerCameraManagerClass = L"PlayerCameraManager";
inline constexpr const wchar_t* GetCameraLocationFn = L"GetCameraLocation";
inline constexpr const wchar_t* GetCameraRotationFn = L"GetCameraRotation";

// Component visibility (USceneComponent BlueprintCallable): forces the third-person body meshes
// visible on an unpossessed remote pawn.
inline constexpr const wchar_t* SceneComponentClass = L"SceneComponent";
inline constexpr const wchar_t* SetVisibilityFn = L"SetVisibility";
// K2_SetRelativeRotation runs the transform propagation (UpdateComponentToWorld and the child
// re-evaluation) that a direct RelativeRotation write skips.
inline constexpr const wchar_t* SetRelativeRotationFn = L"K2_SetRelativeRotation";
inline constexpr const wchar_t* SetHiddenInGameFn = L"SetHiddenInGame";
inline constexpr const wchar_t* GetComponentLocationFn = L"K2_GetComponentLocation";
inline constexpr const wchar_t* GetComponentForwardFn = L"GetForwardVector";
// The kerfur body-facing sync: the kerfur BP rotates its ACharacter::Mesh world rotation each tick
// to face the local player, which never runs on a tick-less mirror; the host reads
// K2_GetComponentRotation and the mirror is driven with K2_SetWorldRotation
// (Engine.hpp:17941/17950).
inline constexpr const wchar_t* GetComponentRotationFn = L"K2_GetComponentRotation";
inline constexpr const wchar_t* SetWorldRotationFn = L"K2_SetWorldRotation";

// Component destruction (UActorComponent::K2_DestroyComponent): removes the local-only systems a
// remote pawn must not own (its PostProcessComponent grades the local screen).
inline constexpr const wchar_t* ActorComponentClass = L"ActorComponent";
inline constexpr const wchar_t* DestroyComponentFn = L"K2_DestroyComponent";
inline constexpr const wchar_t* PostProcessComponentClass = L"PostProcessComponent";

// The world-space nameplate: a translucent UWidgetComponent (TextRender cannot do partial alpha)
// rendering our own UMG (UUserWidget + UTextBlock through SpawnObject) to a render target
// composited with Widget3DPassThrough_Translucent; BlendMode must be set Transparent (the ctor
// default is Masked).
inline constexpr const wchar_t* WidgetComponentClass = L"WidgetComponent";
inline constexpr const wchar_t* AddComponentByClassFn = L"AddComponentByClass";      // on Actor
inline constexpr const wchar_t* FinishAddComponentFn = L"FinishAddComponent";        // on Actor
inline constexpr const wchar_t* SetTintColorAndOpacityFn = L"SetTintColorAndOpacity";// FLinearColor
inline constexpr const wchar_t* RequestRedrawFn = L"RequestRedraw";                  // sets bRedrawRequested
inline constexpr const wchar_t* RequestRenderUpdateFn = L"RequestRenderUpdate";      // forces render-state/RT refresh
inline constexpr const wchar_t* SetComponentTickEnabledFn = L"SetComponentTickEnabled";  // on UActorComponent -- a runtime-added WidgetComponent doesn't tick -> never draws its RT
inline constexpr const wchar_t* NameplateSetTextFn = L"SetText";                     // UTextBlock::SetText(FText)
inline constexpr const wchar_t* TextBlockClass = L"TextBlock";
inline constexpr const wchar_t* ImageClass = L"Image";                              // UImage -- every frame, fill and scrim we build
inline constexpr const wchar_t* KismetTextLibraryClass = L"KismetTextLibrary";
inline constexpr const wchar_t* ConvStringToTextFn = L"Conv_StringToText";           // FString -> FText
// The world-space WidgetComponent's translucent material (a runtime-added component may have the
// slot null, a blank quad); bIsTwoSided=true uses the two-sided slot.
inline constexpr const wchar_t* Widget3DTranslucentMatName = L"Widget3DPassThrough_Translucent";
inline constexpr const wchar_t* Widget3DTranslucentOneSidedMatName = L"Widget3DPassThrough_Translucent_OneSided";
inline constexpr const wchar_t* MaterialInstanceConstantClass = L"MaterialInstanceConstant";  // class of those MICs

// The nameplate is our own UMG (no cooked widget reused): NewObject is the reflected
// UGameplayStatics::SpawnObject(objectClass, Outer); a UUserWidget -> UWidgetTree -> UTextBlock
// root, then UWidgetComponent::SetWidget.
inline constexpr const wchar_t* SpawnObjectFn = L"SpawnObject";        // on GameplayStatics: (objectClass, Outer)->UObject*
inline constexpr const wchar_t* UserWidgetClass = L"UserWidget";
inline constexpr const wchar_t* WidgetTreeClass = L"WidgetTree";
// The two-line nameplate (the nick and a separately coloured health bar): a UVerticalBox root
// stacks two UTextBlocks so each carries its own ColorAndOpacity; AddChildToVerticalBox returns
// the slot, and the default Fill slot plus centre justification needs no slot writes.
inline constexpr const wchar_t* VerticalBoxClass = L"VerticalBox";
inline constexpr const wchar_t* AddChildToVerticalBoxFn = L"AddChildToVerticalBox";  // UVerticalBox::AddChildToVerticalBox(UWidget*)->UVerticalBoxSlot*
inline constexpr const wchar_t* SetWidgetFn = L"SetWidget";            // UWidgetComponent::SetWidget(UUserWidget*)
inline constexpr const wchar_t* FontName = L"Roboto";                  // /Engine/EngineFonts/Roboto.Roboto
inline constexpr const wchar_t* FontClassName = L"Font";
inline constexpr const wchar_t* MenuFontName = L"font_ui";             // /Game/main/fonts/font_ui (Share Tech Mono), the game's menu and subtitle font, size 16 on the menu labels
// The screen-space HUD feed (a UUserWidget added to the viewport, not a world WidgetComponent).
inline constexpr const wchar_t* AddToViewportFn = L"AddToViewport";        // UUserWidget::AddToViewport(int32 ZOrder)
inline constexpr const wchar_t* RemoveFromViewportFn = L"RemoveFromViewport";  // UUserWidget::RemoveFromViewport()
inline constexpr const wchar_t* WidgetClass = L"Widget";                   // owns SetVisibility (FindFunction = owning class, no super walk)
inline constexpr const wchar_t* WidgetSetVisibilityFn = L"SetVisibility";  // UWidget::SetVisibility(ESlateVisibility); HitTestInvisible=3
inline constexpr const wchar_t* SetPositionInViewportFn = L"SetPositionInViewport";    // UUserWidget(FVector2D Position, bool bRemoveDPIScale)
inline constexpr const wchar_t* SetAlignmentInViewportFn = L"SetAlignmentInViewport";  // UUserWidget(FVector2D Alignment)
// The client's pause-menu "Save Game" button grey-out; all owned by UWidget, resolved on
// WidgetClass.
inline constexpr const wchar_t* WidgetSetIsEnabledFn = L"SetIsEnabled";       // UWidget::SetIsEnabled(bool bInIsEnabled) -- blocks input + semantic disable (NOT a raw 0xB4 write: packed bitfield)
inline constexpr const wchar_t* WidgetSetRenderOpacityFn = L"SetRenderOpacity";  // UWidget::SetRenderOpacity(float InOpacity) -- brush-independent visual dim
inline constexpr const wchar_t* WidgetGetIsEnabledFn = L"GetIsEnabled";       // UWidget::GetIsEnabled()->bool ReturnValue (read-back diagnostic)
inline constexpr const wchar_t* UiMenuClass = L"ui_menu_C";                   // VOTV unified main+pause menu widget (GameInstance-owned, one live instance)
inline constexpr const wchar_t* UiMenuTickFn = L"Tick";                       // ui_menu_C::Tick(FGeometry,float) -- engine ProcessEvent-dispatched while visible (self-heal anchor)
inline constexpr const wchar_t* UiMenuButtonSaveProp = L"button_Save";        // UButton* @ ui_menu_C+0x2D0 (the "Save Game" button)
inline constexpr const wchar_t* UiMenuIsPauseProp = L"isPause";               // bool @ ui_menu_C+0x4C0 (true => the in-game pause menu, not the main menu)

// The menu button: the UMG classes and UFunctions that construct a UButton at runtime inside the
// live menu. Class names are UE FNames (no U prefix); each UFunction resolves on its owning class.
inline constexpr const wchar_t* ButtonClass = L"Button";                      // UButton
inline constexpr const wchar_t* PanelWidgetClass = L"PanelWidget";            // UPanelWidget (owns Slots/ClearChildren; base of CanvasPanel/HBox/VBox)
inline constexpr const wchar_t* ContentWidgetClass = L"ContentWidget";        // UContentWidget (owns SetContent; UButton is-a ContentWidget)
inline constexpr const wchar_t* SetContentFn = L"SetContent";                 // UContentWidget::SetContent(UWidget*)->UPanelSlot*
inline constexpr const wchar_t* GetContentFn = L"GetContent";                 // UContentWidget::GetContent() -> UWidget*
inline constexpr const wchar_t* WidgetIsHoveredFn = L"IsHovered";             // UWidget::IsHovered()->bool ReturnValue
inline constexpr const wchar_t* ClearChildrenFn = L"ClearChildren";           // UPanelWidget::ClearChildren() -- detach all children (objects survive); for the insert-at-top reorder

// The native main-menu version label: our own UTextBlock injected as the top row of the
// VerticalBox that holds the game's label rows (txt_version's HorizontalBox row sits in
// VerticalBox_138), so the coop version line reads as one more native label and shows and hides
// with the menu.
inline constexpr const wchar_t* UiMenuTxtVersionProp = L"txt_version";        // UTextBlock* @ ui_menu_C+0x430 (the version label -- our anchor)
inline constexpr const wchar_t* TextBlockSetColorFn = L"SetColorAndOpacity";  // UTextBlock::SetColorAndOpacity(FSlateColor) -- the SETTER; a raw ColorAndOpacity write after Slate construction never propagates (UMG bakes props at attach)
// ui_menu_C fields for the inject, resolved by FindPropertyOffset (recook-robust).
inline constexpr const wchar_t* UiMenuButtonStartProp = L"button_start";      // UButton* @ +0x2E0 (NEW GAME). The inject derives its parent VerticalBox from this button's slot and clones its FButtonStyle, tints and VBox slot layout (engine_widget.cpp); the four brushes carry no ResourceObject, so no texture loads. The label style is not cloned (null at some inject timings) but set to measured constants.
inline constexpr const wchar_t* MainPlayerEscapeFn = L"InpActEvt_Escape_K2Node_InputKeyEvent_0";  // engine input event that opens the pause menu (ProcessEvent-dispatched, same class as the flashlight InpActEvt_* we already observe)
// Head-bone anchoring (USceneComponent::GetSocketLocation, world; the bones are enumerated to find
// the head).
inline constexpr const wchar_t* GetSocketLocationFn = L"GetSocketLocation";  // (FName)->FVector (world)
inline constexpr const wchar_t* GetNumBonesFn = L"GetNumBones";
inline constexpr const wchar_t* GetBoneNameFn = L"GetBoneName";              // (int32)->FName

// Physics-prop pickup: the hook surface is the engine-native UPhysicsHandleComponent UFunctions.
// The BP-side verbs on mainPlayer_C (smoothGrab, pickupObject, dropGrabObject, ...) are BP-pure
// and inlined by their callers, so they never appear as ProcessEvent's function. What does
// dispatch during a grab: InpActEvt_use (the E press), grab__UpdateFunc / grab__FinishedFunc (the
// `grab` timeline), GrabComponentAtLocation* (pickup), SetTargetLocation* (every tick),
// ReleaseComponent (drop).

// The UPhysicsHandleComponent UFunctions (the light-grab path): ProcessEvent-dispatched, matched
// by the UFunction* resolved through reflection::FindFunction on the class.
inline constexpr const wchar_t* PhysicsHandleComponentClass            = L"PhysicsHandleComponent";
inline constexpr const wchar_t* GrabComponentAtLocationFn              = L"GrabComponentAtLocation";
inline constexpr const wchar_t* GrabComponentAtLocationWithRotationFn  = L"GrabComponentAtLocationWithRotation";
inline constexpr const wchar_t* SetTargetLocationFn                    = L"SetTargetLocation";
inline constexpr const wchar_t* SetTargetLocationAndRotationFn         = L"SetTargetLocationAndRotation";
inline constexpr const wchar_t* ReleaseComponentFn                     = L"ReleaseComponent";

// The UPhysicsConstraintComponent UFunctions (the heavy-drag path: mainPlayer_C.heavyGrab is a
// constraint, distinct from grabHandle); also ProcessEvent-dispatched.
inline constexpr const wchar_t* PhysicsConstraintComponentClass        = L"PhysicsConstraintComponent";
inline constexpr const wchar_t* SetConstrainedComponentsFn             = L"SetConstrainedComponents";
inline constexpr const wchar_t* BreakConstraintFn                      = L"BreakConstraint";

// UPrimitiveComponent::AddImpulse(FVector, FName BoneName, bool bVelChange): the throw signal; the
// BP-inlined throwHoldingProp must call it on the released prop's component.
inline constexpr const wchar_t* PrimitiveComponentClass                = L"PrimitiveComponent";
inline constexpr const wchar_t* AddImpulseFn                           = L"AddImpulse";

// Secondary, the BP timeline and input level: the grabbed prop's identity is read from
// mainPlayer_C.grabbing_actor inside the InpActEvt observer. The timeline name is content; the
// "_41" K2Node ordinal is version-fragile (a BP recook can renumber it).
inline constexpr const wchar_t* MainPlayerGrabUpdateFn       = L"grab__UpdateFunc";
inline constexpr const wchar_t* MainPlayerGrabFinishedFn     = L"grab__FinishedFunc";
inline constexpr const wchar_t* MainPlayerUseInputEventFn    = L"InpActEvt_use_K2Node_InputActionEvent_41";
// The "use" action has three delegate bindings (mainPlayer.json Export 483): _41 = IE_Pressed
// (the grab press intercepted), _38 = a second IE_Pressed, _42 = IE_Released; all three reach
// useAction's `use_deny`, so the client suppresses the deny on the two extra seams too (a
// side-effect-free cancel; _41 stays the sole intent sender). The ordinals are recook-fragile
// (sdk_check flags it).
inline constexpr const wchar_t* MainPlayerUseInputEventFn38  = L"InpActEvt_use_K2Node_InputActionEvent_38";
inline constexpr const wchar_t* MainPlayerUseInputEventFn42R = L"InpActEvt_use_K2Node_InputActionEvent_42";
// The LMB (fire) action: the native throw-when-holding seam (InpActEvt_fire -> throwHoldingProp),
// separate from use/E. Two handlers (_58 press, _59 release); the client registers both and gates
// on ClientCarryEid so only the edge that finds a live carry acts.
inline constexpr const wchar_t* MainPlayerFireInputEventFn58 = L"InpActEvt_fire_K2Node_InputActionEvent_58";
inline constexpr const wchar_t* MainPlayerFireInputEventFn59 = L"InpActEvt_fire_K2Node_InputActionEvent_59";
// The flashlight: `updateFlashlight` is BP-inlined into `Flashlight Update` and never dispatches
// through ProcessEvent, so a POST observer is registered on both. The outer name has a literal
// space (mainPlayer.hpp:488 emits `void Flashlight Update();`) and the FName lookup matches that
// exact string.
inline constexpr const wchar_t* MainPlayerUpdateFlashlightFn = L"updateFlashlight";
inline constexpr const wchar_t* MainPlayerFlashlightUpdateFn = L"Flashlight Update";

// ULightComponent::SetIntensity(float) (Engine.hpp:13551), inherited by USpotLightComponent: the
// receiver mirrors the sender's intensity on the puppet's light_R with it (the BP toggles
// Intensity, not bVisible); it marks the render state dirty, which a direct field write does not.
inline constexpr const wchar_t* SetIntensityFn      = L"SetIntensity";

// USpotLightComponent's cone-angle setters: both call MarkRenderStateDirty, so the receiver uses
// them to mirror the sender's cone; a direct field write leaves the proxy stale.
inline constexpr const wchar_t* SetOuterConeAngleFn = L"SetOuterConeAngle";
inline constexpr const wchar_t* SetInnerConeAngleFn = L"SetInnerConeAngle";

// The F input events themselves: the engine input system always dispatches them as UFunctions
// (the grab observer relies on the same shape). The K2Node ordinals (_13 / _14) are
// version-fragile. Two events per input (press and release); both are observed and the sender
// dedups on last-sent state.
inline constexpr const wchar_t* MainPlayerFlashlightInput13Fn = L"InpActEvt_flashlight_K2Node_InputActionEvent_13";
inline constexpr const wchar_t* MainPlayerFlashlightInput14Fn = L"InpActEvt_flashlight_K2Node_InputActionEvent_14";

// timerHoldFlashlight fires when the player holds F long enough to switch the beam mode (spread
// <-> focused); its BP body mutates `flashlightMode` and light_R's cone angles and intensity, and
// a POST observer reads the post-mutation state to send the cone shape (mainPlayer.hpp:655).
inline constexpr const wchar_t* MainPlayerTimerHoldFlashlightFn = L"timerHoldFlashlight";

// The flashlight click is the USoundWave `/Game/audio/effects/flashlight.flashlight`; the receiver
// plays it at the puppet's location through UGameplayStatics::PlaySoundAtLocation. Resolved by
// leaf name `flashlight` filtered by class `SoundWave` (a GUObjectArray walk, done once and
// cached).
inline constexpr const wchar_t* FlashlightClickSoundName = L"flashlight";
inline constexpr const wchar_t* SoundWaveClass           = L"SoundWave";
inline constexpr const wchar_t* PlaySoundAtLocationFn    = L"PlaySoundAtLocation";

// A USoundWave alone has no spatialisation (PlaySoundAtLocation plays 2D without an attenuation
// override), so a USoundAttenuation is constructed at runtime through UGameplayStatics::SpawnObject
// rather than borrowing the game's `att_*` assets; its field offsets are `off::att`.
inline constexpr const wchar_t* SoundAttenuationClass = L"SoundAttenuation";

// mainPlayer_C drives every ragdoll cause through `ragdollMode(bool ragdoll, bool passOut, bool
// death)` (mainPlayer.hpp:484) and recovers through `forceGetUp()` (line 641); both drive an
// unpossessed puppet (ragdollMode(true,true,false) flips isRagdoll and spawns the ragdoll actor;
// forceGetUp clears the AnimBP gate).
inline constexpr const wchar_t* MainPlayerRagdollModeFn = L"ragdollMode";
inline constexpr const wchar_t* MainPlayerForceGetUpFn  = L"forceGetUp";
// `forceWakeup()`: the unconditional stand-up. It jumps straight to the ubergraph block (@25800)
// that restores the movement mode, capsule collision, camera attach and EnableInput and detaches
// the ragdoll mesh, reading no gate; `wakeup(passOut)` early-outs on `dead || isWakingUp ||
// noWakeup` (@26584) and `forceGetUp()` runs a 0.2 s latent Delay first (@39625), so the KO
// respawn calls this one.
inline constexpr const wchar_t* MainPlayerForceWakeupFn = L"forceWakeup";

// The primary player-damage entry: the owning peer invokes it on its own possessed mainPlayer_C
// when the host relays an enemy hit, so the damage runs through that peer's armour and inventory
// BP and drops its saveSlot.health; it early-outs on an unpossessed puppet. The FName keeps its
// spaces.
inline constexpr const wchar_t* MainPlayerAddPlayerDamageFn = L"Add Player Damage";

// The damage body pulse: the puppet's skin exposes no drivable tint parameter, so the hurt flash
// swaps the body mesh materials to an existing pak material and restores them. It must be a
// skeletal-character material: a static-mesh or particle material has no GPUSkinVertexFactory
// permutation and UE substitutes its default grey. inst_goregibs_organsSK is a gore skin material
// and renders its bloody-red texture on the kerfur; swapped onto both visible body meshes
// (Mesh@0x280 and mesh_playerVisible).
inline constexpr const wchar_t* PlayerHurtFlashMaterialName = L"inst_goregibs_organsSK";
inline constexpr const wchar_t* MaterialInstanceConstantClassName = L"MaterialInstanceConstant";

// A fresh USpotLightComponent is spawned on the puppet through AddComponentByClass +
// FinishAddComponent, the only reflection-callable path that runs
// UActorComponent::RegisterComponent (which creates the FSceneProxy); the class-default light_R's
// SceneProxy stays null under reflection-only writes.
inline constexpr const wchar_t* SpotLightComponentClass = L"SpotLightComponent";

// The autotest's give/equip/battery setup: AmainPlayer_C::addPropToPlayer(FName prop), the
// cheat-menu path (spawns the actor, adds it to the inventory and may auto-equip an equipment
// item).
inline constexpr const wchar_t* MainPlayerAddPropToPlayerFn = L"addPropToPlayer";

// The standard flashlight's battery class (Aprop_batts_C): a TSubclassOf written into
// saveSlot.flashlightBattery marks "a battery is inserted" (a class reference, not an instance).
inline constexpr const wchar_t* BattsClass = L"prop_batts_C";

// The flashlight equipment class (the FName the BP uses to identify the item).
inline constexpr const wchar_t* FlashlightEquipmentClass = L"prop_equipment_flashlight_C";

// The UTimelineComponent BP-callable methods (the autotest forces the `grab` timeline so the
// timeline observers fire without a real E press); ProcessEvent-dispatched.
inline constexpr const wchar_t* TimelineComponentClass       = L"TimelineComponent";
inline constexpr const wchar_t* TimelinePlayFromStartFn      = L"PlayFromStart";

// The Aprop_C BP class name (the prop registry's GUObjectArray scan).
inline constexpr const wchar_t* PropClass             = L"prop_C";

// `Aprop_C.thrown(AmainPlayer_C* Player)`: the throw event the prop's BP wires to the whoosh sound
// and the particle trail; the receiver calls it on release with a non-zero impulse so the effects
// fire as for a local throw (prop.hpp:166).
inline constexpr const wchar_t* PropThrownFn          = L"thrown";

// Aprop_C.Init: the BP construction script that runs on every Aprop spawn (the FinishSpawningActor
// path). A POST hook on the base class catches every derivative's spawn (mushroom growth, save
// load, inventory drops); the host broadcasts, the client skips.
inline constexpr const wchar_t* PropInitFn            = L"Init";

// Classes that are intermediate state of a host-authoritative pipeline. Mushrooms grow through
// class identity, mushroom7_C (growing, hidden, collision off) to a mature class through a
// timer-driven Transform. On a client a local spawn of these is destroyed on Init POST and a wire
// PropSpawn for them is dropped; the mature variant arrives through the host's broadcast. Each
// entry is the BP class FName.
inline constexpr const wchar_t* PropMushroomGrowingClass = L"prop_food_mushroom7_C";

// The BP function all four inventory drop paths funnel through: a POST hook on UpropInventory_C
// reads its out-params to broadcast PropSpawn. On the receiver Aprop_C.setKey is dispatched
// between Begin and Finish so the prop's Init() does not overwrite Key with NewGuid.
inline constexpr const wchar_t* PropInventoryClass      = L"propInventory_C";
inline constexpr const wchar_t* PropInventoryTakeObjFn  = L"takeObj";
inline constexpr const wchar_t* PropSetKeyFn            = L"setKey";

// UKismetStringLibrary::Conv_StringToName(const FString&) -> FName (BlueprintPure, dispatched on
// the CDO): converts the wire Key string into a live FName for Aprop_C.setKey before
// FinishSpawningActor runs Init().
inline constexpr const wchar_t* KismetStringLibraryClass = L"KismetStringLibrary";
inline constexpr const wchar_t* ConvStringToNameFn       = L"Conv_StringToName";

// The UPrimitiveComponent physics UFunctions (exec thunks SetPhysicsLinearVelocity 0x1430DFA40,
// SetPhysicsAngularVelocityInDegrees 0x1430DF7B0, GetPhysicsLinearVelocity 0x1430DC550,
// GetPhysicsAngularVelocityInDegrees 0x1430DC320): the host reads the held mesh's velocity at the
// release edge and the receiver writes it after SetSimulatePhysics(true), so the body re-enters
// the simulation with the launch state.
inline constexpr const wchar_t* SetSimulatePhysicsFn                 = L"SetSimulatePhysics";
inline constexpr const wchar_t* GetPhysicsLinearVelocityFn           = L"GetPhysicsLinearVelocity";
inline constexpr const wchar_t* GetPhysicsAngularVelocityInDegreesFn = L"GetPhysicsAngularVelocityInDegrees";
inline constexpr const wchar_t* SetPhysicsLinearVelocityFn           = L"SetPhysicsLinearVelocity";
inline constexpr const wchar_t* SetPhysicsAngularVelocityInDegreesFn = L"SetPhysicsAngularVelocityInDegrees";

// AdaynightCycle_C: the singleton weather authority (the scheduler timers, the state fields, the
// mutator UFunctions), resolved through R::FindObjectByClass.
inline constexpr const wchar_t* DaynightCycleClass = L"daynightCycle_C";

// AlightningStrike_C: the one-shot strike actor (three audio components, a point light, a sphere
// collider, a timeline self-destruct); its location is its own transform. The receiver spawns it
// through the deferred pair.
inline constexpr const wchar_t* LightningStrikeClass = L"lightningStrike_C";

// AsuperFog_C: the dense "super fog" event actor. Not stored on the cycle; its existence is its
// active state, so the receiver finds and destroys live instances by class.
inline constexpr const wchar_t* SuperFogClass = L"superFog_C";

// AdirectionalWind_C: the wind actor (a singleton), resolved by class like the cycle
// (coop/weather_sync through ue_wrap/directionalwind).
inline constexpr const wchar_t* DirectionalWindClass = L"directionalWind_C";

// The scheduler UFunctions: the client intercepts them (a PRE cancel) so it never decides "rain
// now"; the host's POST observer reads the post-mutation cycle state and broadcasts WeatherState.
inline constexpr const wchar_t* DaynightCycle_timerRainFn       = L"timerRain";
inline constexpr const wchar_t* DaynightCycle_timerLightningFn  = L"timerLightning";
inline constexpr const wchar_t* DaynightCycle_fogEventFn        = L"fogEvent";
inline constexpr const wchar_t* DaynightCycle_superFogEventFn   = L"superFogEvent";
inline constexpr const wchar_t* DaynightCycle_permaRainTimerFn  = L"permaRain_timer";

// The mutator UFunctions the receiver invokes to apply the host's state: causeRain(bool) starts
// or stops the visible rain; setRainProperties(bool, 4 floats) writes the scalar block and drives
// setRainParameters; intComs_triggerSnow fans out to 53 BP listeners (a direct field write would
// miss them); setWindParameters() reads the cycle state into AdirectionalWind_C; spawnFog spawns
// the rolling-fog actor (mirror-spawned by coop/weather_fog, suppressed on the client).
// SetFogDensity is only the dev fogprobe's.
inline constexpr const wchar_t* DaynightCycle_causeRainFn          = L"causeRain";
inline constexpr const wchar_t* DaynightCycle_setRainPropertiesFn  = L"setRainProperties";
inline constexpr const wchar_t* DaynightCycle_setWindParametersFn  = L"setWindParameters";
inline constexpr const wchar_t* DaynightCycle_intComsTriggerSnowFn = L"intComs_triggerSnow";
inline constexpr const wchar_t* DaynightCycle_spawnFogFn           = L"spawnFog";
inline constexpr const wchar_t* DaynightCycle_setFogDensityFn      = L"SetFogDensity";  // dev fogprobe only
// setRainParticles directly activates or deactivates the cycle's rainEffect
// UParticleSystemComponent; causeRain does not always activate it, so the receiver calls this
// after causeRain.
inline constexpr const wchar_t* DaynightCycle_setRainParticlesFn   = L"setRainParticles";

// The red-sky path on AmainGamemode_C: spawnRedSky() spawns redSkyEvent_C and stashes it at
// mainGamemode.redSky @0x0888 (mainGamemode.hpp:150); later toggles call redSky.set(bool isred),
// which swaps the four colour-curve assets on the daynightCycle deterministically.
inline constexpr const wchar_t* MainGamemode_SpawnRedSkyFn      = L"spawnRedSky";
inline constexpr const wchar_t* RedSkyEventClass               = L"redSkyEvent_C";
inline constexpr const wchar_t* RedSkyEvent_SetFn              = L"set";
// The other two newDay weather-event birth classes (weather_event_births' client birth catch);
// both present in the bp_reflection dumps.
inline constexpr const wchar_t* WeatherFogControllerClass      = L"weatherFogController_C";
inline constexpr const wchar_t* BlackFogClass                  = L"blackFog_C";
// SetCollisionEnabled (UPrimitiveComponent): remote_prop::OnSpawn restores default collision
// (QueryAndPhysics=3) on wire-converged props whose local copy a natural-spawn pipeline left at
// NoCollision. The param is `NewType` (ECollisionEnabled::Type).
inline constexpr const wchar_t* SetCollisionEnabledFn                = L"SetCollisionEnabled";
// The cap-mushroom class: it overrides Aprop_C::Init and its natural-spawn path
// (AmushroomSpawner_C::Spawn -> spawnedNaturally()) sets its StaticMesh to NoCollision.
// mushroom7_C grows into prop_puffballMature_C, a separate species.
inline constexpr const wchar_t* PropFoodMushroomClass                = L"prop_food_mushroom_C";
}  // namespace name

}  // namespace ue_wrap::profile
