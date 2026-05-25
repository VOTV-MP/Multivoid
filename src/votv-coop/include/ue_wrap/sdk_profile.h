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
inline constexpr size_t UTextBlock_Font = 0x0188;              // FSlateFontInfo {FontObject@0, Size(int32)@0x48, OutlineSettings@0x10 (size 0x20)}
inline constexpr size_t UTextBlock_ShadowOffset = 0x0268;      // FVector2D (2 floats). UMG.hpp:1447
inline constexpr size_t UTextBlock_ShadowColorAndOpacity = 0x0270;  // FLinearColor (4 floats RGBA). UMG.hpp:1448
inline constexpr size_t UTextLayoutWidget_Justification = 0x010B;  // TEnumAsByte<ETextJustify> (1=Center)
inline constexpr size_t FSlateFontInfo_Size = 0x48;            // within UTextBlock_Font
inline constexpr size_t FSlateFontInfo_OutlineSettings = 0x10; // FFontOutlineSettings (size 0x20) within FSlateFontInfo. SlateCore.hpp:313
inline constexpr size_t FFontOutlineSettings_OutlineSize  = 0x00; // int32 within FFontOutlineSettings. SlateCore.hpp:161
inline constexpr size_t FFontOutlineSettings_OutlineColor = 0x10; // FLinearColor within FFontOutlineSettings. SlateCore.hpp:165
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
inline constexpr size_t APawn_Controller = 0x0258;          // AController* (live possessing controller; null on unpossessed/orphan pawn). Engine.hpp:7646. The puppet's mainPlayer_C orphan has this null by design (auto-possess disabled); we need the LOCAL player's controller for the AnimBP Controller-cache write (animation-fix v2, 2026-05-25: BP state-machine idle->walk transition requires AnimBP.Controller != None).
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

// 2026-05-25 puppet rework (mainPlayer_C orphan path, per
// research/findings/votv-puppet-mainplayer-body-RE-2026-05-25.md): the
// remote-player puppet now spawns mainPlayer_C directly (inertPawn=true)
// instead of ASkeletalMeshActor. We need to:
//   * destroy the per-screen PostProcess components (these affect the
//     LOCAL camera's color grading -- a puppet must not own them);
//   * hide the FP-arms viewmodel (camera-space attached, wrong on a
//     puppet);
//   * keep mesh_playerVisible visible (the third-person body the local
//     player sees when they look down + the OTHER peer's puppet body).
// All offsets from mainPlayer.hpp; verified there.
inline constexpr size_t AmainPlayer_PostProcess_overlays_OBSOLETE = 0x04C8;  // UPostProcessComponent*  mainPlayer.hpp:9
inline constexpr size_t AmainPlayer_mic                           = 0x0518;  // UAudioCaptureComponent*  mainPlayer.hpp (mic input; must be destroyed on the orphan to prevent latent device hold)
inline constexpr size_t AmainPlayer_PostProcess_pl                = 0x0590;  // UPostProcessComponent*  mainPlayer.hpp:50
inline constexpr size_t AmainPlayer_arms                          = 0x05F8;  // USkeletalMeshComponent*  mainPlayer.hpp:55 (FP viewmodel)
inline constexpr size_t AmainPlayer_playermodel                   = 0x0638;  // USkeletalMeshComponent*  mainPlayer.hpp:58 (legacy/equipment overlay; review visibility)
inline constexpr size_t AmainPlayer_GameMode                      = 0x0C80;  // AmainGamemode_C*  mainPlayer.hpp (cached at BeginPlay; nulled on the puppet so subsequent gamemode-interaction BP paths return early)
inline constexpr size_t mainGamemode_mainPlayer                   = 0x0630;  // AmainPlayer_C*  mainGamemode.hpp (the canonical "the local player" ref the gamemode uses for save/sleep/damage; the orphan's BeginPlay would overwrite it via intComs_gamemodeBeginPlay -- the puppet path captures + restores)

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
// AnimBP_kerfur_* offsets MOVED to reflection-resolved accessors in
// ue_wrap/reflected_offset.{h,cpp} (2026-05-25 adapt-5/N). These fields
// belong to a BP-cooked class (AnimBlueprint_kerfurOmega_regular_C); a
// VOTV BP recook shifts their offsets while the field NAMES stay
// stable. FindPropertyOffset(class, fieldName) auto-adapts; no
// hardcoded offset bracket needed.

