// ue_wrap/sdk_profile.h -- the version surface: everything specific to one game build.
//
//   * AOB signatures   break on any engine recompile (even a patch);
//   * struct offsets   stable within an engine version, shift across them;
//   * content names    change with the game's blueprints and levels (sdk_profile_names.h).
//
// Re-derive on a new game version: UE4SS's log gives the GUObjectArray / FName::ToString
// addresses; IDA confirms each RVA and yields a unique AOB (rip displacements wildcarded); the
// boot HealthCheck reports what still fails.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ue_wrap::profile {

// ---- target identity -----------------------------------------------------
inline constexpr const char* kTargetGameVersion = "Alpha 0.9.0-n";
inline constexpr const char* kTargetEngineVersion = "UE4.27";  // exe FileVersion 4.27.2.0

// The exe the signatures were derived against; the boot HealthCheck warns on a size or version
// mismatch. 0 = not recorded.
inline constexpr unsigned long long kExpectedExeSize = 84751360;

// ---- AOB signatures (any recompile breaks these) ----------------------------
// FName::ToString: a unique prologue; the match is the function address.
inline constexpr const char* kSigFNameToString =
    "48 89 5C 24 18 55 56 57 48 8B EC 48 83 EC 30 8B 01 48 8B F1 "
    "44 8B 49 04 8B F8 C1 EF 10 48 8B DA 0F B7 C8";

// GUObjectArray: the static init guard's `lea rcx,[rip+&GUObjectArray]`; addr = match +
// kGUObjArrayLeaEndOff + the disp32 at match + kGUObjArrayLeaDispOff.
inline constexpr const char* kSigGUObjectArray =
    "8B 05 ?? ?? ?? ?? 3B 05 ?? ?? ?? ?? 75 13 48 8D 15 ?? ?? ?? ?? "
    "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 45 33 C9 "
    "48 89 45 ?? 4C 8D 45";
inline constexpr size_t kGUObjArrayLeaDispOff = 24;
inline constexpr size_t kGUObjArrayLeaEndOff = 28;

// UObject::ProcessEvent: a unique prologue (vtable index 68); the match is the address.
inline constexpr const char* kSigProcessEvent =
    "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC F0 00 00 00 48 8D 6C 24 30 "
    "48 89 9D 18 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C5 48 89 85 B0 00 00 00";

// FMemory::Realloc, matched only to recover GMalloc from its `mov rcx, cs:GMalloc` (&GMalloc =
// hit + kGMallocLeaEndOff + disp32). FMalloc::Free (vtable +0x30) then releases engine-allocated
// buffers: FName::ToString orphans the caller's FString buffer on every call.
inline constexpr const char* kSigFMemoryRealloc =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F1 41 8B D8 "
    "48 8B 0D ?? ?? ?? ?? 48 8B FA 48 85 C9 75 0C";
inline constexpr size_t kGMallocLeaDispOff = 24;
inline constexpr size_t kGMallocLeaEndOff  = 28;
inline constexpr size_t kFMallocFreeVtOff    = 0x30;  // FMalloc::Free    vtable byte offset (UE4.27)
inline constexpr size_t kFMallocReallocVtOff = 0x20;  // FMalloc::Realloc vtable byte offset (UE4.27)
// EngineAlloc goes through FMalloc::Realloc(nullptr, size, align), the slot the signature is
// matched from; paired with EngineFree on the same GMalloc, so the engine's own realloc or GC
// free of that buffer is allocator-matched.

// UGameplayStatics::SaveGameToSlot(USaveGame*, const FString& slot, int32) -> bool: the single
// physical write chokepoint of every save trigger and both save containers. Hooked here because
// BP-to-BP calls dispatch through ProcessInternal, past the ProcessEvent detour. IDA
// 0x142B59AA0; the match is the function address.
inline constexpr const char* kSigSaveGameToSlot =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B DA 33 F6 "
    "48 8D 54 24 30 48 89 74 24 30 48 89 74 24 38 41 8B F8";

// FD3D11Viewport::PresentChecked(int32 SyncInterval): the overlay's draw seam, the once-per-frame
// choke point both Present paths funnel through (it reads the swapchain at this+0x70 and calls
// IDXGISwapChain::Present at vtbl[8]). Hooked here, upstream of the DXGI vtable RTSS and OBS
// hook, so RTSS cannot unlink it and OBS's capture sees our pixels. IDA sub_1416F4BA0; the /GS
// displacement wildcarded.
inline constexpr const char* kSigD3D11ViewportPresentChecked =
    "48 89 5C 24 18 55 56 57 48 81 EC B0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 "
    "C4 48 89 84 24 A0 00 00 00 33 F6 89 54 24 30 48 8B D9 40 B5 01 48 8B 89";
// FD3D11Viewport::SwapChain (IDXGISwapChain*); validated by QueryInterface before use.
inline constexpr size_t kD3D11Viewport_SwapChain = 0x70;

// FD3D11Viewport::Resize(uint32,uint32,bool,EPixelFormat): the engine's own resize bracket. Its
// ResizeBuffers call fails fatally while our render target view is outstanding, and an inline
// hook on IDXGISwapChain::ResizeBuffers is one RTSS unlinks, so the release bracket hangs off the
// engine side. Unique from 26 bytes after masking the /GS displacement at +14; 32 shipped. IDA
// sub_141703750.
inline constexpr const char* kSigD3D11ViewportResize =
    "4C 8B DC 57 48 81 EC C0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 "
    "C4 48 89 84 24 80 00 00 00 49 89 5B";

