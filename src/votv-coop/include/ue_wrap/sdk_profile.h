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
// Re-derivation workflow (see research/findings/phase0-bootstrap/ue-wrap-reflection-2026-05-22.md):
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

// FMemory::Realloc -- the tiny wrapper `{ if(!GMalloc) GCreateMalloc(); return
// GMalloc->Realloc(ptr,size,align); }`. We match it ONLY to recover the GMalloc
// pointer: decode the disp32 of its `mov rcx, cs:GMalloc` (at +kGMallocLeaDispOff;
// the instruction ends at +kGMallocLeaEndOff, so &GMalloc = hit + EndOff + disp32).
// With GMalloc we can call FMalloc::Free (vtable byte offset kFMallocFreeVtOff) to
// release engine-allocated buffers -- needed because FName::ToString ORPHANS the
// caller's FString buffer every call (zeroes Out.Data without freeing, then
// allocates fresh), so our reused render scratch leaked an engine FString per render
// (the 2026-06-13 RAM balloon). vtable offsets IDA-verified against this UE4.27
// build: Realloc=+0x20, Free=+0x30.
inline constexpr const char* kSigFMemoryRealloc =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F1 41 8B D8 "
    "48 8B 0D ?? ?? ?? ?? 48 8B FA 48 85 C9 75 0C";
inline constexpr size_t kGMallocLeaDispOff = 24;
inline constexpr size_t kGMallocLeaEndOff  = 28;
inline constexpr size_t kFMallocFreeVtOff    = 0x30;  // FMalloc::Free    vtable byte offset (UE4.27)
inline constexpr size_t kFMallocReallocVtOff = 0x20;  // FMalloc::Realloc vtable byte offset (UE4.27)
// EngineAlloc allocates via FMalloc::Realloc(nullptr, size, align) -- the SAME slot the
// kSigFMemoryRealloc signature is matched FROM (verified-in-use), not the never-exercised
// Malloc slot. Realloc(null,n) == Malloc(n); allocator-matched with EngineFree (both GMalloc),
// so the engine's later Array realloc / GC free of a buffer WE allocated does not corrupt.

// UGameplayStatics::SaveGameToSlot(USaveGame* obj, const FString& slot, int32 idx)
// -> bool. The SINGLE physical write chokepoint: every save trigger (autosave/
// sleep/quicksave/menu/forced + the direct-SaveGameToSlot world triggers) and
// both save containers (saveSlot_C world save + save_main_C meta save) funnel
// their TArray<uint8> through this native fn -> ISaveGameSystem::SaveGame.
// Hooking HERE (not the BP saveToSlot funnel) is mechanism-correct: BP->BP calls
// dispatch via ProcessInternal, bypassing our ProcessEvent detour, so a UFunction
// interceptor on saveToSlot would never fire. RE: research/findings/
// votv-save-path-RE-2026-05-30.md; IDA UGameplayStatics__SaveGameToSlot @0x142B59AA0.
// Match == function address (unique-verified prologue through `mov edi,r8d`).
inline constexpr const char* kSigSaveGameToSlot =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B DA 33 F6 "
    "48 8D 54 24 30 48 89 74 24 30 48 89 74 24 38 41 8B F8";

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
inline constexpr size_t mainGameInstance_GameMode = 0x01E1;  // TEnumAsByte<enum_gamemode::Type> (story/sandbox/...; the menu sets it on load from the slot-name prefix, our LoadStorySave must too) -- mainGameInstance.hpp:11
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