// AController::ControlRotation -- the view rotation the input system accumulates
// (pitch + yaw). Direct write to set the camera direction without going through
// the BP-callable SetControlRotation UFunction (which does the same thing).
// Engine.hpp:7071.
inline constexpr size_t AController_ControlRotation = 0x0288;  // FRotator

// ACharacter capsule -- needed for the foot-on-ground correction in RemotePlayer.
// The puppet's SkeletalMeshComponent is the actor's root (no sub-component slot
// for the standard ACharacter::Mesh.RelLoc.Z = -halfH shim), so RemotePlayer
// reconstructs the same world-Z offset at the actor transform level. capsule
// half-height = the distance from the source's actor centre down to source
// ground (where the source's feet ARE meant to be). See [[project-remote-
// player-open-issues]] for the full derivation.
inline constexpr size_t ACharacter_Mesh             = 0x0280;          // USkeletalMeshComponent*  Engine.hpp:6970 (native body slot; mainPlayer_C uses mesh_playerVisible @0x04F8 as the authoritative body and the native slot is typically hidden)
inline constexpr size_t ACharacter_CharacterMovement = 0x0288;         // UCharacterMovementComponent*  Engine.hpp:6971 (mainPlayer_C orphan: must be tick-disabled on the puppet so the satellite is the only Velocity source)
inline constexpr size_t ACharacter_CapsuleComponent = 0x0290;          // UCapsuleComponent*  Engine.hpp:6972
inline constexpr size_t UCapsuleComponent_CapsuleHalfHeight = 0x0468;  // float  Engine.hpp:9883

// USceneComponent::AttachParent @ +0x00C0 (USceneComponent*). The scene-graph
// parent the component is attached TO. Used by SpawnPuppetMainPlayer's
// diagnostic dump to verify mesh_playerVisible's parent chain (BP authoring
// can attach it to either the capsule root or the native ACharacter::Mesh
// slot -- hiding the latter with bPropagateToChildren=true would cascade
// down and hide the body, which is exactly the "no visible model"
// regression the user hit 2026-05-25). Engine.hpp:17900.
inline constexpr size_t USceneComponent_AttachParent      = 0x00C0;
// USceneComponent flag bytes (bitfields). bHiddenInGame is bit 2 of the
// byte at 0x14D (per Engine.hpp:17917 ordering); bVisible is bit 4 of the
// byte at 0x14C. We log the raw bytes for diagnosis -- masking out the
// individual bits is done inline at the read site.
inline constexpr size_t USceneComponent_VisFlagsByte      = 0x014C;  // bVisible @ bit 4
inline constexpr size_t USceneComponent_HiddenFlagsByte   = 0x014D;  // bHiddenInGame @ bit 2

// USceneComponent::RelativeLocation @ +0x011C (FVector, 12 bytes). The
// BP-AUTHORED relative offset to the parent component -- a STATIC field whose
// settled value is what the BP construction script writes (mainPlayer_C's
// construction sets mesh_playerVisible.RelativeLocation.Z to its authored
// shim, typically ~-halfH). Read directly via raw memory (not via
// K2_GetComponentLocation, which returns the computed WORLD location and is
// affected by transient mid-init values). RULE 1 Option-G fix: stream
// source.actor.Z (stable capsule centre), receiver reads the LOCAL player's
// settled mesh_playerVisible.RelativeLocation.Z ONCE at puppet Spawn (the BP
// constant -- same value on every instance of the same class, same value on
// every peer), apply as actor-level Z offset on the puppet. See Engine.hpp
// line 17904 for the FProperty descriptor. IDA-confirmed.
inline constexpr size_t USceneComponent_RelativeLocation = 0x011C;     // FVector  Engine.hpp:17904

// ---- Physics-prop pickup interaction --------------------------------------
// See research/findings/physics-object-pickup-architecture-2026-05-23.md.
// Three converging research agents (IDA / SDK-reflection / MTA-fidelity)
// established: mainPlayer_C grabs ANY of ~540 Aprop_C derivatives via E-press;
// the lift/drag distinction is DATA-DRIVEN (Fstruct_prop.heavy bool, loaded
// once from DataTable at prop Init -- NOT a mass comparison), so both peers
// compute the same answer from the same prop identity. We observe the BP
// dispatch surface via ProcessEvent name filter; the native PhysicsHandle
// layer (GrabComponentAtLocation @ 0x1430c64b0 + 4 sibling exec thunks,
// renamed in IDB) is a clean cross-cut we can also intercept if needed.

