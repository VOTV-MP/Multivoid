// ue_wrap/sdk_profile.h -- THE VERSION SURFACE.
//
// Everything in this file is specific to one game build. When the mod is
// brought up against a new VOTV version (e.g. 1.0.0), THIS is the file you
// review and re-derive; the rest of the code is version-agnostic logic.
//
// Three kinds of knowledge, three failure modes:
//   * AOB signatures  -- break on ANY engine recompile (even a patch).
//   * struct offsets   -- stable within an engine version; shift across them.
//   * content names    -- change when the game's blueprints/levels change.
//
// Re-derivation workflow (see research/findings/ue-wrap-reflection-2026-05-22.md):
//   1. Launch once with UE4SS; its log prints GUObjectArray / FName::ToString
//      addresses (ground truth). Compute RVAs (addr - module base).
//   2. In IDA (the .exe is loaded), confirm each RVA, derive a unique AOB
//      (wildcard rip displacements), verify uniqueness with find_bytes.
//   3. For ProcessEvent, dump a UObject vtable at runtime (HealthCheck can),
//      find the un-overridden slot, decompile to confirm, derive its AOB.
//   4. Update the constants here; the boot HealthCheck reports what still fails.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ue_wrap::profile {

// ---- target identity -----------------------------------------------------
inline constexpr const char* kTargetGameVersion = "Alpha 0.9.0-n";
inline constexpr const char* kTargetEngineVersion = "UE4.27";  // exe FileVersion 4.27.2.0

// Exe fingerprint of the build these signatures were derived against. The boot
// HealthCheck logs the running exe's size + file version and WARNS on a
// mismatch (signatures/offsets are then suspect). Recorded from a passing
// health check on the 0.9.0-n shipping exe. 0 = not recorded.
inline constexpr unsigned long long kExpectedExeSize = 84751360;

// ---- AOB signatures (most volatile: any recompile breaks these) ----------
// FName::ToString -- unique function prologue; match == function address.
inline constexpr const char* kSigFNameToString =
    "48 89 5C 24 18 55 56 57 48 8B EC 48 83 EC 30 8B 01 48 8B F1 "
    "44 8B 49 04 8B F8 C1 EF 10 48 8B DA 0F B7 C8";

// GUObjectArray -- static init-guard that does `lea rcx,[rip+&GUObjectArray]`.
// Decode the disp32 at match+kGUObjArrayLeaDispOff; the lea ends (rip base) at
// match+kGUObjArrayLeaEndOff, so addr = match + EndOff + disp32.
inline constexpr const char* kSigGUObjectArray =
    "8B 05 ?? ?? ?? ?? 3B 05 ?? ?? ?? ?? 75 13 48 8D 15 ?? ?? ?? ?? "
    "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 45 33 C9 "
    "48 89 45 ?? 4C 8D 45";
inline constexpr size_t kGUObjArrayLeaDispOff = 24;
inline constexpr size_t kGUObjArrayLeaEndOff = 28;

// UObject::ProcessEvent -- unique prologue (vtable index 68). Match == address.
inline constexpr const char* kSigProcessEvent =
    "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC F0 00 00 00 48 8D 6C 24 30 "
    "48 89 9D 18 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C5 48 89 85 B0 00 00 00";