// MULTIPLAYER menu-button inject (P1, 2026-06-04): insert a UButton at the TOP of
// the UVerticalBox that holds button_start (NEW GAME), inside ui_menu_C. All UMG.hpp /
// SlateCore.hpp cited.
inline constexpr size_t UWidget_Slot = 0x0028;                 // UWidget::Slot (UPanelSlot*) -- the layout-slot back-pointer. UMG.hpp:1742
// UVerticalBoxSlot layout region (UMG.hpp:1705-1710): Size@0x38 (FSlateChildSize,8)
// + Padding@0x40 (FMargin,16) + HorizontalAlignment@0x58 + VerticalAlignment@0x59.
// Cloned as ONE block so an injected item's slot matches a native item's exactly
// (indent + spacing). Excludes the base UPanelSlot Parent/Content ptrs (< 0x38).
inline constexpr size_t UVerticalBoxSlot_LayoutStart = 0x0038;
inline constexpr size_t UVerticalBoxSlot_LayoutSize  = 0x0022;  // 0x38..0x5A (Size+Padding+H/VAlign)
inline constexpr size_t UPanelSlot_Parent = 0x0028;            // UPanelSlot::Parent (UPanelWidget*) -- the containing panel. UMG.hpp:1009
inline constexpr size_t UPanelSlot_Content = 0x0030;           // UPanelSlot::Content (UWidget*). UMG.hpp:1010
inline constexpr size_t UPanelWidget_Slots = 0x0108;           // UPanelWidget::Slots (TArray<UPanelSlot*>). UMG.hpp:1016
// UButtonSlot (a UButton's single content slot, holding its label). UMG.hpp:314-318.
// A freshly-spawned button's content slot defaults to HAlign_Center -> the label sits
// indented/centered; we set it Fill + left-justified text to sit flush like the native
// menu items. EHorizontalAlignment: Fill=0/Left=1/Center=2; EVerticalAlignment: Center=2.
inline constexpr size_t UButtonSlot_Padding = 0x0038;         // FMargin (0x10)
inline constexpr size_t UButtonSlot_HAlign  = 0x0048;         // TEnumAsByte<EHorizontalAlignment>
inline constexpr size_t UButtonSlot_VAlign  = 0x0049;         // TEnumAsByte<EVerticalAlignment>
inline constexpr size_t UButton_WidgetStyle = 0x0128;          // FButtonStyle (size 0x278). UMG.hpp:287
inline constexpr size_t UButton_ColorAndOpacity = 0x03A0;      // FLinearColor. UMG.hpp:288
inline constexpr size_t UButton_BackgroundColor = 0x03B0;      // FLinearColor. UMG.hpp:289
inline constexpr size_t FButtonStyle_Size = 0x278;             // clone size for the button-style copy (matches NEW GAME)
// FButtonStyle carries two FSlateSound members. Each is { UObject* ResourceObject @
// 0x00 (the USoundBase to play) } + an UNREFLECTED TSharedPtr<FSlateSoundResource>
// cache @ 0x08 (0x10 bytes). A raw FButtonStyle memcpy shallow-aliases that refcounted
// cache (no AddRef). The clone therefore KEEPS the copied ResourceObject (so Slate plays
// the native press/hover sounds on our real UButton) but ZEROES only the trailing cache
// (SlateCore rebuilds it lazily from ResourceObject). Offsets WITHIN FButtonStyle.
// SlateCore.hpp:18-19,320-324.
inline constexpr size_t FButtonStyle_PressedSlateSound = 0x248;  // FSlateSound (0x18) within FButtonStyle
inline constexpr size_t FButtonStyle_HoveredSlateSound = 0x260;  // FSlateSound (0x18) within FButtonStyle
inline constexpr size_t FSlateSound_Size = 0x18;                 // SlateCore.hpp:324
inline constexpr size_t FSlateSound_CacheStart = 0x08;           // trailing TSharedPtr cache (zero only this, keep ResourceObject @ 0x00)

// UStruct / UFunction / FField / FProperty layout (UE4.27, 4.25+ FField system).
// Derived from the shipping UObject::ProcessEvent decompile (rva 0x1465930):
// it allocs PropertiesSize, memcpy's ParmsSize from the caller params, walks the
// param chain from ChildProperties via FField::Next, and reads each property's
// flags/size/offset. See research/findings/uproperty-param-marshaling-*.
inline constexpr size_t UStruct_ChildProperties = 0x50;   // FField* (params first, then locals)
inline constexpr size_t UStruct_PropertiesSize = 0x58;    // int32 (full frame size)
inline constexpr size_t UFunction_ParmsSize = 0xB6;       // uint16 (param-region size)
inline constexpr size_t UFunction_ReturnValueOffset = 0xB8;  // uint16 (0xFFFF = none)
// UFunction::Func -- the FNativeFuncPtr the BP-VM invokes for EVERY call path (a
// native function's C++ exec thunk; a script function's ProcessInternal). IDA-pinned
// 2026-06-21 against exe ad478218 (size kExpectedExeSize): UFunction::Invoke
// (0x141302DC0) does `(*(Function+0xD8))(Object, &Frame, Result)`. Patching this
// pointer = the standalone "UE4SS RegisterHook" technique -- catches an EX_CallMath
// call a ProcessEvent observer can NEVER see (the chipPile/clump spawn). The value
// must land in .text. See ue_wrap/ufunction_hook.{h,cpp} +
// research/findings/piles-trash/votv-chippile-dispatch-and-thunk-hook-RE-2026-06-21.md.
inline constexpr size_t UFunction_Func = 0xD8;