// mainPlayer_C grab-state fields (mainPlayer.hpp, all writers BP-only).
// LIGHT grab path is UPhysicsHandleComponent; HEAVY grab path is a DIFFERENT
// component class (UPhysicsConstraintComponent) -- our PHC observers will NOT
// fire on heavy drag; needs the constraint observers below.
// mainPlayer_* offsets MOVED to reflection-resolved accessors in
// ue_wrap/reflected_offset.{h,cpp} (2026-05-25 adapt-5/N). The
// in-shipping ones (heavyGrab, grabHandle, grabTimeline, grabbing_actor,
// grabbing_component, grabsHeavy, grabLen, Heavy) auto-adapt across
// VOTV BP recooks via FindPropertyOffset.
//
// (Fields formerly listed here but NEVER referenced in shipping code --
// mainPlayer_holding_actor, mainPlayer_grabRelativeLocation,
// mainPlayer_heavyGrabLocation, mainPlayer_heavyGrabArm -- have been
// dropped per RULE 2: no migration baggage. If a future feature needs
// one, add a reflection-resolved accessor in reflected_offset.{h,cpp}.)

// UPhysicsHandleComponent internal layout (IDA-confirmed via ReleaseComponent
// + UpdateHandleTransform decompile at 0x142D7C670 / 0x142D7EE30; matches
// stock UE4.27 source line 336/413 of PhysicsHandleComponent.cpp).
inline constexpr size_t UPhysicsHandleComponent_GrabbedComponent   = 0x00B0;  // UPrimitiveComponent* (cleared on Release)
inline constexpr size_t UPhysicsHandleComponent_GrabbedBoneName    = 0x00B8;  // FName (8 bytes)
inline constexpr size_t UPhysicsHandleComponent_Flags              = 0x00C0;  // DWORD (&=~1 clears bConstrained)
inline constexpr size_t UPhysicsHandleComponent_TargetRotation     = 0x00E0;  // FQuat (16 bytes)
inline constexpr size_t UPhysicsHandleComponent_TargetLocation     = 0x00F0;  // FVector (per-tick PhysX kinematic target)
inline constexpr size_t UPhysicsHandleComponent_KinActorData       = 0x0148;  // FPhysicsActorHandle*
inline constexpr size_t UPhysicsHandleComponent_ConstraintHandle   = 0x0150;  // FPhysicsConstraintHandle*

// Aprop_C field offsets (prop.hpp).
//
// Aprop_C.Key is an FName (8 bytes: ComparisonIndex u32 + Number u32). Its
// ComparisonIndex is a globally stable index into the GNames pool that is
// IDENTICAL across peers for the same FName string (because cooked content
// shares the same name table). This makes the ComparisonIndex our 32-bit
// cross-peer prop identifier -- though the ASSIGNMENT of a Key value to a
// specific prop instance still happens at spawn-time on the spawning
// machine, which is the known prerequisite (spawn-from-craft / spawn-from-
// menu paths must route through host to agree on which Key goes on which
// physical prop). See [[project-physics-object-pickup]] for the blocker.
inline constexpr size_t Aprop_propData      = 0x0260;  // Fstruct_prop (sizeof 0x72)
inline constexpr size_t Fstruct_prop_heavy  = 0x006C;  // bool inside propData -- THE heavy flag
inline constexpr size_t Aprop_propData_heavy = Aprop_propData + Fstruct_prop_heavy;  // = 0x02CC
inline constexpr size_t Aprop_Static        = 0x02D8;  // bool (a Static prop can't be grabbed)
inline constexpr size_t Aprop_frozen        = 0x02DA;  // bool
inline constexpr size_t Aprop_Key           = 0x02E0;  // FName (ComparisonIndex @ +0, Number @ +4)
inline constexpr size_t Aprop_StaticMesh    = 0x0238;  // UStaticMeshComponent*

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