// FD3D12Viewport::ResizeInternal(): the DX12 resize bracket (the backend AddRefs up to 8 back
// buffers behind one release). It takes only `this`: `mov edx,3` clobbers the second-argument
// register and the extent is read from [rbx+0x90]; the four-argument Resize is its single caller
// (image+0x1777110). A detour declares one parameter. Unique from 20 bytes with no wildcards; 32
// shipped. IDA sub_14177E8B0.
inline constexpr const char* kSigD3D12ViewportResizeInternal =
    "48 8B C4 55 57 48 8D 68 A1 48 81 EC 98 00 00 00 48 89 58 20 "
    "BA 03 00 00 00 48 89 70 E8 48 8B D9";

// FD3D12Viewport::SwapChain (IDXGISwapChain*), the DX12 twin; the same runtime validation.
inline constexpr size_t kD3D12Viewport_SwapChain = 0x60;

// FD3D12Viewport::PresentInternal(int32 SyncInterval): the DX12 draw seam (109 bytes, one caller
// past the NeedsNativePresent() gate; reads the swapchain at viewport+0x60 and tail-jumps to
// Present at vtbl[8]). Unique from 18 bytes with no wildcards; 32 shipped. IDA sub_14177E0E0.
inline constexpr const char* kSigD3D12ViewportPresentInternal =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 33 DB 8B F2 48 "
    "8B F9 85 D2 75 10 38 59 54 75 0B 38";

// UGameplayStatics::OpenLevel(WorldContextObject, FName LevelName, bool bAbsolute, FString
// Options): the level-travel seam, the one native hop of the game's death chain (every hop above
// it is EX_LocalVirtualFunction / EX_Context, invisible to the detour;
// docs/COOP_DISPATCH_VISIBILITY.md). The C++ function is detoured, not the exec thunk, so the
// parameters arrive parsed and a cancel is `return;`: SetClientTravel is the whole effect, so
// nothing half-started needs unwinding. OpenLevelBySoftObjectPtr calls the same function;
// nothing else in the image does. ABI: RCX = WorldContextObject, RDX = FName by value, R8B =
// bAbsolute, R9 = FString* Options, which the callee destroys (a cancel frees it; a trampoline
// call must not). Prologue through `mov rsi, rax` with the GEngine load and the
// GetWorldFromContextObject rel32 wildcarded: occ=1 at 0x142B530B0.
inline constexpr const char* kSigOpenLevel =
    "48 89 54 24 10 55 53 56 41 56 48 8D 6C 24 C1 48 81 EC E8 00 00 00 41 0F B6 D8 "
    "48 8B D1 48 8B 0D ?? ?? ?? ?? 41 B8 01 00 00 00 4D 8B F1 E8 ?? ?? ?? ?? 48 89 "
    "45 9F 48 8B F0";