// FFrame layout (the BP-VM execution frame; one passed to every native exec thunk).
// IDA-pinned 2026-06-21 from execBeginDeferredActorSpawnFromClass (0x14300B270): it
// reads Object @ a2[3] (the Step Context) and Code @ a2[4]. Node@0x10/Object@0x18/
// Code@0x20/Locals@0x28/PropertyChainForCompiledIn@0x80/CurrentNativeFunction@0x88.
//   FFrame_Object -- the actor whose bytecode is executing = the SOURCE entity for a
//     spawn issued from its ubergraph (the re-piling clump). Bytecode-confirmed the
//     clump's BeginDeferred passes EX_Self for WorldContextObject (== this Object).
//   FFrame_Code -- non-null on a bytecode (EX_CallMath) dispatch (params stepped from
//     the stream, NOT Locals -- so a native-thunk hook reads Object, never Locals).
inline constexpr size_t FFrame_Object = 0x18;             // UObject* (the executing/source object)
inline constexpr size_t FFrame_Code   = 0x20;             // uint8* (instruction ptr; null on the ProcessInternal path)

// UWorld spawn-refusal window (the join-window BeginDeferred-null root, 2026-07-04).
// IDA-pinned against exe ad478218 from UWorld::SpawnActor (0x142C12D20), the two
// silent early-outs that return null in a Shipping build (LogSpawn is compiled out):
//   0x142c12df7  test byte ptr [world+10Ch], 2    -> bIsRunningConstructionScript
//                (refuses unless SpawnParams.bAllowDuringConstructionScript, +0x29 bit 8
//                -- the K2 BeginDeferredActorSpawnFromClass path never sets it)
//   0x142c12e07  test byte ptr [world+10Dh], 20h  -> bIsTearingDown
// The ONE writer of the 10C bit is AActor::ExecuteConstruction (0x1428c5fe4,
// `or byte [world+10Ch], 2` + a scope-guard restore) -- it brackets the WHOLE
// SCS+UCS construction of every BP actor, so during a save-load's mass actor
// construction the bit is set for most of the frame. UObject_GetWorld_VtblOff is
// the virtual UEngine::GetWorldFromContextObject dispatches through
// (0x142f316e0: `call [vtbl+160h]`) -- the same world-resolution path every
// GameplayStatics spawn takes, so reading these bits through it sees EXACTLY the
// world state the engine's own spawn check reads. Consumed by ue_wrap/spawn_gate.
inline constexpr size_t UWorld_FlagsA = 0x10C;                 // byte holding bIsRunningConstructionScript
inline constexpr uint8_t UWorld_bIsRunningConstructionScript = 0x02;  // bit within FlagsA
inline constexpr size_t UWorld_FlagsB = 0x10D;                 // byte holding bIsTearingDown
inline constexpr uint8_t UWorld_bIsTearingDown = 0x20;         // bit within FlagsB
inline constexpr size_t UObject_GetWorld_VtblOff = 0x160;      // UObject::GetWorld vtable byte offset

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
// research/findings/player-puppet/body-visible-f12-and-pose-mirroring-2026-05-22.md (the dedicated mainplayer-body finding was never filed)): the
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