// Phase 5F flashlight (mainPlayer.hpp + RE doc). The player-held world
// light is the SpotLight on mainPlayer itself (Case b per the RE doc) --
// the flashlight ITEM actor for the _a / _b variants carries no light
// component of its own. Puppets are mainPlayer_C orphans so they have
// the same offsets.
inline constexpr size_t AmainPlayer_light_R         = 0x0678;  // USpotLightComponent* (mainPlayer.hpp:61)
inline constexpr size_t AmainPlayer_lag_fl          = 0x0670;  // USpringArmComponent* parenting light_R (mainPlayer.hpp:60)
inline constexpr size_t AmainPlayer_flashlight     = 0x0838;  // bool -- canonical on/off (mainPlayer.hpp:115)
inline constexpr size_t AmainPlayer_flashlightMode = 0x0C79;  // uint8 -- beam mode (mainPlayer.hpp:251)
inline constexpr size_t AmainPlayer_hasFlashlight  = 0x0CC2;  // bool -- equipped guard (mainPlayer.hpp:260)
inline constexpr size_t AmainPlayer_crankFlashlight = 0x0CC4;  // bool -- _c variant marker (mainPlayer.hpp:262)
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

// kerfur AnimBP head-rotation pipeline (from CXX dump AnimBlueprint_kerfurOmega_regular.hpp
// + AnimGraphRuntime.hpp, confirmed by IDA agent 2026-05-23 PM). The visible head
// twist is driven by TWO FAnimNode_LookAt nodes -- they take a target (bone-socket
// or world-space FVector) and rotate `BoneToModify` toward it. With these enabled,
// no amount of writing `lookingAtPlayer = false` or `headLookAt` will stop the
// head from tracking whatever the LookAt's target is. To bypass entirely we set
// each LookAt's `Alpha` (the SkeletalControlBase blend weight) to 0 + clear
// `bAlphaBoolEnabled`. There are also 7 FAnimNode_ModifyBone instances in the
// graph; the one with BoneToModify=='head' (TBD by runtime probe -- diagnostic
// dumps each instance's name once at puppet spawn) is the bone-rotation slot
// we can drive directly with FRotator writes.
namespace anim {
// FAnimNode_SkeletalControlBase fields (relative to node base):
inline constexpr size_t SkelCtl_bAlphaBoolEnabled = 0x29;   // bool
inline constexpr size_t SkelCtl_Alpha             = 0x2C;   // float
// FAnimNode_LookAt + FAnimNode_ModifyBone share BoneToModify at the same offset:
inline constexpr size_t LookAtMod_BoneToModify    = 0xC8;   // FBoneReference (FName @+0)
// FAnimNode_LookAt specifics:
inline constexpr size_t LookAt_LookAtTarget       = 0xE0;   // FBoneSocketTarget (0x60 bytes)
inline constexpr size_t LookAt_LookAtLocation     = 0x140;  // FVector (fallback if no socket target)
// FAnimNode_ModifyBone specifics (for direct head-rotation writes):
inline constexpr size_t ModBone_Translation       = 0xD8;   // FVector
inline constexpr size_t ModBone_Rotation          = 0xE4;   // FRotator
inline constexpr size_t ModBone_RotationMode      = 0xFD;   // EBoneModificationMode (0=Ignore,1=Add,2=Replace)
// AnimGraphNode_* offsets within the kerfurOmega_regular AnimInstance:
inline constexpr size_t kKerfurLookAt_1           = 0x1730;
inline constexpr size_t kKerfurLookAt             = 0x18E0;
inline constexpr size_t kKerfurModifyBone_6       = 0x1268;
inline constexpr size_t kKerfurModifyBone_5       = 0x1ED0;
inline constexpr size_t kKerfurModifyBone_4       = 0x2450;
inline constexpr size_t kKerfurModifyBone_3       = 0x2578;
inline constexpr size_t kKerfurModifyBone_2       = 0x2680;
inline constexpr size_t kKerfurModifyBone_1       = 0x2A28;
inline constexpr size_t kKerfurModifyBone         = 0x2C60;
}  // namespace anim

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