// ---- struct offsets (stable within UE4.27; re-check on an engine bump) ----
namespace off {
inline constexpr size_t UObject_ObjectFlags = 0x08;   // int32 EObjectFlags -- RF_BeginDestroyed 1<<15 / RF_FinishDestroyed 1<<16 name the destruction STAGE
inline constexpr size_t UObject_InternalIndex = 0x0C;  // int32 -- slot in GUObjectArray (O(1) liveness check)
inline constexpr size_t UObject_ClassPrivate = 0x10;
inline constexpr size_t UObject_NamePrivate = 0x18;
inline constexpr size_t UObject_OuterPrivate = 0x20;

inline constexpr size_t FUObjectArray_ObjObjects = 0x10;  // FChunkedFixedUObjectArray
inline constexpr size_t Chunk_Objects = 0x00;             // FUObjectItem** chunk table
inline constexpr size_t Chunk_NumElements = 0x14;
inline constexpr int32_t ElemsPerChunk = 64 * 1024;
inline constexpr size_t FUObjectItem_Stride = 0x18;       // {Object*, flags, cluster, serial}
inline constexpr size_t FUObjectItem_Flags = 0x08;        // int32 EInternalObjectFlags (PendingKill/Unreachable -> dying)
inline constexpr size_t FUObjectItem_SerialNumber = 0x10; // int32, LAZY (0 until a weak ref exists) -- CachedObjRef serial rule
inline constexpr size_t mainGameInstance_loadObjects = 0x0229;  // bool: apply the save on BeginPlay (vs fresh)
inline constexpr size_t mainGameInstance_GameMode = 0x01E1;  // TEnumAsByte<enum_gamemode::Type> (story/sandbox/...; the menu sets it on load from the slot-name prefix, our LoadStorySave must too) -- mainGameInstance.hpp:11
// The mod's own UMG widget tree (built through SpawnObject):
inline constexpr size_t UUserWidget_WidgetTree = 0x01D8;       // UWidgetTree*
inline constexpr size_t UWidgetTree_RootWidget = 0x0028;       // UWidget*
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

// The menu-button inject (a UButton at the top of the UVerticalBox holding button_start in
// ui_menu_C). UMG.hpp / SlateCore.hpp lines are the CXX header dump's.
inline constexpr size_t UWidget_Slot = 0x0028;                 // UWidget::Slot (UPanelSlot*) -- the layout-slot back-pointer. UMG.hpp:1742
// The UVerticalBoxSlot layout region (UMG.hpp:1705-1710): Size@0x38, Padding@0x40, HAlign@0x58,
// VAlign@0x59, cloned as one block so an injected item's slot matches a native one; the base
// UPanelSlot pointers (< 0x38) are excluded.
inline constexpr size_t UVerticalBoxSlot_LayoutStart = 0x0038;
inline constexpr size_t UVerticalBoxSlot_LayoutSize  = 0x0022;  // 0x38..0x5A (Size+Padding+H/VAlign)
inline constexpr size_t UPanelSlot_Parent = 0x0028;            // UPanelSlot::Parent (UPanelWidget*) -- the containing panel. UMG.hpp:1009
inline constexpr size_t UPanelSlot_Content = 0x0030;           // UPanelSlot::Content (UWidget*). UMG.hpp:1010
inline constexpr size_t UPanelWidget_Slots = 0x0108;           // UPanelWidget::Slots (TArray<UPanelSlot*>). UMG.hpp:1016
// UButtonSlot (a UButton's content slot; UMG.hpp:314-318). Its default HAlign_Center indents the
// label; the inject sets Fill with left-justified text. EHorizontalAlignment: Fill=0 / Left=1 /
// Center=2; EVerticalAlignment: Center=2.
inline constexpr size_t UButtonSlot_Padding = 0x0038;         // FMargin (0x10)
inline constexpr size_t UButtonSlot_HAlign  = 0x0048;         // TEnumAsByte<EHorizontalAlignment>
inline constexpr size_t UButtonSlot_VAlign  = 0x0049;         // TEnumAsByte<EVerticalAlignment>
inline constexpr size_t UButton_WidgetStyle = 0x0128;          // FButtonStyle (size 0x278). UMG.hpp:287
inline constexpr size_t UButton_ColorAndOpacity = 0x03A0;      // FLinearColor. UMG.hpp:288
inline constexpr size_t UButton_BackgroundColor = 0x03B0;      // FLinearColor. UMG.hpp:289
inline constexpr size_t FButtonStyle_Size = 0x278;             // clone size for the button-style copy (matches NEW GAME)
// FButtonStyle's two FSlateSound members are { UObject* ResourceObject @0x00 } plus an
// unreflected TSharedPtr cache @0x08 (0x10 bytes) that a raw memcpy would alias without AddRef;
// the clone keeps ResourceObject and zeroes only the cache, which Slate rebuilds lazily.
// SlateCore.hpp:18-19, 320-324.
inline constexpr size_t FButtonStyle_PressedSlateSound = 0x248;  // FSlateSound (0x18) within FButtonStyle
inline constexpr size_t FButtonStyle_HoveredSlateSound = 0x260;  // FSlateSound (0x18) within FButtonStyle
inline constexpr size_t FSlateSound_Size = 0x18;                 // SlateCore.hpp:324
inline constexpr size_t FSlateSound_CacheStart = 0x08;           // trailing TSharedPtr cache (zero only this, keep ResourceObject @ 0x00)

// ---- FSlateBrush and the style structs that embed it -----------------------------------
// FSlateBrush (0x88) carries an unreflected TSharedPtr handle at +0x70..0x7F (its reflected
// members end at 0x6F, its bitfields resume at 0x80). A raw copy aliases it without AddRef, so
// every brush clone goes through one helper that zeroes the handle from these tables; Slate
// rebuilds it from ResourceObject.
inline constexpr size_t FSlateBrush_Size           = 0x88;
inline constexpr size_t FSlateBrush_ResourceHandle = 0x70;  // UNREFLECTED TSharedPtr, 0x70..0x7F
inline constexpr size_t FSlateBrush_HandleSize     = 0x10;
inline constexpr size_t FSlateBrush_ResourceObject = 0x48;  // UObject* -- the art the handle caches
inline constexpr size_t FSlateBrush_TintColor      = 0x20;  // FSlateColor (0x28); rule byte @ +0x10
// FButtonStyle (0x278): four brushes, then two FMargins, then the two FSlateSounds above.
inline constexpr size_t FButtonStyleBrushes[4] = {0x08, 0x90, 0x118, 0x1A0};
// FScrollBarStyle (0x4D0): nine brushes; a blind WidgetBarStyle copy aliases all nine.
inline constexpr size_t FScrollBarStyle_Size = 0x4D0;
inline constexpr size_t FScrollBarStyleBrushes[9] = {0x08, 0x90, 0x118, 0x1A0, 0x228,
                                                    0x2B0, 0x338, 0x3C0, 0x448};

// The UMG widgets the browser builds (UMG.hpp: UImage:695-721, UScrollBox:1173-1214,
// USizeBox:1227-1262, and the panel slots).
inline constexpr size_t UImage_Brush            = 0x0108;  // FSlateBrush
inline constexpr size_t UScrollBox_WidgetBarStyle     = 0x0348;  // FScrollBarStyle (0x4D0)
// The four fields that decide whether a wheel event moves this widget, read by the browser's
// self-check so a "the wheel did nothing" verdict names its cause; hand-spawned, so each is the
// engine CDO's value.
//   ConsumeMouseWheel: WhenScrollingPossible=0 Always=1 Never=2
//   Orientation:       Horizontal=0 Vertical=1
inline constexpr size_t UScrollBox_Orientation        = 0x0828;  // TEnumAsByte<EOrientation>
inline constexpr size_t UScrollBox_ConsumeMouseWheel  = 0x082A;  // EConsumeMouseWheel
inline constexpr size_t UScrollBox_AnimateWheelScroll = 0x0847;  // bool -- offset lands over frames
inline constexpr size_t UScrollBox_WheelScrollMult    = 0x0854;  // float -- 0 would eat every notch
inline constexpr size_t UOverlaySlot_Padding = 0x0040;
inline constexpr size_t UOverlaySlot_HAlign  = 0x0050;
inline constexpr size_t UOverlaySlot_VAlign  = 0x0051;
inline constexpr size_t UHorizontalBoxSlot_Padding = 0x0040;
inline constexpr size_t UHorizontalBoxSlot_Size    = 0x0050;  // FSlateChildSize (0x8)
inline constexpr size_t UHorizontalBoxSlot_HAlign  = 0x0058;
inline constexpr size_t UHorizontalBoxSlot_VAlign  = 0x0059;
inline constexpr size_t UScrollBoxSlot_Padding = 0x0038;
inline constexpr size_t UScrollBoxSlot_HAlign  = 0x0048;
inline constexpr size_t UScrollBoxSlot_VAlign  = 0x0049;
// UVerticalBoxSlot's individual members, for writing one field (the block above clones the
// region). UMG.hpp:1705-1710.
inline constexpr size_t UVerticalBoxSlot_Size    = 0x0038;  // FSlateChildSize (0x8)
inline constexpr size_t UVerticalBoxSlot_Padding = 0x0040;  // FMargin (0x10)
inline constexpr size_t UVerticalBoxSlot_HAlign  = 0x0058;
inline constexpr size_t UVerticalBoxSlot_VAlign  = 0x0059;
// FSlateChildSize: { float Value @0x00; TEnumAsByte<ESlateSizeRule> SizeRule @0x04 };
// ESlateSizeRule: Automatic = 0, Fill = 1.
inline constexpr size_t FSlateChildSize_Value    = 0x00;
inline constexpr size_t FSlateChildSize_SizeRule = 0x04;

// UStruct / UFunction / FField / FProperty layout (UE4.27's FField system), from the
// UObject::ProcessEvent decompile (rva 0x1465930): it allocates PropertiesSize, copies
// ParmsSize, walks ChildProperties through FField::Next and reads each property's flags, size
// and offset.
inline constexpr size_t UStruct_ChildProperties = 0x50;   // FField* (params first, then locals)
inline constexpr size_t UStruct_PropertiesSize = 0x58;    // int32 (full frame size)
// UFunction::Func: the pointer the BP VM invokes on every call path (UFunction::Invoke,
// 0x141302DC0: `(*(Function+0xD8))(Object, &Frame, Result)`). Patching it catches an
// EX_CallMath call ProcessEvent never sees; the value must land in .text
// (ue_wrap/ufunction_hook).
inline constexpr size_t UFunction_Func = 0xD8;

// FFrame (the BP VM frame every native exec thunk receives), from
// execBeginDeferredActorSpawnFromClass (0x14300B270): Node@0x10, Object@0x18, Code@0x20,
// Locals@0x28, PropertyChainForCompiledIn@0x80, CurrentNativeFunction@0x88. Object is the actor
// whose bytecode is executing (the spawn's source); Code is non-null on an EX_CallMath dispatch,
// whose params are stepped from the stream, not Locals.
inline constexpr size_t FFrame_Object = 0x18;             // UObject* (the executing/source object)
inline constexpr size_t FFrame_Code   = 0x20;             // uint8* (instruction ptr; null on the ProcessInternal path)

// The UWorld spawn-refusal window (ue_wrap/spawn_gate). UWorld::SpawnActor (0x142C12D20) returns
// null silently on `[world+10Ch] & 2` (bIsRunningConstructionScript; the K2 deferred spawn
// never sets bAllowDuringConstructionScript) and on `[world+10Dh] & 20h` (bIsTearingDown). The
// 10C bit's one writer is AActor::ExecuteConstruction (0x1428c5fe4), bracketing every BP actor's
// SCS+UCS construction, so it is set for most of a save load's frame. UObject_GetWorld_VtblOff
// is the virtual GetWorldFromContextObject dispatches through (`call [vtbl+160h]`), the
// world-resolution path every GameplayStatics spawn takes.
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

// UStruct::SuperStruct, for walking the inheritance chain; confirmed 0x40 at runtime (Actor's
// qword at 0x40 is the Object class).
inline constexpr size_t UStruct_SuperStruct = 0x40;

// The APawn / AActor fields that make a spawned pawn act as a local player, zeroed in the
// deferred-spawn window so a remote pawn never auto-possesses or takes input (Engine.hpp: APawn
// 0x230/0x231, AActor 0x5A/0xF3; EAutoReceiveInput::Disabled = 0).
inline constexpr size_t AActor_bBlockInput = 0x5A;          // uint8 (set 1)
inline constexpr size_t AActor_AutoReceiveInput = 0xF3;     // uint8 enum (set 0)
inline constexpr size_t APawn_Controller = 0x0258;          // AController* (null on an orphan pawn). Engine.hpp:7646. The AnimBP's idle-to-walk transition needs AnimBP.Controller != None, fed from the local player's controller.
inline constexpr size_t APawn_AutoPossessPlayer = 0x230;    // uint8 enum (set 0)
inline constexpr size_t APawn_AutoPossessAI = 0x231;        // uint8 enum (set 0)
inline constexpr size_t APawn_AIControllerClass = 0x238;    // UClass* (TSubclassOf<AController>)

// Remote-body animation: an unrendered skeletal mesh stops ticking its pose and collapses to a
// stick, so AlwaysTickPoseAndRefreshBones (=0) is forced; the body AnimBP's walk speed is driven
// directly (0 = idle).
inline constexpr size_t USkinnedMesh_VisibilityBasedAnimTickOption = 0x604;  // uint8 (set 0)  Engine.hpp:18308
inline constexpr size_t USkeletalMesh_AnimScriptInstance = 0x6B0;            // AnimInstance*  Engine.hpp:18095

// ---- the puppet (the remote body) --------------------------------------------
// The body AnimBP poses from its own public variables and needs no possessing controller (the
// figura and kerfurOmega NPC classes share it), so the puppet is an inert mainPlayer_C orphan
// (ue_wrap/actors/puppet) wearing the peer's skin, posed by writing the variables.
inline constexpr size_t AmainPlayer_mesh_playerVisible = 0x04F8;  // USkeletalMeshComponent*  mainPlayer.hpp:13
inline constexpr size_t USkinnedMesh_SkeletalMesh = 0x0480;       // USkeletalMesh* (the skin asset)  Engine.hpp:18299
inline constexpr size_t USkeletalMesh_AnimClass = 0x06A8;         // TSubclassOf<UAnimInstance>  Engine.hpp:18094

// The puppet spawns mainPlayer_C directly (inertPawn=true): its PostProcess components are
// destroyed (they grade the local camera), the first-person arms are hidden, mesh_playerVisible
// stays visible. Offsets from mainPlayer.hpp.
inline constexpr size_t AmainPlayer_PostProcess_overlays_OBSOLETE = 0x04C8;  // UPostProcessComponent*  mainPlayer.hpp:9
inline constexpr size_t AmainPlayer_mic                           = 0x0518;  // UAudioCaptureComponent*  mainPlayer.hpp (mic input; must be destroyed on the orphan to prevent latent device hold)
inline constexpr size_t AmainPlayer_PostProcess_pl                = 0x0590;  // UPostProcessComponent*  mainPlayer.hpp:50
inline constexpr size_t AmainPlayer_arms                          = 0x05F8;  // USkeletalMeshComponent*  mainPlayer.hpp:55 (FP viewmodel)
inline constexpr size_t AmainPlayer_playermodel                   = 0x0638;  // USkeletalMeshComponent*  mainPlayer.hpp:58 (legacy/equipment overlay; review visibility)
inline constexpr size_t AmainPlayer_GameMode                      = 0x0C80;  // AmainGamemode_C*  mainPlayer.hpp (cached at BeginPlay; nulled on the puppet so subsequent gamemode-interaction BP paths return early)
inline constexpr size_t mainGamemode_mainPlayer                   = 0x0630;  // AmainPlayer_C*  mainGamemode.hpp (the canonical "the local player" ref the gamemode uses for save/sleep/damage; the orphan's BeginPlay would overwrite it via intComs_gamemodeBeginPlay -- the puppet path captures + restores)

// The AnimBlueprint_kerfurOmega_regular_C variables (spd, animWalkAlpha, animWalkRate) are
// resolved by name (ue_wrap/reflected_offset): a game recook shifts a BP-cooked class's offsets,
// not its field names.

// AController::ControlRotation: the view rotation the input system accumulates; a direct write
// is equivalent to the SetControlRotation UFunction. Engine.hpp:7071.
inline constexpr size_t AController_ControlRotation = 0x0288;  // FRotator

// ACharacter's capsule, for the puppet's foot-on-ground placement: the half-height is the
// distance from the actor centre to the ground.
inline constexpr size_t ACharacter_Mesh             = 0x0280;          // USkeletalMeshComponent*  Engine.hpp:6970 (native body slot; mainPlayer_C uses mesh_playerVisible @0x04F8 as the authoritative body and the native slot is typically hidden)
inline constexpr size_t ACharacter_CharacterMovement = 0x0288;         // UCharacterMovementComponent*  Engine.hpp:6971 (mainPlayer_C orphan: must be tick-disabled on the puppet so the satellite is the only Velocity source)
inline constexpr size_t ACharacter_CapsuleComponent = 0x0290;          // UCapsuleComponent*  Engine.hpp:6972
inline constexpr size_t UCapsuleComponent_CapsuleHalfHeight = 0x0468;  // float  Engine.hpp:9883

// UCharacterMovementComponent::MovementMode @ +0x0168 (Engine.hpp:9917): MOVE_None=0, Walking=1,
// NavWalking=2, Falling=3, Swimming=4, Flying=5, Custom=6. The source's airborne state rides
// PoseSnapshot.stateBits bit 0 so the receiver clears useLegIK during jumps (the puppet's CMC
// tick is parked, so it never leaves the ground itself).
inline constexpr size_t UCharacterMovement_MovementMode = 0x0168;
inline constexpr uint8_t kMOVE_Falling = 3;

// USceneComponent::AttachParent @ +0x00C0 (Engine.hpp:17900). The puppet spawn's diagnostic dump
// checks mesh_playerVisible's parent: a propagating hide on ACharacter::Mesh would cascade to
// the body if that is the parent.
inline constexpr size_t USceneComponent_AttachParent      = 0x00C0;
// USceneComponent flag bytes: bVisible is bit 4 of 0x14C, bHiddenInGame bit 2 of 0x14D
// (Engine.hpp:17917 ordering); logged raw, masked at the read site.
inline constexpr size_t USceneComponent_VisFlagsByte      = 0x014C;  // bVisible @ bit 4
inline constexpr size_t USceneComponent_HiddenFlagsByte   = 0x014D;  // bHiddenInGame @ bit 2

// USceneComponent::RelativeLocation @ +0x011C (Engine.hpp:17904): the BP-authored offset to the
// parent, read raw because K2_GetComponentLocation returns a world location that is unsettled
// mid-init. The puppet reads the local player's settled mesh_playerVisible.RelativeLocation.Z
// once at spawn (a BP constant, the same on every peer) and applies it as its actor-level Z
// offset.
inline constexpr size_t USceneComponent_RelativeLocation = 0x011C;     // FVector  Engine.hpp:17904

// ---- Physics-prop pickup --------------------------------------------------
// mainPlayer_C grabs any Aprop_C derivative on an E press; lift versus drag is data-driven
// (Fstruct_prop.heavy, loaded from the DataTable at prop Init), so both peers compute the same
// answer from the same prop.

// The mainPlayer_C grab-state fields are resolved by name (ue_wrap/reflected_offset). The light
// grab is a UPhysicsHandleComponent; the heavy grab is a UPhysicsConstraintComponent, so the PHC
// observers do not fire on a heavy drag.

// UPhysicsHandleComponent layout, from the ReleaseComponent / UpdateHandleTransform decompiles
// (0x142D7C670 / 0x142D7EE30); matches stock UE4.27.
inline constexpr size_t UPhysicsHandleComponent_GrabbedComponent   = 0x00B0;  // UPrimitiveComponent* (cleared on Release)

// Aprop_C field offsets (prop.hpp). Key is an FName (ComparisonIndex u32 + Number u32); the game
// mints a random key for a prop that has none, so cross-peer addressing goes through
// coop/element/portable_identity.
inline constexpr size_t Aprop_propData      = 0x0260;  // Fstruct_prop (sizeof 0x72)
inline constexpr size_t Fstruct_prop_heavy  = 0x006C;  // bool inside propData -- THE heavy flag
inline constexpr size_t Aprop_propData_heavy = Aprop_propData + Fstruct_prop_heavy;  // = 0x02CC
// The list_props DataTable row name init() resolves the mesh, mass and collision from (the CDO
// default is 'cube'); a mirror spawn writes it before FinishSpawningActor. prop.hpp:12.
inline constexpr size_t Aprop_Name          = 0x0258;  // FName (ComparisonIndex @ +0, Number @ +4)
inline constexpr size_t Aprop_Static        = 0x02D8;  // bool (a Static prop can't be grabbed)
inline constexpr size_t Aprop_removeWOrespawn = 0x02D9;  // bool (despawn-without-respawn; saved by SP getData/loadData)
inline constexpr size_t Aprop_frozen        = 0x02DA;  // bool
inline constexpr size_t Aprop_sleep         = 0x02DD;  // bool (prop settled/sleeping -> physics NOT simulating). SDK dump prop.hpp:19 `bool sleep; // 0x02DD`. SP: init() = SetSimulatePhysics(NOT(static||frozen||sleep)).
inline constexpr size_t Aprop_Key           = 0x02E0;  // FName (ComparisonIndex @ +0, Number @ +4)
inline constexpr size_t Aprop_StaticMesh    = 0x0238;  // UStaticMeshComponent*
// The per-material prop sound set: prop.physicsImpact -> physSoundData (filled at prop init from
// the lib_C::physSound DataTable row) -> soft_30, the cue the native E grab plays (mainPlayer
// ubergraph @100337, volume 0.5, pitch 1.0).
inline constexpr size_t Aprop_physicsImpact          = 0x0230;  // Ucomp_physicsImpact_C*  prop.hpp:8
inline constexpr size_t CompPhysImpact_physSoundData = 0x00C8;  // Fstruct_physSound (inline, 0x98)  comp_physicsImpact.hpp:9
inline constexpr size_t CompPhysImpact_PhysMat       = 0x0290;  // UPhysicalMaterial*  comp_physicsImpact.hpp (the comp's cached physmat -- input for a fresh lib_C::physSound lookup)
inline constexpr size_t FstructPhysSound_soft        = 0x0010;  // USoundBase* soft_30  struct_physSound.hpp

// The freecam gamma fix: a bare ACameraActor renders with the default post-process, so the
// player camera's FPostProcessSettings are copied onto it, with the one TArray
// (WeightedBlendables @0x550) zeroed so the two cameras do not alias one heap array.
inline constexpr size_t AmainPlayer_Camera = 0x0530;                    // UCameraComponent*  mainPlayer.hpp:20

// The flashlight: the player-held light is the SpotLight on mainPlayer itself (the _a / _b item
// actors carry no light); puppets are mainPlayer_C orphans with the same offsets.
inline constexpr size_t AmainPlayer_light_R         = 0x0678;  // USpotLightComponent* (mainPlayer.hpp:61)
inline constexpr size_t AmainPlayer_lag_fl          = 0x0670;  // USpringArmComponent* parenting light_R (mainPlayer.hpp:60)
inline constexpr size_t AmainPlayer_flashlight     = 0x0838;  // bool -- canonical on/off (mainPlayer.hpp:115)
inline constexpr size_t AmainPlayer_flashlightMode = 0x0C79;  // uint8 -- beam mode (mainPlayer.hpp:251)
inline constexpr size_t AmainPlayer_hasFlashlight  = 0x0CC2;  // bool -- equipped guard (mainPlayer.hpp:260)
inline constexpr size_t AmainPlayer_crankFlashlight = 0x0CC4;  // bool -- _c variant marker (mainPlayer.hpp:262)

// ULightComponentBase::Intensity (Engine.hpp:13569). The game's flashlight BP toggles Intensity,
// not bVisible, so the sender reads it and the receiver mirrors it through SetIntensity.
inline constexpr size_t ULightComponentBase_Intensity = 0x020C;  // float

// USpotLightComponent cone angles (the stock UE4.27 layout): read raw on the sender, written on
// the receiver through the Set*ConeAngle UFunctions so MarkRenderStateDirty runs.
inline constexpr size_t USpotLightComponent_InnerConeAngle = 0x0358;  // float (deg)
inline constexpr size_t USpotLightComponent_OuterConeAngle = 0x035C;  // float (deg)

// USceneComponent::RelativeRotation (FRotator @+0x0128: Pitch, Yaw @+0x012C, Roll @+0x0130).
// Drives the puppet's lag_fl spring-arm pitch; the puppet's actor tick is off, so nothing else
// moves it.
inline constexpr size_t USceneComponent_RelativeRotation = 0x0128;  // FRotator (12 bytes)
inline constexpr size_t ACameraActor_CameraComponent = 0x0228;          // UCameraComponent*  Engine.hpp:6947
inline constexpr size_t UCameraComponent_PostProcessBlendWeight = 0x0240; // float  Engine.hpp:9762
inline constexpr size_t UCameraComponent_PostProcessSettings = 0x0270;    // FPostProcessSettings  Engine.hpp (size 0x560)
inline constexpr size_t FPostProcessSettings_Size = 0x0560;
inline constexpr size_t FPostProcessSettings_WeightedBlendables = 0x0550; // TArray (0x10) within the PP struct

// USoundAttenuation field offsets (FSoundAttenuationSettings over FBaseAttenuationSettings), for
// the attenuation object the mod constructs at runtime. From the object dump:
//   SoundAttenuation UObject @+0x28 = FSoundAttenuationSettings
//     FBaseAttenuationSettings @+0x00: DistanceAlgorithm enum @+0x08, AttenuationShape byte
//     @+0x09 (0=Sphere, 1=Capsule, 2=Box, 3=Cone), dBAttenuationAtMax float @+0x0C, FalloffMode
//     enum @+0x10, AttenuationShapeExtents FVector @+0x14 (a sphere's X is the radius, cm),
//     ConeOffset float @+0x20, FalloffDistance float @+0x24
//     own bytes: bAttenuate bit @+0xB0 mask 0x01, bSpatialize mask 0x02
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

// AdaynightCycle_C weather fields, from the CXX header dump's daynightCycle.hpp; owned by
// mainGamemode.daynightCycle@0x0450 (one per session).
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

// Fog is not the enable_* bits (persistent config gates). The active fog is the
// AweatherFogController_C in fogEventObject, any live AsuperFog_C, or the height-fog density
// SetFogDensity() pushes from finalFogDensity; a host-authoritative clear destroys the actors and
// zeroes finalFogDensity / thickFog.
inline constexpr size_t AdaynightCycle_thickFog             = 0x0330;  // float (per-tick density TARGET)
inline constexpr size_t AdaynightCycle_fogEventObject       = 0x0338;  // AweatherFogController_C* (rolling fog; presence == active)
inline constexpr size_t AdaynightCycle_finalFogDensity      = 0x0418;  // float (active height-fog density pushed via SetFogDensity)
inline constexpr size_t AdaynightCycle_fogProbability       = 0x0428;  // float (scheduler roll weight; host-side only)
inline constexpr size_t AdaynightCycle_permanentFog         = 0x042D;  // bool (sticky-fog gamerule; re-arms the scheduler)

// AweatherFogController_C (held in fogEventObject@0x0338). Its ReceiveTick ramps the height-fog
// density over Duration from 0, so a joiner is snapped to the host's fog by copying the host
// actor's ramp state onto the mirror. From weatherFogController.hpp; confirmed by the fog probe.
inline constexpr size_t WeatherFogController_Time     = 0x0238;  // float (elapsed lifetime clock)
inline constexpr size_t WeatherFogController_Alpha    = 0x023C;  // float (current ramp intensity)
inline constexpr size_t WeatherFogController_Duration = 0x0240;  // float (total lifetime)
inline constexpr size_t WeatherFogController_fogPhase = 0x0244;  // float (eased progress 0..1)
inline constexpr size_t WeatherFogController_Strength = 0x024C;  // float (max density scale)

// AdirectionalWind_C (a singleton, mainGamemode.directionalWind@0x0F70). The visible leaf shake
// is the per-tick spring `intensity`, driven by windTarget's displacement, which a per-peer
// `changeWindOrigin` RNG timer (1-60 s) re-rolls; the sync carries windTarget (below) and
// suppresses the client's roll. These four fields feed rain wind and the particle, audio and
// engine speed; the totals @0x02F4/0x02F8 are derived, and the 1 s updateDirWind timer
// republishes the WindDirectionalSource.
inline constexpr size_t DirectionalWind_windSpeed_rain          = 0x02E4;  // float (= cycle rainWindSpeed)
inline constexpr size_t DirectionalWind_windStrength_rain       = 0x02E8;  // float (= (rainStrength+0.5)*rain)
inline constexpr size_t DirectionalWind_windSpeed_background    = 0x02EC;  // float (ambient; default 5.0; day-rollover Ease)
inline constexpr size_t DirectionalWind_windStrength_background = 0x02F0;  // float (ambient strength; tick rebases to intensity)
// The windTarget UBillboardComponent: the spring reads its RelativeLocation (@0x011C) as the gust
// input; the host reads and the client writes it.
inline constexpr size_t DirectionalWind_windTarget             = 0x0238;  // UBillboardComponent* (RelativeLocation @ +0x011C = the gust input)

// AmainGamemode_C::redSky: the AredSkyEvent_C actor spawnRedSky stashes; the receiver calls
// redSky.set(state) on it, or spawnRedSky when it is null and the state is on.
inline constexpr size_t AmainGamemode_redSky                = 0x0888;  // AredSkyEvent_C*

// AmainGamemode_C::saveSlot: the live world-save container saveObjects() / saveTriggers()
// repopulate and SaveGameToSlot serialises; save_capture reads it (mainGamemode.hpp).
inline constexpr size_t AmainGamemode_saveSlot              = 0x04B0;  // UsaveSlot_C*

// UsaveSlot_C::objectsData: the TArray<Fstruct_save> of every world object (saveSlot.hpp);
// save_capture's safety probe reads its Num (@ +0x8).
inline constexpr size_t UsaveSlot_objectsData              = 0x0300;  // TArray<Fstruct_save>

// AmainGamemode_C::daynightCycle is resolved through R::FindObjectByClass(L"daynightCycle_C"),
// so no gamemode offset is needed.
}  // namespace off