// UCharacterMovementComponent::MovementMode @ +0x0168 (TEnumAsByte<EMovementMode>).
// Engine.hpp:9917. Values: MOVE_None=0, MOVE_Walking=1, MOVE_NavWalking=2,
// MOVE_Falling=3, MOVE_Swimming=4, MOVE_Flying=5, MOVE_Custom=6. Accessed via
// ue_wrap::puppet::ReadCharacterIsFalling (read path, called by coop::net_pump::
// ReadLocalPose) + DriveCharacterMovement (write path) to mirror the source's
// airborne state into PoseSnapshot.stateBits bit 0 (kStateBitInAir) so the
// receiver's BUA-POST observer can clear useLegIK during jumps -- the foot-IK
// trace otherwise plants the puppet's feet to the satellite's grounded
// position because the satellite ACharacter never leaves ground (its CMC tick
// is parked).
inline constexpr size_t UCharacterMovement_MovementMode = 0x0168;
inline constexpr uint8_t kMOVE_Falling = 3;

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
// See research/findings/physics-grab/physics-object-pickup-architecture-2026-05-23.md.
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
// v54: the Aprop_C VISUAL IDENTITY -- the list_props DataTable row name that
// init() resolves the mesh/mass/collision from (CDO default row = 'cube').
// SP's loadData restores it before re-running init(); our mirror spawn writes
// it pre-FinishSpawningActor. SDK dump prop.hpp:12 `FName Name; // 0x0258`.
inline constexpr size_t Aprop_Name          = 0x0258;  // FName (ComparisonIndex @ +0, Number @ +4)
inline constexpr size_t Aprop_Static        = 0x02D8;  // bool (a Static prop can't be grabbed)
inline constexpr size_t Aprop_removeWOrespawn = 0x02D9;  // bool (despawn-without-respawn; saved by SP getData/loadData)
inline constexpr size_t Aprop_frozen        = 0x02DA;  // bool
inline constexpr size_t Aprop_sleep         = 0x02DD;  // bool (prop settled/sleeping -> physics NOT simulating). SDK dump prop.hpp:19 `bool sleep; // 0x02DD`. SP: init() = SetSimulatePhysics(NOT(static||frozen||sleep)).
inline constexpr size_t Aprop_Key           = 0x02E0;  // FName (ComparisonIndex @ +0, Number @ +4)
inline constexpr size_t Aprop_StaticMesh    = 0x0238;  // UStaticMeshComponent*
// Per-material prop sound set (sounds RE 2026-06-11; coop/prop_sound consumer):
// prop.physicsImpact -> comp.physSoundData (inline struct, populated at prop
// init by setPhysSoundData = the lib_C::physSound DataTable row) -> soft_30 =
// the cue the native E-grab plays (mainPlayer uber @100337, vol 0.5 pitch 1.0).
inline constexpr size_t Aprop_physicsImpact          = 0x0230;  // Ucomp_physicsImpact_C*  prop.hpp:8
inline constexpr size_t CompPhysImpact_physSoundData = 0x00C8;  // Fstruct_physSound (inline, 0x98)  comp_physicsImpact.hpp:9
inline constexpr size_t CompPhysImpact_PhysMat       = 0x0290;  // UPhysicalMaterial*  comp_physicsImpact.hpp (the comp's cached physmat -- input for a fresh lib_C::physSound lookup)
inline constexpr size_t FstructPhysSound_soft        = 0x0010;  // USoundBase* soft_30  struct_physSound.hpp

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

// ULightComponentBase Intensity (Engine.hpp:13569). VOTV's flashlight BP
// almost certainly toggles this between 0 and a saved "on" value instead
// of bVisible -- the probe log shows bVisible stays 0 across observed
// toggles while the light visibly flips on/off in-game. Reading this
// field in the probe gives the new state directly; the receiver mirrors
// it on the puppet's light_R via SetIntensity (name::SetIntensityFn).
inline constexpr size_t ULightComponentBase_Intensity = 0x020C;  // float

// ULocalLightComponent::IntensityUnits (Engine.hpp:13636). The unit
// scale for Intensity (ELightUnits enum: 0=Unitless, 1=Candelas, 2=Lumens).
// VOTV's mainPlayer flashlight uses Unitless (we read Intensity=0.2 in
// default state -- 0.2 lumens would be invisible, and stock UE4.27
// CDO Intensity default in Lumens mode is 5000). On Unitless scale,
// real "on" intensity is roughly 5-10. Used by Phase 5F receiver to
// mirror the source's scale when applying intensity to the puppet.
inline constexpr size_t ULocalLightComponent_IntensityUnits = 0x0328;  // uint8 (ELightUnits)