// 2026-05-25 LATE +5h (F3 RestoreVitals dev feature): the live UsaveSlot_C
// instance holds the player's CURRENT runtime vitals (food/sleep/health/
// coffeePower), not just the persisted save record -- the BP code mutates
// these fields in-place, then the SaveGameToSlot UFunction serializes the
// whole object on save. So `FindObjectByClass(SaveSlotClass)` returns the
// object whose float fields ARE the live vitals; writing them via reflection
// max-outs the meters immediately. Field names confirmed via the
// CXXHeaderDump of saveSlot.hpp (food @0x00E4, sleep @0x00E8, health
// @0x0428, coffeePower @0x04D0 -- but offsets resolved at runtime via
// FindPropertyOffset, not hardcoded, per the BP-cooked-offset-volatile
// rule in reflected_offset.{h,cpp}).
inline constexpr const wchar_t* SaveSlotClass = L"saveSlot_C";

// Actor spawning (the BlueprintCallable deferred-spawn pair the K2
// SpawnActorFromClass node uses) + transform get/set.
inline constexpr const wchar_t* GameplayStaticsClass = L"GameplayStatics";

// Phase 5N1 (NPC sync foundation, 2026-05-25): UGameplayStatics's
// BeginDeferredActorSpawnFromClass UFunction is the canonical deferred-
// spawn entry every BP "spawn actor of class" graph compiles down to.
// Intercepting it gives us ONE engine-level hook for all NPC + spawner
// classes; replaces the 14-per-spawner-observer plan (architecture-doc
// Hook surface table line 222) with 1 observer. See
// research/findings/votv-npc-sync-prereqs-RE-2026-05-24.md section 4.
// The constant for this UFunction is `BeginDeferredSpawnFn` (declared
// further down in this file, alongside the engine-side reuse from
// remote_prop.cpp + engine.cpp -- RULE 2: one name across all callers).

// Phase 5N1 NPC class allowlist (12 enemy NPC classes per
// votv-npc-sync-prereqs-RE-2026-05-24.md section 1 verdict). On CLIENT,
// every BeginDeferredActorSpawnFromClass for a class in this list is
// SUPPRESSED (interceptor returns true; ActorClass param zeroed so the
// BP graph receives nullptr and bails per UE4's standard nullptr-check
// pattern). Host runs spawners normally; client's NPC actors arrive via
// the EntityPoseBatch wire (Inc2; not implemented yet).
//
// All 12 classes are confirmed targeting-compatible with our mainPlayer_C
// orphan puppet (RE section 1 verdict: "ALL 12 enemy classes either use
// AIPerception/PawnSensing or generic Cast to AActor / Cast to
// mainPlayer_C. The orphan puppet -- being a real Pawn of the same class
// -- is reachable by every targeting path inspected.").
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

// Compact array for the allowlist resolver (Inc1 of Phase 5N1). Iterated
// once at install time; each name resolved via R::FindClass + cached. The
// classes are loaded on first gameplay-level transition (NOT at menu)
// per VOTV's content-cooking pattern -- so the install retry path must
// re-attempt until ALL 12 resolve (or log per-class failures).
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
};
inline constexpr size_t kNpcAllowlistSize = sizeof(kNpcAllowlist) / sizeof(kNpcAllowlist[0]);
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
inline constexpr const wchar_t* SetActorScale3DFn = L"SetActorScale3D";  // void(FVector NewScale3D) -- world-space scale, used to shrink the nameplate world quad while keeping the WidgetComponent's RT resolution
inline constexpr const wchar_t* SetActorTickEnabledFn = L"SetActorTickEnabled";
inline constexpr const wchar_t* DestroyActorFn = L"K2_DestroyActor";
inline constexpr const wchar_t* TeleportToFn = L"K2_TeleportTo";  // bool(FVector, FRotator); large-distance teleport that survives Character/CMC constraints
inline constexpr const wchar_t* GetActorBoundsFn = L"GetActorBounds";   // (bool,FVector&,FVector&,bool); world-space AABB of the actor's mesh

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