// ---- struct offsets (stable within UE4.27; re-check on an engine bump) ----
namespace off {
inline constexpr size_t UObject_InternalIndex = 0x0C;  // int32 -- slot in GUObjectArray (O(1) liveness check)
inline constexpr size_t UObject_ClassPrivate = 0x10;
inline constexpr size_t UObject_NamePrivate = 0x18;
inline constexpr size_t UObject_OuterPrivate = 0x20;

inline constexpr size_t FUObjectArray_ObjObjects = 0x10;  // FChunkedFixedUObjectArray
inline constexpr size_t Chunk_Objects = 0x00;             // FUObjectItem** chunk table
inline constexpr size_t Chunk_MaxElements = 0x10;
inline constexpr size_t Chunk_NumElements = 0x14;
inline constexpr size_t Chunk_NumChunks = 0x1C;
inline constexpr int32_t ElemsPerChunk = 64 * 1024;
inline constexpr size_t FUObjectItem_Stride = 0x18;       // {Object*, flags, cluster, serial}
inline constexpr size_t FUObjectItem_Flags = 0x08;        // int32 EInternalObjectFlags (PendingKill/Unreachable -> dying)
inline constexpr size_t mainGameInstance_loadObjects = 0x0229;  // bool: apply the save on BeginPlay (vs fresh)
inline constexpr size_t UWidgetComponent_WidgetClass = 0x0480;  // TSubclassOf<UUserWidget> (set before register so InitWidget builds it)
inline constexpr size_t UWidgetComponent_BlendMode = 0x04E4;    // EWidgetBlendMode (uint8): 0=Opaque,1=Masked(ctor DEFAULT!),2=Transparent
inline constexpr size_t UWidgetComponent_bIsTwoSided = 0x04E5;  // bool
inline constexpr size_t UWidgetComponent_bDrawAtDesiredSize = 0x04A8;  // bool: quad fits the widget (centers text on the actor)
inline constexpr size_t UWidgetComponent_TranslucentMaterial = 0x04F0;          // UMaterialInterface* (two-sided slot)
inline constexpr size_t UWidgetComponent_TranslucentMaterialOneSided = 0x04F8;  // UMaterialInterface*
inline constexpr size_t UWidgetComponent_MaterialInstance = 0x0528;             // UMaterialInstanceDynamic* (the live drawn material)
inline constexpr size_t UWidgetComponent_RenderTarget = 0x0520;                 // UTextureRenderTarget2D*
inline constexpr size_t UWidgetComponent_bManuallyRedraw = 0x0490;             // bool (must be 0 to auto-redraw on tick)
inline constexpr size_t UWidgetComponent_RedrawTime = 0x0494;                  // float (0 = redraw every tick)
inline constexpr size_t UWidgetComponent_BackgroundColor = 0x04C0;             // FLinearColor (RT clear color)
// Own-UMG widget tree (built via SpawnObject):
inline constexpr size_t UUserWidget_WidgetTree = 0x01D8;       // UWidgetTree*
inline constexpr size_t UWidgetTree_RootWidget = 0x0028;       // UWidget*
inline constexpr size_t UTextBlock_Text = 0x0128;              // FText
inline constexpr size_t UTextBlock_ColorAndOpacity = 0x0150;   // FSlateColor {FLinearColor@0, ColorUseRule(uint8)@0x10}
inline constexpr size_t UTextBlock_Font = 0x0188;              // FSlateFontInfo {FontObject@0, Size(int32)@0x48}
inline constexpr size_t UTextLayoutWidget_Justification = 0x010B;  // TEnumAsByte<ETextJustify> (1=Center)
inline constexpr size_t FSlateFontInfo_Size = 0x48;            // within UTextBlock_Font
inline constexpr size_t FSlateColor_ColorUseRule = 0x10;       // within UTextBlock_ColorAndOpacity (0=UseColor_Specified)

// UStruct / UFunction / FField / FProperty layout (UE4.27, 4.25+ FField system).
// Derived from the shipping UObject::ProcessEvent decompile (rva 0x1465930):
// it allocs PropertiesSize, memcpy's ParmsSize from the caller params, walks the
// param chain from ChildProperties via FField::Next, and reads each property's
// flags/size/offset. See research/findings/uproperty-param-marshaling-*.
inline constexpr size_t UStruct_ChildProperties = 0x50;   // FField* (params first, then locals)
inline constexpr size_t UStruct_PropertiesSize = 0x58;    // int32 (full frame size)
inline constexpr size_t UFunction_ParmsSize = 0xB6;       // uint16 (param-region size)
inline constexpr size_t UFunction_ReturnValueOffset = 0xB8;  // uint16 (0xFFFF = none)

inline constexpr size_t FField_Next = 0x20;               // FField*
inline constexpr size_t FField_NamePrivate = 0x28;        // FName
inline constexpr size_t FProperty_ElementSize = 0x38;     // int32
inline constexpr size_t FProperty_ArrayDim = 0x3C;        // int32
inline constexpr size_t FProperty_PropertyFlags = 0x40;   // uint64
inline constexpr size_t FProperty_Offset_Internal = 0x4C; // int32 (byte offset in the frame)

// UStruct::SuperStruct -- the parent class/struct, for walking the inheritance
// chain when resolving an inherited UProperty/UFunction. CONFIRMED 0x40 at
// runtime (the Actor class's qword at 0x40 == the Object class pointer).
inline constexpr size_t UStruct_SuperStruct = 0x40;

// APawn / AActor instance fields that make a spawned pawn act as a LOCAL player.
// Zeroed in the deferred-spawn window (before FinishSpawningActor/BeginPlay) so a
// remote pawn never auto-acquires a PlayerController / steals input. Verified
// against the CXX SDK dump (Engine.hpp): APawn 0x230/0x231, AActor 0x5A/0xF3.
// EAutoReceiveInput::Disabled = 0.
inline constexpr size_t AActor_bBlockInput = 0x5A;          // uint8 (set 1)
inline constexpr size_t AActor_AutoReceiveInput = 0xF3;     // uint8 enum (set 0)
inline constexpr size_t APawn_AutoPossessPlayer = 0x230;    // uint8 enum (set 0)
inline constexpr size_t APawn_AutoPossessAI = 0x231;        // uint8 enum (set 0)
inline constexpr size_t APawn_AIControllerClass = 0x238;    // UClass* (TSubclassOf<AController>)

// Remote-body animation (verified against CXX SDK dump). An unpossessed/off-
// screen remote pawn's body collapses to a "stick" because the skeletal mesh
// only ticks its pose when rendered; force AlwaysTickPoseAndRefreshBones(=0).
// And drive the body AnimBP (UAnimBlueprint_kerfurOmega_regular_C on
// mesh_playerVisible) walk speed directly: 0 = idle.
inline constexpr size_t USkinnedMesh_VisibilityBasedAnimTickOption = 0x604;  // uint8 (set 0)  Engine.hpp:18308
inline constexpr size_t USkeletalMesh_AnimScriptInstance = 0x6B0;            // AnimInstance*  Engine.hpp:18095

// USceneComponent transform-locals (parent-relative). Engine.hpp:17904-17905.
// The puppet's SkeletalMeshComponent must MIRROR the source mainPlayer_C's
// `mesh_playerVisible` RelLoc + RelRot, otherwise the visible body floats above
// the ground (RelLoc.Z carries the BP-authored mesh-to-actor offset, e.g. the
// kerfur skin's root-bone-vs-feet shim) and faces 90 deg sideways (RelRot.Yaw
// = -90 is the standard "mesh authored along Y+, actor +X" reconciliation).
// The wire stays in the source actor's NATIVE frame (actor.Z, actor.Yaw); the
// per-class visual offset is reconciled once at puppet spawn by copying these
// two fields off the local mainPlayer's mesh component (both peers run the
// same mainPlayer_C class, so its mesh component carries the same RelLoc/RelRot
// the source uses -- pull the truth from the local copy at runtime, no
// hard-coded compensation).
inline constexpr size_t USceneComponent_RelativeLocation = 0x011C;  // FVector
inline constexpr size_t USceneComponent_RelativeRotation = 0x0128;  // FRotator

// ---- skin-puppet (the remote body) ---------------------------------------
// Root-cause finding (two converging agents, CXX dump): the body AnimBP poses
// purely from its own public variables; it does NOT require a possessing
// controller (the SAME AnimBP is shared by NPC classes figura/kerfurOmega, so
// its owner casts are null-safe). So the remote body is a BARE actor we own
// (ASkeletalMeshActor) wearing the local player's exact skin + this AnimBP,
// posed by driving the variables below directly -- no pawn, no controller, no
// hijack surface. (RULE 3 parallel class hierarchy; RULE 1 root-cause.)
inline constexpr size_t AmainPlayer_mesh_playerVisible = 0x04F8;  // USkeletalMeshComponent*  mainPlayer.hpp:13
inline constexpr size_t USkinnedMesh_SkeletalMesh = 0x0480;       // USkeletalMesh* (the skin asset)  Engine.hpp:18299
inline constexpr size_t USkeletalMesh_AnimClass = 0x06A8;         // TSubclassOf<UAnimInstance>  Engine.hpp:18094

// UAnimBlueprint_kerfurOmega_regular_C public variables (offsets within the live
// AnimInstance). The locomotion-drive variables are PULL: BlueprintUpdateAnimation
// (the AnimBP's sole custom writer per the CXX dump) is expected to read the
// owning Pawn's velocity (Pawn->GetVelocity() or Movement->Velocity) and write
// spd / animWalkAlpha / animWalkRate from it.
//
// On our puppet (a SkeletalMeshActor, NOT a Pawn -> Pawn cache is null), that
// read fails. Whether our direct writes to these variables SURVIVE depends on
// whether BlueprintUpdateAnimation early-outs on null Pawn (standard BP pattern
// = our writes survive) or writes 0 unconditionally (= our writes get clobbered).
// The DriveAnimBP path read-backs animWalkAlpha each frame to log the truth.
inline constexpr size_t AnimBP_kerfur_walkSpeed = 0x2D68;          // float  (CONFIG cache: max walk speed; local has 0.0 even while WALKING -> NOT the BlendSpace input)
inline constexpr size_t AnimBP_kerfur_Pawn = 0x2D70;               // APawn*   (cached owner; null on puppet)
inline constexpr size_t AnimBP_kerfur_Controller = 0x2D78;         // AController* (cached; null on puppet)
inline constexpr size_t AnimBP_kerfur_Movement = 0x2D80;           // UPawnMovementComponent* (cached; null on puppet -> BP's Movement->Velocity branch dead)
inline constexpr size_t AnimBP_kerfur_animWalkAlpha = 0x2D88;      // float  CONFIRMED NOT the walk gate: spawn-time diagnostic 2026-05-23 showed the LOCAL has animWalkAlpha=0.00 while WALKING. BUA computes it but it does not gate the BlendSpace. Plan B1 leaves it untouched.
inline constexpr size_t AnimBP_kerfur_animWalkRate = 0x2D8C;       // float  BlendSpace play-rate scalar. BUAInterceptor sets to 1.0 so a default of 0 cannot freeze BlendSpace sample interp.
inline constexpr size_t AnimBP_kerfur_lookAt = 0x2D90;             // FVector
inline constexpr size_t AnimBP_kerfur_lookingAtPlayer = 0x2E01;    // bool
inline constexpr size_t AnimBP_kerfur_kerfur = 0x2E08;             // AkerfurOmega_C* (null for a player body too)
inline constexpr size_t AnimBP_kerfur_walkSpeedMultiplier = 0x2E18;// float
inline constexpr size_t AnimBP_kerfur_spd = 0x2E1C;                // float  (live speed; local has 600 while walking -> the BlendSpace X input)
inline constexpr size_t AnimBP_kerfur_useLegIK = 0x2E39;           // bool (false = skip floor-trace IK)
inline constexpr size_t AnimBP_kerfur_headLookAt = 0x2E3C;         // FRotator
inline constexpr size_t AnimBP_kerfur_isFace = 0x2E48;             // bool

// ---- HUD / Canvas (screen-space nameplate, MTA-style) --------------------
// We hook AHUD::ReceiveDrawHUD (ProcessEvent-dispatched), read the live UCanvas
// off the HUD, project the remote head world->screen and draw the nickname with
// UCanvas::K2_DrawText (takes an FString -> no FText, outline+shadow built in).

// Freecam gamma fix: a bare ACameraActor renders with default post-process, so the
// player's exposure/grading is lost and the view goes dark. Copy the player camera's
// FPostProcessSettings onto the freecam camera. The struct has ONE TArray
// (WeightedBlendables @0x550) -- zero it in the copy so the two cameras don't alias
// the same heap array (double-free).
inline constexpr size_t AmainPlayer_Camera = 0x0530;                    // UCameraComponent*  mainPlayer.hpp:20
inline constexpr size_t ACameraActor_CameraComponent = 0x0228;          // UCameraComponent*  Engine.hpp:6947
inline constexpr size_t UCameraComponent_PostProcessBlendWeight = 0x0240; // float  Engine.hpp:9762
inline constexpr size_t UCameraComponent_PostProcessSettings = 0x0270;    // FPostProcessSettings  Engine.hpp (size 0x560)
inline constexpr size_t FPostProcessSettings_Size = 0x0560;
inline constexpr size_t FPostProcessSettings_WeightedBlendables = 0x0550; // TArray (0x10) within the PP struct

inline constexpr size_t AHUD_bShowHUD = 0x0228;     // uint8  Engine.hpp:7413 (VOTV draws via UMG -> default HUD canvas off; force 1 to get ReceiveDrawHUD)
inline constexpr size_t AHUD_Canvas = 0x0270;       // UCanvas*  Engine.hpp:7422
inline constexpr size_t UCanvas_SizeX = 0x0040;     // int32     Engine.hpp:9846
inline constexpr size_t UCanvas_SizeY = 0x0044;     // int32     Engine.hpp:9847
inline constexpr size_t UEngine_SmallFont = 0x0050; // UFont*    Engine.hpp:10732
}  // namespace off