// ULightComponent::SceneProxy raw pointer (IDA-confirmed 2026-05-26 via
// the SetIntensity decompile: sub_142A930E0 -> sub_142A98430 reads
// *(QWORD*)(a1 + 0x3F8) as the proxy; the function HARD NO-OPS if
// it's null (no MarkRenderStateDirty fallback). This is the single
// most diagnostic field for the Phase 5F cone-doesn't-render problem
// -- if it's null on the puppet, MarkRenderStateDirty is a no-op and
// every Set*-on-light UFunction silently does nothing.
inline constexpr size_t ULightComponent_SceneProxy = 0x03F8;  // FLightSceneProxy*

// UActorComponent bRegistered bit (IDA-confirmed 2026-05-26: the
// SetIntensity early-out guard at sub_142A930E0:0x142A93112 tests
// `(*(BYTE*)(a1 + 0x88) & 1) == 0` which matches UE4.27's
// `IsRegistered()` && Mobility check. So bRegistered = byte 0x0088,
// bit 0. Note this packed byte ALSO holds bNetAddressable/bReplicates
// (reflected names) -- the engine union'd them onto the same byte.
inline constexpr size_t UActorComponent_RegFlagsByte = 0x0088;  // bRegistered @ bit 0

// USceneComponent::Mobility (Engine.hpp:17916). 0=Static, 1=Stationary,
// 2=Movable. Static lights don't get runtime-mutable SceneProxies, so
// if a light is mistakenly Static we'd see exactly the puppet symptom.
inline constexpr size_t USceneComponent_Mobility = 0x014F;  // uint8

// USpotLightComponent cone-angle offsets (Engine.hpp via UE4.27 stock layout).
// Read directly from sender's local light_R to snapshot the cone shape; the
// receiver writes them via the SetOuterConeAngle / SetInnerConeAngle UFunctions
// so the proxy MarkRenderStateDirty is called (direct field writes don't
// notify the renderer; the cone shape wouldn't update visually until the next
// proxy recreate). Phase 5F v6 wire-format (2026-05-26 PM).
inline constexpr size_t USpotLightComponent_InnerConeAngle = 0x0358;  // float (deg)
inline constexpr size_t USpotLightComponent_OuterConeAngle = 0x035C;  // float (deg)
inline constexpr size_t UPointLightComponent_AttenuationRadius = 0x0330;  // float (cm)

// ULightComponentBase flags byte at +0x0214 (bAffectsWorld @ bit 0,
// CastShadows @ bit 1, bCastVolumetricShadow @ bit 6, bCastDeepShadow
// @ bit 7). bAffectsWorld=false on the CDO is one of the candidate
// root causes for "SceneProxy never created" -- the OnRegister path
// skips proxy creation when bAffectsWorld is false even if bVisible
// is true.
inline constexpr size_t ULightComponentBase_FlagsByte = 0x0214;  // bAffectsWorld @ bit 0

// USceneComponent RelativeRotation (Engine.hpp:17900-ish, FRotator at
// offset 0x0128). FRotator layout is {Pitch, Yaw, Roll} so Pitch is the
// first float at +0x0128, Yaw at +0x012C, Roll at +0x0130. Used to
// drive the puppet's lag_fl spring arm pitch from the pose snapshot
// (the cone direction depends on the spring arm's pitch; without this
// the spring arm freezes at spawn-time orientation because we disable
// the puppet's actor tick).
inline constexpr size_t USceneComponent_RelativeRotation = 0x0128;  // FRotator (12 bytes)
inline constexpr size_t ACameraActor_CameraComponent = 0x0228;          // UCameraComponent*  Engine.hpp:6947
inline constexpr size_t UCameraComponent_PostProcessBlendWeight = 0x0240; // float  Engine.hpp:9762
inline constexpr size_t UCameraComponent_PostProcessSettings = 0x0270;    // FPostProcessSettings  Engine.hpp (size 0x560)
inline constexpr size_t FPostProcessSettings_Size = 0x0560;
inline constexpr size_t FPostProcessSettings_WeightedBlendables = 0x0550; // TArray (0x10) within the PP struct