// Physics-prop pickup interaction. RIGHT HOOK SURFACE -- the engine-native
// UPhysicsHandleComponent UFunctions. Determined empirically (debug build
// 14d0787 hands-on test 2026-05-23): the original SDK-header guesses
// (smoothGrab, pickupObject, dropGrabObject, throwHoldingProp,
// switchToHeavyDrag, pickupObjectDirect, playerTryToGrab, canPickup) ARE
// BP-pure inline functions on mainPlayer_C -- they have valid UFunction
// pointers (FindFunction succeeds) but BP-compiled callers inline them, so
// they NEVER appear as ProcessEvent's `function` arg. Zero observer hits in
// 3 minutes of active grabbing. All deleted per RULE 2.
//
// What DOES dispatch through ProcessEvent during a grab:
//   * InpActEvt_use_K2Node_InputActionEvent_41 on mainPlayer_C  (E press)
//   * grab__UpdateFunc / grab__FinishedFunc on mainPlayer_C
//       (auto-generated by the `grab` UTimelineComponent; BP body calls
//        grabHandle->SetTargetLocation each tick)
//   * UPhysicsHandleComponent.GrabComponentAtLocation* (at pickup)
//   * UPhysicsHandleComponent.SetTargetLocation* (every tick during grab)
//   * UPhysicsHandleComponent.ReleaseComponent (at drop)
//
// We hook the engine-native UFunctions (universal -- captures light grab
// AND heavy drag), with the BP-Timeline ones as secondary diagnostic.
// IDA decompile + log analysis written up in
// research/findings/votv-physics-interaction-deep-re-2026-05-23.md.

// Engine UPhysicsHandleComponent UFunctions (light-grab path; ProcessEvent-
// dispatched -- the exec thunks at rva 0x1430C64B0 / 0x1430C65D0 / 0x1430C6AD0 /
// 0x1430C6B60 / 0x142FEA9B0 are UFunction.Func pointers; ProcessEvent's
// `function` arg IS the UFunction* we match on, looked up by FName via
// reflection::FindFunction(UPhysicsHandleComponent::StaticClass(), name)).
inline constexpr const wchar_t* PhysicsHandleComponentClass            = L"PhysicsHandleComponent";
inline constexpr const wchar_t* GrabComponentAtLocationFn              = L"GrabComponentAtLocation";
inline constexpr const wchar_t* GrabComponentAtLocationWithRotationFn  = L"GrabComponentAtLocationWithRotation";
inline constexpr const wchar_t* SetTargetLocationFn                    = L"SetTargetLocation";
inline constexpr const wchar_t* SetTargetLocationAndRotationFn         = L"SetTargetLocationAndRotation";
inline constexpr const wchar_t* ReleaseComponentFn                     = L"ReleaseComponent";

// Engine UPhysicsConstraintComponent UFunctions (HEAVY drag path -- VOTV uses
// a physics CONSTRAINT for heavy drag, not a handle; SDK dump shows
// mainPlayer_C.heavyGrab is UPhysicsConstraintComponent*, distinct from
// mainPlayer_C.grabHandle which is UPhysicsHandleComponent*). These ARE
// ProcessEvent-dispatched too -- BP calls them via CallFunction nodes.
inline constexpr const wchar_t* PhysicsConstraintComponentClass        = L"PhysicsConstraintComponent";
inline constexpr const wchar_t* SetConstrainedComponentsFn             = L"SetConstrainedComponents";
inline constexpr const wchar_t* BreakConstraintFn                      = L"BreakConstraint";
inline constexpr const wchar_t* SetDisableCollisionFn                  = L"SetDisableCollision";

// Engine UPrimitiveComponent.AddImpulse (THROW path -- VOTV's throwHoldingProp
// is BP-pure inline, but it MUST call AddImpulse on the released prop's
// component to send it flying. AddImpulse IS ProcessEvent-dispatched). Same
// signature for primary owner UE4 4.27: (FVector impulse, FName BoneName,
// bool bVelChange). Used as the throw signal.
inline constexpr const wchar_t* PrimitiveComponentClass                = L"PrimitiveComponent";
inline constexpr const wchar_t* AddImpulseFn                           = L"AddImpulse";

// Secondary -- BP-Timeline / input level. Useful for triangulation and to
// learn the grabbed prop's identity (read mainPlayer_C.grabbing_actor from
// inside the InpActEvt observer just after the BP graph has decided what to
// grab). The Timeline name "grab" is content; the "_41" K2Node ordinal is
// version-fragile (a BP graph re-cook can renumber it).
inline constexpr const wchar_t* MainPlayerGrabUpdateFn       = L"grab__UpdateFunc";
inline constexpr const wchar_t* MainPlayerGrabFinishedFn     = L"grab__FinishedFunc";
inline constexpr const wchar_t* MainPlayerUseInputEventFn    = L"InpActEvt_use_K2Node_InputActionEvent_41";