// EPropertyFlags bits we test (engine-stable).
namespace cpf {
inline constexpr uint64_t Parm = 0x80;
inline constexpr uint64_t OutParm = 0x100;
inline constexpr uint64_t ReturnParm = 0x400;
}  // namespace cpf

// ---- content names (change with game content, not the engine) ------------
namespace name {
inline constexpr const wchar_t* MainPlayerClass = L"mainPlayer_C";
inline constexpr const wchar_t* GamemodeClass = L"mainGamemode_C";
inline constexpr const wchar_t* ActorClass = L"Actor";
inline constexpr const wchar_t* WorldClass = L"World";
inline constexpr const wchar_t* SetActorLocationFn = L"K2_SetActorLocation";
inline constexpr const wchar_t* GameplayLevel = L"untitled_1";

// КПП (kontrolno-propusknoi punkt / guard-checkpoint) spawn point on the
// `s_may2026` save -- the visible landmark at the base entrance. Host
// naturally lands here when the save loads (per the LoadStorySave log:
// "in gameplay (mainPlayer @ -37695,69978,6420)"). The CLIENT instance
// loads the same save but lands ~14 m away at a different wakeup spot
// (per [[project-autotest-spawn-pose]]); the user's "spawn remote on КПП"
// rule (2026-05-23) wants the client teleported to this known landmark
// after gameplay loads. Value is the actor center (capsule centre), in
// world cm. Re-derive per game version.
inline constexpr float kKPPSpawnX = -37695.f;
inline constexpr float kKPPSpawnY =  69978.f;
inline constexpr float kKPPSpawnZ =   6420.f;

// Engine classes/functions we dispatch through (stable engine names, not VOTV
// content -- but kept here so the porting surface is one file). The persistent
// GameInstance subclass is VOTV content (the world context that survives a
// level load).
inline constexpr const wchar_t* KismetSystemLibraryClass = L"KismetSystemLibrary";
inline constexpr const wchar_t* ExecuteConsoleCommandFn = L"ExecuteConsoleCommand";
inline constexpr const wchar_t* GameInstanceClass = L"mainGameInstance_C";

// Actor spawning (the BlueprintCallable deferred-spawn pair the K2
// SpawnActorFromClass node uses) + transform get/set.
inline constexpr const wchar_t* GameplayStaticsClass = L"GameplayStatics";
inline constexpr const wchar_t* BeginDeferredSpawnFn = L"BeginDeferredActorSpawnFromClass";
inline constexpr const wchar_t* FinishSpawningActorFn = L"FinishSpawningActor";

// Story-save load: VOTV's own entry path (GameplayStatics::LoadGameFromSlot ->
// mainGameInstance_C::setSaveSlotObject + loadObjects=true -> open untitled_1).
// The gameplay map is untitled_1 for ALL modes; the SAVE selects story vs sandbox.
inline constexpr const wchar_t* LoadGameFromSlotFn = L"LoadGameFromSlot";
inline constexpr const wchar_t* SetSaveSlotObjectFn = L"setSaveSlotObject";
inline constexpr const wchar_t* ActorClassName = L"Actor";  // owns K2_Get/SetActorLocation
inline constexpr const wchar_t* GetActorLocationFn = L"K2_GetActorLocation";
inline constexpr const wchar_t* GetActorRotationFn = L"K2_GetActorRotation";
inline constexpr const wchar_t* GetActorVelocityFn = L"GetVelocity";  // AActor::GetVelocity -> FVector (cm/s)
inline constexpr const wchar_t* GetActorForwardVectorFn = L"GetActorForwardVector";
inline constexpr const wchar_t* SetActorRotationFn = L"K2_SetActorRotation";
inline constexpr const wchar_t* SetActorTickEnabledFn = L"SetActorTickEnabled";
inline constexpr const wchar_t* DestroyActorFn = L"K2_DestroyActor";

// Controller removal -- a remote pawn auto-acquires its OWN PlayerController on
// spawn (mainPlayer_C auto-possesses Player0), and that 2nd controller fights the
// LOCAL player's input/view. Strip it: GetController -> Detach -> destroy.
inline constexpr const wchar_t* PawnClassName = L"Pawn";
inline constexpr const wchar_t* AIControllerClassName = L"AIController";  // /Script/AIModule.AIController
inline constexpr const wchar_t* GetControllerFn = L"GetController";
inline constexpr const wchar_t* DetachFromControllerFn = L"DetachFromControllerPendingDestroy";
// Give the remote pawn an AIController (poses the body via the possession drive
// path) WITHOUT a PlayerController's viewport/input/camera -> no local hijack.
inline constexpr const wchar_t* SpawnDefaultControllerFn = L"SpawnDefaultController";

// Skin-puppet: a bare ASkeletalMeshActor wearing the local player's skin +
// the body AnimBP. SetSkeletalMesh is owned by USkinnedMeshComponent; SetAnimClass
// by USkeletalMeshComponent (FindFunction does NOT climb to super, so each is
// resolved from its OWN class).
inline constexpr const wchar_t* SkeletalMeshActorClass = L"SkeletalMeshActor";
inline constexpr const wchar_t* SkeletalMeshComponentClass = L"SkeletalMeshComponent";
inline constexpr const wchar_t* SkinnedMeshComponentClass = L"SkinnedMeshComponent";
inline constexpr const wchar_t* SetSkeletalMeshFn = L"SetSkeletalMesh";   // USkinnedMeshComponent
inline constexpr const wchar_t* SetAnimClassFn = L"SetAnimClass";         // USkeletalMeshComponent

// Bug 2 Plan B1: BUA-interceptor target. The kerfur AnimBP class (its
// AnimBlueprintGeneratedClass leaf name; the AnimBP source asset is
// 'AnimBlueprint_kerfurOmega_regular' but the generated UClass adds the '_C'
// suffix UE4 uses for BP-generated classes). BlueprintUpdateAnimation is the
// per-frame BP event whose body writes spd=0 on null-Pawn puppets.
inline constexpr const wchar_t* AnimBPKerfurRegularClass = L"AnimBlueprint_kerfurOmega_regular_C";
inline constexpr const wchar_t* BlueprintUpdateAnimationFn = L"BlueprintUpdateAnimation";

// Dev freecam: a spawned ACameraActor we point the view at (SetViewTargetWithBlend),
// look driven by the game's own control rotation (smooth), moved by WASD per frame.
inline constexpr const wchar_t* CameraActorClass = L"CameraActor";
inline constexpr const wchar_t* ControllerClassName = L"Controller";
inline constexpr const wchar_t* GetControlRotationFn = L"GetControlRotation";
inline constexpr const wchar_t* PlayerControllerClassName = L"PlayerController";
inline constexpr const wchar_t* SetViewTargetWithBlendFn = L"SetViewTargetWithBlend";
inline constexpr const wchar_t* PlayerCameraManagerClass = L"PlayerCameraManager";
inline constexpr const wchar_t* GetCameraLocationFn = L"GetCameraLocation";
inline constexpr const wchar_t* GetCameraRotationFn = L"GetCameraRotation";

// Component visibility (USceneComponent BlueprintCallable) -- to force the
// third-person body meshes visible on an unpossessed remote pawn.
inline constexpr const wchar_t* SceneComponentClass = L"SceneComponent";
inline constexpr const wchar_t* SetVisibilityFn = L"SetVisibility";
inline constexpr const wchar_t* SetHiddenInGameFn = L"SetHiddenInGame";
inline constexpr const wchar_t* GetComponentLocationFn = L"K2_GetComponentLocation";
inline constexpr const wchar_t* GetComponentForwardFn = L"GetForwardVector";
inline constexpr const wchar_t* CameraComponentClass = L"CameraComponent";

// Component destruction (UActorComponent::K2_DestroyComponent(Object)) -- to
// remove the local-only systems a remote pawn must NOT own (its unbound
// PostProcessComponent stomps the local screen's gamma/exposure).
inline constexpr const wchar_t* ActorComponentClass = L"ActorComponent";
inline constexpr const wchar_t* DestroyComponentFn = L"K2_DestroyComponent";
inline constexpr const wchar_t* PostProcessComponentClass = L"PostProcessComponent";

// Nameplate via a world-space UWidgetComponent (TRANSLUCENT). TextRender can't do
// partial alpha (UnlitText is Masked). A WidgetComponent renders its UMG to a render
// target composited with /Engine/EngineMaterials/Widget3DPassThrough_Translucent
// (BlendMode must be set Transparent -- ctor defaults Masked). We build our OWN UMG
// (UUserWidget+UTextBlock via SpawnObject) -- no cooked-widget dependency.
inline constexpr const wchar_t* WidgetComponentClass = L"WidgetComponent";
inline constexpr const wchar_t* AddComponentByClassFn = L"AddComponentByClass";      // on Actor
inline constexpr const wchar_t* FinishAddComponentFn = L"FinishAddComponent";        // on Actor
inline constexpr const wchar_t* SetTintColorAndOpacityFn = L"SetTintColorAndOpacity";// FLinearColor
inline constexpr const wchar_t* RequestRedrawFn = L"RequestRedraw";                  // sets bRedrawRequested
inline constexpr const wchar_t* RequestRenderUpdateFn = L"RequestRenderUpdate";      // forces render-state/RT refresh
inline constexpr const wchar_t* SetComponentTickEnabledFn = L"SetComponentTickEnabled";  // on UActorComponent -- a runtime-added WidgetComponent doesn't tick -> never draws its RT
inline constexpr const wchar_t* NameplateSetTextFn = L"SetText";                     // UTextBlock::SetText(FText)
inline constexpr const wchar_t* TextBlockClass = L"TextBlock";
inline constexpr const wchar_t* KismetTextLibraryClass = L"KismetTextLibrary";
inline constexpr const wchar_t* ConvStringToTextFn = L"Conv_StringToText";           // FString -> FText
inline constexpr const wchar_t* WidgetBaseClass = L"Widget";                         // UWidget (owns SetVisibility; SetVisibilityFn defined above)
// World-space WidgetComponent translucent material (the slot a runtime-added
// component may have null -> blank quad). bIsTwoSided=true uses the two-sided slot.
inline constexpr const wchar_t* Widget3DTranslucentMatName = L"Widget3DPassThrough_Translucent";
inline constexpr const wchar_t* Widget3DTranslucentOneSidedMatName = L"Widget3DPassThrough_Translucent_OneSided";
inline constexpr const wchar_t* MaterialInstanceConstantClass = L"MaterialInstanceConstant";  // class of those MICs

// RULE-1 nameplate: build our OWN UMG (no cooked-widget reuse). NewObject is the
// reflected UFunction UGameplayStatics::SpawnObject(objectClass, Outer). We make a
// UUserWidget -> UWidgetTree -> UTextBlock (root), then UWidgetComponent::SetWidget.
inline constexpr const wchar_t* SpawnObjectFn = L"SpawnObject";        // on GameplayStatics: (objectClass, Outer)->UObject*
inline constexpr const wchar_t* UserWidgetClass = L"UserWidget";
inline constexpr const wchar_t* WidgetTreeClass = L"WidgetTree";
inline constexpr const wchar_t* SetWidgetFn = L"SetWidget";            // UWidgetComponent::SetWidget(UUserWidget*)
inline constexpr const wchar_t* FontName = L"Roboto";                  // /Engine/EngineFonts/Roboto.Roboto
inline constexpr const wchar_t* FontClassName = L"Font";
// Screen-space HUD feed (UUserWidget added to the viewport, not a world WidgetComponent).
inline constexpr const wchar_t* AddToViewportFn = L"AddToViewport";        // UUserWidget::AddToViewport(int32 ZOrder)
inline constexpr const wchar_t* RemoveFromViewportFn = L"RemoveFromViewport";  // UUserWidget::RemoveFromViewport()
inline constexpr const wchar_t* WidgetClass = L"Widget";                   // owns SetVisibility (FindFunction = owning class, no super walk)
inline constexpr const wchar_t* WidgetSetVisibilityFn = L"SetVisibility";  // UWidget::SetVisibility(ESlateVisibility); HitTestInvisible=3
inline constexpr const wchar_t* SetPositionInViewportFn = L"SetPositionInViewport";    // UUserWidget(FVector2D Position, bool bRemoveDPIScale)
inline constexpr const wchar_t* SetAlignmentInViewportFn = L"SetAlignmentInViewport";  // UUserWidget(FVector2D Alignment)
// Head-bone anchoring (USceneComponent::GetSocketLocation world; enumerate bones to find head).
inline constexpr const wchar_t* GetSocketLocationFn = L"GetSocketLocation";  // (FName)->FVector (world)
inline constexpr const wchar_t* GetNumBonesFn = L"GetNumBones";
inline constexpr const wchar_t* GetBoneNameFn = L"GetBoneName";              // (int32)->FName
}  // namespace name

}  // namespace ue_wrap::profile