// USoundAttenuation field offsets (FSoundAttenuationSettings + parent
// FBaseAttenuationSettings). Used to configure a runtime-constructed
// attenuation object for 3D positional sound (Phase 5F v6 click sound at
// puppet, RULE-1 native path -- no borrowing of VOTV `att_*` assets).
// Confirmed via UE4SS_ObjectDump_GAMEPLAY_SAVE.txt lines 34449-36817:
//   SoundAttenuation UObject @+0x28 = FSoundAttenuationSettings struct
//     extends FBaseAttenuationSettings @+0x00:
//       DistanceAlgorithm: enum @+0x08
//       AttenuationShape:  byte @+0x09 (0=Sphere, 1=Capsule, 2=Box, 3=Cone)
//       dBAttenuationAtMax:float @+0x0C
//       FalloffMode:        enum @+0x10
//       AttenuationShapeExtents: FVector @+0x14 (sphere -> X is radius cm)
//       ConeOffset:        float @+0x20
//       FalloffDistance:   float @+0x24
//     plus SoundAttenuationSettings own bytes:
//       bAttenuate  bit @ byte+0xB0 mask 0x01
//       bSpatialize bit @ byte+0xB0 mask 0x02
namespace att {
inline constexpr size_t DistanceAlgorithm       = 0x28 + 0x08;  // uint8 enum
inline constexpr size_t AttenuationShape        = 0x28 + 0x09;  // uint8 enum
inline constexpr size_t dBAttenuationAtMax      = 0x28 + 0x0C;  // float
inline constexpr size_t FalloffMode             = 0x28 + 0x10;  // uint8 enum
inline constexpr size_t AttenuationShapeExtents = 0x28 + 0x14;  // FVector (12 B)
inline constexpr size_t ConeOffset              = 0x28 + 0x20;  // float
inline constexpr size_t FalloffDistance         = 0x28 + 0x24;  // float
inline constexpr size_t FlagsByte               = 0x28 + 0xB0;  // bAttenuate@0x01, bSpatialize@0x02
}  // namespace att

inline constexpr size_t AHUD_bShowHUD = 0x0228;     // uint8  Engine.hpp:7413 (VOTV draws via UMG -> default HUD canvas off; force 1 to get ReceiveDrawHUD)
inline constexpr size_t AHUD_Canvas = 0x0270;       // UCanvas*  Engine.hpp:7422
inline constexpr size_t UCanvas_SizeX = 0x0040;     // int32     Engine.hpp:9846
inline constexpr size_t UCanvas_SizeY = 0x0044;     // int32     Engine.hpp:9847
inline constexpr size_t UEngine_SmallFont = 0x0050; // UFont*    Engine.hpp:10732

// Phase 5W (2026-05-26): AdaynightCycle_C weather state fields. All offsets
// from CXXHeaderDump/daynightCycle.hpp -- literal `@ 0xNNN` comments in the
// dump. Owned by mainGamemode.daynightCycle@0x0450 (singleton per session).
// See research/findings/weather-wind/votv-weather-RE-mainGamemode-2026-05-26.md for the
// derivation.
inline constexpr size_t AdaynightCycle_eff_rain             = 0x0228;  // UParticleSystemComponent* (the actual rain VFX driver)
inline constexpr size_t AdaynightCycle_rain                 = 0x02E0;  // float (per-frame Ease interp target -- write to anchor rainStrength)
inline constexpr size_t AdaynightCycle_isRaining            = 0x02E4;  // bool
inline constexpr size_t AdaynightCycle_isSnow               = 0x03B0;  // bool
inline constexpr size_t AdaynightCycle_enableSunlight       = 0x03D8;  // bool
inline constexpr size_t AdaynightCycle_rainStrength         = 0x0404;  // float
inline constexpr size_t AdaynightCycle_rainLightningChance  = 0x0408;  // float
inline constexpr size_t AdaynightCycle_rainDeactivateChance = 0x040C;  // float
inline constexpr size_t AdaynightCycle_rainWindSpeed        = 0x041C;  // float
inline constexpr size_t AdaynightCycle_permanentRain        = 0x042C;  // bool
inline constexpr size_t AdaynightCycle_enableMoonlight      = 0x0448;  // bool
inline constexpr size_t AdaynightCycle_enable_fog           = 0x0449;  // bool
inline constexpr size_t AdaynightCycle_enable_superfog      = 0x044A;  // bool
inline constexpr size_t AdaynightCycle_enable_rain          = 0x044B;  // bool