// The EPropertyFlags bits tested (engine-stable).
namespace cpf {
inline constexpr uint64_t Parm = 0x80;
inline constexpr uint64_t OutParm = 0x100;
inline constexpr uint64_t ReturnParm = 0x400;
}  // namespace cpf

// The kerfur AnimBP head-rotation pipeline (AnimBlueprint_kerfurOmega_regular.hpp,
// AnimGraphRuntime.hpp). Two FAnimNode_LookAt nodes rotate `BoneToModify` toward their target
// regardless of `lookingAtPlayer` or `headLookAt`; the bypass sets each LookAt's `Alpha` to 0
// and clears `bAlphaBoolEnabled`. Of the 7 FAnimNode_ModifyBone instances, the one with
// BoneToModify == 'head' takes direct FRotator writes.
namespace anim {
// FAnimNode_SkeletalControlBase fields (relative to the node base):
inline constexpr size_t SkelCtl_bAlphaBoolEnabled = 0x29;   // bool
inline constexpr size_t SkelCtl_Alpha             = 0x2C;   // float
// FAnimNode_LookAt and FAnimNode_ModifyBone share BoneToModify at the same offset:
inline constexpr size_t LookAtMod_BoneToModify    = 0xC8;   // FBoneReference (FName @+0)
// FAnimNode_LookAt specifics:
inline constexpr size_t LookAt_LookAtTarget       = 0xE0;   // FBoneSocketTarget (0x60 bytes)
inline constexpr size_t LookAt_LookAtLocation     = 0x140;  // FVector (fallback if no socket target)
inline constexpr size_t LookAt_Clamp              = 0x170;  // float degrees (FAnimNode_LookAt::LookAtClamp; class-default 45)
// FAnimNode_ModifyBone specifics (for direct head-rotation writes):
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

// AnimBP node-region diagnostic offsets (DumpAnimNodeRegions in puppet.cpp): each pair brackets
// a region of the kerfur AnimBP instance to scan and log.
inline constexpr size_t kKerfurBlendSpacePlayer_Start = 0x1180;
inline constexpr size_t kKerfurBlendSpacePlayer_End   = 0x1268;
inline constexpr size_t kKerfurStateMachine1_Start    = 0x1AC0;
inline constexpr size_t kKerfurStateMachine1_End      = 0x1B70;
inline constexpr size_t kKerfurStateMachine_Start     = 0x1CC8;
inline constexpr size_t kKerfurStateMachine_End       = 0x1D78;
// The AnimBP instance-level variable tail after the AnimGraphNode block; the class ends at
// 0x2E4A, rounded to 0x2E50.
inline constexpr size_t kKerfurAnimBPVarsAll_Start    = 0x2D60;
inline constexpr size_t kKerfurAnimBPVarsAll_End      = 0x2E50;
}  // namespace anim

// ---- content names: ue_wrap/sdk_profile_names.h, included below so P::name:: resolves
//      through this single header.

}  // namespace ue_wrap::profile

#include "ue_wrap/core/sdk_profile_names.h"
