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
inline constexpr const wchar_t* ActorClassName = L"Actor";  // owns K2_Get/SetActorLocation
inline constexpr const wchar_t* GetActorLocationFn = L"K2_GetActorLocation";
inline constexpr const wchar_t* GetActorForwardVectorFn = L"GetActorForwardVector";

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

// 3D world-space text marker (renders as geometry -> works in shipping, unlike
// stripped debug-draw). ATextRenderActor + UTextRenderComponent; FText built via
// UKismetTextLibrary::Conv_StringToText.
inline constexpr const wchar_t* TextRenderActorClass = L"TextRenderActor";
inline constexpr const wchar_t* TextRenderComponentClass = L"TextRenderComponent";
inline constexpr const wchar_t* KismetTextLibraryClass = L"KismetTextLibrary";
inline constexpr const wchar_t* ConvStringToTextFn = L"Conv_StringToText";
inline constexpr const wchar_t* SetTextFn = L"SetText";
inline constexpr const wchar_t* SetWorldSizeFn = L"SetWorldSize";
inline constexpr const wchar_t* SetTextRenderColorFn = L"SetTextRenderColor";
}  // namespace name

}  // namespace ue_wrap::profile