// Phase 5W fog (2026-06-01, 4-agent RE workflow): fog is NOT the enable_* bits
// -- those are persistent CONFIG gates ("rolls allowed"), NOT active-state. The
// ACTIVE fog is (a) the rolling-fog event ACTOR AweatherFogController_C held in
// fogEventObject, (b) any live AsuperFog_C, or (c) the height-fog DENSITY the
// cycle's SetFogDensity() pushes from finalFogDensity into ExponentialHeightFog.
// Host-authoritative clear = destroy the actors + zero finalFogDensity/thickFog +
// SetFogDensity(). See research/findings/votv-weather-RE-* + the workflow output.
inline constexpr size_t AdaynightCycle_ExponentialHeightFog = 0x0270;  // UExponentialHeightFogComponent*
inline constexpr size_t AdaynightCycle_thickFog             = 0x0330;  // float (per-tick density TARGET)
inline constexpr size_t AdaynightCycle_fogEventObject       = 0x0338;  // AweatherFogController_C* (rolling fog; presence == active)
inline constexpr size_t AdaynightCycle_finalFogDensity      = 0x0418;  // float (active height-fog density pushed via SetFogDensity)
inline constexpr size_t AdaynightCycle_fogProbability       = 0x0428;  // float (scheduler roll weight; host-side only)
inline constexpr size_t AdaynightCycle_permanentFog         = 0x042D;  // bool (sticky-fog gamerule; re-arms the scheduler)

// AweatherFogController_C (the rolling-fog actor held in fogEventObject@0x0338).
// Its OWN ReceiveTick ramps the height-fog density over its Duration -- a fresh
// actor ramps from 0, which is the late-joiner fog "warm-up". To SNAP a joiner to
// the host's CURRENT fog, copy the host actor's ramp state onto the mirror actor.
// Offsets from research/findings/weather-wind/votv-weather-RE-effect-actors-2026-05-26.md §5c
// (CXX header dump weatherFogController.hpp). Confirmed at runtime by the fog probe.
inline constexpr size_t WeatherFogController_Time     = 0x0238;  // float (elapsed lifetime clock)
inline constexpr size_t WeatherFogController_Alpha    = 0x023C;  // float (current ramp intensity)
inline constexpr size_t WeatherFogController_Duration = 0x0240;  // float (total lifetime)
inline constexpr size_t WeatherFogController_fogPhase = 0x0244;  // float (eased progress 0..1)
inline constexpr size_t WeatherFogController_Strength = 0x024C;  // float (max density scale)

// AdirectionalWind_C (the wind actor; singleton, held by mainGamemode.directionalWind
// @0x0F70). The user-visible "strong leaves shaking" is the per-tick spring `intensity`
// (a low-pass of windTarget's displacement from origin), NOT these 4 fields -- the tick
// even OVERWRITES windStrength_background = intensity every frame. The gust is energized
// per-peer by a `changeWindOrigin` RNG timer (random 1-60 s) that re-rolls windTarget's
// RelativeLocation; each peer rolls its own stream, so host is mid-gust while client is
// calm. So the real divergence is windTarget (v50, below) -- synced + the client's
// changeWindOrigin suppressed; the 4 rain/background fields stay (correct for rain-wind +
// the particle/audio/engine SPEED, which has no leaf-shake term). The totals @0x02F4/0x02F8
// are derived; the 1 s updateDirWind timer republishes the engine WindDirectionalSource.
// RE: votv-wind-basefog-RE-2026-06-08.md + votv-wind-event-driver-RE-2026-06-09.md.
inline constexpr size_t DirectionalWind_windSpeed_rain          = 0x02E4;  // float (= cycle rainWindSpeed)
inline constexpr size_t DirectionalWind_windStrength_rain       = 0x02E8;  // float (= (rainStrength+0.5)*rain)
inline constexpr size_t DirectionalWind_windSpeed_background    = 0x02EC;  // float (ambient; default 5.0; day-rollover Ease)
inline constexpr size_t DirectionalWind_windStrength_background = 0x02F0;  // float (ambient strength; tick rebases to intensity)
// v50 (2026-06-09): windTarget UBillboardComponent ptr. The spring reads its
// RelativeLocation (@USceneComponent_RelativeLocation 0x011C) as the gust input -- THE
// leaf-shake driver. Host reads / client writes that FVector; client suppresses its
// changeWindOrigin so its local RNG roll stops fighting the synced target.
inline constexpr size_t DirectionalWind_windTarget             = 0x0238;  // UBillboardComponent* (RelativeLocation @ +0x011C = the gust input)