// Phase 5F flashlight (item activation sync). Two UFunctions in the chain;
// hands-on testing 2026-05-25 NIGHT-3 showed `updateFlashlight` is BP-INLINED
// into `Flashlight Update` and never dispatches via ProcessEvent (the inner
// observer never fired across a session of F-presses), so we register a
// POST observer on BOTH and whichever actually dispatches wins. The OUTER
// function name has a LITERAL SPACE -- mainPlayer.hpp:488 emitted
// `void Flashlight Update();`, and the FName lookup matches that exact
// string verbatim.
//   InpActEvt_flashlight_K2Node_InputActionEvent_13/14  (F press/release)
//   -> Flashlight Update()    <- dispatched via ProcessEvent
//   -> updateFlashlight()     <- INLINED; never reaches ProcessEvent
inline constexpr const wchar_t* MainPlayerUpdateFlashlightFn = L"updateFlashlight";
inline constexpr const wchar_t* MainPlayerFlashlightUpdateFn = L"Flashlight Update";

// Phase 5F retest fallback (2026-05-25 NIGHT-3): hands-on showed that
// BOTH updateFlashlight AND 'Flashlight Update' are BP-inlined into
// their callers and never dispatch via ProcessEvent. The remaining
// candidate is the input event itself, which the engine input system
// always dispatches as a UFunction (the grab_observer relies on the
// same shape for InpActEvt_use). The K2Node ordinal (_13 / _14) is
// version-fragile -- a BP recook can renumber it; we follow the same
// brittleness the grab observer accepted with _use_..._41. Two events
// per input (press + release); we register on both and dedup at sender
// via last-sent-state (release fires but the bool didn't change ->
// no-op).
inline constexpr const wchar_t* MainPlayerFlashlightInput13Fn = L"InpActEvt_flashlight_K2Node_InputActionEvent_13";
inline constexpr const wchar_t* MainPlayerFlashlightInput14Fn = L"InpActEvt_flashlight_K2Node_InputActionEvent_14";

// UTimelineComponent BP-callable methods (used to force the `grab` Timeline
// from the autonomous test, so the 3 BP-Timeline observers fire without a
// real E-press). These ARE ProcessEvent-dispatched (BP CallFunction nodes).
inline constexpr const wchar_t* TimelineComponentClass       = L"TimelineComponent";
inline constexpr const wchar_t* TimelinePlayFromStartFn      = L"PlayFromStart";
inline constexpr const wchar_t* TimelineStopFn               = L"Stop";

// Aprop_C BP class name (for PropKeyRegistry GUObjectArray scan, Stage 4).
inline constexpr const wchar_t* PropClass             = L"prop_C";

// `Aprop_C.thrown(AmainPlayer_C* Player)` -- the natural throw event the
// prop's BP wires to throw-whoosh sound + particle-trail activation.
// Calling this on the receiver as part of OnRelease (when impulse non-zero)
// makes the sound/effects fire identically to a local throw -- RULE 1
// natural dispatch, NOT a separate sound-trigger crutch. Per prop.hpp:166.
inline constexpr const wchar_t* PropThrownFn          = L"thrown";

// Phase 5S0 Inc2: Aprop_C.Init is the BP UserConstructionScript that runs
// on every Aprop spawn (FinishSpawningActor path). POST-hooking it on the
// PropClass base gives us a single observer that catches EVERY Aprop
// derivative spawn -- mushroom growth, save-load (filtered by
// !g_session.connected()), inventory drops on the spawning peer, etc.
// HOST broadcasts; CLIENT skips (host-authoritative).
inline constexpr const wchar_t* PropInitFn            = L"Init";

// Phase 5N Stream B (host-authoritative spawners 2026-05-24): classes that
// represent INTERMEDIATE STATE for a host-authoritative pipeline. Mushrooms
// have a 2-class growth state machine: mushroom7_C (growing, hidden,
// collision-off) -> mushroom_C (mature) via timer-driven Transform.
// Per mushroom-state RE: growth is encoded by CLASS IDENTITY, not a field.
//
// On CLIENT: any local spawn of these classes (from per-peer spawners) is
// DESTROYED on Init POST. Any wire-received PropSpawn for these classes is
// DROPPED in event_feed. The mature variant flows through normally via
// Inc2 broadcast when host's growth timer transforms mushroom7_C -> mushroom_C.
//
// Extensible -- add other intermediate-variant classes as identified in
// future RE (e.g. growable plants, food spoilage transitions). Each entry
// is the BP class FName (no A- prefix; matches sdk_profile convention).
inline constexpr const wchar_t* PropMushroomGrowingClass = L"prop_food_mushroom7_C";