// Phase 5W Inc-fix-2: AmainGamemode_C::redSky stash pointer for the
// red-sky discrete event. Lazily filled by the gamemode's first
// spawnRedSky call. Receiver reads this to find the existing
// AredSkyEvent_C actor and invokes redSky.set(state) on it for
// subsequent toggles; if null AND newState==1, calls spawnRedSky to
// instantiate.
inline constexpr size_t AmainGamemode_redSky                = 0x0888;  // AredSkyEvent_C*

// AmainGamemode_C::saveSlot -- the live world-save container (UsaveSlot_C*), the
// object saveObjects()/saveTriggers() repopulate and SaveGameToSlot serializes.
// save_capture reads it to emit a LIVE host-world blob for a joiner (mainGamemode.hpp).
inline constexpr size_t AmainGamemode_saveSlot              = 0x04B0;  // UsaveSlot_C*

// UsaveSlot_C::objectsData -- TArray<Fstruct_save> of every world object (saveSlot.hpp).
// save_capture's rebuild-vs-append safety probe reads its Num (TArray Num @ +0x8).
inline constexpr size_t UsaveSlot_objectsData              = 0x0300;  // TArray<Fstruct_save>

// AmainGamemode_C::daynightCycle is the cycle singleton pointer. We RESOLVE
// this via R::FindObjectByClass(L"daynightCycle_C") rather than dereferencing
// off the gamemode -- saves needing the gamemode offset and matches the
// resolution pattern used elsewhere in the project.
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
inline constexpr size_t LookAt_Clamp              = 0x170;  // float degrees (FAnimNode_LookAt::LookAtClamp; class-default 45)
// FAnimNode_ModifyBone specifics (for direct head-rotation writes):
inline constexpr size_t ModBone_Translation       = 0xD8;   // FVector
inline constexpr size_t ModBone_Rotation          = 0xE4;   // FRotator
inline constexpr size_t ModBone_RotationMode      = 0xFD;   // EBoneModificationMode (0=Ignore,1=Replace,2=Additive)
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

// AnimBP node-region diagnostic offsets (used by DumpAnimNodeRegions in
// puppet.cpp). Each pair brackets a contiguous region of the kerfur
// AnimBP instance memory to scan + log for the Bug 2 puppet-locomotion
// investigation. Moved here 2026-05-27 (audit M19) from inline magic
// constants in puppet.cpp.
inline constexpr size_t kKerfurBlendSpacePlayer_Start = 0x1180;
inline constexpr size_t kKerfurBlendSpacePlayer_End   = 0x1268;
inline constexpr size_t kKerfurStateMachine1_Start    = 0x1AC0;
inline constexpr size_t kKerfurStateMachine1_End      = 0x1B70;
inline constexpr size_t kKerfurStateMachine_Start     = 0x1CC8;
inline constexpr size_t kKerfurStateMachine_End       = 0x1D78;
// AnimBP INSTANCE-LEVEL public vars tail (post-AnimGraphNode block per
// CXX dump). End-of-class is 0x2E4A; we round to 0x2E50 to cover any
// trailing padding.
inline constexpr size_t kKerfurAnimBPVarsAll_Start    = 0x2D60;
inline constexpr size_t kKerfurAnimBPVarsAll_End      = 0x2E50;
}  // namespace anim

// ---- content names: moved to ue_wrap/sdk_profile_names.h (2026-07-04 split;
//      the 1500-LOC hard cap). Included below so P::name:: keeps resolving
//      through this single header.


}  // namespace ue_wrap::profile

#include "ue_wrap/sdk_profile_names.h"