// v5 Bug C (inventory drop): the BP function that all 4 drop paths funnel
// through (per research/findings/votv-inventory-drop-spawn-RE-2026-05-24.md).
// We POST-hook this on UpropInventory_C and read the out-params to broadcast
// PropSpawn. ON the receiver, Aprop_C.setKey is dispatched between Begin and
// Finish spawning so the prop's Init() doesn't overwrite Key with NewGuid.
inline constexpr const wchar_t* PropInventoryClass      = L"propInventory_C";
inline constexpr const wchar_t* PropInventoryTakeObjFn  = L"takeObj";
inline constexpr const wchar_t* PropSetKeyFn            = L"setKey";

// UKismetStringLibrary::Conv_StringToName(const FString&) -> FName.
// BlueprintPure, ProcessEvent-dispatched on the KismetStringLibrary CDO.
// Used by remote_prop::OnSpawn to convert the wire Key string into a live
// FName (registers in the engine's FName pool with a fresh local index),
// then passed to Aprop_C.setKey on the just-spawned actor BEFORE
// FinishSpawningActor runs Init() so the Init's NewGuid branch is skipped.
inline constexpr const wchar_t* KismetStringLibraryClass = L"KismetStringLibrary";
inline constexpr const wchar_t* ConvStringToNameFn       = L"Conv_StringToName";

// Native UPrimitiveComponent UFunctions used by the receiver-side puppet
// path to put a held prop into kinematic mode + apply inherited physics
// velocity. These ARE ProcessEvent-dispatchable (native exec thunks already
// RVA'd in IDB by the IDA agent: SetPhysicsLinearVelocity @ 0x1430DFA40,
// SetPhysicsAngularVelocityInDegrees @ 0x1430DF7B0,
// GetPhysicsLinearVelocity @ 0x1430DC550,
// GetPhysicsAngularVelocityInDegrees @ 0x1430DC320 -- see
// research/findings/votv-throw-release-pipeline-RE-2026-05-24.md). Host reads
// GetPhysics* on the held mesh AT THE RELEASE EDGE to capture the body's
// inherited tracking velocity; receiver writes the matching SetPhysics*
// AFTER SetSimulatePhysics(true) so the body re-enters dynamic sim with the
// correct launch state.
inline constexpr const wchar_t* SetSimulatePhysicsFn                 = L"SetSimulatePhysics";
inline constexpr const wchar_t* GetPhysicsLinearVelocityFn           = L"GetPhysicsLinearVelocity";
inline constexpr const wchar_t* GetPhysicsAngularVelocityInDegreesFn = L"GetPhysicsAngularVelocityInDegrees";
inline constexpr const wchar_t* SetPhysicsLinearVelocityFn           = L"SetPhysicsLinearVelocity";
inline constexpr const wchar_t* SetPhysicsAngularVelocityInDegreesFn = L"SetPhysicsAngularVelocityInDegrees";
// SetCollisionEnabled lives on UPrimitiveComponent; used by remote_prop::OnSpawn
// to restore default collision (QueryAndPhysics=3) on wire-converged props
// whose local copy had collision disabled by a natural-spawn pipeline (e.g.
// Aprop_food_mushroom_C goes through spawnedNaturally() -> NoCollision on the
// spawning peer). Param is `NewType` (ECollisionEnabled::Type, uint8 enum).
inline constexpr const wchar_t* SetCollisionEnabledFn                = L"SetCollisionEnabled";
// The class that overrides Aprop_C::Init AND triggers spawnedNaturally() ->
// NoCollision on its StaticMesh in the natural-spawn path
// (AmushroomSpawner_C::Spawn). 2026-05-25 RE
// (research/findings/votv-mushroom-fall-through-RE-2026-05-25.md): this is
// the actual cap-mushroom class. mushroom7_C (PropMushroomGrowingClass) grows
// into prop_puffballMature_C, NOT this -- separate species.
inline constexpr const wchar_t* PropFoodMushroomClass                = L"prop_food_mushroom_C";
}  // namespace name

}  // namespace ue_wrap::profile
